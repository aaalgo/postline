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
  if (argc != 2) return 1;
  int cnt = 0;
  Journal j(std::string(), argv[1], [&cnt](Message const &message){
        std::cout << "======== access_id=" << message.access_id() << '\n';
        if (message.header().contains("To")) {
            message.formatEmail(std::cout);
        }
        else {
            std::cout << message.header().dump(4) << std::endl;
            if (!message.body().empty()) {
                std::cout << message.body() << std::endl;
            }
        }
        std::cout << std::endl;
  });
  return 0;
}
