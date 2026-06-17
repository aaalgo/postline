/*
Patch protocol
==============

`apply_patch` reads one patch document from stdin or from an optional file
argument. The document is byte-oriented. Marker lines are matched after removing
one trailing LF or CRLF; file payload lines keep their original line endings.

Grammar:

    patch       := comment* begin item* end comment*
    item        := comment | op
    begin       := "*** Begin Patch" eol
    end         := "*** End Patch" eol?
    comment     := "#" bytes eol

    op          := add_file | delete_file | update_file

    add_file    := "*** Add File: " path eol (comment | add_line)*
    add_line    := "+" bytes

    delete_file := "*** Delete File: " path eol

    update_file := "*** Update File: " path eol comment* move_to? (comment | hunk)*
    move_to     := "*** Move to: " path eol
    hunk        := "@@" eol (comment | hunk_line)+
    hunk_line   := (" " | "-" | "+") bytes

Operation boundaries are any line beginning with `*** Add File: `,
`*** Delete File: `, `*** Update File: `, or the exact end marker. Add-file
payload lines must begin with `+`; their stored payload is the remainder of the
line, including its newline if present. Update hunk lines use a leading space
for context, `-` for removed content, and `+` for inserted content. Lines whose
first byte is `#` are comments and are ignored.

Paths are normalized before use:

    - NUL bytes are rejected.
    - Backslashes are converted to slashes.
    - Leading `./` prefixes are removed.
    - Empty, absolute, empty-component, `.`, and `..` paths are rejected or
      collapsed so the result cannot escape the requested root.

Semantics:

    - Add creates a new file and any missing parent directories.
    - Delete removes an existing regular file.
    - Update finds each hunk by exact unique match of its context plus removed
      lines, then replaces that region with context plus added lines.
    - Update with `*** Move to:` renames the file when content is unchanged, or
      deletes the source and creates the destination when content changes.

The tool first plans all actions against an in-memory view. Application is then
guarded by exact content checks so a file changed between planning and writing
fails instead of being overwritten. Writes are atomic within the destination
directory. A rollback record is written before the first action and updated
after each applied action; `--rollback <record>` reverts applied actions in
reverse order.
*/

#include <CLI/CLI.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <vector>

namespace fs = std::filesystem;

namespace {

class PatchError : public std::runtime_error {
public:
    explicit PatchError(std::string const &message)
        : std::runtime_error(message)
    {}
};

void check(bool cond, std::string const &message)
{
    if (!cond) {
        throw PatchError(message);
    }
}

std::string strip_eol(std::string_view line)
{
    if (line.ends_with("\r\n")) {
        return std::string(line.substr(0, line.size() - 2));
    }
    if (line.ends_with('\n')) {
        return std::string(line.substr(0, line.size() - 1));
    }
    return std::string(line);
}

bool is_valid_utf8(std::string_view input)
{
    size_t i = 0;
    while (i < input.size()) {
        unsigned char c = static_cast<unsigned char>(input[i]);
        size_t extra = 0;
        uint32_t value = 0;
        uint32_t min_value = 0;
        if (c <= 0x7f) {
            ++i;
            continue;
        }
        if ((c & 0xe0) == 0xc0) {
            extra = 1;
            value = c & 0x1f;
            min_value = 0x80;
        } else if ((c & 0xf0) == 0xe0) {
            extra = 2;
            value = c & 0x0f;
            min_value = 0x800;
        } else if ((c & 0xf8) == 0xf0) {
            extra = 3;
            value = c & 0x07;
            min_value = 0x10000;
        } else {
            return false;
        }
        if (i + extra >= input.size()) {
            return false;
        }
        for (size_t j = 1; j <= extra; ++j) {
            unsigned char cc = static_cast<unsigned char>(input[i + j]);
            if ((cc & 0xc0) != 0x80) {
                return false;
            }
            value = (value << 6) | (cc & 0x3f);
        }
        if (value < min_value || value > 0x10ffff ||
            (value >= 0xd800 && value <= 0xdfff)) {
            return false;
        }
        i += extra + 1;
    }
    return true;
}

std::string safe_path(std::string path)
{
    check(path.find('\0') == std::string::npos, "path contains NUL");
    check(is_valid_utf8(path), "path is not valid UTF-8");
    std::replace(path.begin(), path.end(), '\\', '/');
    while (path.starts_with("./")) {
        path.erase(0, 2);
    }

    bool absolute = !path.empty() && path.front() == '/';
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= path.size()) {
        size_t slash = path.find('/', start);
        std::string part = slash == std::string::npos
            ? path.substr(start)
            : path.substr(start, slash - start);
        if (!part.empty() && part != ".") {
            check(part != "..", "path escapes root: " + path);
            parts.push_back(part);
        }
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }

