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

inline void welcome ()
{
    using namespace termcolor;
    std::cout << "\n"
              << green
              << R"(

██████╗  ██████╗ ███████╗████████╗██╗     ██╗███╗   ██╗███████╗
██╔══██╗██╔═══██╗██╔════╝╚══██╔══╝██║     ██║████╗  ██║██╔════╝
██████╔╝██║   ██║███████╗   ██║   ██║     ██║██╔██╗ ██║█████╗
██╔═══╝ ██║   ██║╚════██║   ██║   ██║     ██║██║╚██╗██║██╔══╝
██║     ╚██████╔╝███████║   ██║   ███████╗██║██║ ╚████║███████╗
╚═╝      ╚═════╝ ╚══════╝   ╚═╝   ╚══════╝╚═╝╚═╝  ╚═══╝╚══════╝
)"
              << reset
              << "\n"
              << cyan << "Postline Agent Runtime\nBy Ann Arbor Algorithms\n\n" << reset
              << "  Version: " << postline::build::VERSION << "\n"
              << "  Build:   " << postline::build::BUILD_TYPE << "\n"
              << "  Commit:  " << postline::build::GIT_COMMIT << "\n";
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
    {
        CLI::App app{"Postline driver tester"};
        argv = app.ensure_utf8(argv);
        app.add_option("-j,--journal", config.journal_path, "");
        CLI11_PARSE(app, argc, argv);
    }

    welcome();
    init_logging();
    setup_environ();

    Runtime runtime(config);

    std::thread runtime_thread([&runtime] {
        runtime.run();
    });

    std::cout << "Runtime has started, enter a number to stop." << std::endl;
    int i;
    std::cin >> i;
    runtime.stop();
    runtime_thread.join();
    log::info("Runtime has been gracefully shutdown.");
    log::info("Bye.");

    return 0;
}
