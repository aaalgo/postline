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

static void restore_terminal_after_tui()
{
    static constexpr char seq[] =
        "\033[?1000l"
        "\033[?1002l"
        "\033[?1003l"
        "\033[?1005l"
        "\033[?1006l"
        "\033[?1015l"
        "\033[?1016l"
        "\033[?1049l"
        "\033[?25h"
        "\033[?7h"
        "\033[0m";
    ::write(STDERR_FILENO, seq, sizeof(seq) - 1);
}

static void install_atexit_handlers(bool restore_terminal)
{
    std::atexit([]{
        spdlog::shutdown();
    });

    if (restore_terminal) {
        std::atexit(restore_terminal_after_tui);
    }
}

#if 0
std::string read_file(std::filesystem::path const& input_path) {
    std::ifstream in(input_path, std::ios::binary);

    if (!in) {
        throw std::runtime_error(
            "failed to open file: " + input_path.string());
    }

    std::ostringstream ss;

    ss << in.rdbuf();

    return ss.str();
}

class Autopilot: public Service {
    Message payload;
    Message exit;
public:
    Autopilot (std::string const &payload_path)
        : exit(json{{"To", "runtime"}, {"Subject", "exit"}})

    {
        std::string buf = read_file(payload_path);
        payload = Message::parseEmail(buf);
    }
    void init (Response &resp) override {
        resp.append(std::move(payload));
    };

    void call (Message &&msg, Response &resp) override {
        resp.append(std::move(exit));
    }
};
#endif

#include <mutex>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    // Parse CLI options
    CLI::App app{"Postline Agent Runtime"};
    Runtime::Config config;
    config.journal_path = make_journal_name();
    std::string ui_choice = "tui";
    //std::string ui_server = "ftxcli";
    /*
    std::string input_path;
    std::string user_stdin;
    std::string user_stdout;
    */
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
        /*
        app.add_option("--user-stdin", user_stdin, "");
        app.add_option("--user-stdout", user_stdout, "");
        app.add_option("-i,--input", input_path, "");
        */
        //app.add_option("--ui", ui_server, "");
        //app.add_flag("--detach", detach, "");
        CLI11_PARSE(app, argc, argv);
#if 0
        if (input_path.empty()) {
            if (user_stdin.empty()
                    || user_stdout.empty()) {
                std::cerr << "You must either set -i,--input" << std::endl;
                std::cerr << "         or set both --user-stdin and user-stdout" << std::endl;
                return 1;
            }
            user_driver = std::make_unique<ShellDriver>(user_stdin, user_stdout);
        }
        else {
            CHECK(0, "Autopilot not supported.");
            if (user_stdin.size()
                    || user_stdout.size()) {
                std::cerr << "When you set -i,--input you cannot set --user-stdin or --user-stdout." << std::endl;
                return 1;
            }
            autopilot = std::make_unique<Autopilot>(input_path);
            user_driver = std::make_unique<LoopDriver>(autopilot.get());
        }
#endif
    }

    setup_environ();
    welcome();

    init_logging();
    install_atexit_handlers(ui_choice == "tui");

    log::info("constructing runtime");

    Runtime runtime(config);
    auto ui = ui::make_ui(ui_choice, &runtime);
    runtime.attachUser(ui.get());
    ui->initArena();    // create starting threads

    std::thread runtime_thread([&runtime]() {
            log::info("Starting runtime.");
            runtime.run();
            });
    ui->run();
    log::info("UI has been gracefully shutdown.");
    runtime_thread.join();
    log::info("Runtime has been gracefully shutdown.");
    ui.reset();
    std::cerr << "[exit]" << std::endl;
    return 0;
}
