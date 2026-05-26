#include <httplib.h>
#include <postline/ui.h>

#include <postline/observer.h>

namespace postline { namespace ui {

static constexpr int POSTLINE_PORT = 7799;

class WebUI: public UI, public Observer {
    httplib::Server server;

protected:
    void on_exit() override {
        server.stop();
    }

public:
    std::function<void(Message&&)> consume() override {
        return [this](Message &&m) {
            Observer::consume(std::move(m));
        };
    }

    void run() override {
        server.Get("/api/program/dump/", [this](httplib::Request const&,
                                                httplib::Response &res) {
            Observer::process();
            res.set_content(Observer::dump().dump(), "application/json");
        });

        server.Get("/api/exit/", [this](httplib::Request const&,
                                        httplib::Response &res) {
            send(0, Message(json{{"To", "runtime"},
                         {"Subject", "exit"}}));
            res.set_content(json{{"ok", true}}.dump(), "application/json");
        });

        CHECK(server.listen("0.0.0.0", POSTLINE_PORT));
    }
};

std::unique_ptr<UI> make_web () {
    return std::make_unique<WebUI>();
}

}};
