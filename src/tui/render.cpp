#include "render.h"

#include <chrono>
#include <ctime>
#include <format>
#include <iomanip>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_set>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <postline/common.h>
#include <spdlog/details/log_msg_buffer.h>

#include "build_info.hpp"

extern char const *banner;

namespace postline { namespace ui {

using namespace ftxui;

static constexpr char POSTLINE_WEB_URL[] = "https://github.com/aaalgo/postline";

namespace theme {

Color background() {
    return Color::RGB(10, 14, 20);
}

Color surface() {
    return Color::RGB(18, 25, 35);
}

Color surfaceActive() {
    return Color::RGB(24, 36, 50);
}

Color text() {
    return Color::RGB(226, 232, 240);
}

Color muted() {
    return Color::RGB(148, 163, 184);
}

Color border() {
    return Color::RGB(71, 85, 105);
}

Color accent() {
    return Color::RGB(45, 212, 191);
}

Color danger() {
    return Color::RGB(248, 113, 113);
}

}

static Decorator logLevelColor(spdlog::level::level_enum level) {
    switch (level) {
    case spdlog::level::trace:
        return color(Color::GrayLight);
    case spdlog::level::debug:
        return color(Color::Cyan);
    case spdlog::level::info:
        return color(Color::Green);
    case spdlog::level::warn:
        return color(Color::Yellow);
    case spdlog::level::err:
        return color(Color::RedLight) | bold;
    case spdlog::level::critical:
        return color(Color::Red) | bold;
    default:
        return color(Color::GrayDark);
    }
}

static std::string toString(spdlog::string_view_t view) {
    return std::string(view.data(), view.size());
}

Element renderLogEntry(spdlog::details::log_msg const &msg) {
    std::string level = toString(spdlog::level::to_string_view(msg.level));
    std::string payload = toString(msg.payload);

    return hbox({
        text("["),
        text(level) | logLevelColor(msg.level),
        text("] "),
        text(payload),
    });
}

static std::string formatLogTime(spdlog::log_clock::time_point time) {
    auto time_t = spdlog::log_clock::to_time_t(time);
    std::tm tm;
    localtime_r(&time_t, &tm);

    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
        time.time_since_epoch()).count() % 1000;

