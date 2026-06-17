#pragma once

#include <chrono>
#include <string>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

#include "tabs.h"

namespace postline { namespace ui {

class StatTab : public Tab {
    struct StatSnapshot {
        std::vector<std::string> process_lines;
        std::vector<std::string> system_lines;
        std::vector<std::string> pstree_lines;
    };

    StatSnapshot snapshot;
    std::chrono::steady_clock::time_point last_refresh;
    int tree_scroll = 0;

    ftxui::Component renderer;

    void refresh();
    ftxui::Element renderProcessStats() const;
    ftxui::Element renderSystemStats() const;
    ftxui::Element renderProcessTree() const;

public:
    StatTab();

    std::string label() const override;
    ftxui::Component component() override;
};

}}
