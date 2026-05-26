#include <execinfo.h>
#include <csignal>
#include <cerrno>
#include <limits>
#include <iostream>
#include <CRC.h>
#include <postline/common.h>

namespace postline {

inline void print_stacktrace(int fd = STDERR_FILENO)
{
    constexpr int max_frames = 64;
    void* frames[max_frames];
    int n = ::backtrace(frames, max_frames);
    ::backtrace_symbols_fd(frames, n, fd);
}

[[noreturn]] void check_fail(
    const char* expr,
    const char* file,
    int line,
    const std::string& msg)
{
    restore_terminal();
    std::cerr << std::format(
            "CHECK failed: {}\nat {}:{}\n{}\n",
            expr, file, line, msg);
    std::fflush(stderr);
    print_stacktrace();
    std::cerr << "The program is to be put into a stop state." << std::endl;
    std::cerr << "In shell to continue a stopped job run 'fg'" << std::endl;
    std::cerr << "You have two choices:" << std::endl;
    std::cerr << "    1. Kill the program: kill -cont " << ::getpid() << "; kill -9 " << ::getpid() << std::endl;
    std::cerr << "    1. Debug the program: sudo gdb -p " << ::getpid() << std::endl;
    std::cerr << "        Inside gdb, run 'kill' to kill the program." << std::endl;
    std::raise(SIGSTOP);
    std::abort();
}

int64_t parse_i64(std::string const& s) {
    int64_t value = 0;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);

    // Fail if:
    // 1. conversion error
    // 2. not all characters consumed (e.g. "123abc")
    if (ec != std::errc() || ptr != s.data() + s.size()) {
        return -1;
    }

    return value;
}


constexpr uint32_t RECORD_MAGIC = 0x54534f50; // "POST"

// access id layout
// -- highest --
// 1-bit 1: receiving bit
// ..... [ unused ]
// 16-bit segment
// 40-bit offset
// -- lowest --
constexpr uint64_t RECEIVING_MASK = 1ULL << 63;
constexpr uint32_t ACCESS_SEGMENT_BITS = 16;
constexpr uint32_t ACCESS_OFFSET_BITS = 40;
constexpr uint64_t ACCESS_OFFSET_MASK =
    (1ULL << ACCESS_OFFSET_BITS) - 1;

// Binary framing header (on disk / on wire)
struct RecordHeader
{
    uint32_t magic;        // constant magic number
    uint32_t header_size;  // serialized JSON header size
    uint32_t body_size;    // serialized JSON body size
    uint32_t crc;          // crc32 of [header_json + body_json]
};

void write_all(int fd, void const* buf, std::size_t n)
{
    char const* p = static_cast<char const*>(buf);
    std::size_t off = 0;

    while (off < n) {
        ssize_t w = ::write(fd, p + off, n - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error("write failed");
        }
        off += static_cast<std::size_t>(w);
    }
}

void read_all(int fd, void* buf, std::size_t n)
{
    char* p = static_cast<char*>(buf);
    size_t off = 0;

    while (off < n) {
        ssize_t r = ::read(fd, p + off, n - off);
        if (r < 0) {
            if (errno == EINTR) continue;
            CHECK(0);
        }
        if (r == 0) {   // EOF  // TODO. adapter crash causes this
            throw eof_error("");
        }
        off += static_cast<std::size_t>(r);
    }
}

void pread_all(int fd, void* buf, std::size_t n, off_t offset)
{
    char* p = static_cast<char*>(buf);
    std::size_t done = 0;

    while (done < n) {
        ssize_t r = ::pread(fd, p + done, n - done, offset + done);
        if (r < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error("pread failed");
        }
        if (r == 0) {
            throw std::runtime_error("unexpected EOF in pread_all");
        }
        done += static_cast<std::size_t>(r);
    }
}

AccessID make_access_id(uint32_t segment, uint64_t offset)
{
    CHECK(segment < (uint32_t(1) << ACCESS_SEGMENT_BITS));
    CHECK(offset < (uint64_t(1) << ACCESS_OFFSET_BITS));
    uint64_t id = (uint64_t(segment) << ACCESS_OFFSET_BITS) | offset;
    CHECK(id <= std::numeric_limits<int64_t>::max());
    return AccessID(id);
}

