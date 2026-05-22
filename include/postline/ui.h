#pragma once
#include <postline/runtime.h>

namespace postline { namespace ui {

    class UI: public Service {
        Runtime *rt;
    protected:
        void send (Message &&msg) {
            rt->enqueueUser(std::move(msg));
        }

    public:
        UI (Runtime *rt_): rt(rt_) {
        }

        virtual ~UI ();

        virtual void appendLog (std::string &&) {
        }

        virtual void run () = 0;
    };

    std::unique_ptr<UI> make_ui (std::string const &name, Runtime *runtime);

}
}