    check(!parts.empty(), "empty path");
    check(!absolute, "absolute path rejected: " + path);

    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) {
            out += '/';
        }
        out += parts[i];
    }
    return out;
}

fs::path full_path(fs::path const &root, std::string const &path)
{
    return root / fs::path(path);
}

std::string read_file(fs::path const &root, std::string const &path)
{
    std::ifstream input(full_path(root, path), std::ios::binary);
    check(input.good(), "file does not exist: " + path);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    check(!input.bad(), "failed while reading: " + path);
    return buffer.str();
}

void write_all(int fd, std::string const &content)
{
    size_t offset = 0;
    while (offset < content.size()) {
        ssize_t n = ::write(fd, content.data() + offset, content.size() - offset);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        check(n >= 0, "write failed: " + std::string(std::strerror(errno)));
        offset += static_cast<size_t>(n);
    }
}

std::string random_hex(size_t bytes)
{
    static std::random_device rd;
    std::ostringstream out;
    for (size_t i = 0; i < bytes; ++i) {
        unsigned value = rd() & 0xff;
        out << std::hex << std::setw(2) << std::setfill('0') << value;
    }
    return out.str();
}

void write_file_atomic(fs::path const &root,
                       std::string const &path,
                       std::string const &content)
{
    fs::path target = full_path(root, path);
    check(fs::is_directory(target.parent_path()),
          "parent directory does not exist: " + path);

    fs::path tmp = target.parent_path() /
        ("." + target.filename().string() + ".tmp-" +
         std::to_string(::getpid()) + "-" + random_hex(4));
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
    check(fd >= 0, "cannot create temporary file: " + tmp.string());

    bool closed = false;
    try {
        write_all(fd, content);
        check(::fsync(fd) == 0, "fsync failed: " + std::string(std::strerror(errno)));
        check(::close(fd) == 0, "close failed: " + std::string(std::strerror(errno)));
        closed = true;
        fs::rename(tmp, target);
    } catch (...) {
        if (!closed) {
            ::close(fd);
        }
        std::error_code ec;
        fs::remove(tmp, ec);
        throw;
    }
}

class PatchLine {
public:
    enum class Kind {
        Context,
        Remove,
        Add,
    };

    Kind kind;
    std::string text;
};

class PatchHunk {
public:
    std::vector<PatchLine> lines;
};

class PatchOp {
public:
    enum class Kind {
        AddFile,
        DeleteFile,
        UpdateFile,
    };

    Kind kind;
    std::string path;
    std::optional<std::string> move_to;
    std::vector<PatchHunk> hunks;
    std::vector<std::string> add_lines;
};

std::vector<std::string> split_lines_keepends(std::string const &content)
{
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < content.size()) {
        size_t newline = content.find('\n', start);
        if (newline == std::string::npos) {
            lines.push_back(content.substr(start));
            break;
        }
        lines.push_back(content.substr(start, newline - start + 1));
        start = newline + 1;
    }
    return lines;
}

