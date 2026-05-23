#pragma once
#include <postline/runtime.h>

namespace postline { namespace ui {

    class UI: public Service {
        Runtime *rt;
    protected:
        void send (Message &&msg) {
            rt->enqueueUser(std::move(msg));
        }

        virtual std::vector<Message> on_message (Message &&msg) override {
            return std::vector<Message>();
        }

        virtual void call (Message &&, Response &) override {
            CHECK(0);   // not used
        }

    public:
        UI (Runtime *rt_): rt(rt_) {}

        virtual ~UI ();

        void initArena ();

        virtual void appendLog (std::string &&) {
        }

        virtual void run () = 0;
    };

    std::unique_ptr<UI> make_ui (std::string const &name, Runtime *runtime);

}
}

