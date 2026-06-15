#include <iostream>
#include <latch>
#include <spdlog/spdlog.h>
#include <spdlog/details/log_msg.h>
#include <spdlog/details/log_msg_buffer.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <postline/ui.h>

namespace postline { namespace ui {

class Sink final : public spdlog::sinks::base_sink<std::mutex> {
    UI* ui;
    std::vector<spdlog::details::log_msg_buffer> pending_logs;

    std::string format (spdlog::details::log_msg const& msg) {
        spdlog::memory_buf_t buf;
        this->formatter_->format(msg, buf);
        return fmt::to_string(buf);
    }
public:
    Sink(): ui(nullptr) {
    }

    ~Sink () {
        for (auto const &msg: pending_logs) {
            std::cerr << format(msg) << std::endl;
        }
    }

    void attachUI(ui::UI* ui_) {
        CHECK(!ui);
        CHECK(ui_);
        std::lock_guard<std::mutex> lock(mutex_);
        ui = ui_;
        for (auto const& msg : pending_logs) {
            ui->appendLog(msg);
        }
        pending_logs.clear();
    }

    void detachUI() {
        std::lock_guard<std::mutex> lock(mutex_);
        ui = nullptr;
    }

protected:
    void sink_it_(spdlog::details::log_msg const& msg) override {
        if (ui) {
            ui->appendLog(msg);
        } else {
            pending_logs.emplace_back(msg);
        }
    }

    void flush_() override {}

};

static std::shared_ptr<Sink> sink;


void UI::initArena (json const &spec) {
    CHECK(spec.is_array());

    for (auto msg_spec: spec) {
        CHECK(msg_spec.is_object());
        CHECK(msg_spec.contains("Thread-ID"));
        CHECK(msg_spec["Thread-ID"].is_string());
        CHECK(msg_spec.contains("body"));

        ThreadID thread_id = std::stoi(msg_spec["Thread-ID"].get<std::string>());
        std::string body = msg_spec["body"].dump();
        msg_spec.erase("Thread-ID");
        msg_spec.erase("body");

        syscall(thread_id, Message(std::move(msg_spec), std::move(body)));
    }
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
    Null (): ready(1) {
    }

    void run () override {
        send(0, Message(json{{"To", "runtime"},
                     {"Subject", "exit"}}));
        ready.wait();
    };
};


std::unique_ptr<UI> make_auto () {
    CHECK(0);
    return nullptr;
}

std::unique_ptr<UI> make_tui ();
std::unique_ptr<UI> make_cli ();
std::unique_ptr<UI> make_web ();


std::unique_ptr<UI> make_ui (std::string const &name) {
    std::unique_ptr<UI> ui;
    if (name == "null") ui = std::make_unique<Null>();
    else if (name == "cli") ui =  make_cli();
    else if (name == "tui") ui = make_tui();
    else if (name == "web") ui = make_web();
    else if (name == "auto") ui = make_auto();
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
}

}
