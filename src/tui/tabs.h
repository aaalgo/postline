#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <ftxui/component/component.hpp>
#include <postline/runtime.h>
#include <spdlog/details/log_msg_buffer.h>

#include "../ftx_list.hpp"
#include "limits.h"
#include "observer.h"

namespace postline { namespace ui {

struct Tab {
    virtual ~Tab() = default;

    virtual std::string label() const = 0;
    virtual ftxui::Component component() = 0;
    virtual ThreadID threadId() const;
};

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

public:
    explicit GlobalTab(GlobalData *d);

    std::string label() const override;
    ftxui::Component component() override;
};

class MessageReader {
    Message const *message;

    int message_scroll = 0;
    int message_line_count = 0;

    ftxui::Component renderer;

    ftxui::Element renderMessage();
    bool onMessageViewerEvent(ftxui::Event event);

public:
    explicit MessageReader(Message const *message_);

    void resetScroll();
    ftxui::Component component();
};

class MessageEditor {
    std::string editor_content;

    ftxui::Component editor;
    ftxui::Component renderer;

public:
    MessageEditor();

    ftxui::Component component();
};

class ThreadTab : public Tab {
    Runtime *runtime;
    Observer::Thread *data;

    Message current_message;
    AccessID current_message_id = NO_ACCESS_ID;
    MessageReader message_reader;
    MessageEditor message_editor;

    int nav_mode = 0;
    int nav_selected = 0;
    int trace_selected = 0;
    bool trace_follow_tail = true;

    std::vector<std::string> nav_modes = {
        "tree", "stack", "members",
    };

    std::vector<std::string> tree_entries = {
        "/",
        "/build",
        "/build/compiler",
        "/build/test",
        "/ui",
    };

    std::vector<std::string> stack_entries = {
        "user",
        "planner",
        "writer",
        "compiler",
    };

    std::vector<std::string> member_entries = {
        "@user",
        "@runtime",
        "@planner",
        "@writer",
        "@compiler",
        "domain:build",
        "domain:ui",
    };

    ftxui::Component nav_mode_menu;
    ftxui::Component tree_list;
    ftxui::Component stack_list;
    ftxui::Component member_list;
    ftxui::Component nav_content;
    ftxui::Component trace_list;

    ftxui::Component left_renderer;
    ftxui::Component middle_renderer;
    ftxui::Component right_renderer;
    ftxui::Component root;
    ftxui::Component renderer;

    void reloadCurrentMessage();

public:
    explicit ThreadTab(Runtime *runtime_, Observer::Thread *data_);

    std::string label() const override;
    ftxui::Component component() override;
    ThreadID threadId() const override;
};

}}