bool is_comment(std::string const &line)
{
    return line.starts_with('#');
}

bool is_op_boundary(std::string const &line)
{
    return line == "*** End Patch" ||
        line.starts_with("*** Add File: ") ||
        line.starts_with("*** Delete File: ") ||
        line.starts_with("*** Update File: ");
}

void skip_comments(std::vector<std::string> const &lines, size_t &index)
{
    while (index < lines.size() && is_comment(strip_eol(lines[index]))) {
        ++index;
    }
}

std::vector<PatchOp> parse_patch(std::string const &data)
{
    auto lines = split_lines_keepends(data);
    check(!lines.empty(), "empty patch");
    size_t i = 0;
    skip_comments(lines, i);
    check(i < lines.size(), "empty patch");
    check(strip_eol(lines[i]) == "*** Begin Patch", "missing begin sentinel");

    std::vector<PatchOp> ops;
    ++i;
    while (i < lines.size()) {
        skip_comments(lines, i);
        if (i >= lines.size()) {
            break;
        }
        std::string line = strip_eol(lines[i]);
        if (line == "*** End Patch") {
            ++i;
            skip_comments(lines, i);
            check(i == lines.size(), "content after end sentinel");
            return ops;
        }

        if (line.starts_with("*** Add File: ")) {
            PatchOp op;
            op.kind = PatchOp::Kind::AddFile;
            op.path = safe_path(line.substr(std::strlen("*** Add File: ")));
            ++i;
            while (i < lines.size()) {
                std::string marker = strip_eol(lines[i]);
                if (is_comment(marker)) {
                    ++i;
                    continue;
                }
                if (marker.starts_with("*** ") && marker != "*** Move to: ") {
                    break;
                }
                check(lines[i].starts_with('+'),
                      "add-file line must start with '+': " + op.path);
                op.add_lines.push_back(lines[i].substr(1));
                ++i;
            }
            ops.push_back(std::move(op));
            continue;
        }

        if (line.starts_with("*** Delete File: ")) {
            PatchOp op;
            op.kind = PatchOp::Kind::DeleteFile;
            op.path = safe_path(line.substr(std::strlen("*** Delete File: ")));
            ops.push_back(std::move(op));
            ++i;
            continue;
        }

        if (line.starts_with("*** Update File: ")) {
            PatchOp op;
            op.kind = PatchOp::Kind::UpdateFile;
            op.path = safe_path(line.substr(std::strlen("*** Update File: ")));
            ++i;
            skip_comments(lines, i);
            if (i < lines.size() &&
                strip_eol(lines[i]).starts_with("*** Move to: ")) {
                std::string marker = strip_eol(lines[i]);
                op.move_to = safe_path(marker.substr(std::strlen("*** Move to: ")));
                ++i;
            }

            while (i < lines.size()) {
                skip_comments(lines, i);
                if (i >= lines.size()) {
                    break;
                }
                std::string marker = strip_eol(lines[i]);
                if (is_op_boundary(marker)) {
                    break;
                }
                check(marker == "@@", "expected hunk header for update: " + op.path);
                ++i;
                PatchHunk hunk;
                while (i < lines.size()) {
                    marker = strip_eol(lines[i]);
                    if (is_comment(marker)) {
                        ++i;
                        continue;
                    }
                    if (marker == "@@" || is_op_boundary(marker)) {
                        break;
                    }
                    check(!lines[i].empty(), "bad hunk line prefix in " + op.path);
                    char prefix = lines[i].front();
                    check(prefix == ' ' || prefix == '-' || prefix == '+',
                          "bad hunk line prefix in " + op.path);
                    PatchLine patch_line;
                    patch_line.kind = prefix == ' ' ? PatchLine::Kind::Context
                        : prefix == '-' ? PatchLine::Kind::Remove
                        : PatchLine::Kind::Add;
                    patch_line.text = lines[i].substr(1);
                    hunk.lines.push_back(std::move(patch_line));
                    ++i;
                }
                check(!hunk.lines.empty(), "empty hunk in " + op.path);
                op.hunks.push_back(std::move(hunk));
            }

            check(!op.hunks.empty() || op.move_to.has_value(),
                  "update has no hunks: " + op.path);
            ops.push_back(std::move(op));
            continue;
        }

        throw PatchError("unknown patch line: " + line);
    }

    throw PatchError("missing end sentinel");
}

