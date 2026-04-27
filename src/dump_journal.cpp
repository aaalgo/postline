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
  if (argc != 2) return 1;
  int cnt = 0;
  Journal j(std::string(), argv[1], [&cnt](Message const &msg){
          std::string type;
          json const &header = msg.header();
          auto it = header.find("type");
          if (it != header.end()) {
            type = it->get<std::string>();
          }
          std::cout << "======== " << cnt << " : " << type << std::endl;
          msg.formatEmail(std::cout);
          std::cout << std::endl;
  });
  return 0;
}
