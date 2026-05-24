#include <chrono>
#include <ctime>
#include <format>
#include <iomanip>
#include <optional>
#include <memory>
#include <mutex>
#include <charconv>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <spdlog/details/log_msg_buffer.h>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/loop.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <postline/ui.h>
#include "ftx_list.hpp"

namespace postline { namespace ui {

using namespace ftxui;
using postline::NOT_A_THREAD;
using postline::ThreadID;


static constexpr size_t MAX_VISIBLE_THREADS = 4096;
static constexpr size_t MAX_VISIBLE_LOGS = 16384;
static constexpr size_t MAX_VISIBLE_MESSAGES = 16384;

#include "observer.h"

static Decorator logLevelColor(spdlog::level::level_enum level) {
    switch (level) {
    case spdlog::level::trace:
        return color(Color::GrayLight);
    case spdlog::level::debug:
        return color(Color::Cyan);
    case spdlog::level::info:
        return color(Color::Green);
    case spdlog::level::warn:
        return color(Color::Yellow);
    case spdlog::level::err:
        return color(Color::RedLight) | bold;
    case spdlog::level::critical:
        return color(Color::Red) | bold;
    default:
        return color(Color::GrayDark);
    }
}

std::string toString(spdlog::string_view_t view) {
    return std::string(view.data(), view.size());
}


static Element renderLogEntry(spdlog::details::log_msg const& msg) {
    std::string level = toString(spdlog::level::to_string_view(msg.level));
    std::string payload = toString(msg.payload);

    return hbox({
        text("["),
        text(level) | logLevelColor(msg.level),
        text("] "),
        text(payload),
    });
}

static std::string formatLogTime(spdlog::log_clock::time_point time) {
    auto time_t = spdlog::log_clock::to_time_t(time);
    std::tm tm;
    localtime_r(&time_t, &tm);

    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
        time.time_since_epoch()).count() % 1000;

    std::ostringstream out;
    out << std::put_time(&tm, "%H:%M:%S")
        << "." << std::setfill('0') << std::setw(3) << millis;
    return out.str();
}


static Element renderLogDetail(spdlog::details::log_msg const& msg) {
    std::string level = toString(spdlog::level::to_string_view(msg.level));
    std::string logger = toString(msg.logger_name);
    std::string payload = toString(msg.payload);
    std::string filename = msg.source.filename ? msg.source.filename : "";
    std::string funcname = msg.source.funcname ? msg.source.funcname : "";

    if (logger.empty()) {
        logger = "(default)";
    }

    if (filename.empty()) {
        filename = "(unknown)";
    }

    if (funcname.empty()) {
        funcname = "(unknown)";
    }

    return vbox({
        hbox({
            text("["),
            text(level) | logLevelColor(msg.level),
            text("] "),
            text(logger) | color(Color::Cyan),
            text("  "),
            text(formatLogTime(msg.time)) | color(Color::Green),
            text("  #"),
            text(std::format("{}", msg.thread_id)) | color(Color::Yellow),
        }),
        hbox({
            text(std::format("{}:{}", filename, msg.source.line))
                | color(Color::GrayLight),
            text("  "),
            text(funcname) | color(Color::Magenta),
        }),
        paragraph(payload),
    });
}

struct Tab {
    virtual ~Tab() = default;

    virtual std::string label() const = 0;
    virtual Component component() = 0;
    virtual ThreadID threadId() const { return NOT_A_THREAD; }
};

struct GlobalData {
    Runtime *runtime;
    ListData<spdlog::details::log_msg_buffer> log_entries;
    ListData<std::string> threads;

    std::function<void(ThreadID)> onOpenThread;

    GlobalData()
        : log_entries(MAX_VISIBLE_LOGS,
              [](spdlog::details::log_msg const &msg) {
                  return renderLogEntry(msg);
              }),
          threads(MAX_VISIBLE_THREADS,
              [](std::string const &s) {
                  return text(s);
              }) {
    }
};


struct GlobalTab : Tab {
    GlobalData *data;
    int thread_selected = 0;
    bool thread_follow_tail = false;
    int log_selected = -1;
    bool log_follow_tail = true;

    Component thread_list;
    Component log_list;
    Component left_renderer;
    Component right_renderer;
    Component root;
    Component renderer;

    GlobalTab(GlobalData *d): data(d) {
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
            return vbox({
                text("threads") | bold,
                thread_list->Render() | flex,
            });
        });