size_t find_unique(std::vector<std::string> const &haystack,
                   std::vector<std::string> const &needle)
{
    if (needle.empty()) {
        check(haystack.empty(), "add-only hunk is ambiguous for non-empty file");
        return 0;
    }

    std::vector<size_t> matches;
    if (needle.size() <= haystack.size()) {
        for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
            if (std::equal(needle.begin(), needle.end(), haystack.begin() + i)) {
                matches.push_back(i);
            }
        }
    }
    check(!matches.empty(), "hunk does not match");
    check(matches.size() == 1, "hunk matches multiple locations");
    return matches[0];
}

std::string apply_hunks(std::string const &content,
                        std::vector<PatchHunk> const &hunks)
{
    std::vector<std::string> file_lines = split_lines_keepends(content);
    for (auto const &hunk : hunks) {
        std::vector<std::string> old_lines;
        std::vector<std::string> new_lines;
        for (auto const &line : hunk.lines) {
            if (line.kind == PatchLine::Kind::Context ||
                line.kind == PatchLine::Kind::Remove) {
                old_lines.push_back(line.text);
            }
            if (line.kind == PatchLine::Kind::Context ||
                line.kind == PatchLine::Kind::Add) {
                new_lines.push_back(line.text);
            }
        }
        size_t pos = find_unique(file_lines, old_lines);
        std::vector<std::string> updated;
        updated.insert(updated.end(), file_lines.begin(), file_lines.begin() + pos);
        updated.insert(updated.end(), new_lines.begin(), new_lines.end());
        updated.insert(updated.end(),
                       file_lines.begin() + pos + old_lines.size(),
                       file_lines.end());
        file_lines = std::move(updated);
    }

    std::string out;
    for (auto const &line : file_lines) {
        out += line;
    }
    return out;
}

class Action {
public:
    enum class Kind {
        CreateDirectories,
        CreateFile,
        DeleteFile,
        ReplaceFile,
        MoveFile,
    };

    Kind kind;
    std::vector<std::string> paths;
    std::string path;
    std::string src;
    std::string dst;
    std::string expected;
    std::string content;

    static Action create_directories(std::vector<std::string> paths)
    {
        Action action;
        action.kind = Kind::CreateDirectories;
        action.paths = std::move(paths);
        return action;
    }

    static Action create_file(std::string path, std::string content)
    {
        Action action;
        action.kind = Kind::CreateFile;
        action.path = std::move(path);
        action.content = std::move(content);
        return action;
    }

    static Action delete_file(std::string path, std::string expected)
    {
        Action action;
        action.kind = Kind::DeleteFile;
        action.path = std::move(path);
        action.expected = std::move(expected);
        return action;
    }

    static Action replace_file(std::string path,
                               std::string expected,
                               std::string content)
    {
        Action action;
        action.kind = Kind::ReplaceFile;
        action.path = std::move(path);
        action.expected = std::move(expected);
        action.content = std::move(content);
        return action;
    }

    static Action move_file(std::string src,
                            std::string dst,
                            std::string expected)
    {
        Action action;
        action.kind = Kind::MoveFile;
        action.src = std::move(src);
        action.dst = std::move(dst);
        action.expected = std::move(expected);
        return action;
    }

