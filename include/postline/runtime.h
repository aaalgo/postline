#pragma once
#include "program.h"
namespace postline {


class Runtime: public Program, public LinearService {

    std::function<void(Message &&)> consume;
    Journal journal;
    Poller poller;
    Accounting accounting;
    bool stop_requested;


    void regularizeAgentParams (json &m, Domain *domain);

    int cmd_create_agents (Message const &, json *resp);
    int cmd_create_domain (Message const &, json *resp);
    int cmd_create_snapshot (Message const &, json *resp);

    void updateMemory (Agent *, AccessID end);

public:
    struct Config {
        std::string journal_path;
        std::string resume_path;
        std::function<void(Message &&)> consume;
    };

    Runtime(Config const &config, Service *user_service)
        : consume(config.consume),
          journal(config.journal_path, config.resume_path,
                  [this](Message &&msg) { 
                      apply(msg);
                      if (consume) consume(std::move(msg));
                  }),
          stop_requested(false) {
        log::info("Initializing runtime");
        runtime->driver = std::make_unique<LoopDriver>(this);
        user->driver = std::make_unique<LoopDriver>(user_service);
    }

    ~Runtime() = default;

    int enqueueUser (Message && msg) {
        auto *driver = dynamic_cast<LoopDriver*>(user->driver.get());
        CHECK(driver);
        return driver->enqueue(std::move(msg));
    }

    void call (Message &&msg, Response &) override; // runtime as a service

    EntityRef syscall (json const &op);

    void syscalls (json const &ops) {
        for (json const &j: ops.get_ref<json::array_t const &>()) {
            syscall(j);
        }
    }

    void run ();

    Message readMessage (AccessID access_id) const {
        return journal.read(access_id);
    }
};
}