        right_renderer = Renderer(log_list, [&] {
            Element selected_log = text("");

            if (data->log_entries.contains(log_selected)) {
                selected_log = renderLogDetail(data->log_entries.at(log_selected));
            }

            return vbox({
                text("runtime log") | bold,
                log_list->Render() | flex,
                separator(),
                selected_log | size(HEIGHT, EQUAL, 5),
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
    Runtime *runtime;
    Observer::Thread *data;

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

    std::string editor_content;

    Component nav_mode_menu;
    Component tree_list;
    Component stack_list;
    Component member_list;
    Component nav_content;
    Component trace_list;
    Component editor;

    Component left_renderer;
    Component right_renderer;
    Component root;
    Component renderer;

    Element renderMessage (AccessID id) {
        Message msg = runtime->readMessage(id);
        // below is a placeholder to be replaced
        return vbox({
                        text("selected message") | bold,
                        text("From: compiler"),
                        text("To: writer"),
                        text("Type: return"),
                        text(""),
                        paragraph("Compilation finished successfully. "
                                  "No diagnostics were produced."),
                    });
    }

    explicit ThreadTab(Runtime *runtime_, Observer::Thread *data_)
        : runtime(runtime_), data(data_)
    {
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

        editor = Input(&editor_content, "message");

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
                    separator(),
                    nav_mode_menu->Render(),
                    separator(),
                    nav_content->Render() | flex,
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
                    trace_selected < int(data->trace.size())) {
                    selected_message = renderMessage(data->trace.at(trace_selected).id);
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
        return "T" + std::to_string(data->id);
    }

    Component component() override {
        return renderer;
    }

    ThreadID threadId() const override {
        return data->id;
    }
};

class TUI: public UI, public Observer {
    GlobalData global;
    std::mutex pending_log_mutex;
    std::vector<spdlog::details::log_msg_buffer> pending_logs;

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
        CHECK(id < Observer::threads.size());

        for (size_t i = 0; i < tabs.size(); ++i) {
            if (tabs[i]->threadId() == id) {
                tab_index = int(i);
                return;
            }
        }

        addTab(std::make_unique<ThreadTab>(global.runtime, Observer::threads[id].get()));

        tab_index = int(tabs.size()) - 1;
        if (main_renderer) {
            screen.PostEvent(ftxui::Event::Custom);
        }
    }

    void syncObserver() {
        for (unsigned i = global.threads.size(); i < Observer::threads.size(); ++i) {
            Observer::Thread *th = Observer::threads[i].get();
            std::string name;
            if (th->name.empty()) {
                name = std::format("{}", th->id);
            }
            else {
                name = std::format("{}: {}", th->id, th->name);
            }
            global.threads.push_back(std::move(name));
        }
    }

protected:
    void on_exit () override {
        screen.Exit();
    }

    virtual std::vector<Message> on_message (Message &&msg) override {
        screen.PostEvent(ftxui::Event::Custom);
        return UI::on_message(std::move(msg));
    }

public:
    TUI() : screen(ScreenInteractive::Fullscreen()) {
        global.runtime = nullptr;
        global.onOpenThread = [this](int thread) {
            openThreadTab(thread);
        };

        // create tabs
        addTab(std::make_unique<GlobalTab>(&global));


        tab_selection = Menu(
            &tab_labels,
            &tab_index,
            MenuOption::Horizontal());

        tab_content = Container::Tab(
            tab_components,
            &tab_index);

        ButtonOption exit_option;
        exit_option.label = "Exit";
        exit_option.on_click = [this] {
            this->send(0, Message(json{{"To", "runtime"},
                         {"Subject", "exit"}}));
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
            drainLogs();
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

    void setRuntime (Runtime *rt) override {
        UI::setRuntime(rt);
        global.runtime = rt;
    }

    void appendLog (spdlog::details::log_msg const& msg) override {
        {
            std::lock_guard<std::mutex> lock(pending_log_mutex);
            pending_logs.emplace_back(msg);
        }
        screen.PostEvent(ftxui::Event::Custom);
    }

    std::function<void(Message&&)> consume () override {
        return [this](Message &&m) {
            Observer::consume(std::move(m));
            screen.PostEvent(ftxui::Event::Custom);
        };
    }

    void run() override {

        Loop loop(&screen, main_renderer);

        while (!loop.HasQuitted()) {
            Observer::process();
            syncObserver();
            screen.RequestAnimationFrame();

            loop.RunOnce();

            std::this_thread::sleep_for(
                std::chrono::milliseconds(1000 / 60));
        }
    }
};

std::unique_ptr<UI> make_tui () {
    return std::make_unique<TUI>();
}


}}
