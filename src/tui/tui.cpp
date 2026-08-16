#include <algorithm>
#include <format>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/loop.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <postline/ui.h>
#include <spdlog/details/log_msg_buffer.h>

#include <postline/observer.h>
#include "global_tab.h"
#include "render.h"
#include "stat_tab.h"
#include "tabs.h"
#include "thread_tab.h"

namespace postline { namespace ui {

using namespace ftxui;

class TUI: public UI, public Observer {
    GlobalData global;
    std::mutex pending_log_mutex;
    std::vector<spdlog::details::log_msg_buffer> pending_logs;

    std::vector<std::unique_ptr<Tab>> tabs;
    std::unordered_map<ThreadID, size_t> thread_tab_indexes;

    std::vector<std::string> tab_labels;
    std::vector<Component> tab_components;

    int tab_index = 0;

    Component tab_selection;
    Component tab_content;

    bool about_shown = false;
    Component about_button;
    Component about_modal;
    Component exit_button;
    Component main_container;
    Component main_renderer;
    ScreenInteractive screen;

    static bool isPassiveMouseMove(Event event) {
        return event.is_mouse() &&
               event.mouse().motion == Mouse::Moved &&
               event.mouse().button == Mouse::None;
    }

    static Element renderChromeButton(EntryState const& state,
                                      Color normal,
                                      Color focused) {
        Element element = text(" " + state.label + " ") |
                          color(state.focused ? focused : normal) |
                          bold;
        if (state.focused) {
            element |= bgcolor(theme::surfaceActive());
        }
        return element;
    }

    static Color topBarBackground() {
        return Color::RGB(30, 41, 59);
    }

    static Color topBarText() {
        return Color::RGB(125, 211, 252);
    }

    static Color topBarMutedText() {
        return Color::RGB(167, 139, 250);
    }

    static Color topBarDangerText() {
        return Color::RGB(248, 113, 113);
    }

    static Element renderTopBarButton(EntryState const& state,
                                      Color normal,
                                      Color focused) {
        Element element = text(" " + state.label + " ") |
                          color(state.focused ? focused : normal) |
                          bgcolor(topBarBackground()) |
                          bold;
        if (state.focused) {
            element |= underlined;
        }
        return element;
    }

    bool handleShortcut(Event const &event) {
        if (event != Event::F10 && event != Event::F12) {
            return false;
        }
        if (tab_index < 0 || tab_index >= int(tabs.size())) {
            return true;
        }
        return tabs[tab_index]->onShortcut(event);
    }

    void addTab(std::unique_ptr<Tab> tab) {
        tab_labels.push_back(tab->label());
        tab_components.push_back(tab->component());

        if (tab_content) {
            tab_content->Add(tab_components.back());
        }

        tabs.push_back(std::move(tab));
    }

    void drainLogs() {
        std::vector<spdlog::details::log_msg_buffer> logs;
        {
            std::lock_guard<std::mutex> lock(pending_log_mutex);
            logs.swap(pending_logs);
        }

        for (auto &msg: logs) {
            global.log_entries.push_back(std::move(msg));
        }
    }

    void openThreadTab(ThreadID id) {
        CHECK(id >= 0);
        CHECK(id < int(Observer::threads.size()));

        auto it = thread_tab_indexes.find(id);
        CHECK(it != thread_tab_indexes.end());
        tab_index = int(it->second);
    }

    void syncObserver() {
        for (unsigned i = global.threads.size(); i < Observer::threads.size(); ++i) {
            auto *th = Observer::threads[i].get();
            std::string name;
            if (th->name.empty()) {
                name = std::format("{}", th->id);
            }
            else {
                name = std::format("{}: {}", th->id, th->name);
            }
            global.threads.push_back(std::move(name));
            size_t tab_position = tabs.size();
            addTab(std::make_unique<ThreadTab>(
                this, th,
                [this](AccessID access_id) {
                    return getMessage(access_id);
                },
                [this](ThreadID thread_id, Message &&msg) {
                    send(thread_id, std::move(msg));
                }));
            CHECK(thread_tab_indexes.emplace(th->id, tab_position).second);
        }

        for (size_t i = 0; i < tabs.size(); ++i) {
            tabs[i]->sync(int(i) == tab_index);
            tab_labels[i] = tabs[i]->label();
        }

        for (int i = global.threads.firstValid();
             i < global.threads.end(); ++i) {
            CHECK(i >= 0);
            CHECK(i < int(Observer::threads.size()));
            Thread const *thread = Observer::threads.at(i).get();
            std::string name = thread->name.empty()
                ? std::format("{}", thread->id)
                : std::format("{}: {}", thread->id, thread->name);
            auto tab_it = thread_tab_indexes.find(thread->id);
            CHECK(tab_it != thread_tab_indexes.end());
            CHECK(tab_it->second < tabs.size());
            if (tabs[tab_it->second]->label().ends_with('*')) {
                name += "*";
            }
            global.threads.set(i, std::move(name));
        }
    }

protected:
    void on_exit() override {
        screen.Exit();
    }

