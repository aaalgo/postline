#include <latch>
#include <spdlog/spdlog.h>
#include <spdlog/details/log_msg.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <postline/ui.h>

namespace postline { namespace ui {

class Sink final : public spdlog::sinks::base_sink<std::mutex> {
    UI* ui;
    std::vector<std::string> pending_lines;
public:
    Sink(): ui(nullptr) {
    }

    ~Sink () {
        for (auto const &line: pending_lines) {
            std::cerr << line << std::endl;
        }
    }

    void attachUI(ui::UI* ui_) {
        CHECK(!ui);
        CHECK(ui_);
        std::lock_guard<std::mutex> lock(mutex_);
        ui = ui_;
        for (auto& line : pending_lines) {
            ui->appendLog(std::move(line));
        }
        pending_lines.clear();
    }

    void detachUI() {
        std::lock_guard<std::mutex> lock(mutex_);
        ui = nullptr;
    }

protected:
    void sink_it_(spdlog::details::log_msg const& msg) override {
        spdlog::memory_buf_t buf;
        this->formatter_->format(msg, buf);

        std::string line = fmt::to_string(buf);

        if (ui) {
            ui->appendLog(std::move(line));
        } else {
            pending_lines.push_back(std::move(line));
        }
    }

    void flush_() override {}

};

static std::shared_ptr<Sink> sink;


void UI::initArena () {
    {
// TODO: don't do this when resume
        json h{{"To", "runtime"},
               {"Subject", "create_domain"},
               {"Thread-ID", "0"}
            };
        json op{{"detach", true}};
        send(Message(std::move(h),op.dump()));
    }
    {
        json h{{"To", "runtime"},
               {"Subject", "create_agents"},
               {"Thread-ID", "1"},
            };
        send(Message(std::move(h),std::string(LOCAL_AGENTS)));
    }
#if 0
    {
        json h{{"To", "runtime"},
               {"Subject", "create_domain"},
               {"Thread-ID", "0"}
            };
        json op{{"detach", true}};
        send(Message(std::move(h),op.dump()));
    }
    {
        json h{{"To", "runtime"},
               {"Subject", "create_agents"},
               {"Thread-ID", "2"},
            };
static char const *LOCAL_AGENTS_2 = R"(
[
{"from": "zero", "name": "echo", "service": "pipe:echo", "flags": []},
{"from": "zero", "name": "ai1", "comment": "openai", "service": "pipe:openai", "flags": ["history"]},
{"from": "zero", "name": "ai2", "comment": "anthropic", "service": "pipe:claude", "flags": ["history"]},
{"from": "zero", "name": "ai3", "comment": "openrouter", "service": "pipe:v1", "flags": ["history"]},
{"from": "zero", "name": "shell", "service": "pipe:shell", "flags": []},
{"from": "zero", "name": "mcp", "service": "pipe:mcp_bridge", "flags": []},
{"from": "zero", "name": "memory", "service": "pipe:echo -m", "flags": ["history"]},
{"from": "zero", "name": "login", "service": "pipe:login", "flags": ["clone"]},
{"from": "zero", "name": "benchmark", "service": "pipe:benchmark", "flags": []}
]
)";
        send(Message(std::move(h),std::string(LOCAL_AGENTS_2)));
    }
#endif
}

UI::~UI () {
    sink->detachUI();
}

class Null: public UI {
    // for testing,
    // exit upon start
    std::latch ready;
protected:
    void on_exit () override {
        ready.count_down();
    }
public:
    Null (Runtime *rt_): UI(rt_), ready(1) {
    }

    void run () override {
        send(Message(json{{"To", "runtime"},
                     {"Subject", "exit"},
                     {"Thread-ID", "0"}}));
        ready.wait();
    };
};


std::unique_ptr<UI> make_auto (Runtime *) {
    CHECK(0);
    return nullptr;
}

std::unique_ptr<UI> make_tui (Runtime *);
std::unique_ptr<UI> make_cli (Runtime *);
std::unique_ptr<UI> make_web (Runtime *);


std::unique_ptr<UI> make_ui (std::string const &name, Runtime *runtime) {
    std::unique_ptr<UI> ui;
    if (name == "null") ui = std::make_unique<Null>(runtime);
    else if (name == "cli") ui =  make_cli(runtime);
    else if (name == "tui") ui = make_tui(runtime);
    else if (name == "web") ui = make_web(runtime);
    else if (name == "auto") ui = make_auto(runtime);
    else CHECK(0, "UI {} not supported.", name);
    sink->attachUI(ui.get());
    return ui;
}

}

void init_logging () {
    ui::sink = std::make_shared<ui::Sink>();
    ui::sink->set_level(spdlog::level::info);
    ui::sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");

    auto file_sink =
        std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        "postline.log",
        5 * 1024 * 1024,
        3
    );
    file_sink->set_level(spdlog::level::debug);
    file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] %v");

    std::vector<spdlog::sink_ptr> sinks {ui::sink, file_sink};
    auto logger = std::make_shared<spdlog::logger>("postline", sinks.begin(), sinks.end());
    logger->set_level(spdlog::level::debug);  // overall level
    // Keep the file sink useful while the runtime is still running, and across
    // CHECK/abort paths that do not run atexit handlers.
    logger->flush_on(spdlog::level::debug);

    spdlog::set_default_logger(logger);
    std::atexit([]{
        spdlog::shutdown();
    });
}

}


