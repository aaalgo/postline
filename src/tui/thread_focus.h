#pragma once

#include <ftxui/component/event.hpp>

namespace postline { namespace ui {

class ThreadPaneFocus {
    static constexpr int PANE_COUNT = 4;

    int pane = 0;

public:
    enum class FocusedPane {
        Navigation,
        Trace,
        Reader,
        Composer,
    };

    FocusedPane focusedPane() const {
        return static_cast<FocusedPane>(pane);
    }

    bool onEvent(ftxui::Event const &event) {
        if (event == ftxui::Event::Tab) {
            pane = (pane + 1) % PANE_COUNT;
            return true;
        }
        if (event == ftxui::Event::TabReverse) {
            pane = (pane + PANE_COUNT - 1) % PANE_COUNT;
            return true;
        }
        return false;
    }

    void setFocusedPane(FocusedPane focused) {
        pane = static_cast<int>(focused);
    }
};

}}