    std::vector<Message> on_message(Message &&msg) override {
        screen.PostEvent(ftxui::Event::Custom);
        return UI::on_message(std::move(msg));
    }

public:
    TUI() : screen(ScreenInteractive::Fullscreen()) {
        global.runtime = nullptr;
        global.onOpenThread = [this](int thread) {
            openThreadTab(thread);
        };

        addTab(std::make_unique<GlobalTab>(&global));
        addTab(std::make_unique<StatTab>());

        MenuOption tab_option = MenuOption::Horizontal();
        tab_option.entries_option.transform = [](EntryState const& state) {
            Element element = text(" " + state.label + " ") |
                              bgcolor(topBarBackground());
            if (state.active) {
                element |= color(topBarText()) | bold;
            }
            else {
                element |= color(topBarMutedText());
            }
            if (state.focused) {
                element |= underlined;
            }
            return element;
        };
        tab_option.elements_infix = [] {
            return text(" ");
        };
        tab_selection = Menu(
            &tab_labels,
            &tab_index,
            std::move(tab_option));

        tab_content = Container::Tab(
            tab_components,
            &tab_index);

        ButtonOption about_option;
        about_option.label = "About";
        about_option.on_click = [this] {
            about_shown = true;
        };
        about_option.transform = [](EntryState const& state) {
            return renderTopBarButton(state, topBarText(), topBarText());
        };
        about_button = Button(std::move(about_option));

        ButtonOption about_close_option;
        about_close_option.label = "Close";
        about_close_option.on_click = [this] {
            about_shown = false;
        };
        about_close_option.transform = [](EntryState const& state) {
            return renderChromeButton(state, theme::accent(), theme::text());
        };
        auto about_close_button = Button(std::move(about_close_option));
        about_modal = Renderer(about_close_button, [about_close_button] {
            return renderAboutBox(about_close_button->Render());
        });
        about_modal |= CatchEvent([this](Event event) {
            if (event == Event::Escape) {
                about_shown = false;
                return true;
            }
            return false;
        });

        ButtonOption exit_option;
        exit_option.label = "Exit";
        exit_option.on_click = [this] {
            this->send(0, Message(json{{"To", "runtime"},
                         {"Subject", "exit"}}));
        };
        exit_option.transform = [this](EntryState const& state) {
            return renderTopBarButton(state, topBarDangerText(), topBarDangerText());
        };
        exit_button = Button(std::move(exit_option));

        main_container = Container::Vertical({
                Container::Horizontal({
                    tab_selection,
                    about_button,
                    exit_button,
                }),
                tab_content,
            });

        main_renderer = Renderer(main_container, [&] {
            Observer::process();
            syncObserver();
            drainLogs();
            return vbox({
                hbox({
                    text(" Postline ") |
                        color(topBarText()) |
                        bgcolor(topBarBackground()) |
                        bold,
                    text(" "),
                    tab_selection->Render(),
                    filler(),
                    about_button->Render(),
                    text(" "),
                    exit_button->Render(),
                }) | bgcolor(topBarBackground()),
                tab_content->Render() | flex,
                text("Tab: next pane | Shift-Tab: previous pane | F10: maximize | F12: send | ?: help") |
                    color(theme::muted()) |
                    bgcolor(theme::surface()),
            }) | bgcolor(theme::background());
        });
        main_renderer |= CatchEvent([this](Event event) {
            if (isPassiveMouseMove(event)) {
                return true;
            }
            return handleShortcut(event);
        });
        main_renderer |= Modal(about_modal, &about_shown);
    }

    void setRuntime(Runtime *rt) override {
        UI::setRuntime(rt);
        global.runtime = rt;
    }

    void appendLog(spdlog::details::log_msg const& msg) override {
        {
            std::lock_guard<std::mutex> lock(pending_log_mutex);
            pending_logs.emplace_back(msg);
        }
        screen.PostEvent(ftxui::Event::Custom);
    }

    std::function<void(Message&&)> consume() override {
        return [this](Message &&m) {
            Observer::consume(std::move(m));
            screen.PostEvent(ftxui::Event::Custom);
        };
    }

    void run() override {
        Loop loop(&screen, main_renderer);
        loop.Run();
    }
};

std::unique_ptr<UI> make_tui() {
    return std::make_unique<TUI>();
}

}}
