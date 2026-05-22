#include <cstdint>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <generator>
#include <unordered_set>
//#include <termcolor/termcolor.hpp>
#include <postline/common.h>

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

std::string lowercase(std::string_view s)
{
    std::string ret(s);

    std::transform(
        ret.begin(),
        ret.end(),
        ret.begin(),
        [](unsigned char c) {
            return std::tolower(c);
        });

    return ret;
}

namespace postline {

json parse_list (std::string_view range, bool to_lower = false) {
    std::vector<std::string> ret;
    lstrip(range);
    rstrip(range);
    json list = json::array();
    for (;;) {
        auto off = range.find(',');
        if (off == std::string::npos) break;
        std::string_view one(range.substr(0, off));
        rstrip(one);
        if (!one.empty()) {
            if (to_lower) {
                list.push_back(lowercase(one));
            }
            else {
                list.push_back(one);
            }
        }
        range = range.substr(off+1);
        lstrip(range);
    }
    if (!range.empty()) {
        if (to_lower) {
            list.push_back(lowercase(range));
        }
        else {
         list.push_back(range);
        }
    }
    return list;
}


#if 0
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
#endif

struct FieldView {
    std::string_view name;
    std::string_view value;
};


static std::generator<FieldView> parse_headers (std::string_view &range) {
    // will modify range and eat up till the starting of body
    // expected format:  (case 1)
    //      <totally empty>
    // or   (case 2)
    //      Key: value\n
    //      Key: value\n
    //      Key: value           after, range will be empty
    // or   (case 3)
    //      Key: value\n
    //      Key: value\n
    //      Key: value\n         after, range will be empty
    // or   (case 4)
    //      Key: value\n
    //      Key: value\n
    //      Key: value\n
    //      \n
    //      body                 after, range will start at "body"
    if (range.empty()) {
        // case 1
        co_return;
    }
    while (!range.empty()) {
        auto pos = range.find('\n');
        if (pos == 0) {
            range.remove_prefix(1);
            co_return;
        }
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
            CHECK(0);   // but throw error
            co_return;
        }
        FieldView f;
        f.name = key_value.substr(0, pos);
        f.value = key_value.substr(pos+1);
        rstrip(f.name);
        lstrip(f.value);
        rstrip(f.value);
        co_yield f;
    }
}

