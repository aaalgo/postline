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
#include "postline/journal.h"
#include "postline/actor.h"

using namespace postline;

int main(int argc, char** argv) {
  // Parse CLI options
  std::string address = "agent@localhost";
  std::string from = "user@localhost";
  std::string actor_name = "echo";
  std::string journal_path;
  {
    CLI::App app{"Postline actor testor"};
    argv = app.ensure_utf8(argv);
    app.add_option("-n,--name", actor_name, "actor address");
    app.add_option("-a,--address", address, "actor address");
    app.add_option("--from", from, "from address");
    app.add_option("-j,--journal", journal_path, "journal path");
    CLI11_PARSE(app, argc, argv);
  }

  setup_environ();

  std::string cmd = (POSTLINE_HOME/"bin"/"actors"/actor_name).string();
  log::info("actor: {}", cmd);

  Actor* actor = new Actor(cmd);
  Journal *journal = nullptr;
  if (!journal_path.empty()) {
      bool hist = actor->history_mode() == ActorHistoryMode::ALL;
      if (hist) {
          log::info("begin_history");
          Message msg(json{{"type", "actor:begin_history"}});
          actor->send(msg);
      }
      if (!fs::exists(journal_path)) {
          // create journal
          Journal tmp(journal_path, std::string(), [](Message const &){});
      }
      journal = new Journal(std::string(), journal_path, [hist, actor](Message const &msg){
                if (hist) {
                    log::info("send history");
                    actor->send(msg);
                }
              });
      if (hist) {
          log::info("end_history");
          Message msg(json{{"type", "actor:end_history"}});
          actor->send(msg);
      }
  }

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
        };
        Message to(std::move(header), std::move(body));
        if (journal) {
            journal->append(to);
        }
        actor->send(to);
        std::cout << ">>>" << std::endl;
        to.formatEmail(std::cout, true);

        Message echoed = actor->recv();
        if (journal) {
            journal->append(echoed);
        }
        std::cerr << echoed.header().dump() << std::endl;
        std::cout << "<<<" << std::endl;
        echoed.formatEmail(std::cout, true);
    }
    delete actor;
    if (journal) {
        delete journal;
    }

    return 0;
}
