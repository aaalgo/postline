// CLI.cpp

#include <chrono>
#include <format>
#include <memory>
#include <string>
#include <thread>
#include <vector>

//#include <ftxui/component/app.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/loop.hpp>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include "ftx_list.hpp"

#include <postline/ui.h>

namespace postline { namespace ui {

using namespace ftxui;

static constexpr size_t MAX_VISIBLE_THREADS = 4096;
static constexpr size_t MAX_VISIBLE_LOGS = 4096;

using ThreadID = size_t;

struct ThreadData {
    ThreadID id;
    std::string name;
};

Component SlimButton(std::string label, std::function<void()> on_click) {
    class Impl : public ComponentBase {
    public:
        Impl(std::string label_, std::function<void()> on_click_)
            : label(std::move(label_)),
              on_click(std::move(on_click_)) {}

        Element OnRender() override {
            auto e = text(label);
            if (Focused())
                e = e | inverted;
            return e;
        }

        bool OnEvent(Event event) override {
            if (event == Event::Return ||
                event == Event::Character(' ')) {
                on_click();
                return true;
            }

            /*
            if (event.is_mouse() &&
                event.mouse().button == Mouse::Left &&
                event.mouse().motion == Mouse::Pressed) {
                on_click();
                return true;
            }
            */

            return false;
        }

    private:
        std::string label;
        std::function<void()> on_click;
    };

    return Make<Impl>(std::move(label), std::move(on_click));
}

struct Tab {
    virtual ~Tab() = default;

    virtual std::string label() const = 0;
    virtual Component component() = 0;
};


struct GlobalData {
    Runtime *runtime;
    ListData<std::string> log_entries;
    ListData<ThreadSummary> thread_summaries;

    std::function<void(ThreadSummary const &)> onOpenThread;

    GlobalData()
        : log_entries(MAX_VISIBLE_LOGS,
              [](std::string const &line, ListEntryState const& state) {
                  Element element = text(line);

                  if (state.focused) {
                      element |= inverted;
                  }
                  if (state.active) {
                      element |= bold;
                  }

                  return element;
              }),
          thread_summaries(MAX_VISIBLE_THREADS,
              [](ThreadSummary const &summary, ListEntryState const& state) {
                  Element element;

                  if (summary.name.empty()) {
                      element = text(std::format("{}", summary.id));
                  }
                  else {
                      element = text(std::format("{}: {}", summary.id, summary.name));
                  }

                  if (state.focused) {
                      element |= inverted;
                  }
                  if (state.active) {
                      element |= bold;
                  }

                  return element;
              }) {}

    void syncThreads () {
        runtime->updateThreadSummaries(&thread_summaries);
    }
};


struct GlobalTab : Tab {
    GlobalData *data;
    int thread_selected = -1;
    bool thread_follow_tail = true;
    int log_selected = -1;
    bool log_follow_tail = true;

    /*
    std::vector<std::string> thread_entries = {
        "T0  paused   /root/user",
        "T1  running  /root/build",
        "T2  blocked  /root/ui",
        "T3  error    /root/compiler",
        "T4  paused   /root/test",
    };
    */

    Component thread_list;
    Component log_list;
    Component left_renderer;
    Component right_renderer;
    Component root;
    Component renderer;

    GlobalTab(GlobalData *d): data(d) {
        data->syncThreads();
        thread_list = List(&data->thread_summaries,
                           &thread_selected,
                           &thread_follow_tail);
        log_list = List(&data->log_entries,
                        &log_selected,
                        &log_follow_tail);

        left_renderer = Renderer(thread_list, [&] {
            data->syncThreads();
            return vbox({
                text("threads") | bold,
                thread_list->Render() | flex,
            });
        });

        right_renderer = Renderer(log_list, [&] {
            Element selected_log = text("");

            if (data->log_entries.contains(log_selected)) {
                selected_log = vbox({
                    text("selected log") | bold,
                    text(data->log_entries.at(log_selected)),
                    text(""),
                    paragraph("This is simulated global runtime log detail. "
                              "Later this can show journal position, message "
                              "headers, domain/thread identifiers, and error "
                              "diagnostics."),
                });
            }

            return vbox({
                text("runtime log") | bold,
                log_list->Render() | flex,
                separator(),
                selected_log | size(HEIGHT, EQUAL, 6),
            });
        });

        root = Container::Horizontal({
            left_renderer,
            right_renderer,
        });

        renderer = Renderer(root, [&] {
            return hbox({
                left_renderer->Render() | size(WIDTH, EQUAL, 16),
                separator(),
                right_renderer->Render() | flex,
            }) | flex;
        });
    }

    std::string label() const override {
        return "Global";
    }

    Component component() override {
        return renderer;
    }
};

struct ThreadTab : Tab {
    ThreadData *thread;

    int nav_mode = 0;
    int nav_selected = 0;
    int trace_selected = 0;

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

    std::vector<std::string> trace_entries = {
        "000 user     -> planner   call",
        "001 planner  -> writer    call",
        "002 writer   -> compiler  call",
        "003 compiler -> writer    return",
        "004 writer   -> planner   return",
        "005 planner  -> user      pause",
    };

    std::string editor_content;

    Component nav_mode_menu;
    Component nav_list;
    Component trace_list;
    Component editor;

    Component left_renderer;
    Component right_renderer;
    Component root;
    Component renderer;

