// email.h - Email AST and parsing API
#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <fstream>
#include <functional>
#include <generator>
#include <stdexcept>
#include <cctype>

namespace postline {

struct field {
    std::string name;
    std::string value;
};

struct part {
    std::string        filename;    // canonical for Content-Disposition
    std::vector<field> headers;
    std::string        body;        // part can have empty body
};

// Email protocol
// (When we talk about email it doesn't include the email delimiter)
// Validation
//      - Must have from
//      - Must have to
//      - (So there must be at least 2 headers)
//      - Must always end with \n
//          * case 1. has non-empty body
//                    if body doesn't end with \n, parser must add \n
//          * case 2. has empty body & non-empty parts
//                    the part delimiter guarantees and with \n
//          * case 3. empty body and empty parts
//                    the headers end with \n
//          * case 4. non-empty body and non-empty parts
//                    this is invalid.
//      - If there's parts, part must have filename
//          (if a part has no header it will break parsing as
//              there's no way to decide whether the 1st line is header or body)
//          but a part can have an empty body.

struct message {
    // Raw RFC-style headers, excluding canonical fields.
    std::vector<field> headers;
    std::string        body;
    std::vector<part>  parts;      // only if multipart

    // Canonical fields (e.g., From, To, Cc, Subject, Content-Disposition)
    // are not stored in the generic headers vector.
    // Their values are projected into dedicated structured members,
    // which serve as the single source of truth.
    // Accordingly, canonical field names must not appear in
    // message.headers or in any part.headers.
    std::vector<std::string> from;
    std::vector<std::string> to;
    std::vector<std::string> cc;
    std::string          subject;
    bool parse_error = false;

    void format(std::ostream& os) const;

    // Colorized, human-friendly rendering (for interactive display only).
    void format_color(std::ostream& os) const;

    void format_context(std::ostream& os) const;

    // ---- Canonical rules and validation ----
    static bool iequals(const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            unsigned char ca = static_cast<unsigned char>(a[i]);
            unsigned char cb = static_cast<unsigned char>(b[i]);
            if (std::tolower(ca) != std::tolower(cb))
                return false;
        }
        return true;
    }

    static bool is_canonical(const std::string& name) {
        return iequals(name, "From") ||
               iequals(name, "To") ||
               iequals(name, "Cc") ||
               iequals(name, "Subject") ||
               iequals(name, "Content-Type") ||
               iequals(name, "Content-Disposition");
    }

    void validate() const {
        if (from.empty())
            throw std::logic_error("Message must have From");
        if (to.empty())
            throw std::logic_error("Message must have To");

        // Ensure no canonical headers leak into raw headers
        for (const auto& h : headers) {
            if (is_canonical(h.name))
                throw std::logic_error("Canonical header present in message.headers: " + h.name);
        }
        for (const auto& p : parts) {
            for (const auto& h : p.headers) {
                if (is_canonical(h.name))
                    throw std::logic_error("Canonical header present in part.headers: " + h.name);
            }
        }
        if (body.size()) {
            if (body.back() != '\n') {
                throw std::logic_error("Non-empty body must end with \\n");
            }
            if (parts.size()) {
                throw std::logic_error("body and parts cannot both be non-empty");
            }
        }
    }
};

// Fluent builder for constructing message safely.

class message_builder {
public:
    message_builder() = default;

    void set_parse_error () {
        msg_.parse_error = true;
    }

    // ----- Canonical fields -----

    message_builder& from(std::string addr) {
        msg_.from.push_back(std::move(addr));
        return *this;
    }

    message_builder& from(const std::vector<std::string>& addrs) {
        msg_.from.insert(msg_.from.end(), addrs.begin(), addrs.end());
        return *this;
    }

    message_builder& to(std::string addr) {
        msg_.to.push_back(std::move(addr));
        return *this;
    }

    message_builder& to(const std::vector<std::string>& addrs) {
        msg_.to.insert(msg_.to.end(), addrs.begin(), addrs.end());
        return *this;
    }

    message_builder& cc(std::string addr) {
        msg_.cc.push_back(std::move(addr));
        return *this;
    }

    message_builder& cc(const std::vector<std::string>& addrs) {
        msg_.cc.insert(msg_.cc.end(), addrs.begin(), addrs.end());
        return *this;
    }

    message_builder& subject(std::string s) {
        msg_.subject = std::move(s);
        return *this;
    }

    // ----- Raw header (non-canonical only) -----

    message_builder& header(std::string name, std::string value, bool check=true) {
        if (check && message::is_canonical(name))
            throw std::logic_error("Do not set canonical header via header()");
        msg_.headers.push_back({std::move(name), std::move(value)});
        return *this;
    }

    // ----- Body -----

    message_builder& body(std::string text) {
        msg_.body = std::move(text);
        if (msg_.body.size() > 0) {
            if (msg_.body.back() != '\n') {
                msg_.body.push_back('\n');
            }
        }
        return *this;
    }

    // ----- Attachment -----

    message_builder& attach(std::string filename, std::string content) {
        part &p = add_part();
        // keep a default content type for attachments unless caller overrides later
        p.filename = std::move(filename);
        p.body = std::move(content);
        return *this;
    }

    part &add_part () {
        msg_.parts.emplace_back();
        return msg_.parts.back();
    }

    // ----- Finalization -----

    message build() {
        msg_.validate();
        return std::move(msg_);
    }

private:
    message msg_;
};

// Global mailbox delimiter line used by parse_mbox()/formatting utilities.
extern const char POSTLINE_DELIM[];

// Parse a single email message from the given buffer.
std::vector<std::string> parse_address_list (std::string_view);

struct field_view {
    std::string_view name;
    std::string_view value;
};

class message_parser: public message_builder {
public:
    message_parser (std::string_view);
    const std::vector<std::string> &errors() const {
        return errors_;
    }
private:
    std::vector<std::string> errors_;
    void record_error (char const *msg, std::string_view where);
    std::generator<field_view> parse_headers (std::string_view &);
    std::generator<std::string_view> parse_parts (std::string_view, std::string_view);
};


class mailbox {
public:
    mailbox (std::string const &path, const std::function<void(message &&)>& on_message);
    ~mailbox ();
    void append (message const &);
private:
    std::ofstream out_;
};

}