    void apply(fs::path const &root) const
    {
        switch (kind) {
        case Kind::CreateDirectories:
            for (auto const &item : paths) {
                fs::path target = full_path(root, item);
                check(!fs::exists(target), "directory already exists: " + item);
                fs::create_directory(target);
            }
            return;
        case Kind::CreateFile:
            check(!fs::exists(full_path(root, path)), "path already exists: " + path);
            write_file_atomic(root, path, content);
            return;
        case Kind::DeleteFile:
            check(read_file(root, path) == expected,
                  "file changed before delete: " + path);
            fs::remove(full_path(root, path));
            return;
        case Kind::ReplaceFile:
            check(read_file(root, path) == expected,
                  "file changed before replace: " + path);
            write_file_atomic(root, path, content);
            return;
        case Kind::MoveFile:
            check(read_file(root, src) == expected,
                  "file changed before move: " + src);
            check(!fs::exists(full_path(root, dst)),
                  "move destination exists: " + dst);
            fs::rename(full_path(root, src), full_path(root, dst));
            return;
        }
    }

    void rollback(fs::path const &root) const
    {
        switch (kind) {
        case Kind::CreateDirectories:
            for (auto it = paths.rbegin(); it != paths.rend(); ++it) {
                fs::path target = full_path(root, *it);
                check(fs::is_directory(target),
                      "not a directory during rollback: " + *it);
                fs::remove(target);
            }
            return;
        case Kind::CreateFile:
            check(read_file(root, path) == content,
                  "file changed after apply: " + path);
            fs::remove(full_path(root, path));
            return;
        case Kind::DeleteFile:
            check(!fs::exists(full_path(root, path)),
                  "path exists during rollback: " + path);
            write_file_atomic(root, path, expected);
            return;
        case Kind::ReplaceFile:
            check(read_file(root, path) == content,
                  "file changed after apply: " + path);
            write_file_atomic(root, path, expected);
            return;
        case Kind::MoveFile:
            check(read_file(root, dst) == expected,
                  "file changed after move: " + dst);
            check(!fs::exists(full_path(root, src)),
                  "move source exists during rollback: " + src);
            fs::rename(full_path(root, dst), full_path(root, src));
            return;
        }
    }
};

class Planner {
    fs::path root_;
    std::vector<std::pair<std::string, std::optional<std::string>>> files_;
    std::vector<std::string> created_dirs_;
    std::vector<Action> actions_;

    std::optional<size_t> file_index(std::string const &path) const
    {
        for (size_t i = 0; i < files_.size(); ++i) {
            if (files_[i].first == path) {
                return i;
            }
        }
        return std::nullopt;
    }

    bool created_dir(std::string const &path) const
    {
        return std::find(created_dirs_.begin(), created_dirs_.end(), path) !=
            created_dirs_.end();
    }

    void set_file(std::string path, std::optional<std::string> content)
    {
        if (auto index = file_index(path)) {
            files_[*index].second = std::move(content);
        } else {
            files_.push_back({std::move(path), std::move(content)});
        }
    }

    std::optional<std::string> read_planned(std::string const &path)
    {
        if (auto index = file_index(path)) {
            return files_[*index].second;
        }
        fs::path target = full_path(root_, path);
        if (fs::is_regular_file(target)) {
            std::ifstream input(target, std::ios::binary);
            std::ostringstream buffer;
            buffer << input.rdbuf();
            check(!input.bad(), "failed while reading: " + path);
            return buffer.str();
        }
        if (fs::exists(target)) {
            throw PatchError("path is not a file: " + path);
        }
        return std::nullopt;
    }

    bool dir_exists(std::string const &path)
    {
        if (auto index = file_index(path); index && files_[*index].second.has_value()) {
            return false;
        }
        if (created_dir(path)) {
            return true;
        }
        fs::path target = full_path(root_, path);
        if (fs::is_directory(target)) {
            return true;
        }
        if (fs::exists(target)) {
            throw PatchError("path is not a directory: " + path);
        }
        return false;
    }

