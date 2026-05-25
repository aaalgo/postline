#pragma once

#include <deque>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <postline/common.h>
#include <postline/runtime.h>

#include "../ftx_list.hpp"
#include "limits.h"

namespace postline { namespace ui {

class ThreadTab;

class Observer {
    std::mutex mutex;
    std::deque<Message> pendings;

    void commit(json const &m);
    void apply(Message &&msg);
    friend class ThreadTab;

protected:
    Observer();

    void consume(Message &&m);
    void process();

    struct MessageHeader {
        AccessID id;
        ThreadID thread_id;
        std::string from;
        std::string to;
        std::string subject;

        MessageHeader(Message const &msg);
    };

public:
    struct Thread {
        ThreadID id;
        std::string name;
        bool pending;
        ftxui::ListData<MessageHeader> trace;

        Thread();
    };

    std::vector<std::unique_ptr<Thread>> threads;
};

}}
