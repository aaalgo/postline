#include <chrono>
#include <ctime>
#include <format>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <spdlog/details/log_msg_buffer.h>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/component_options.hpp>
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

struct ThreadData {
    ThreadID id;
    std::string name;
};

struct Tab {
    virtual ~Tab() = default;

    virtual std::string label() const = 0;
    virtual Component component() = 0;
    virtual ThreadID threadId() const { return NOT_A_THREAD; }
};


struct GlobalData {
    Runtime *runtime;
    ListData<spdlog::details::log_msg_buffer> log_entries;
    ListData<ThreadSummary> thread_summaries;

    std::function<void(ThreadSummary const &)> onOpenThread;

    GlobalData()
        : log_entries(MAX_VISIBLE_LOGS,
              [](spdlog::details::log_msg const &msg) {
                  return renderLogEntry(msg);
              }),
          thread_summaries(MAX_VISIBLE_THREADS,
              [](ThreadSummary const &summary) {
                  Element element;

                  if (summary.name.empty()) {
                      element = text(std::format("{}", summary.id));
                  }
                  else {
                      element = text(std::format("{}: {}", summary.id, summary.name));
                  }

                  return element;
              }) {}

    void syncThreads () {
        runtime->updateThreadSummaries(&thread_summaries);
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
        data->syncThreads();

        ListOption thread_list_option;
        thread_list_option.on_open_selection = [this] {
            if (!data->thread_summaries.contains(thread_selected)) {
                return;
            }
            if (data->onOpenThread) {
                data->onOpenThread(data->thread_summaries.at(thread_selected));
            }
        };
        thread_list = List(&data->thread_summaries,
                           &thread_selected,
                           &thread_follow_tail,
                           std::move(thread_list_option));
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

    ThreadID threadId() const override {
        return thread->id;
    }
};

class TUI: public UI {
    GlobalData global;
    std::mutex pending_log_mutex;
    std::vector<spdlog::details::log_msg_buffer> pending_logs;

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
        return UI::on_message(std::move(msg));
    }

public:
    TUI(Runtime *runtime)
        : UI(runtime),
        screen(ScreenInteractive::Fullscreen()) {
        global.runtime = runtime;
        global.onOpenThread = [this](ThreadSummary const &summary) {
            openThreadTab(summary);
        };

        // create tabs
        tabs.push_back(
            std::make_unique<GlobalTab>(&global));

        openThreadTab(ThreadSummary{.id = 0, .name = ""});

        rebuild_tabs();

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

    void appendLog (spdlog::details::log_msg const& msg) override {
        {
            std::lock_guard<std::mutex> lock(pending_log_mutex);
            pending_logs.emplace_back(msg);
        }
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

    void openThreadTab(ThreadSummary const &summary) {
        for (size_t i = 0; i < tabs.size(); ++i) {
            if (tabs[i]->threadId() == summary.id) {
                tab_index = int(i);
                return;
            }
        }

        auto thread = std::make_unique<ThreadData>();
        thread->id = summary.id;
        thread->name = summary.name;

        ThreadData *thread_ptr = thread.get();
        threads.push_back(std::move(thread));
        tabs.push_back(std::make_unique<ThreadTab>(thread_ptr));

        tab_index = int(tabs.size()) - 1;
        rebuild_tabs();
    }

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
