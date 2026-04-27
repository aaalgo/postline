#include <cstdint>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <termcolor/termcolor.hpp>
#include <postline/postline.h>
#include <postline/email.h>

namespace {

static void lstrip (std::string_view &s) {
    while (!s.empty()) {
        char c = s.front();
        if (c == ' ' || c == '\t') {
            s.remove_prefix(1);
        }
        else
        {
            break;
        }
    }
}

static void rstrip (std::string_view &s) {
    while (!s.empty()) {
        char c = s.back();
        if (c == ' ' || c == '\t') {
            s.remove_suffix(1);
        }
        else
        {
            break;
        }
    }
}

}

namespace postline {

std::vector<std::string> parse_address_list (std::string_view range) {
    std::vector<std::string> ret;
    lstrip(range);
    rstrip(range);
    for (;;) {
        auto off = range.find(',');
        if (off == std::string::npos) break;
        std::string_view one(range.substr(0, off));
        rstrip(one);
        if (!one.empty()) ret.emplace_back(one);
        range = range.substr(off+1);
        lstrip(range);
    }
    if (!range.empty()) ret.emplace_back(range);
    return ret;
}


void message_parser::record_error (char const *msg, std::string_view where) {
    std::string out = msg ? std::string(msg) : std::string();
    if (!where.empty()) {
        constexpr std::size_t MAX_WHERE = 20;
        std::size_t n = where.size();
        if (n > MAX_WHERE) n = MAX_WHERE;
        std::string snippet(where.substr(0, n));
        if (where.size() > n) snippet += "...";
        if (!out.empty()) out += ": ";
        out += snippet;
    }
    errors_.emplace_back(std::move(out));
}

std::generator<field_view> message_parser::parse_headers (std::string_view &range) {
    // will modify range and eat up till the starting of body
    // expected format:  (case 1)
    //      <totally empty>
    // or   (case 2)
    //      Key: value\n
    //      Key: value\n
    //      Key: value
    // or   (case 3)
    //      Key: value\n
    //      Key: value\n
    //      Key: value\n
    // or   (case 4)
    //      Key: value\n
    //      Key: value\n
    //      Key: value\n
    //      \n
    //      body
    if (range.empty()) {
        // case 1
        co_return;
    }
    while (!range.empty()) {
        auto pos = range.find('\n');
        int has_n = 1;
        if (pos == std::string::npos) {
            pos = range.size();     // we are at the end of case 2, no ending \n
            has_n = 0;
        }
        auto key_value = range.substr(0, pos);
        range.remove_prefix(pos + has_n);
        lstrip(key_value);
        if (key_value.empty()) break;   // the empty line detected, quit
        pos = key_value.find(":");
        if (pos == std::string::npos) {
            record_error("header has no :", key_value);
            co_return;
        }
        field_view f;
        f.name = key_value.substr(0, pos);
        f.value = key_value.substr(pos+1);
        rstrip(f.name);
        lstrip(f.value);
        rstrip(f.value);
        co_yield f;
    }
}

std::generator<std::string_view> message_parser::parse_parts (std::string_view range, std::string_view delim) {
    std::string::size_type delim_size = delim.size();
    auto begin = std::string::npos;     // don't emit part upon 1st appearance
    std::string::size_type offset = 0;  // start search from here
    for (;;) {
        auto end = range.find("--", offset);
        if (end == std::string::npos) {
            record_error("no part delimiter found", range);
            co_return;
        }
        if (end > 0) {
            if (range[end-1] != '\n') {
                offset = end + 2;
                continue;
            }
        }
        // now [begin, end) is possible part
        // if boundary is determined
        auto next = end + 2;
        if (range.substr(next, delim_size).compare(delim) != 0) {
            offset = next; // this is not a full match, keep looking
            continue;
        }
        // now we have matched until --$delim
        if (begin != std::string::npos) {
            // emit the part
            co_yield range.substr(begin, end - begin);
        }
        next += delim_size; // we have matched up till --$delim
        // next -> \n
        //         --\n
        auto rem = range.substr(next);
        if (rem.starts_with("\n")) {
            next += 1;
            begin = next;
            offset = next;
            continue;
        }
        if (rem.starts_with("--\n")) {
            break;
        }
        record_error("part doesn't end properly", rem);
        break;
    }
}

message_parser::message_parser (std::string_view buffer)
{
    std::string_view boundary;

    for (auto &&f: parse_headers(buffer)) {
        if (f.name == "Subject") {
            this->subject(std::string(f.value));
        }
        else if (f.name == "From") {
            this->from(parse_address_list(f.value));
        }
        else if (f.name == "To") {
            this->to(parse_address_list(f.value));
        }
        else if (f.name == "Cc") {
            this->cc(parse_address_list(f.value));
        }
        else if (f.name == "Content-Type") {
            constexpr std::string_view prefix = "multipart/mixed; boundary=\"";
            constexpr std::string::size_type prefix_len = prefix.size();
            if (f.value.starts_with(prefix)) {
                auto rem = f.value.substr(prefix_len);
                if (rem.ends_with('"')) {
                    rem.remove_suffix(1);
                    boundary = rem;
                }
                else {
                    record_error("bad multipart boundary format", f.value);
                    break;
                }
            }
        }
        else {
            this->header(std::string(f.name), std::string(f.value), false);
        }
    }

    if (errors_.size()) return;

    if (boundary.empty()) {
        this->body(std::string(buffer));
        return;
    }

    for (std::string_view part_range: parse_parts(buffer, boundary)) {
        part &part = this->add_part();
        for (auto &&f: parse_headers(part_range)) {
            if (f.name == "Content-Type") {
            }
            else if (f.name == "Content-Disposition") {
                std::string_view prefix("attachment; filename=\"");
                if (!f.value.starts_with(prefix)) {
                    record_error("bad content-disposition", f.value);
                    break;
                }
                auto rem = f.value.substr(prefix.size());
                if (!rem.ends_with('"')) {
                    record_error("bad part content-disposition format", f.value);
                    break;
                }
                rem.remove_suffix(1);
                part.filename = rem;
            }
            else {
                part.headers.emplace_back();
                field &ff = part.headers.back();
                ff.name = f.name;
                ff.value = f.value;
            }
        }
        if (errors_.size()) break;
        part.body = part_range;
    }
}