void split_access_id(AccessID access_id, uint32_t *segment, uint64_t *offset)
{
    CHECK(access_id != NO_ACCESS_ID);
    CHECK(segment != nullptr);
    CHECK(offset != nullptr);

    uint64_t value = static_cast<uint64_t>(access_id);
    *segment = static_cast<uint32_t>((value >> ACCESS_OFFSET_BITS) & ((1ULL << ACCESS_SEGMENT_BITS) - 1));
    *offset = value & ACCESS_OFFSET_MASK;
}

AccessID mark_receiving (AccessID id) {
    return id | RECEIVING_MASK;
}

AccessID unmark_receiving (AccessID id) {
    return id & ~RECEIVING_MASK;
}

bool is_receiving (AccessID id) {
    return (id & RECEIVING_MASK) != 0;
}


uint32_t crc (std::string_view header_raw_,
              std::string_view body_raw_) {
    uint32_t crc;
	crc = CRC::Calculate(header_raw_.data(), header_raw_.size(), CRC::CRC_32());
	crc = CRC::Calculate(body_raw_.data(), body_raw_.size(), CRC::CRC_32(), crc);
    return crc;
}

size_t Message::write(int fd) const
{
    std::string header_raw_(header_.dump());
    RecordHeader rh{};
    rh.magic = RECORD_MAGIC;
    rh.header_size = header_raw_.size();
    rh.body_size = body_raw_.size();
    rh.crc = crc(header_raw_, body_raw_);

    write_all(fd, &rh, sizeof(rh));
    write_all(fd, header_raw_.data(), header_raw_.size());
    write_all(fd, body_raw_.data(), body_raw_.size());
    return sizeof(rh) + header_raw_.size() + body_raw_.size();
}

Message Message::read(int fd)
{
    RecordHeader rh{};
    read_all(fd, &rh, sizeof(rh));

    CHECK(rh.magic == RECORD_MAGIC);

    std::string header_raw;
    header_raw.resize(rh.header_size);
    read_all(fd, header_raw.data(), header_raw.size());

    std::string body_raw;
    body_raw.resize(rh.body_size);
    read_all(fd, body_raw.data(), body_raw.size());

    CHECK(crc(header_raw, body_raw) == rh.crc);

    return Message(header_raw, std::move(body_raw));
}

Message Message::read(int fd, uint64_t offset, unsigned segment, size_t *read_size)
{
    uint64_t offset0 = offset;
    AccessID access_id = make_access_id(segment, offset);
    RecordHeader rh{};
    pread_all(fd, &rh, sizeof(rh), offset);
    offset += sizeof(rh);

    CHECK(rh.magic == RECORD_MAGIC);

    std::string header_raw;
    header_raw.resize(rh.header_size);
    pread_all(fd, header_raw.data(), header_raw.size(), offset);
    offset += header_raw.size();

    std::string body_raw;
    body_raw.resize(rh.body_size);
    pread_all(fd, body_raw.data(), body_raw.size(), offset);
    offset += body_raw.size();

    CHECK(crc(header_raw, body_raw) == rh.crc);

    if (read_size) {
        *read_size = offset - offset0;
    }
    return Message(header_raw, std::move(body_raw), access_id);
}

char const *LOCAL_AGENTS = R"(
[
{"from": "zero", "name": "echo", "service": "pipe:echo", "flags": []},
{"from": "zero", "name": "ai1", "comment": "openai", "service": "pipe:openai", "flags": ["history"]},
{"from": "zero", "name": "ai2", "comment": "anthropic", "service": "pipe:claude", "flags": ["history"]},
{"from": "zero", "name": "ai3", "comment": "openrouter", "service": "pipe:v1", "flags": ["history"]},
{"from": "zero", "name": "shell", "service": "pipe:shell", "flags": []},
{"from": "zero", "name": "mcp", "service": "pipe:mcp_bridge", "flags": []},
{"from": "zero", "name": "memory", "service": "pipe:echo -m", "flags": ["history"]},
{"from": "zero", "name": "login", "service": "pipe:login", "flags": ["clone"]},
{"from": "zero", "name": "benchmark", "service": "pipe:benchmark", "flags": []}
]
)";

void restore_terminal () {
    static constexpr char seq[] =
        "\033[?1000l"
        "\033[?1002l"
        "\033[?1003l"
        "\033[?1005l"
        "\033[?1006l"
        "\033[?1015l"
        "\033[?1016l"
        "\033[?1049l"
        "\033[?25h"
        "\033[?7h"
        "\033[0m";
    ::write(STDERR_FILENO, seq, sizeof(seq) - 1);
}


} // namespace postline
