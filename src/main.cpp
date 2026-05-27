#include <cstdlib> // atexit
#include <string>
#include <filesystem>
#include <iostream>
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
#include <spdlog/spdlog.h>
#include <unistd.h>
#include <postline/runtime.h>
#include <postline/ansi.h>
#include <postline/ui.h>
#include "build_info.hpp"

using namespace postline;

extern char const *banner;

namespace postline { namespace ui {
    extern std::string web_listen_host;
    extern int web_listen_port;
}}

inline void welcome ()
{
    
    std::cout << ansi::green << banner
              << ansi::reset
              << ansi::cyan << "Postline Agent Runtime\nBy Ann Arbor Algorithms\n" << ansi::reset
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

static void install_atexit_handlers(bool is_tui)
{
    std::atexit([]{
        spdlog::shutdown();
    });

    if (is_tui) {
        std::atexit(restore_terminal);
    }
}

int main(int argc, char** argv) {
    // Parse CLI options
    CLI::App app{"Postline Agent Runtime"};
    Runtime::Config config;
    config.journal_path = make_journal_name();
    std::string ui_choice = "tui";
    std::unique_ptr<Service> autopilot;
    std::unique_ptr<Driver> user_driver;
    bool detach = false;

    Message message_input;
    Message message_exit;
    {
        CLI::App app{"Postline driver tester"};
        argv = app.ensure_utf8(argv);
        app.add_option("-j,--journal", config.journal_path, "");
        app.add_option("-r,--resume", config.resume_path, "");
        app.add_option("--ui", ui_choice, "UI: null, cli, tui, web, auto");
        app.add_option("--host", ui::web_listen_host, "Web UI listen host");
        app.add_option("--port", ui::web_listen_port, "Web UI listen port");
        CLI11_PARSE(app, argc, argv);
    }

    setup_environ();
    welcome();

    init_logging();
    install_atexit_handlers(ui_choice == "tui");

    log::info("constructing runtime");

    auto ui = ui::make_ui(ui_choice);
    config.consume = ui->consume();
    Runtime runtime(config, ui.get());
    ui->setRuntime(&runtime);
    std::thread runtime_thread([&runtime]() {
            log::info("Starting runtime.");
            runtime.run();
            });
    ui->initArena();    // create starting threads
    ui->run();
    log::info("UI has been gracefully shutdown.");
    runtime_thread.join();
    log::info("Runtime has been gracefully shutdown.");
    ui.reset();
    std::cerr << "[exit]" << std::endl;
    return 0;
}