    std::generator<std::string_view> parse_mailbox (std::string_view &range, std::string_view delim) {
        // if parser error, the range will have remainer
        // if parser succeeds, the range will be empty
        //
        // always expect a beginning delim line
        // yield only the mail bodies without the delim line
        std::string::size_type delim_size = delim.size();
        std::string::size_type begin = std::string::npos;  // don't emit part upon 1st appearance
        std::string::size_type offset = 0;
        for (;;) {
            auto end = range.find(delim, offset);
            if (end == std::string::npos) break;
            if (end > 0 && range[end-1] != '\n') {
                // delim doesn't follow \n is an error
                range.remove_prefix(end-1);
                co_return;
            }
            if (begin != std::string::npos) {
                co_yield range.substr(begin, end-begin);
            }
            offset = end + delim_size;
            offset = range.find('\n', offset);
            if (offset == std::string::npos) {
                range.remove_prefix(end + delim_size);
                co_return;     // delim line doesn't end with \n
            }
            offset += 1;
            begin = offset;
        }
        if (begin != std::string::npos && begin < range.size()) {
            co_yield(range.substr(begin));
        }
        range = std::string_view();
    };



const char POSTLINE_DELIM[] = "========== POSTLINE MESSAGE ==========";

void message::format(std::ostream& os) const
{
    // Canonicalize: always render structured fields first.
    // Then render any remaining headers (excluding duplicates of canonical fields).

    auto format_addresses = [&os](const std::vector<std::string>& addrs) {
        for (std::size_t i = 0; i < addrs.size(); ++i) {
            if (i) os << ", ";
            os << addrs[i];
        }
    };

    std::string boundary;
    if (!parts.empty())
        boundary = "== POSTLINE PART ==";

    bool wrote_any_header = false;

    // 1) Structured fields (canonical)
    if (!from.empty()) {
        os << "From: ";
        format_addresses(from);
        os << "\n";
        wrote_any_header = true;
    }
    if (!to.empty()) {
        os << "To: ";
        format_addresses(to);
        os << "\n";
        wrote_any_header = true;
    }
    if (!cc.empty()) {
        os << "Cc: ";
        format_addresses(cc);
        os << "\n";
        wrote_any_header = true;
    }
    if (!subject.empty()) {
        os << "Subject: " << subject << "\n";
        wrote_any_header = true;
    }

    // 2) Remaining headers (skip canonical and, for multipart, skip Content-Type that we'll regenerate)
    for (const auto& h : headers) {
        os << h.name << ": " << h.value << "\n";
        wrote_any_header = true;
    }

    // 3) Multipart Content-Type
    if (!parts.empty()) {
        os << "Content-Type: multipart/mixed; boundary=\"" << boundary << "\"\n";
        wrote_any_header = true;
    }

    // Separator between headers and body/parts
    if (wrote_any_header)
        os << "\n";

    // 4) Body or multipart
    if (parts.empty()) {
        os << body;
        return;
    }

    // 5) Multipart rendering
    for (const auto& p : parts) {
        os << "--" << boundary << "\n";

        // Synthesize Content-Disposition from canonical filename if available.
        if (!p.filename.empty()) {
            os << "Content-Disposition: attachment; filename=\"" << p.filename << "\"\n";
        }

        for (const auto& h : p.headers) {
            os << h.name << ": " << h.value << "\n";
        }

        os << "\n";
        os << p.body;
        if (!p.body.empty() && p.body.back() != '\n')
            os << "\n";
    }

    os << "--" << boundary << "--";
}

void message::format_context(std::ostream& os) const
{
    // Canonicalize: always render structured fields first.
    // Then render any remaining headers (excluding duplicates of canonical fields).

    auto format_addresses = [&os](const std::vector<std::string>& addrs) {
        for (std::size_t i = 0; i < addrs.size(); ++i) {
            if (i) os << ", ";
            os << addrs[i];
        }
    };

    bool wrote_any_header = false;

    // 1) Structured fields (canonical)
    if (!from.empty()) {
        os << "From: ";
        format_addresses(from);
        os << "\n";
        wrote_any_header = true;
    }
    if (!to.empty()) {
        os << "To: ";
        format_addresses(to);
        os << "\n";
        wrote_any_header = true;
    }
    if (!cc.empty()) {
        os << "Cc: ";
        format_addresses(cc);
        os << "\n";
        wrote_any_header = true;
    }
    if (!subject.empty()) {
        os << "Subject: " << subject << "\n";
        wrote_any_header = true;
    }

    // 2) Remaining headers (skip canonical and, for multipart, skip Content-Type that we'll regenerate)
    for (const auto& h : headers) {
        os << h.name << ": " << h.value << "\n";
        wrote_any_header = true;
    }

    // Separator between headers and body/parts
    if (wrote_any_header)
        os << "\n";

    // 4) Body or multipart
    if (parts.empty()) {
        os << body;
        return;
    }

    // 5) Multipart rendering
    for (const auto& p : parts) {
        os << "-- " << p.filename << "\n";

        for (const auto& h : p.headers) {
            os << h.name << ": " << h.value << "\n";
        }
        if (p.headers.size()) os << "\n";
        os << p.body;
        if (!p.body.empty() && p.body.back() != '\n')
            os << "\n";
    }
}

void message::format_color(std::ostream& os) const
{
    using namespace termcolor;

    auto format_addresses = [&os](const std::vector<std::string>& addrs) {
        for (std::size_t i = 0; i < addrs.size(); ++i) {
            if (i) os << ", ";
            os << addrs[i];
        }
    };


    // Primary headers (friendly view)
    if (!from.empty()) {
        os << cyan << "From: " << reset;
        format_addresses(from);
        os << '\n';
    }
    if (!to.empty()) {
        os << green << "To: " << reset;
        format_addresses(to);
        os << '\n';
    }
    if (!cc.empty()) {
        os << yellow << "Cc: " << reset;
        format_addresses(cc);
        os << '\n';
    }
    if (!subject.empty()) {
        os << magenta << "Subject: " << reset << subject << '\n';
    }

    // Show any remaining headers that aren't the common ones
    for (const auto& h : headers)
    {
        if (h.name == "From" || h.name == "To" || h.name == "Cc" || h.name == "Subject")
            continue;
        os << blue << h.name << ": " << reset << h.value << '\n';
    }

    os << '\n';

    // Body or multipart summary
    if (parts.empty())
    {
        os << white << body << reset;
    }
    else
    {
        // For multipart, fall back to canonical formatting but bracket with a hint
        os << yellow << "[multipart message]" << reset << '\n';
        format(os);
    }
}

mailbox::mailbox (std::string const &path, const std::function<void(message &&)>& on_message) {
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        log::info("Loading mailbox from {}...", path);
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            log::error("Failed to open for read: {}",path);
            throw std::runtime_error("failed to open mailbox");
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        std::string content = ss.str();

        std::string_view delim(POSTLINE_DELIM);
        std::string_view buffer(content);
        for (std::string_view range: parse_mailbox(buffer, delim)) {
            message_parser parser(range);
            if (parser.errors().size()) {
                for (std::string const &e: parser.errors()) {
                    log::error("message parser: {}", e);
                }
                throw std::runtime_error("failed to parse message");
            }
            on_message(std::move(parser.build()));
        };
        if (buffer.size()) {
            log::error("failed to parse mailbox");
            throw std::runtime_error("failed to parse message");
        }
    }
    else {
        std::ofstream touch(path, std::ios::out | std::ios::app | std::ios::binary);
    }
    out_.open(path, std::ios::out | std::ios::app | std::ios::binary);
    if (!out_.is_open()) {
        log::error("Failed to open for append: {}", path);
        throw std::runtime_error("failed to append to mailbox");
    }
}

mailbox::~mailbox () {
    out_.close();
}

void mailbox::append (message const &msg) {
    out_ << POSTLINE_DELIM << '\n';
    msg.format(out_);
    out_.flush();
}

}
