#pragma once

#include <functional>
#include <string>

#include <ftxui/component/component.hpp>
#include <ftxui/screen/box.hpp>
#include <postline/runtime.h>
#include <spdlog/details/log_msg_buffer.h>

#include "ftx_list.hpp"
#include "limits.h"
#include "tabs.h"

namespace postline { namespace ui {

struct GlobalData {
    Runtime *runtime;
    ftxui::ListData<spdlog::details::log_msg_buffer> log_entries;
    ftxui::ListData<std::string> threads;

    std::function<void(ThreadID)> onOpenThread;

    GlobalData();
};

class GlobalTab : public Tab {
    GlobalData *data;
    int thread_selected = 0;
    bool thread_follow_tail = false;
    int log_selected = -1;
    bool log_follow_tail = true;

    ftxui::Component thread_list;
    ftxui::Component log_list;

    ftxui::Component left_renderer;
    ftxui::Component right_renderer;
    ftxui::Component root;
    ftxui::Component renderer;
    ftxui::Box left_pane_box;
    ftxui::Box right_pane_box;

public:
    explicit GlobalTab(GlobalData *d);

    std::string label() const override;
    ftxui::Component component() override;
};

}}