    void ensure_parent_dirs(std::string const &path)
    {
        fs::path parent_path = fs::path(path).parent_path();
        std::string parent = parent_path.empty() ? "." : parent_path.generic_string();
        if (parent == ".") {
            return;
        }

        std::vector<std::string> missing;
        size_t start = 0;
        while (start < parent.size()) {
            size_t slash = parent.find('/', start);
            std::string current = slash == std::string::npos
                ? parent
                : parent.substr(0, slash);
            if (!dir_exists(current)) {
                missing.push_back(current);
                created_dirs_.push_back(current);
            }
            if (slash == std::string::npos) {
                break;
            }
            start = slash + 1;
        }

        if (!missing.empty()) {
            actions_.push_back(Action::create_directories(std::move(missing)));
        }
    }

public:
    explicit Planner(fs::path root)
        : root_(std::move(root))
    {}

    void add_op(PatchOp const &op)
    {
        if (op.kind == PatchOp::Kind::AddFile) {
            check(!read_planned(op.path).has_value(),
                  "add destination exists: " + op.path);
            std::string content;
            for (auto const &line : op.add_lines) {
                content += line;
            }
            ensure_parent_dirs(op.path);
            actions_.push_back(Action::create_file(op.path, content));
            set_file(op.path, std::move(content));
            return;
        }

        if (op.kind == PatchOp::Kind::DeleteFile) {
            auto expected = read_planned(op.path);
            check(expected.has_value(), "delete target does not exist: " + op.path);
            actions_.push_back(Action::delete_file(op.path, *expected));
            set_file(op.path, std::nullopt);
            return;
        }

        if (op.kind == PatchOp::Kind::UpdateFile) {
            auto expected = read_planned(op.path);
            check(expected.has_value(), "update target does not exist: " + op.path);
            std::string content = apply_hunks(*expected, op.hunks);

            if (!op.move_to.has_value()) {
                actions_.push_back(Action::replace_file(op.path, *expected, content));
                set_file(op.path, std::move(content));
                return;
            }

            check(!read_planned(*op.move_to).has_value(),
                  "move destination exists: " + *op.move_to);
            ensure_parent_dirs(*op.move_to);
            if (content == *expected) {
                actions_.push_back(Action::move_file(op.path, *op.move_to, *expected));
            } else {
                actions_.push_back(Action::delete_file(op.path, *expected));
                actions_.push_back(Action::create_file(*op.move_to, content));
            }
            set_file(op.path, std::nullopt);
            set_file(*op.move_to, std::move(content));
            return;
        }

        throw PatchError("unknown op kind");
    }

    std::vector<Action> actions() const
    {
        return actions_;
    }
};

std::vector<Action> plan_actions(fs::path const &root,
                                 std::vector<PatchOp> const &ops)
{
    Planner planner(root);
    for (auto const &op : ops) {
        planner.add_op(op);
    }
    return planner.actions();
}

void put_u64(std::string &out, uint64_t value)
{
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<char>((value >> (i * 8)) & 0xff));
    }
}

uint64_t get_u64(std::string const &input, size_t &offset)
{
    check(offset + 8 <= input.size(), "truncated rollback record");
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(
            static_cast<unsigned char>(input[offset + i])) << (i * 8);
    }
    offset += 8;
    return value;
}

void put_string(std::string &out, std::string const &value)
{
    put_u64(out, value.size());
    out += value;
}

std::string get_string(std::string const &input, size_t &offset)
{
    uint64_t size = get_u64(input, offset);
    check(size <= input.size() - offset, "truncated rollback record");
    std::string value = input.substr(offset, static_cast<size_t>(size));
    offset += static_cast<size_t>(size);
    return value;
}

uint64_t action_kind_code(Action::Kind kind)
{
    switch (kind) {
    case Action::Kind::CreateDirectories: return 1;
    case Action::Kind::CreateFile: return 2;
    case Action::Kind::DeleteFile: return 3;
    case Action::Kind::ReplaceFile: return 4;
    case Action::Kind::MoveFile: return 5;
    }
    return 0;
}

