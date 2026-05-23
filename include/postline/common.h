#pragma once
#include <cstring>
#include <cerrno>
#include <string>
#include <string_view>
#include <format>
#include <vector>
#include <filesystem>
#include <iostream>
#include <stdexcept>
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
#define CHECK_ERRNO(cond)  CHECK(cond, "errno: {} ({})", errno, std::strerror(errno));

    class Error : public std::runtime_error {
    public:
        template <typename... Args>
        Error(std::format_string<Args...> fmt, Args&&... args)
            : std::runtime_error(std::format(fmt, std::forward<Args>(args)...)) {}
    };

    void write_all(int fd, void const* buf, std::size_t n);
    void read_all(int fd, void* buf, std::size_t n);

    namespace fs = std::filesystem;
    extern fs::path POSTLINE_HOME;

    void setup_environ ();
    void init_logging ();

    using AccessID = int64_t;
    static constexpr AccessID NO_ACCESS_ID = -1;

    int64_t parse_i64(std::string const&);

    int64_t constexpr MESSAGE_QUIET = 0x00000001;

    struct noncopyable {
    protected:
        noncopyable() = default;
        ~noncopyable() = default;

        noncopyable(noncopyable const&) = delete;
        noncopyable& operator=(noncopyable const&) = delete;

        noncopyable(noncopyable&&) noexcept = default;
        noncopyable& operator=(noncopyable&&) noexcept = default;
    };

    struct immobile {
    protected:
        immobile() = default;
        ~immobile() = default;

        immobile(immobile const&) = delete;
        immobile& operator=(immobile const&) = delete;

        immobile(immobile&&) = delete;
        immobile& operator=(immobile&&) = delete;
    };

    class eof_error: public std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    constexpr char const *CONTEXT_HEADER_NAME = "__context";

    // High-level message abstraction
    class Message: noncopyable {
        //std::string header_raw_;
        std::string body_raw_;
        AccessID access_id_;
        json header_;

        static constexpr std::size_t MAX_HEADER_SIZE = 0x100000;
        static constexpr std::size_t MAX_BODY_SIZE = 0x100000000ULL;

        Message(std::string const &header_raw,
                std::string&& body_raw,
                AccessID access_id = NO_ACCESS_ID)
            : //header_raw_(std::move(header_raw)),
            body_raw_(std::move(body_raw)),
            access_id_(access_id),
            header_(json::parse(header_raw))
        {
            CHECK(body_raw_.size() <= MAX_BODY_SIZE);
        }

        void importMultipartBody (std::vector<Message> const &parts);

    public:
        Message ()
            : access_id_(NO_ACCESS_ID)
        {}

        Message (json &&header,
                 std::string &&body_raw = "")
              : //header_raw_(header.dump()),
              body_raw_(std::move(body_raw)),
              access_id_(NO_ACCESS_ID),
              header_(std::move(header))
        {
            //CHECK(header_raw_.size() <= MAX_HEADER_SIZE);
            CHECK(body_raw_.size() <= MAX_BODY_SIZE);
        }

        Message (json &&header, std::vector<Message> const &parts)
              : //header_raw_(header.dump()),
              access_id_(NO_ACCESS_ID),
              header_(std::move(header))
        {
            importMultipartBody(parts);
            //CHECK(header_raw_.size() <= MAX_HEADER_SIZE);
            CHECK(body_raw_.size() <= MAX_BODY_SIZE);
        }

        std::string const& get(char const* key) const
        {
            auto it = header_.find(key);
            if (it == header_.end() || it->is_null()) {
                static const std::string empty;
                return empty;
            }
            CHECK(it->is_string());
            return it->get_ref<std::string const&>();
        }

        int64_t get_id(char const* key, int64_t def = -1) const
        {
            auto it = header_.find(key);
            if (it == header_.end()) {
                return def;
            }
            if (it->is_null() || !it->is_string()) {
                log::error("get_id {} found {}", key, it->dump());
                return def;
            }
            CHECK(it->is_string());
            return parse_i64(it->get_ref<std::string const &>());
        }


        std::string const &type () const {
            return get("type");
        }

        std::string const &from () const {
            return get("From");
        }

        std::string const &replyTo () const {
            auto it = header_.find("Reply-To");
            if (it == header_.end() || it->is_null()) {
                return from();
            }
            CHECK(it->is_string());
            return it->get_ref<std::string const&>();
        }

        std::string const &to () const {
            std::string const &cloned_to = get("Postline-Cloned-To");
            if (!cloned_to.empty()) return cloned_to;
            return get("To");
        }

        int64_t flags () const {
            return get_id("Postline-Flags", 0);
        }

        int64_t thread_id () const {
            return get_id("Thread-ID");
        }

        int64_t message_id () const {
            return get_id("Message-ID");
        }

        int64_t in_reply_to () const {
            return get_id("In-Reply-To");
        }

        int64_t in_response_to () const {
            return get_id("In-Response-To");
        }

        std::vector<std::string> cc () const {
            std::vector<std::string> ret;
            auto it = header_.find("Cc");
            if (it == header_.end() || it->is_null()) {
                return ret;
            }
            if (it->is_string()) {
                ret.emplace_back(it->get<std::string>());
                return ret;
            }
            else if (it->is_array()) {
                for (auto const &item : *it) {
                    CHECK(item.is_string());
                    ret.emplace_back(item.get<std::string>());
                }
                return ret;
            }
            CHECK(0);
        }

        std::string const &subject () const {
            return get("Subject");
        }

        void updateHeader (std::function<void(json &)> callback) {
            callback(header_);
        }

        size_t write(int fd) const;     // returns number of bytes written
                                        //
        static Message read(int fd);    // stream version
        static Message read(int fd, uint64_t offset, unsigned segment, size_t *read_size = nullptr); // pread version

        void formatEmail (std::ostream &os, bool compact = false) const;
        static Message parseEmail (std::string_view);    // without parsing body

        AccessID access_id() const { return access_id_; }
        bool has_access_id() const { return access_id_ >= 0; }
        void set_access_id (AccessID access_id) {
            access_id_ = access_id;
            header_["Message-ID"] = std::format("{}", access_id);
        }

        json const& header() const { return header_; }

        std::string const &body () const { return body_raw_;}
    };

extern char const *LOCAL_AGENTS;

void restore_terminal();

} // namespace postline
  //
#include "protocol.h"
