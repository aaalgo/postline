#include "tabs.h"

#include <algorithm>
#include <format>
#include <utility>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
#include <postline/common.h>

#include "render.h"

namespace postline { namespace ui {

using namespace ftxui;

Element MessageReader::renderMessage() {
    Elements lines;
    if (!(*message)->header().contains(CONTEXT_HEADER_NAME)) {
        return vbox(std::move(lines));
    }
    json const &header = (*message)->header();

    /*
    if (header.contains(CONTEXT_HEADER_NAME)) {
        appendTextLines(lines, header.at(CONTEXT_HEADER_NAME).dump(2));
    }
    */

    appendMessageHeaders(lines, header);
    lines.push_back(text(""));
    appendTextLines(lines, (*message)->body());
    message_line_count = int(lines.size());
    message_scroll = std::clamp(message_scroll, 0,
                                std::max(0, message_line_count - 1));

    return vbox(std::move(lines));
}

bool MessageReader::onMessageViewerEvent(Event event) {
    if (event.is_mouse()) {
        return onMessageViewerMouseEvent(event);
    }

    if (event == Event::ArrowUp || event == Event::Character('k')) {
        message_scroll = std::max(0, message_scroll - 1);
        return true;
    }
    if (event == Event::ArrowDown || event == Event::Character('j')) {
        message_scroll = std::min(std::max(0, message_line_count - 1),
                                  message_scroll + 1);
        return true;
    }
    if (event == Event::PageUp) {
        message_scroll = std::max(0, message_scroll - 10);
        return true;
    }
    if (event == Event::PageDown) {
        message_scroll = std::min(std::max(0, message_line_count - 1),
                                  message_scroll + 10);
        return true;
    }
    if (event == Event::Home) {
        message_scroll = 0;
        return true;
    }
    if (event == Event::End) {
        message_scroll = std::max(0, message_line_count - 1);
        return true;
    }
    return false;
}

bool MessageReader::onMessageViewerMouseEvent(Event event) {
    if (event.mouse().button != Mouse::WheelUp &&
        event.mouse().button != Mouse::WheelDown) {
        return false;
    }

    if (!message_viewport_box.Contain(event.mouse().x, event.mouse().y)) {
        return false;
    }

    if (event.mouse().button == Mouse::WheelUp) {
        message_scroll = std::max(0, message_scroll - 1);
        return true;
    }

    message_scroll = std::min(std::max(0, message_line_count - 1),
                              message_scroll + 1);
    return true;
}

MessageReader::MessageReader(Message const **message_)
    : message(message_) {
    renderer = Renderer([&](bool) {
        return renderMessage() |
            focusPosition(0, message_scroll) |
            vscroll_indicator |
            yframe |
            reflect(message_viewport_box);
    });
    renderer |= CatchEvent([&](Event event) {
        return onMessageViewerEvent(event);
    });
}

void MessageReader::resetScroll() {
    message_scroll = 0;
}

Component MessageReader::component() {
    return renderer;
}

MessageEditor::MessageEditor(Thread const *thread_,
                             SendCallback on_send_)
    : thread(thread_),
      on_send(std::move(on_send_)) {
    DropdownOption address_option;
    address_option.radiobox.entries = &addresses;
    address_option.radiobox.selected = &address_selected;
    address_option.checkbox.transform = [](EntryState const &state) {
        Element element = hbox({
            text(state.state ? "v " : "> ") | color(Color::GrayLight),
            text(state.label),
        });
        if (state.focused) {
            element |= inverted;
        }
        return element;
    };
    address_option.radiobox.transform = [](EntryState const &state) {
        Element element = text((state.active ? "> " : "  ") + state.label);
        if (state.focused) {
            element |= inverted;
        }
        if (state.active) {
            element |= bold;
        }
        return element;
    };
    address_option.transform = [](bool is_open,
                                  Element checkbox_element,
                                  Element radiobox_element) {
        if (is_open) {
            return vbox({
                std::move(checkbox_element),
                std::move(radiobox_element) |
                    size(HEIGHT, LESS_THAN, 6),
            });
        }
        return std::move(checkbox_element);
    };
    address_choice = Dropdown(std::move(address_option));

    ButtonOption send_option;
    send_option.label = "Send";
    send_option.on_click = [this] {
        CHECK(!thread->pending);
        CHECK(address_selected >= 0);
        CHECK(address_selected < int(addresses.size()));

        on_send(addresses[address_selected], subject_content, body_content);
        subject_content.clear();
        body_content.clear();
    };
    send_option.transform = [this](EntryState const &state) {
        Element element = text(state.label);
        if (thread->pending) {
            return element | color(Color::GrayDark) | dim;
        }

        element |= color(Color::GreenLight) | bold;
        if (state.focused) {
            element |= inverted;
        }
        return element;
    };
    send_button = Button(std::move(send_option));
    send_button |= CatchEvent([this](Event event) {
        if (!thread->pending) {
            return false;
        }
        if (event == Event::Return || event.is_mouse()) {
            return true;
        }
        return false;
    });

    auto subject_option = InputOption::Default();
    subject_option.multiline = false;
    subject_option.transform = [](InputState state) {
        if (state.is_placeholder) {
            state.element |= dim;
        }
        return state.element;
    };
    subject_editor = Input(&subject_content, "subject", subject_option);

    auto body_option = InputOption::Default();
    body_option.multiline = true;
    body_option.transform = [](InputState state) {
        if (state.is_placeholder) {
            state.element |= dim;
        }
        return state.element;
    };
    body_editor = Input(&body_content, body_option);

    renderer = Renderer(
        Container::Vertical({
            address_choice,
            send_button,
            subject_editor,
            body_editor,
        }),
        [&] {
            return vbox({
                hbox({
                    text("To: ") | color(Color::Cyan) | bold,
                    address_choice->Render() | flex,
                    filler(),
                    send_button->Render(),
                }),
                hbox({
                    text("Subject: ") | color(Color::Magenta) | bold,
                    subject_editor->Render() | flex,
                }),
                body_editor->Render() | flex,
            }) | flex;
        });
}

bool MessageEditor::sendCurrentMessage() {
    if (thread->pending) {
        return false;
    }
    if (subject_content.empty() && body_content.empty()) {
        return false;
    }

    CHECK(address_selected >= 0);
    CHECK(address_selected < int(addresses.size()));

    on_send(addresses[address_selected], subject_content, body_content);
    subject_content.clear();
    body_content.clear();
    return true;
}

Component MessageEditor::component() {
    return renderer;
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
                     ReadMessageCallback read_message_,
                     SendCallback on_send_)
    : data(data_),
      trace(observer, &data_->trace),
      read_message(std::move(read_message_)),
      on_send(std::move(on_send_)),
      message_reader(&current_message),
      message_editor(data_, [this](std::string to,
                                   std::string subject,
                                   std::string body) {
          on_send(data->id,
                  Message(json{{"To", std::move(to)},
                               {"Subject", std::move(subject)}},
                          std::move(body)));
      }) {
    nav_mode_menu = Menu(
        &nav_modes,
        &nav_mode,
        MenuOption::Horizontal());

    tree_list = Menu(&tree_entries, &nav_selected);
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
            Element thread_name = text("");
            if (!data->name.empty()) {
                thread_name = hbox({
                    text(" "),
                    text(data->name),
                });
            }

            return vbox({
                hbox({
                    text("thread ") | bold,
                    text(std::format("{}", data->id)) | color(Color::Yellow),
                    thread_name,
                }),
                nav_mode_menu->Render(),
                separator(),
                nav_content->Render() | flex,
            });
        });

    middle_renderer = Renderer(
        trace_list,
        [&] {
            return trace_list->Render();
        });

    right_renderer = Renderer(
        Container::Vertical({
            message_reader.component(),
            message_editor.component(),
        }),
        [&] {
            reloadCurrentMessage();
            return vbox({
                message_reader.component()->Render() | flex,

                separator(),

                message_editor.component()->Render() | size(HEIGHT, EQUAL, 12),
            });
        });

    auto middle_right_split =
        ResizableSplitLeft(middle_renderer, right_renderer,
                           &middle_column_width);
    root = ResizableSplitLeft(left_renderer, middle_right_split,
                              &left_column_width);

    renderer = Renderer(root, [&] {
        if (right_pane_mode == RightPaneMode::Message) {
            reloadCurrentMessage();
            return message_reader.component()->Render() | flex;
        }
        if (right_pane_mode == RightPaneMode::Editor) {
            return message_editor.component()->Render() | flex;
        }

        return root->Render() | flex;
    });
}

std::string ThreadTab::label() const {
    return "T" + std::to_string(data->id);
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
