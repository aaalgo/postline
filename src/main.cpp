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
#include <postline/tmux_ui.h>
#include "build_info.hpp"

using namespace postline;

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
    bool fake = false;
    {
        CLI::App app{"Postline driver tester"};
        argv = app.ensure_utf8(argv);
        app.add_option("-j,--journal", config.journal_path, "");
        app.add_flag("-f,--fake", fake, "");
        CLI11_PARSE(app, argc, argv);
    }

    TMuxUI ui;

    setup_environ();
    welcome();
    init_logging();
    ui.start_helper(POSTLINE_HOME/"bin"/"drivers"/"ftxcli", fake);
    config.cli_input_path = ui.cli_input_path();
    config.cli_output_path = ui.cli_output_path();
    Runtime runtime(config);
    log::info("Starting runtime.");
    runtime.run();
    log::info("Runtime has been gracefully shutdown.");
    log::info("Stopping tmux UI.");
    ui.stop();
    return 0;
}