    explicit ThreadTab(ThreadData *thread_)
        : thread(thread_)
    {
        nav_mode_menu = Menu(
            &nav_modes,
            &nav_mode,
            MenuOption::Horizontal());

        nav_list = Menu(&tree_entries, &nav_selected);

        trace_list = Menu(&trace_entries, &trace_selected);

        editor = Input(&editor_content, "message");

        left_renderer = Renderer(
            Container::Vertical({
                nav_mode_menu,
                nav_list,
            }),
            [&] {
                std::vector<std::string> *entries = &tree_entries;

                if (nav_mode == 1)
                    entries = &stack_entries;
                else if (nav_mode == 2)
                    entries = &member_entries;

                // Rebind list source when mode changes.
                nav_list = Menu(entries, &nav_selected);

                return vbox({
                    nav_mode_menu->Render(),
                    separator(),
                    nav_list->Render() | flex,
                });
            });

        right_renderer = Renderer(
            Container::Vertical({
                trace_list,
                editor,
            }),
            [&] {
                Element selected_message = text("");

                if (trace_selected >= 0 &&
                    trace_selected < int(trace_entries.size())) {
                    selected_message = vbox({
                        text("selected message") | bold,
                        text("From: compiler"),
                        text("To: writer"),
                        text("Type: return"),
                        text(""),
                        paragraph("Compilation finished successfully. "
                                  "No diagnostics were produced."),
                    });
                }

                return vbox({
                    text("trace") | bold,
                    trace_list->Render() | flex,

                    separator(),

                    selected_message | size(HEIGHT, EQUAL, 4),

                    separator(),

                    vbox({
                        text("new message") | bold,
                        hbox({
                            text("To: "),
                            editor->Render() | flex,
                        }),
                    }) | size(HEIGHT, EQUAL, 7),
                });                        
            });

        root = Container::Horizontal({
            left_renderer,
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
                    left_renderer->Render() | size(WIDTH, EQUAL, 24),
                    separator(),
                    right_renderer->Render() | flex,
                }) | flex,
            });
        });
    }

    std::string label() const override {
        return "T" + std::to_string(thread->id);
    }

    Component component() override {
        return renderer;
    }
};

class TUI: public UI {
    GlobalData global;

    std::vector<std::unique_ptr<ThreadData>> threads;

    std::vector<std::unique_ptr<Tab>> tabs;

    std::vector<std::string> tab_labels;
    std::vector<Component> tab_components;

    int tab_index = 0;

    Component tab_selection;
    Component tab_content;

    Component exit_button;
    Component main_container;
    Component main_renderer;
    ScreenInteractive screen;

protected:
    void on_exit () override {
        screen.Exit();
    }

    virtual std::vector<Message> on_message (Message &&msg) override {
        screen.PostEvent(ftxui::Event::Custom);
        return std::vector<Message>();
    }

public:
    TUI(Runtime *runtime)
        : UI(runtime),
        screen(ScreenInteractive::Fullscreen()) {
        global.runtime = runtime;
        // fake threads
        /*
        for (size_t i = 0; i < 3; ++i) {
            auto t = std::make_unique<ThreadData>();
            t->id = i;
            t->name = "Thread_" + std::to_string(i);

            threads.push_back(std::move(t));
        }*/

        // create tabs
        tabs.push_back(
            std::make_unique<GlobalTab>(&global));

        /*
        tabs.push_back(
            std::make_unique<ThreadTab>(threads[0].get()));
            */

        rebuild_tabs();

        ButtonOption exit_option;
        exit_option.label = "Exit";
        exit_option.on_click = [this] {
            this->send(Message(json{{"To", "runtime"},
                         {"Subject", "exit"},
                         {"Thread-ID", "0"}}));
        };
        exit_option.transform = [this](EntryState const& state) {
            Element element = text(state.label);
            /*
            if (!can_send_) {
                element |= dim;
            }
            */
            if (state.focused) {
                element |= inverted;
            }
            return element;
        };
        exit_button = Button(std::move(exit_option));

        main_container = Container::Vertical({
            Container::Horizontal({
                tab_selection,
                exit_button,
            }),
            tab_content,
        });

        main_renderer = Renderer(main_container, [&] {
            return vbox({
                hbox({
                    text("Postline") | bold,
                    text(" | "),
                    tab_selection->Render(),
                    filler(),
                    exit_button->Render(),
                }),
                separator(),
                tab_content->Render() | flex,
            });
        });
    }

    virtual void appendLog (std::string &&line) {
        global.log_entries.push_back(std::move(line));
        screen.PostEvent(ftxui::Event::Custom);
    }

    void run() override {

        Loop loop(&screen, main_renderer);

        while (!loop.HasQuitted()) {
            screen.RequestAnimationFrame();

            loop.RunOnce();

            std::this_thread::sleep_for(
                std::chrono::milliseconds(1000 / 60));
        }
    }

private:
    void rebuild_tabs() {
        tab_labels.clear();
        tab_components.clear();

        for (auto &t : tabs) {
            tab_labels.push_back(t->label());
            tab_components.push_back(t->component());
        }

        tab_selection = Menu(
            &tab_labels,
            &tab_index,
            MenuOption::Horizontal());

        tab_content = Container::Tab(
            tab_components,
            &tab_index);
    }
};

std::unique_ptr<UI> make_tui (Runtime *runtime) {
    return std::make_unique<TUI>(runtime);
}


}}
