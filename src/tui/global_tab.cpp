#include "tabs.h"

#include <format>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include "render.h"

namespace postline { namespace ui {

using namespace ftxui;

ThreadID Tab::threadId() const {
    return NOT_A_THREAD;
}

bool Tab::onShortcut(Event const &) {
    return false;
}

GlobalData::GlobalData()
    : log_entries(MAX_VISIBLE_LOGS,
          [](spdlog::details::log_msg const &msg) {
              return renderLogEntry(msg);
          }),
      threads(MAX_VISIBLE_THREADS,
          [](std::string const &s) {
              return text(s);
          }) {
}

GlobalTab::GlobalTab(GlobalData *d): data(d) {
    ListOption thread_list_option;
    thread_list_option.on_open_selection = [this] {
        if (thread_selected >= 0 && data->onOpenThread) {
            data->onOpenThread(static_cast<ThreadID>(thread_selected));
        }
    };
    thread_list = List(&data->threads,
                       &thread_selected,
                       &thread_follow_tail,
                       std::move(thread_list_option));
    log_list = List(&data->log_entries,
                    &log_selected,
                    &log_follow_tail);

    left_renderer = Renderer(thread_list, [&] {
        return renderTopLevelWindowPane("Threads",
                                        thread_list->Render() | flex,
                                        left_renderer->Focused()) |
            reflect(left_pane_box);
    });

    right_renderer = Renderer(log_list, [&] {
        Element selected_log = text("");

        if (data->log_entries.contains(log_selected)) {
            selected_log = renderLogDetail(data->log_entries.at(log_selected));
        }

        return renderTopLevelWindowPane(
            "Runtime Log",
            vbox({
                log_list->Render() | flex,
                separator(),
                selected_log | size(HEIGHT, EQUAL, 5),
            }),
            right_renderer->Focused()) |
            reflect(right_pane_box);
    });

    root = Container::Horizontal({
        left_renderer,
        right_renderer,
    });

    renderer = Renderer(root, [&] {
        return hbox({
            left_renderer->Render() | size(WIDTH, EQUAL, 18),
            right_renderer->Render() | flex,
        }) | flex;
    });
    renderer |= CatchEvent([this](Event event) {
        if (!event.is_mouse() ||
            event.mouse().button != Mouse::Left ||
            event.mouse().motion != Mouse::Pressed) {
            return false;
        }
        if (left_pane_box.Contain(event.mouse().x, event.mouse().y)) {
            left_renderer->TakeFocus();
        }
        else if (right_pane_box.Contain(event.mouse().x, event.mouse().y)) {
            right_renderer->TakeFocus();
        }
        return false;
    });
}

std::string GlobalTab::label() const {
    return "Global";
}

Component GlobalTab::component() {
    return renderer;
}

}}