    std::ostringstream out;
    out << std::put_time(&tm, "%H:%M:%S")
        << "." << std::setfill('0') << std::setw(3) << millis;
    return out.str();
}

Element renderLogDetail(spdlog::details::log_msg const &msg) {
    std::string level = toString(spdlog::level::to_string_view(msg.level));
    std::string logger = toString(msg.logger_name);
    std::string payload = toString(msg.payload);
    std::string filename = msg.source.filename ? msg.source.filename : "";
    std::string funcname = msg.source.funcname ? msg.source.funcname : "";

    if (logger.empty()) {
        logger = "(default)";
    }

    if (filename.empty()) {
        filename = "(unknown)";
    }

    if (funcname.empty()) {
        funcname = "(unknown)";
    }

    return vbox({
        hbox({
            text("["),
            text(level) | logLevelColor(msg.level),
            text("] "),
            text(logger) | color(Color::Cyan),
            text("  "),
            text(formatLogTime(msg.time)) | color(Color::Green),
            text("  #"),
            text(std::format("{}", msg.thread_id)) | color(Color::Yellow),
        }),
        hbox({
            text(std::format("{}:{}", filename, msg.source.line))
                | color(Color::GrayLight),
            text("  "),
            text(funcname) | color(Color::Magenta),
        }),
        paragraph(payload),
    });
}

Element renderWindowPane(std::string title, Element content, bool focused) {
    Color boundary_color = focused ? theme::accent() : theme::border();
    Color title_color = focused ? theme::accent() : theme::muted();

    Element title_row = hbox({
        text(" " + title + " ") | bold | color(title_color),
        filler(),
    });

    return vbox({
        std::move(title_row),
        std::move(content) | flex,
    }) | borderStyled(ROUNDED, boundary_color);
}

Element renderTopLevelWindowPane(std::string title,
                                 Element content,
                                 bool focused) {
    Color boundary_color = focused ? theme::accent() : theme::border();
    Color title_color = focused ? theme::accent() : theme::muted();
    Color title_background = focused ? theme::surfaceActive() : theme::surface();

    Element title_row = hbox({
        text(" " + title + " ") | bold | color(title_color),
        filler(),
    }) | bgcolor(title_background);

    return vbox({
        std::move(title_row),
        std::move(content) | flex,
    }) | borderStyled(ROUNDED, boundary_color) | bgcolor(theme::surface());
}

static std::string renderHeaderValue(json const &value, bool force_list) {
    if (value.is_array()) {
        std::ostringstream os;
        bool is_first = true;
        for (auto const &v: value) {
            CHECK(v.is_string());
            if (is_first) {
                is_first = false;
            }
            else {
                os << ", ";
            }
            os << v.get_ref<std::string const &>();
        }
        return os.str();
    }

    CHECK(!force_list);
    CHECK(value.is_string());
    return value.get_ref<std::string const &>();
}

void appendTextLines(Elements &lines, std::string_view text_lines) {
    for (;;) {
        auto off = text_lines.find('\n');
        if (off == std::string_view::npos) {
            if (!text_lines.empty()) {
                lines.push_back(paragraph(std::string(text_lines)));
            }
            return;
        }

        lines.push_back(paragraph(std::string(text_lines.substr(0, off))));
        text_lines.remove_prefix(off + 1);
    }
}

static void appendHeaderLine(
        Elements &lines,
        std::string const &key,
        json const &value,
        bool force_list) {
    if (value.is_null()) {
        return;
    }

    lines.push_back(hbox({
        text(key + ": ") | color(Color::Cyan) | bold,
        text(renderHeaderValue(value, force_list)),
    }));
}

void appendMessageHeaders(Elements &lines, json const &header) {
    static const std::vector<std::tuple<char const *, bool, bool>> canonical = {
        {"From", false, true},
        {"To", false, true},
        {"Cc", true, true},
        {"Subject", false, true},
        {"Content-Type", false, false},
        {"Content-Disposition", false, false},
    };

    std::unordered_set<std::string> used;
    used.insert("type");
    used.insert("Thinking");
    used.insert("Trash");
    used.insert(CONTEXT_HEADER_NAME);

    auto thinking = header.find("Thinking");
    if (thinking != header.end() && thinking->is_string()) {
        appendTextLines(lines, thinking->get_ref<std::string const &>());
    }

    for (auto const &[key, is_list, is_essential] : canonical) {
        auto it = header.find(key);
        if (it == header.end()) {
            continue;
        }
        used.insert(key);
        appendHeaderLine(lines, key, *it, is_list);
    }

    for (auto const &[key, value] : header.items()) {
        if (used.contains(key)) {
            continue;
        }
        appendHeaderLine(lines, key, value, false);
    }
}

static void appendBannerLines(Elements &lines) {
    std::string_view text_art(banner);

    while (!text_art.empty() && text_art.front() == '\n') {
        text_art.remove_prefix(1);
    }

    for (;;) {
        auto off = text_art.find('\n');
        std::string_view line = text_art.substr(0, off);
        if (!line.empty()) {
            lines.push_back(text(std::string(line)) |
                            color(Color::GreenLight) |
                            bold);
        }

        if (off == std::string_view::npos) {
            return;
        }
        text_art.remove_prefix(off + 1);
    }
}

Element renderAboutBox(Element close_button) {
    Elements lines;
    appendBannerLines(lines);

    lines.push_back(separator());
    lines.push_back(text("Postline Agent Runtime") |
                    color(Color::Cyan) |
                    bold |
                    hcenter);
    lines.push_back(text("By Ann Arbor Algorithms") |
                    color(Color::CyanLight) |
                    hcenter);
    lines.push_back(text(POSTLINE_WEB_URL) |
                    color(Color::RedLight) |
                    hcenter);
    lines.push_back(separator());
    lines.push_back(hbox({
        text("Version:    ") | color(Color::Yellow) | bold,
        text(postline::build::VERSION),
    }));
    lines.push_back(hbox({
        text("Commit:     ") | color(Color::Yellow) | bold,
        text(postline::build::GIT_COMMIT),
    }));
    lines.push_back(hbox({
        text("Build Type: ") | color(Color::Yellow) | bold,
        text(postline::build::BUILD_TYPE),
    }));
    lines.push_back(hbox({
        text("Build Time: ") | color(Color::Yellow) | bold,
        text(postline::build::BUILD_TIME),
    }));
    lines.push_back(separator());
    lines.push_back(std::move(close_button) | hcenter);

    return window(text(" About Postline ") | color(Color::Cyan) | bold,
                  vbox(std::move(lines)) | borderEmpty)
        | borderEmpty
        | size(WIDTH, LESS_THAN, 60)
        | clear_under;
}

}}
