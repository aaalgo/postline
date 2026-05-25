#include "tabs.h"

#include <algorithm>
#include <format>
#include <utility>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <postline/common.h>

#include "render.h"

namespace postline { namespace ui {

using namespace ftxui;

Element MessageReader::renderMessage() {
    Elements lines;
    json const &header = message->header();

    /*
    if (header.contains(CONTEXT_HEADER_NAME)) {
        appendTextLines(lines, header.at(CONTEXT_HEADER_NAME).dump(2));
    }
    */

    appendMessageHeaders(lines, header);
    lines.push_back(text(""));
    appendTextLines(lines, message->body());
    message_line_count = int(lines.size());
    message_scroll = std::clamp(message_scroll, 0,
                                std::max(0, message_line_count - 1));

    return vbox(std::move(lines));
}

bool MessageReader::onMessageViewerEvent(Event event) {
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

MessageReader::MessageReader(Message const *message_)
    : message(message_) {
    renderer = Renderer([&](bool) {
        return renderMessage() | focusPosition(0, message_scroll) | yframe;
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

MessageEditor::MessageEditor() {
    editor = Input(&editor_content, "message");
    renderer = Renderer(editor, [&] {
        return vbox({
            text("new message") | bold,
            hbox({
                text("To: "),
                editor->Render() | flex,
            }),
        });
    });
}

Component MessageEditor::component() {
    return renderer;
}

void ThreadTab::reloadCurrentMessage() {
    AccessID next_id = NO_ACCESS_ID;

    if (trace_selected >= 0 &&
        trace_selected < int(data->trace.size())) {
        next_id = data->trace.at(trace_selected).id;
    }

    if (next_id == current_message_id) {
        return;
    }

    if (next_id == NO_ACCESS_ID) {
        Message next;
        std::swap(current_message, next);
    }
    else {
        Message next = runtime->readMessage(next_id);
        std::swap(current_message, next);
    }

    current_message_id = next_id;
    message_reader.resetScroll();
}

ThreadTab::ThreadTab(Runtime *runtime_, Observer::Thread *data_)
    : runtime(runtime_),
      data(data_),
      message_reader(&current_message) {
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

    trace_list = List(&data->trace,
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

    root = Container::Horizontal({
        left_renderer,
        middle_renderer,
        right_renderer,
    });

    renderer = Renderer(root, [&] {
        return vbox({
            /*
            hbox({
                text("T" + std::to_string(thread->id)) | bold,
                text(" "),
                text(thread->name),
            }),*/
            hbox({
                left_renderer->Render() | size(WIDTH, LESS_THAN, 24),
                separator(),
                middle_renderer->Render() | size(WIDTH, LESS_THAN, 40) | flex,
                separator(),
                right_renderer->Render() | flex,
            }) | flex,
        });
    });
}

std::string ThreadTab::label() const {
    return "T" + std::to_string(data->id);
}

Component ThreadTab::component() {
    return renderer;
}

ThreadID ThreadTab::threadId() const {
    return data->id;
}

}}
