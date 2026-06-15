#pragma once
#include "program.h"
#include "accounting.h"
#include "driver.h"
#include "journal.h"
#include "poller.h"
#include "service.h"
namespace postline {


class Runtime: public Program, public LinearService {
    struct AgentState {
        bool dead = false;
        std::unique_ptr<Driver> driver;
        int obligation_count = 0;
    };

    std::vector<AgentState> agent_states;

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

    AgentState &agentState (Agent const *);
    AgentState const &agentState (Agent const *) const;
    void syncAgentStates ();
    json dumpAgent (Agent const *) const;

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
        syncAgentStates();
        agentState(runtime).driver = std::make_unique<LoopDriver>(this);
        agentState(user).driver = std::make_unique<LoopDriver>(user_service);
    }

    ~Runtime() = default;

    int enqueueUser (Message && msg) {
        auto *driver = dynamic_cast<LoopDriver*>(agentState(user).driver.get());
        CHECK(driver);
        return driver->enqueue(std::move(msg));
    }

    json dump () const;

    EntityRef apply (Message const &msg);

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
