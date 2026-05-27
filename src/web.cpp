#include <httplib.h>
#include <postline/ui.h>

#include <postline/observer.h>

namespace postline { namespace ui {

std::string web_listen_host = "0.0.0.0";
int web_listen_port = 6677;

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

        log::info("Web UI listening on http://{}:{}/", web_listen_host, web_listen_port);
        CHECK(server.listen(web_listen_host, web_listen_port));
    }
};

std::unique_ptr<UI> make_web () {
    return std::make_unique<WebUI>();
}

}};
