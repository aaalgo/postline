#include <cstdlib> // atexit
#include <string>
#include <filesystem>
#include <fstream>
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
    std::string ui_choice = "tui";
    fs::path arena_path;
    std::unique_ptr<Service> autopilot;
    std::unique_ptr<Driver> user_driver;
    bool detach = false;
    fs::path workdir_latest;

    Message message_input;
    Message message_exit;
    {
        CLI::App app{"Postline driver tester"};
        argv = app.ensure_utf8(argv);
        std::string workdir;
        auto workdir_option = app.add_option("-w,--workdir", workdir, "Work directory");
        auto journal_option = app.add_option("-j,--journal", config.journal_path, "");
        auto resume_option = app.add_option("-r,--resume", config.resume_path, "");
        auto arena_option = app.add_option("--arena", arena_path, "Arena JSON path");
        arena_option->excludes(resume_option);
        app.add_option("--ui", ui_choice, "UI: null, cli, tui, web, auto");
        app.add_option("--host", ui::web_listen_host, "Web UI listen host");
        app.add_option("--port", ui::web_listen_port, "Web UI listen port");
        CLI11_PARSE(app, argc, argv);

        CHECK(workdir_option->count() == 0 ||
              (journal_option->count() == 0 && resume_option->count() == 0),
              "--workdir cannot be combined with --journal or --resume");

        if (workdir_option->count() == 0 &&
            journal_option->count() == 0 && resume_option->count() == 0) {
            char const *home = std::getenv("HOME");
            CHECK(home && *home, "HOME is not set; specify --workdir or --journal");
            workdir = (fs::path(home) / ".postline").string();
            std::cout << "Using default work directory " << workdir << '\n';
        }

        if (!workdir.empty()) {
            fs::path workdir_path = fs::absolute(workdir);
            std::error_code ec;
            fs::create_directories(workdir_path, ec);
            CHECK(!ec, "Cannot create work directory {}: {}",
                  workdir_path.string(), ec.message());
            workdir_path = fs::canonical(workdir_path, ec);
            CHECK(!ec, "Cannot resolve work directory {}: {}",
                  workdir.c_str(), ec.message());

            workdir_latest = workdir_path / "latest";
            fs::file_status latest_status = fs::symlink_status(workdir_latest, ec);
            CHECK(!ec || ec == std::errc::no_such_file_or_directory,
                  "Cannot inspect {}: {}", workdir_latest.string(), ec.message());
            if (!ec && latest_status.type() != fs::file_type::not_found) {
                CHECK(fs::is_symlink(latest_status), "{} is not a symbolic link",
                      workdir_latest.string());
                fs::path resume_path = fs::read_symlink(workdir_latest, ec);
                CHECK(!ec, "Cannot read {}: {}", workdir_latest.string(), ec.message());
                if (resume_path.is_relative()) {
                    resume_path = workdir_path / resume_path;
                }
                resume_path = fs::canonical(resume_path, ec);
                CHECK(!ec, "Cannot resolve journal from {}: {}",
                      workdir_latest.string(), ec.message());
                config.resume_path = resume_path.string();
            }

            std::string journal_name = make_journal_name();
            fs::path journal_path = workdir_path / journal_name;
            for (unsigned suffix = 1; fs::exists(journal_path); ++suffix) {
                journal_path = workdir_path /
                    (journal_name + "." + std::to_string(suffix));
            }
            config.journal_path = journal_path.string();
        }
    }

    setup_environ();

    welcome();

    init_logging();
    install_atexit_handlers(ui_choice == "tui");

    log::info("constructing runtime");

    auto ui = ui::make_ui(ui_choice);
    config.consume = ui->consume();
    Runtime runtime(config, ui.get());
    if (!workdir_latest.empty()) {
        fs::path temporary_latest = workdir_latest;
        temporary_latest += ".tmp." + std::to_string(::getpid());
        std::error_code ec;
        fs::remove(temporary_latest, ec);
        CHECK(!ec, "Cannot remove stale {}: {}", temporary_latest.string(), ec.message());
        fs::create_symlink(fs::path(config.journal_path).filename(), temporary_latest, ec);
        CHECK(!ec, "Cannot create {}: {}", temporary_latest.string(), ec.message());
        fs::rename(temporary_latest, workdir_latest, ec);
        CHECK(!ec, "Cannot update {}: {}", workdir_latest.string(), ec.message());
    }
    ui->setRuntime(&runtime);
    std::thread runtime_thread([&runtime]() {
            log::info("Starting runtime.");
            runtime.run();
            });
    if (config.resume_path.empty()) {
        json arena_spec;
        if (arena_path.empty()) {
            arena_path = POSTLINE_HOME / "arena.json";
            log::info("Using default arena {}", arena_path.native());
        }
        std::ifstream arena_file(arena_path);
        CHECK(arena_file, "Cannot open arena file {}", arena_path.string());
        arena_spec = json::parse(arena_file);
        ui->initArena(arena_spec);
    }
    ui->run();
    log::info("UI has been gracefully shutdown.");
    runtime_thread.join();
    log::info("Runtime has been gracefully shutdown.");
    ui.reset();
    std::cerr << "[exit]" << std::endl;
    return 0;
}
