#include "tabs.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <string_view>
#include <unistd.h>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <postline/common.h>

namespace postline { namespace ui {

using namespace ftxui;

namespace {

std::string trim(std::string_view sv) {
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front()))) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back()))) {
        sv.remove_suffix(1);
    }
    return std::string(sv);
}

std::vector<std::string> readLines(std::filesystem::path const &path) {
    std::ifstream input(path);
    CHECK(input.good(), "failed to read {}", path.string());

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) {
        lines.push_back(std::move(line));
    }
    return lines;
}

std::string statusValue(std::vector<std::string> const &lines,
                        std::string_view key) {
    std::string prefix = std::format("{}:", key);
    for (std::string const &line: lines) {
        if (line.starts_with(prefix)) {
            return trim(std::string_view(line).substr(prefix.size()));
        }
    }
    return "-";
}

long long meminfoKb(std::vector<std::string> const &lines,
                    std::string_view key) {
    std::string value = statusValue(lines, key);
    if (value == "-") return -1;

    std::istringstream input(value);
    long long kb = -1;
    input >> kb;
    CHECK(kb >= 0, "failed to parse /proc/meminfo field {}", key);
    return kb;
}

long long statusKb(std::vector<std::string> const &lines,
                   std::string_view key) {
    std::string value = statusValue(lines, key);
    CHECK(value != "-", "missing /proc/self/status field {}", key);

    std::istringstream input(value);
    long long kb = -1;
    input >> kb;
    CHECK(kb >= 0, "failed to parse /proc/self/status field {}", key);
    return kb;
}

std::string formatMemory(long long kb) {
    if (kb < 0) return "-";

    double mib = double(kb) / 1024.0;
    if (mib < 1024.0) {
        return std::format("{:.1f} MiB", mib);
    }
    return std::format("{:.2f} GiB", mib / 1024.0);
}

std::vector<std::string> readPstree(pid_t pid) {
    std::string command = std::format("pstree -p {}", pid);
    FILE *pipe = popen(command.c_str(), "r");
    CHECK(pipe != nullptr, "popen failed: errno: {} ({})",
          errno, std::strerror(errno));

    std::vector<std::string> lines;
    std::array<char, 512> buffer;
    while (fgets(buffer.data(), int(buffer.size()), pipe) != nullptr) {
        std::string line(buffer.data());
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
            line.pop_back();
        }
        lines.push_back(std::move(line));
    }

    int status = pclose(pipe);
    CHECK(status == 0, "pstree failed with status {}", status);
    return lines;
}

Elements lineElements(std::vector<std::string> const &lines) {
    Elements elements;
    for (std::string const &line: lines) {
        elements.push_back(text(line));
    }
    if (elements.empty()) {
        elements.push_back(text("-"));
    }
    return elements;
}

}

StatTab::StatTab()
    : last_refresh(std::chrono::steady_clock::time_point::min()) {
    renderer = Renderer([&](bool) {
        auto now = std::chrono::steady_clock::now();
        if (snapshot.process_lines.empty()
            || now - last_refresh > std::chrono::seconds(1)) {
            refresh();
        }

        return vbox({
            hbox({
                renderProcessStats() | size(WIDTH, EQUAL, 36),
                separator(),
                renderSystemStats() | flex,
            }) | size(HEIGHT, EQUAL, 12),
            separator(),
            renderProcessTree() | flex,
        }) | flex;
    });

    renderer |= CatchEvent([&](Event event) {
        int last_line = std::max(0, int(snapshot.pstree_lines.size()) - 1);
        if (event == Event::ArrowUp || event == Event::Character('k')) {
            tree_scroll = std::max(0, tree_scroll - 1);
            return true;
        }
        if (event == Event::ArrowDown || event == Event::Character('j')) {
            tree_scroll = std::min(last_line, tree_scroll + 1);
            return true;
        }
        if (event == Event::PageUp) {
            tree_scroll = std::max(0, tree_scroll - 10);
            return true;
        }
        if (event == Event::PageDown) {
            tree_scroll = std::min(last_line, tree_scroll + 10);
            return true;
        }
        if (event == Event::Home) {
            tree_scroll = 0;
            return true;
        }
        if (event == Event::End) {
            tree_scroll = last_line;
            return true;
        }
        return false;
    });
}

void StatTab::refresh() {
    StatSnapshot next;
    std::vector<std::string> status = readLines("/proc/self/status");
    std::vector<std::string> meminfo = readLines("/proc/meminfo");
    std::vector<std::string> loadavg = readLines("/proc/loadavg");
    CHECK(!loadavg.empty(), "/proc/loadavg is empty");

    long long vm_size = statusKb(status, "VmSize");
    long long vm_rss = statusKb(status, "VmRSS");
    long long vm_hwm = statusKb(status, "VmHWM");
    long long rss_anon = statusKb(status, "RssAnon");
    long long rss_file = statusKb(status, "RssFile");
    long long rss_shmem = statusKb(status, "RssShmem");

    next.process_lines = {
        std::format("pid: {}", getpid()),
        std::format("name: {}", statusValue(status, "Name")),
        std::format("state: {}", statusValue(status, "State")),
        std::format("threads: {}", statusValue(status, "Threads")),
        std::format("fd size: {}", statusValue(status, "FDSize")),
        std::format("virtual total: {}", formatMemory(vm_size)),
        std::format("resident total: {}", formatMemory(vm_rss)),
        std::format("peak resident: {}", formatMemory(vm_hwm)),
        std::format("resident anon: {}", formatMemory(rss_anon)),
        std::format("resident file: {}", formatMemory(rss_file)),
        std::format("resident shared: {}", formatMemory(rss_shmem)),
    };

    long long total = meminfoKb(meminfo, "MemTotal");
    long long available = meminfoKb(meminfo, "MemAvailable");
    long long free = meminfoKb(meminfo, "MemFree");
    long long used = total >= 0 && available >= 0 ? total - available : -1;

    next.system_lines = {
        std::format("memory total: {}", formatMemory(total)),
        std::format("memory available: {}", formatMemory(available)),
        std::format("memory used: {}", formatMemory(used)),
        std::format("memory free: {}", formatMemory(free)),
        std::format("load average: {}", trim(loadavg.at(0))),
    };

    next.pstree_lines = readPstree(getpid());

    snapshot = std::move(next);
    tree_scroll = std::clamp(tree_scroll, 0,
                             std::max(0, int(snapshot.pstree_lines.size()) - 1));
    last_refresh = std::chrono::steady_clock::now();
}

Element StatTab::renderProcessStats() const {
    return vbox({
        text("process") | bold,
        vbox(lineElements(snapshot.process_lines)) | flex,
    });
}

Element StatTab::renderSystemStats() const {
    return vbox({
        text("system") | bold,
        vbox(lineElements(snapshot.system_lines)) | flex,
    });
}

Element StatTab::renderProcessTree() const {
    return vbox({
        text("process tree") | bold,
        vbox(lineElements(snapshot.pstree_lines)) |
            focusPosition(0, tree_scroll) |
            yframe |
            flex,
    });
}

std::string StatTab::label() const {
    return "Stat";
}

Component StatTab::component() {
    return renderer;
}

}}