Action::Kind action_kind_from_code(uint64_t code)
{
    switch (code) {
    case 1: return Action::Kind::CreateDirectories;
    case 2: return Action::Kind::CreateFile;
    case 3: return Action::Kind::DeleteFile;
    case 4: return Action::Kind::ReplaceFile;
    case 5: return Action::Kind::MoveFile;
    default:
        throw PatchError("unknown rollback action kind");
    }
}

class RollbackRecord {
public:
    fs::path root;
    std::vector<Action> actions;
    uint64_t applied_count = 0;
};

std::string serialize_record(RollbackRecord const &record)
{
    std::string out = "POSTLINE_PATCH_ROLLBACK_V1\n";
    put_string(out, record.root.string());
    put_u64(out, record.applied_count);
    put_u64(out, record.actions.size());
    for (auto const &action : record.actions) {
        put_u64(out, action_kind_code(action.kind));
        put_u64(out, action.paths.size());
        for (auto const &path : action.paths) {
            put_string(out, path);
        }
        put_string(out, action.path);
        put_string(out, action.src);
        put_string(out, action.dst);
        put_string(out, action.expected);
        put_string(out, action.content);
    }
    return out;
}

RollbackRecord deserialize_record(std::string const &input)
{
    std::string magic = "POSTLINE_PATCH_ROLLBACK_V1\n";
    check(input.starts_with(magic), "unsupported rollback version");
    size_t offset = magic.size();

    RollbackRecord record;
    record.root = fs::path(get_string(input, offset));
    record.applied_count = get_u64(input, offset);
    uint64_t action_count = get_u64(input, offset);
    for (uint64_t i = 0; i < action_count; ++i) {
        Action action;
        action.kind = action_kind_from_code(get_u64(input, offset));
        uint64_t path_count = get_u64(input, offset);
        for (uint64_t j = 0; j < path_count; ++j) {
            action.paths.push_back(safe_path(get_string(input, offset)));
        }
        action.path = get_string(input, offset);
        if (!action.path.empty()) {
            action.path = safe_path(action.path);
        }
        action.src = get_string(input, offset);
        if (!action.src.empty()) {
            action.src = safe_path(action.src);
        }
        action.dst = get_string(input, offset);
        if (!action.dst.empty()) {
            action.dst = safe_path(action.dst);
        }
        action.expected = get_string(input, offset);
        action.content = get_string(input, offset);
        record.actions.push_back(std::move(action));
    }
    check(offset == input.size(), "trailing data in rollback record");
    check(record.applied_count <= record.actions.size(), "invalid applied_count");
    return record;
}

std::string read_input(std::optional<std::string> const &path)
{
    if (!path.has_value() || *path == "-") {
        std::ostringstream buffer;
        buffer << std::cin.rdbuf();
        return buffer.str();
    }

    std::ifstream input(*path, std::ios::binary);
    check(input.good(), "cannot open patch file: " + *path);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    check(!input.bad(), "failed while reading patch file: " + *path);
    return buffer.str();
}

void write_record(fs::path const &path, RollbackRecord const &record)
{
    fs::create_directories(path.parent_path());
    fs::path tmp = path.parent_path() / ("." + path.filename().string() + ".tmp");
    std::string content = serialize_record(record);

    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    check(fd >= 0, "cannot create rollback record: " + tmp.string());

    bool closed = false;
    try {
        write_all(fd, content);
        check(::fsync(fd) == 0, "fsync failed: " + std::string(std::strerror(errno)));
        check(::close(fd) == 0, "close failed: " + std::string(std::strerror(errno)));
        closed = true;
        fs::rename(tmp, path);
    } catch (...) {
        if (!closed) {
            ::close(fd);
        }
        std::error_code ec;
        fs::remove(tmp, ec);
        throw;
    }
}