#if 0
    std::generator<std::string_view> parse_parts (std::string_view range, std::string_view delim) {
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
#endif

static const std::vector<std::tuple<char const *, bool, bool, bool>> CANONICAL = {
    // field,   is_list,    is_essential, to_lower
    {"From",    false,       true,  true},
    {"To",      false,       true,  true},
    {"Cc",      true,       true,   true},
    {"Subject", false,      true,   false},
    {"Content-Type", false, false,  false}, // default is plain text
    {"Content-Disposition", false, false,   false}
};

Message Message::parseEmail (std::string_view buffer)
{
    json header;
    header["type"] == "email";
    
    if (!buffer.starts_with("From:")) {
        auto off = buffer.find("\nFrom:");
        if (off != buffer.npos && off != 0) {
            ++off;
            header["Thinking"] = buffer.substr(0, off);
            buffer = buffer.substr(off);
        }
    }

    {
        auto off = buffer.find("From:", 1);  // buffer starts with From:
                                        // in case there's another email we need to trim that
        if (off != buffer.npos && off != 0) {
            if (buffer[off-1] == '\n') --off;
            header["Trash"] = buffer.substr(off);
            buffer =  buffer.substr(0, off);
        }
    }



    for (auto &&f: parse_headers(buffer)) {
        bool missing = true;
        for (auto const [key, is_list, is_essential, to_lower] : CANONICAL) {
            if (f.name == key) {
                if (is_list) {
                    header[f.name] = parse_list(f.value, to_lower);
                }
                else {
                    if (to_lower) {
                        header[f.name] = lowercase(f.value);
                    }
                    else {
                        header[f.name] = f.value;
                    }
                }
                missing = false;
                break;
            }
        }
        if (missing) {
            header[f.name] = f.value;
        }
    }


    return Message(std::move(header), std::string(buffer));
}

#if 0
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
#endif

void write_header_value (std::ostream &os, json const &value, bool force_list) {
    if (value.is_array()) {
        bool is_first = true;
        for (auto const &v: value) {
            if (is_first) {
                is_first = false;
            }
            else {
                os << ", ";
            }
            os << v.get_ref<std::string const &>();
        }
    }
    else {
        CHECK(!force_list);
        os << value.get_ref<std::string const &>();
    }
}

void write_headers (std::ostream &os, json const &header, bool compact) {
    std::unordered_set<std::string> used;
    used.insert("type");
    used.insert("Thinking");
    used.insert("Trash");
    used.insert(CONTEXT_HEADER_NAME);
    {
        auto it = header.find("Thinking");
        if (it != header.end() && it->is_string()) {
            std::string const &thinking = it->get_ref<std::string const &>();
            os.write(thinking.data(), thinking.size());
        }
    }
    for (auto const [key, is_list, is_essential, to_lower] : CANONICAL) {
        auto it = header.find(key);
        if (it == header.end()) continue;
        used.insert(key);
        if (compact && ! is_essential) continue;
        if (it->is_null()) continue;
        os << key << ": ";
        write_header_value(os, *it, is_list);
        os << '\n';
    }
    if (!compact) {
        for (auto const &[key, value] : header.items()) {
            if (used.contains(key)) continue;
            if (value.is_null()) continue;
            os << key << ": ";
            write_header_value(os, value, false);
            os << '\n';
        }
    }
}


void format_part_compact (std::ostream &os, std::string_view buffer, bool first) {
    static std::string const DISPOSITION_PREFIX("attachment; filename=\"");
    std::string filename;
    for (auto f: parse_headers(buffer)) {
        //header[f.name] = f.value;
        if (f.name == "Content-Type") {
            CHECK(f.value == "text" || f.value.starts_with("text/"));
        }
        else if (f.name == "Content-Disposition") {
            std::string_view v = f.value;
            if (v == "attachment") {
                ; // OK
            }
            else {
                CHECK(v.starts_with(DISPOSITION_PREFIX));
                v.remove_prefix(DISPOSITION_PREFIX.size());
                v.remove_suffix(1);
                filename = v;
            }
        }
    }

    if (buffer.size() == 0) return;

    if (filename.empty()) {
        if (!first) {
            os << "--attachment\n";
        }
    }
    else {
        os << "--" << filename << "\n";
    }
    os.write(buffer.data(), buffer.size());
    os << '\n';
}

static std::string const BOUNDARY_PREFIX("multipart/mixed; boundary=");

void Message::formatEmail(std::ostream& os, bool compact) const
{
    if (!compact) {
        if (header_.contains(CONTEXT_HEADER_NAME)) {
            os << header_.at(CONTEXT_HEADER_NAME).dump(2) << '\n';
        }
    }
    write_headers(os, header_, compact);
    os << '\n';
    if (!compact) {
        os.write(body_raw_.data(), body_raw_.size());
        return;
    }
    // format multi-parts compactly

    auto it = header_.find("Content-Type");
    if (it == header_.end()) {   // no Content-Type means plain text,
                                // so not multi-part
        os.write(body_raw_.data(), body_raw_.size());
        return;
    }

    std::string const &content_type = it->get_ref<std::string const &>();
    if (content_type == "text"
            || content_type.starts_with("text/")) {
        os.write(body_raw_.data(), body_raw_.size());
        return;
    }

    CHECK(content_type.starts_with(BOUNDARY_PREFIX));
    std::string boundary = "--" + content_type.substr(BOUNDARY_PREFIX.size()) + "\n";

    std::string_view buf(body_raw_);

    CHECK(buf.starts_with(boundary));
    buf.remove_prefix(boundary.size());

    bool first = true;
    while (true) {
        auto off = buf.find(boundary);
        if (off == std::string::npos) {
            break;
        }
        format_part_compact(os, buf.substr(0, off), first);
        first = false;
        buf.remove_prefix(off + boundary.size());
    }
}

const char POSTLINE_DELIM[] = "========== POSTLINE MESSAGE ==========";

void Message::importMultipartBody (std::vector<Message> const &parts) {
    updateHeader([](json &header) {
        header["Content-Type"] = BOUNDARY_PREFIX + POSTLINE_DELIM;
    });
    std::ostringstream ss;
    for (auto const &part: parts) {
        ss << "--" << POSTLINE_DELIM << '\n';
        write_headers(ss, part.header_, false);
        CHECK(!part.body_raw_.contains(POSTLINE_DELIM));
        ss << '\n';
        ss.write(part.body_raw_.data(), part.body_raw_.size());
    }
    ss << "--" << POSTLINE_DELIM << '\n';
    body_raw_ = ss.str();
}

#if 0
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
#endif

}
