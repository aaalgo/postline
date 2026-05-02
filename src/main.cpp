#include <cstdlib> // atexit
#include <string>
#include <filesystem>
#include <iostream>
#include <termcolor/termcolor.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <thread>
#include <vector>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>
#include <CLI/CLI.hpp>
#include <postline/runtime.h>
#include "build_info.hpp"

using namespace postline;

char const *LOCAL = R"(
[
{"from": "root", "address": "echo", "service": "shell:echo", "clone": false},
{"from": "root", "address": "ai", "service": "shell:openai", "clone": false},
{"from": "root", "address": "shell", "service": "shell:shell", "clone": false},
{"from": "root", "address": "mcp", "service": "shell:mcp_bridge", "clone": false},
{"from": "root", "address": "memory", "service": "shell:echo -m", "clone": false},
{"from": "root", "address": "clone", "service": "shell:echo", "clone": true}
]
)";

inline void welcome ()
{
    using namespace termcolor;
    termcolor::colorize(std::cout);
    std::cout << green
              << R"(
██████╗  ██████╗ ███████╗████████╗██╗     ██╗███╗   ██╗███████╗
██╔══██╗██╔═══██╗██╔════╝╚══██╔══╝██║     ██║████╗  ██║██╔════╝
██████╔╝██║   ██║███████╗   ██║   ██║     ██║██╔██╗ ██║█████╗
██╔═══╝ ██║   ██║╚════██║   ██║   ██║     ██║██║╚██╗██║██╔══╝
██║     ╚██████╔╝███████║   ██║   ███████╗██║██║ ╚████║███████╗
╚═╝      ╚═════╝ ╚══════╝   ╚═╝   ╚══════╝╚═╝╚═╝  ╚═══╝╚══════╝
)"
              << reset
              << cyan << "Postline Agent Runtime\nBy Ann Arbor Algorithms\n" << reset
              << "Version: " << postline::build::VERSION << "\n"
              << "Commit:  " << postline::build::GIT_COMMIT << "\n"
              << "Build Type:   " << postline::build::BUILD_TYPE << "\n"
              << "Build Time:   " << postline::build::BUILD_TIME << "\n";
}

std::string make_journal_name()
{
    using namespace std::chrono;

    // get current time
    auto now = system_clock::now();
    std::time_t t = system_clock::to_time_t(now);

    // convert to local time (thread-safe version)
    std::tm tm{};
    localtime_r(&t, &tm);  // use localtime_s on Windows

    // format
    std::ostringstream oss;
    oss << "journal."
        << std::put_time(&tm, "%Y%m%d%H%M%S");

    return oss.str();
}

int main(int argc, char** argv) {
    // Parse CLI options

    CLI::App app{"Postline Agent Runtime"};
    Runtime::Config config;
    config.journal_path = make_journal_name();
    std::string ui_server = "ftxcli";
    bool detach = false;
    {
        CLI::App app{"Postline driver tester"};
        argv = app.ensure_utf8(argv);
        app.add_option("-j,--journal", config.journal_path, "");
        app.add_option("-r,--resume", config.resume_path, "");
        app.add_option("--user-stdin", config.cli_input_path, "");
        app.add_option("--user-stdout", config.cli_output_path, "");
        //app.add_option("--ui", ui_server, "");
        //app.add_flag("--detach", detach, "");
        CLI11_PARSE(app, argc, argv);
    }

    setup_environ();
    welcome();
    init_logging();
    //ui.start_helper(POSTLINE_HOME/"bin"/"servers"/ui_server, detach);
    log::info("constructing runtime");
    Runtime runtime(config);

    if (config.resume_path.empty()) {
        log::info("Sending kick off message.");
        json h{{"From", "boot"},
               {"To", "runtime"},
               {"Subject", "spawn"}};
        Message local(std::move(h), std::string(LOCAL));
        runtime.enqueue_boot(std::move(local));
    }
    log::info("Sending message.");
    json h{{"From", "boot"},
           {"To", "runtime"},
           {"Reply-To", "user"},
           {"Subject", "list_agents"}};
    runtime.enqueue_boot(Message(std::move(h)));
    log::info("Starting runtime.");
    runtime.run();
    log::info("Runtime has been gracefully shutdown.");
    std::cerr << "[exit]" << std::endl;
    return 0;
}