fs::path rollback_path(fs::path const &root)
{
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::ostringstream name;
    name << std::put_time(&tm, "%Y%m%d%H%M%S") << "-" << random_hex(3)
         << ".rollback";
    return root / ".patch" / name.str();
}

fs::path apply_actions(fs::path const &root, std::vector<Action> const &actions)
{
    fs::path path = rollback_path(root);
    RollbackRecord record;
    record.root = root;
    record.actions = actions;
    record.applied_count = 0;
    write_record(path, record);

    for (auto const &action : actions) {
        action.apply(root);
        ++record.applied_count;
        write_record(path, record);
    }

    return path;
}

void rollback(fs::path const &record_path)
{
    std::ifstream input(record_path, std::ios::binary);
    check(input.good(), "cannot open rollback record: " + record_path.string());
    std::ostringstream buffer;
    buffer << input.rdbuf();
    check(!input.bad(), "failed while reading rollback record: " + record_path.string());
    RollbackRecord record = deserialize_record(buffer.str());

    while (record.applied_count > 0) {
        Action const &action = record.actions[record.applied_count - 1];
        action.rollback(record.root);
        --record.applied_count;
        write_record(record_path, record);
    }
}

void print_doc()
{
    std::cout <<
        "# apply_patch accepts and applies a patch file from stdin.\n"
        "# The patch file consists multiple patch operations.\n"
        "# Lines begin with # are comments.\n"
        "# Example:\n"
        "*** Begin Patch\n"
        "*** Add File: notes/todo.txt\n"
        "+write patch docs\n"
        "+add shell coverage\n"
        "+keep examples copyable\n"
        "#\n"
        "*** Update File: src/example.txt\n"
        "@@\n"
        " first line\n"
        "-old line\n"
        "-old line2\n"
        "-old line3\n"
        "+new line\n"
        "+new line2\n"
        " last line\n"
        "*** Update File: old/name.txt\n"
        "*** Move to: new/name.txt\n"
        "*** Update File: docs/draft.txt\n"
        "*** Move to: docs/final.txt\n"
        "@@\n"
        " Title: Release Notes\n"
        "-Status: draft\n"
        "+Status: final\n"
        "*** Delete File: generated/cache.txt\n"
        "*** End Patch\n";
}

}

int main(int argc, char **argv)
{
    std::optional<std::string> patch_path;
    std::string root_arg = ".";
    bool check_only = false;
    std::optional<std::string> rollback_arg;

    CLI::App app{"Apply a Postline patch document"};
    app.add_option("patch", patch_path, "patch file, or stdin if omitted");
    app.add_option("--root", root_arg, "repository root");
    app.add_flag("--check", check_only, "check only");
    app.add_flag_function("--doc", [](int64_t) {
        print_doc();
        std::exit(0);
    }, "print patch format examples");
    app.add_option("--rollback", rollback_arg, "rollback record path");
    CLI11_PARSE(app, argc, argv);

    try {
        if (rollback_arg.has_value()) {
            rollback(fs::path(*rollback_arg));
            std::cout << "Rolled back: " << *rollback_arg << '\n';
            return 0;
        }

        fs::path root = fs::absolute(fs::path(root_arg)).lexically_normal();
        auto patch = parse_patch(read_input(patch_path));
        auto actions = plan_actions(root, patch);
        if (check_only) {
            std::cout << "Patch OK: " << actions.size() << " action(s)\n";
            return 0;
        }

        fs::path rb_path = apply_actions(root, actions);
        std::cout << "Applied patch. Rollback reference: " << rb_path.string() << '\n';
        return 0;
    } catch (PatchError const &e) {
        std::cerr << "apply_patch: " << e.what() << '\n';
        return 1;
    } catch (std::exception const &e) {
        std::cerr << "apply_patch: " << e.what() << '\n';
        return 1;
    }
}
