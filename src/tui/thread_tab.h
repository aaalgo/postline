#pragma once

#include <functional>
#include <string>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>
#include <postline/common.h>
#include <postline/observer.h>

#include "message_editor.h"
#include "message_list_data_ref.h"
#include "message_reader.h"
#include "tabs.h"
#include "thread_focus.h"
#include "thread_nav.h"

namespace postline { namespace ui {

class ThreadTab : public Tab {
    enum class RightPaneMode {
        Normal,
        Message,
        Editor,
    };

    Thread *data;
    Agent *user;
    MessageListDataRef trace;
    std::function<Message const *(AccessID)> read_message;
    std::function<void(ThreadID, Message&&)> on_send;

    Message const *current_message;
    AccessID current_message_id = NO_ACCESS_ID;
    MessageReader message_reader;
    MessageEditor message_editor;
    ThreadPaneFocus pane_focus;

    RightPaneMode right_pane_mode = RightPaneMode::Normal;

    int nav_mode = 0;
    int nav_selected = 0;
    int trace_selected = 0;
    bool trace_follow_tail = true;
    int left_column_width = 24;
    int middle_column_width = 40;

    std::vector<std::string> nav_modes = {
        "tree", "stack", "members",
    };

    std::vector<std::string> tree_entries;
    std::vector<ThreadNavTreeEntry> tree_metadata;

    std::vector<std::string> stack_entries;

    std::vector<std::string> member_entries;

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
    ftxui::Box navigation_pane_box;
    ftxui::Box trace_pane_box;
    ftxui::Box reader_pane_box;
    ftxui::Box composer_pane_box;

    void syncTreeEntries();
    void syncStackEntries();
    void syncMemberEntries();
    void reloadCurrentMessage();
    void focusPane();
    bool onFocusEvent(ftxui::Event const &event);
    bool paneFocused(ThreadPaneFocus::FocusedPane pane);
    ftxui::Element renderPane(std::string title,
                              ftxui::Element content,
                              bool focused) const;
    void toggleMaximizeRightPane();

public:
    explicit ThreadTab(Observer *observer, Thread *data_,
                       std::function<Message const *(AccessID)> read_message_,
                       std::function<void(ThreadID, Message&&)> on_send_);

    std::string label() const override;
    ftxui::Component component() override;
    bool onShortcut(ftxui::Event const &event) override;
    ThreadID threadId() const override;
};

}}
