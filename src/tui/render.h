#pragma once

#include <string>
#include <string_view>

#include <ftxui/dom/elements.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/details/log_msg.h>

namespace postline { namespace ui {

using json = nlohmann::json;

namespace theme {
ftxui::Color background();
ftxui::Color surface();
ftxui::Color surfaceActive();
ftxui::Color text();
ftxui::Color muted();
ftxui::Color border();
ftxui::Color accent();
ftxui::Color danger();
}

ftxui::Element renderLogEntry(spdlog::details::log_msg const &msg);
ftxui::Element renderLogDetail(spdlog::details::log_msg const &msg);
ftxui::Element renderWindowPane(std::string title,
                                ftxui::Element content,
                                bool focused = false);
ftxui::Element renderTopLevelWindowPane(std::string title,
                                        ftxui::Element content,
                                        bool focused = false);
void appendTextLines(ftxui::Elements &lines, std::string_view text_lines);
void appendMessageHeaders(ftxui::Elements &lines, json const &header);
ftxui::Element renderAboutBox(ftxui::Element close_button);

}}
