#pragma once

#include <cstring>
#include <cerrno>
#include <string>
#include <string_view>
#include <format>
#include <vector>
#include <filesystem>
#include <iostream>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

namespace postline {

    namespace log = spdlog;
    using json = nlohmann::json;

    [[noreturn]] void check_fail (const char* expr, const char* file, int line, const std::string& msg = "");

#define CHECK(cond, ...)                                      \
    do {                                                      \
        if (!(cond))                                          \
            check_fail(#cond, __FILE__, __LINE__ __VA_OPT__(, std::format(__VA_ARGS__))); \
    } while (0)

#define CHECK_FD(fd)  CHECK(fd >= 0, "errno: {} ({})", errno, std::strerror(errno));

    namespace fs = std::filesystem;
    extern fs::path POSTLINE_HOME;

    void setup_environ ();
    void init_logging ();

    using AccessID = int64_t;
    static constexpr AccessID NO_ACCESS_ID = -1;

    AccessID make_access_id(uint32_t segment, uint64_t offset);
    void split_access_id(AccessID access_id, uint32_t *segment, uint64_t *offset);

    // High-level message abstraction
    class Message {
        std::string header_raw_;
        std::string body_raw_;
        AccessID access_id_;
        json header_;

        static constexpr std::size_t MAX_HEADER_SIZE = 0x100000;
        static constexpr std::size_t MAX_BODY_SIZE = 0x100000000ULL;

        uint32_t crc () const;

        Message(std::string&& header_raw,
                std::string&& body_raw,
                AccessID access_id = NO_ACCESS_ID)
            : header_raw_(std::move(header_raw)),
            body_raw_(std::move(body_raw)),
            access_id_(access_id),
            header_(json::parse(header_raw_))
        {
            CHECK(header_raw_.size() <= MAX_HEADER_SIZE);
            CHECK(body_raw_.size() <= MAX_BODY_SIZE);
        }

        void importMultipartBody (std::vector<Message> const &parts);

    public:
        Message (json &&header,
                 std::string &&body_raw = "")
              : header_raw_(header.dump()),
              body_raw_(std::move(body_raw)),
              access_id_(NO_ACCESS_ID),
              header_(std::move(header))
        {
            CHECK(header_raw_.size() <= MAX_HEADER_SIZE);
            CHECK(body_raw_.size() <= MAX_BODY_SIZE);
        }

        Message (json &&header, std::vector<Message> const &parts)
              : header_raw_(header.dump()),
              access_id_(NO_ACCESS_ID),
              header_(std::move(header))
        {
            importMultipartBody(parts);
            CHECK(header_raw_.size() <= MAX_HEADER_SIZE);
            CHECK(body_raw_.size() <= MAX_BODY_SIZE);
        }

        void updateHeader (std::function<void(json &)> callback) {
            callback(header_);
            header_raw_ = header_.dump();
        }

        size_t write(int fd) const;     // returns number of bytes written
                                        //
        static Message read(int fd);    // stream version
        static Message read(int fd, uint64_t offset, unsigned segment); // pread version

        void formatEmail (std::ostream &os, bool compact = false) const;
        static Message parseEmail (std::string_view);    // without parsing body

        AccessID access_id() const { return access_id_; }
        bool has_access_id() const { return access_id_ >= 0; }

        json const& header() const { return header_; }

        size_t serialized_size () const;
    };

} // namespace postline
  //
#include "protocol.h"
