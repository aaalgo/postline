#include <iostream>
#include <string>
#include <CLI/CLI.hpp>
#include <postline/common.h>

using namespace postline;

int main(int argc, char *argv[])
{
    std::string path;
    bool compact = false;
    {
        CLI::App app{""};
        argv = app.ensure_utf8(argv);
        app.add_option("-i,--input", path, "input path");
        app.add_flag("-c,--compact", compact, "compact");
        CLI11_PARSE(app, argc, argv);
    }

    std::ostringstream ss;
    if (path.empty() || path == "-") {
        ss << std::cin.rdbuf();
    }
    else {
        std::ifstream file(path);
        ss << file.rdbuf();
    }
    std::string input = ss.str();
    Message msg = Message::parseEmail(input);
    msg.formatEmail(std::cout, compact);
    return 0;
}

