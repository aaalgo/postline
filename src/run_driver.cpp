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
#include "postline/driver.h"

using namespace postline;

int main(int argc, char** argv) {
  // Parse CLI options
  std::string address = "agent@localhost";
  std::string from = "user@localhost";
  std::string driver_name = "echo";
  std::string journal_path;
  {
    CLI::App app{"Postline driver tester"};
    argv = app.ensure_utf8(argv);
    app.add_option("-n,--name", driver_name, "driver name");
    app.add_option("-a,--address", address, "driver address");
    app.add_option("--from", from, "from address");
    app.add_option("-j,--journal", journal_path, "journal path");
    CLI11_PARSE(app, argc, argv);
  }

  setup_environ();

  std::string cmd = (POSTLINE_HOME/"bin"/"drivers"/driver_name).string();
  log::info("driver: {}", cmd);

  Driver* driver = new ShellDriver(cmd);
  Journal *journal = nullptr;
  if (!journal_path.empty()) {
      bool hist = driver->history_mode() == DriverHistoryMode::ALL;
      if (hist) {
          log::info("begin_history");
          Message msg(json{{"type", "driver:begin_history"}});
          driver->send(std::move(msg));
      }
      if (!fs::exists(journal_path)) {
          // create journal
          Journal tmp(journal_path, std::string(), [](Message const &){});
      }
      journal = new Journal(std::string(), journal_path, [hist, driver](Message &&msg){
                if (hist) {
                    log::info("send history");
                    driver->send(std::move(msg));
                }
              });
      if (hist) {
          log::info("end_history");
          Message msg(json{{"type", "driver:end_history"}});
          driver->send(std::move(msg));
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
        std::cout << ">>>" << std::endl;
        to.formatEmail(std::cout, true);

        driver->send(std::move(to));

        Message echoed = driver->recv_one();
        if (journal) {
            journal->append(echoed);
        }
        std::cerr << echoed.header().dump() << std::endl;
        std::cout << "<<<" << std::endl;
        echoed.formatEmail(std::cout, true);
    }
    delete driver;
    if (journal) {
        delete journal;
    }

    return 0;
}
