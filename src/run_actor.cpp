#include <cstdint>
#include <cstdlib> // atexit
#include <string>
#include <filesystem>
#include <iostream>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <termcolor/termcolor.hpp>
#include <CLI/CLI.hpp>
#include "build_info.hpp"
#include "postline/common.h"
#include "postline/actor.h"

using namespace postline;

int main(int argc, char** argv) {
  // Parse CLI options
  std::string address;
  std::string from = "user";
  std::string actor_name = "echo";
  {
    CLI::App app{"Postline actor testor"};
    argv = app.ensure_utf8(argv);
    app.add_option("-n,--name", actor_name, "actor address");
    app.add_option("-a,--address", address, "actor address");
    app.add_option("--from", from, "from address");
    CLI11_PARSE(app, argc, argv);
  }

  setup_environ();

    Actor* actor = new Actor((POSTLINE_HOME/"bin"/"actors"/actor_name).string());

    for (;;) {
        std::string subject, body;
        std::cout << "Subject: " << std::flush;
        if (!std::getline(std::cin, subject)) {
            subject.clear();
        }
        if (subject.empty()) break;
        std::cout << "Body: " << std::flush;
        std::getline(std::cin, body);

        json header{
            {"From", from},
            {"To", address},
            {"Subject", subject},
            {"Body", body}
        };

        actor->send(Message(std::move(header)));

        Message echoed = actor->recv();
        std::cout << echoed.header().dump() << '\n';
    }
    delete actor;

    return 0;
}
