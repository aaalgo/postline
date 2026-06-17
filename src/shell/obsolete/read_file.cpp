#include <CLI/CLI.hpp>

#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

int64_t decimal_width(int64_t value)
{
    int64_t width = 1;
    while (value >= 10) {
        value /= 10;
        ++width;
    }
    return width;
}

}

int main(int argc, char** argv)
{
    int64_t offset = 1;
    int64_t limit = -1;
    std::string path;

    CLI::App app{"Print a file with line numbers"};
    app.add_option("-o,--offset", offset, "First 1-based line number to print")
        ->check(CLI::PositiveNumber);
    app.add_option("-l,--limit", limit, "Maximum number of lines to print")
        ->check(CLI::NonNegativeNumber);
    app.add_option("path", path, "File path")->required();
    CLI11_PARSE(app, argc, argv);

    std::ifstream input(path);
    if (!input) {
        std::cerr << "readfile: cannot open " << path << '\n';
        return 1;
    }

    std::vector<std::string> lines;
    std::string line;
    int64_t line_number = 1;
    while (std::getline(input, line)) {
        if (line_number >= offset) {
            if (limit >= 0 && static_cast<int64_t>(lines.size()) >= limit) {
                break;
            }
            lines.push_back(line);
        }
        ++line_number;
    }

    if (input.bad()) {
        std::cerr << "readfile: failed while reading " << path << '\n';
        return 1;
    }

    if (lines.empty()) {
        return 0;
    }

    int64_t last_line_number = offset + static_cast<int64_t>(lines.size()) - 1;
    int64_t width = decimal_width(last_line_number);
    for (size_t index = 0; index < lines.size(); ++index) {
        int64_t current = offset + static_cast<int64_t>(index);
        //std::cout.width(width);
        std::cout << current << ": " << lines[index] << '\n';
    }

    return 0;
}
