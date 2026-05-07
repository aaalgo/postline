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
#include <postline/runtime.h>
#include <postline/ansi.h>
#include "build_info.hpp"

using namespace postline;

char const *LOCAL = R"(
[
{"from": "root", "address": "echo", "service": "pipe:echo", "flags": []},
{"from": "root", "address": "ai", "service": "pipe:claude", "flags": ["history"]},
{"from": "root", "address": "shell", "service": "pipe:shell", "flags": []},
{"from": "root", "address": "mcp", "service": "pipe:mcp_bridge", "flags": []},
{"from": "root", "address": "memory", "service": "pipe:echo -m", "flags": ["history"]},
{"from": "root", "address": "login", "service": "pipe:login", "flags": ["clone"]}
]
)";

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

int main(int argc, char** argv) {
    // Parse CLI options

    CLI::App app{"Postline Agent Runtime"};
    Runtime::Config config;
    config.journal_path = make_journal_name();
    std::string ui_server = "ftxcli";
    std::string input_path;
    std::string user_stdin;
    std::string user_stdout;
    std::unique_ptr<Driver> user_driver;
    bool detach = false;

    Message message_input;
    Message message_exit;
    {
        CLI::App app{"Postline driver tester"};
        argv = app.ensure_utf8(argv);
        app.add_option("-j,--journal", config.journal_path, "");
        app.add_option("-r,--resume", config.resume_path, "");
        app.add_option("--user-stdin", user_stdin, "");
        app.add_option("--user-stdout", user_stdout, "");
        app.add_option("-i,--input", input_path, "");
        //app.add_option("--ui", ui_server, "");
        //app.add_flag("--detach", detach, "");
        CLI11_PARSE(app, argc, argv);
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
            if (user_stdin.size()
                    || user_stdout.size()) {
                std::cerr << "When you set -i,--input you cannot set --user-stdin or --user-stdout." << std::endl;
                return 1;
            }
            std::string buf = read_file(input_path);
            std::cerr << "=== INPUT" << std::endl;
            std::cerr << buf << std::endl;
            message_input = Message::parseEmail(buf);
            json header{{"From", "user"},
                        {"To", "runtime"},
                        {"Subject", "exit"}};
            message_exit = Message(std::move(header));
            user_driver = std::make_unique<LoopDriver>([&message_input, &message_exit](Message const &msg, LoopDriver *driver)
                    {
                    static int count = 0;
                    std::cerr << "====" << std::endl;
                    msg.formatEmail(std::cerr);
                    std::cerr << std::endl;
                    if (count == 0) {
                        std::string thread_id = msg.header()["Thread-ID"].get<std::string>();
                        message_input.updateHeader([&thread_id](json &h) {h["Thread-ID"] = thread_id;});
                        message_exit.updateHeader([&thread_id](json &h) {h["Thread-ID"] = thread_id;});
                        driver->enqueue(std::move(message_input));
                    }
                    else {
                        driver->enqueue(std::move(message_exit));
                    }
                    ++count;
                    return 0;
                    });
        }
    }

    setup_environ();
    welcome();
    init_logging();
    log::info("constructing runtime");

    Runtime runtime(config, std::move(user_driver));

    if (config.resume_path.empty()) {
        log::info("Sending kick off message.");
        json h{{"From", "[boot]"},
               {"To", "runtime"},
               {"Subject", "spawn"}};
        Message local(std::move(h), std::string(LOCAL));
        runtime.enqueue_boot(std::move(local));
    }
    {
        log::info("Sending message.");
        json h{{"From", "[boot]"},
               {"To", "runtime"},
               {"Reply-To", "user"},
               {"Subject", "list_agents"}};
        runtime.enqueue_boot(Message(std::move(h)));
    }
    log::info("Starting runtime.");
    runtime.run();
    log::info("Runtime has been gracefully shutdown.");
    std::cerr << "[exit]" << std::endl;
    return 0;
}

