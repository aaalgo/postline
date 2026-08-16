#include "thread_tab.h"

#include <algorithm>
#include <format>
#include <utility>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>

#include "render.h"
#include "split_pane.h"

namespace postline { namespace ui {

using namespace ftxui;

void ThreadTab::syncTreeEntries() {
    tree_entries.clear();
    tree_metadata = buildThreadTreeEntries(data);
    tree_entries.reserve(tree_metadata.size());
    for (ThreadNavTreeEntry const &entry: tree_metadata) {
        tree_entries.push_back(entry.label);
    }

    if (nav_mode != 0) {
        return;
    }
    nav_selected = std::clamp(
        nav_selected, 0, int(tree_entries.size()) - 1);
}

void ThreadTab::syncStackEntries() {
    stack_entries = buildThreadStackEntries(data);

    if (nav_mode != 1) {
        return;
    }
    if (stack_entries.empty()) {
        nav_selected = 0;
        return;
    }
    nav_selected = std::clamp(
        nav_selected, 0, int(stack_entries.size()) - 1);
}

void ThreadTab::syncMemberEntries() {
    member_entries = buildThreadMemberEntries(data);

    if (nav_mode != 2) {
        return;
    }
    if (member_entries.empty()) {
        nav_selected = 0;
        return;
    }
    nav_selected = std::clamp(
        nav_selected, 0, int(member_entries.size()) - 1);
}

void ThreadTab::reloadCurrentMessage() {
    AccessID next_id = NO_ACCESS_ID;

    if (trace_selected >= 0 &&
        trace_selected < int(data->trace.size())) {
        next_id = data->trace.at(trace_selected);
    }

    if (next_id == current_message_id) {
        return;
    }

    current_message = read_message(next_id);
    current_message_id = next_id;
    message_reader.resetScroll();
}

void ThreadTab::observeNewMessages() {
    CHECK(observed_trace_size <= data->trace.size());
    while (observed_trace_size < data->trace.size()) {
        AccessID id = data->trace.at(observed_trace_size++);
        Message const *msg = read_message(id);
        CHECK(msg);
        CHECK(msg->header().contains(CONTEXT_HEADER_NAME));
        json const &ctx = msg->header().at(CONTEXT_HEADER_NAME);
        AgentID from_agent_id = ctx.at("from_agent_id").get<AgentID>();
        if (from_agent_id != user->id) {
            unread.observe(id);
        }
    }
}

void ThreadTab::focusPane() {
    switch (pane_focus.focusedPane()) {
    case ThreadPaneFocus::FocusedPane::Navigation:
        left_renderer->TakeFocus();
        return;
    case ThreadPaneFocus::FocusedPane::Trace:
        trace_list->TakeFocus();
        return;
    case ThreadPaneFocus::FocusedPane::Reader:
        message_reader.component()->TakeFocus();
        return;
    case ThreadPaneFocus::FocusedPane::Composer:
        message_editor.component()->TakeFocus();
        return;
    }
    CHECK(0);
}

bool ThreadTab::onFocusEvent(Event const &event) {
    if (left_renderer->Focused()) {
        pane_focus.setFocusedPane(
            ThreadPaneFocus::FocusedPane::Navigation);
    }
    else if (trace_list->Focused()) {
        pane_focus.setFocusedPane(ThreadPaneFocus::FocusedPane::Trace);
    }
    else if (message_reader.component()->Focused()) {
        pane_focus.setFocusedPane(ThreadPaneFocus::FocusedPane::Reader);
    }
    else if (message_editor.component()->Focused()) {
        pane_focus.setFocusedPane(ThreadPaneFocus::FocusedPane::Composer);
    }

    if (!pane_focus.onEvent(event)) {
        return false;
    }
    focusPane();
    return true;
}

bool ThreadTab::paneFocused(ThreadPaneFocus::FocusedPane pane) {
    switch (pane) {
    case ThreadPaneFocus::FocusedPane::Navigation:
        return left_renderer->Focused();
    case ThreadPaneFocus::FocusedPane::Trace:
        return trace_list->Focused();
    case ThreadPaneFocus::FocusedPane::Reader:
        return message_reader.component()->Focused();
    case ThreadPaneFocus::FocusedPane::Composer:
        return message_editor.component()->Focused();
    }
    CHECK(0);
}

Element ThreadTab::renderPane(std::string title,
                              Element content,
                              bool focused) const {
    return renderWindowPane(std::move(title), std::move(content), focused);
}

void ThreadTab::toggleMaximizeRightPane() {
    if (right_pane_mode != RightPaneMode::Normal) {
        right_pane_mode = RightPaneMode::Normal;
        return;
    }

    if (message_editor.component()->Focused()) {
        right_pane_mode = RightPaneMode::Editor;
        message_editor.component()->TakeFocus();
        return;
    }

    right_pane_mode = RightPaneMode::Message;
    message_reader.component()->TakeFocus();
}

ThreadTab::ThreadTab(Observer *observer, Thread *data_,
                     std::function<Message const *(AccessID)> read_message_,
                     std::function<void(ThreadID, Message&&)> on_send_)
    : data(data_),
      user(observer->user),
      trace(observer, &data_->trace,
            [this](AccessID id) {
                return unread.isUnread(id);
            }),
      read_message(std::move(read_message_)),
      on_send(std::move(on_send_)),
      message_reader(&current_message),
      message_editor(
          data_,
          [observer](std::vector<Agent *> *agents) {
              CHECK(agents);
              CHECK(observer->runtime);
              agents->push_back(observer->runtime);
          },
          [this](Message &&msg) {
              std::string to = msg.get("To");
              CHECK(!to.empty());
              CHECK(msg.get("Thread-ID") == std::format("{}", data->id));
              if (!data->stack.empty()) {
                  Frame const &frame = data->stack.back();
                  CHECK(frame.to.agent);
                  CHECK(frame.from.agent);
                  if (frame.to.agent == user &&
                      frame.from.agent->name == to) {
                      msg.updateHeader([&frame](json &header) {
                          header["In-Reply-To"] =
                              std::format("{}", frame.message_id);
                      });
                  }
              }
              on_send(data->id, std::move(msg));
          }),
      observed_trace_size(data_->trace.size()) {
    CHECK(user);

    nav_mode_menu = Menu(
        &nav_modes,
        &nav_mode,
        MenuOption::Horizontal());

    MenuOption tree_option;
    tree_option.entries_option.transform = [this](EntryState const &state) {
        CHECK(state.index >= 0);
        CHECK(state.index < int(tree_metadata.size()));
        ThreadNavTreeEntry const &entry = tree_metadata.at(state.index);
        CHECK(state.label == entry.label);

        Element element = text((state.active ? "> " : "  ") + state.label) |
                          color(entry.pruned ? theme::muted() : theme::text());
        if (entry.pruned) {
            element |= dim;
        }
        if (state.focused) {
            element |= inverted;
        }
        if (state.active) {
            element |= bold;
        }
        return element;
    };
    tree_list = Menu(&tree_entries, &nav_selected, tree_option);
    stack_list = Menu(&stack_entries, &nav_selected);
    member_list = Menu(&member_entries, &nav_selected);
    nav_content = Container::Tab({
        tree_list,
        stack_list,
        member_list,
    }, &nav_mode);

    trace_list = List(&trace,
                       &trace_selected,
                       &trace_follow_tail);

    left_renderer = Renderer(
        Container::Vertical({
            nav_mode_menu,
            nav_content,
        }),
        [&] {
            syncTreeEntries();
            syncStackEntries();
            syncMemberEntries();

            Element thread_name = text("");
            if (!data->name.empty()) {
                thread_name = hbox({
                    text(" "),
                    text(data->name),
                });
            }

            return renderPane(
                "Navigation",
                vbox({
                    hbox({
                        text("thread ") | bold,
                        text(std::format("{}", data->id)) |
                            color(Color::Yellow),
                        thread_name,
                    }),
                    nav_mode_menu->Render(),
                    separator(),
                    nav_content->Render() | flex,
                }),
                paneFocused(ThreadPaneFocus::FocusedPane::Navigation)) |
                reflect(navigation_pane_box);
        });

    middle_renderer = Renderer(
        trace_list,
        [&] {
            return renderPane(
                "Trace",
                trace_list->Render(),
                paneFocused(ThreadPaneFocus::FocusedPane::Trace)) |
                reflect(trace_pane_box);
        });

    right_renderer = Renderer(
        Container::Vertical({
            message_reader.component(),
            message_editor.component(),
        }),
        [&] {
            reloadCurrentMessage();
            return vbox({
                renderPane(
                    "Reader",
                    message_reader.component()->Render() | flex,
                    paneFocused(ThreadPaneFocus::FocusedPane::Reader)) |
                    reflect(reader_pane_box) | flex,
                renderPane(
                    "Composer",
                    message_editor.component()->Render(),
                    paneFocused(ThreadPaneFocus::FocusedPane::Composer)) |
                    reflect(composer_pane_box) |
                    size(HEIGHT, EQUAL, 12),
            });
        });

    auto middle_right_split =
        borderlessSplitLeft(middle_renderer, right_renderer,
                            &middle_column_width);
    root = borderlessSplitLeft(left_renderer, middle_right_split,
                               &left_column_width);

    renderer = Renderer(root, [&] {
        if (right_pane_mode == RightPaneMode::Message) {
            reloadCurrentMessage();
            return renderPane(
                "Reader",
                message_reader.component()->Render() | flex,
                paneFocused(ThreadPaneFocus::FocusedPane::Reader)) |
                reflect(reader_pane_box) | flex;
        }
        if (right_pane_mode == RightPaneMode::Editor) {
            return renderPane(
                "Composer",
                message_editor.component()->Render() | flex,
                paneFocused(ThreadPaneFocus::FocusedPane::Composer)) |
                reflect(composer_pane_box) | flex;
        }

        return root->Render() | flex;
    });
    renderer |= CatchEvent([this](Event event) {
        if (event.is_mouse() &&
            event.mouse().button == Mouse::Left &&
            event.mouse().motion == Mouse::Pressed) {
            if (navigation_pane_box.Contain(event.mouse().x, event.mouse().y)) {
                left_renderer->TakeFocus();
            }
            else if (trace_pane_box.Contain(event.mouse().x, event.mouse().y)) {
                trace_list->TakeFocus();
            }
            else if (reader_pane_box.Contain(event.mouse().x, event.mouse().y)) {
                message_reader.component()->TakeFocus();
            }
            else if (composer_pane_box.Contain(event.mouse().x,
                                              event.mouse().y)) {
                message_editor.component()->TakeFocus();
            }
        }
        return onFocusEvent(event);
    });
    focusPane();
}

std::string ThreadTab::label() const {
    return "T" + std::to_string(data->id) +
           (unread.hasUnread() ? "*" : "");
}

void ThreadTab::sync(bool active_) {
    observeNewMessages();
    if (!active_) {
        return;
    }
    if (trace_selected >= 0 &&
        trace_selected < int(data->trace.size())) {
        unread.markRead(data->trace.at(trace_selected));
    }
}

Component ThreadTab::component() {
    return renderer;
}

bool ThreadTab::onShortcut(Event const &event) {
    if (event == Event::F12) {
        return message_editor.sendCurrentMessage();
    }
    if (event == Event::F10) {
        toggleMaximizeRightPane();
        return true;
    }
    return false;
}

ThreadID ThreadTab::threadId() const {
    return data->id;
}

}}
