#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <stdexcept>

#include "common.h"
#include "driver.h"
#include "agent.h"
#include "journal.h"
#include "poller.h"
#include "session.h"

namespace postline {

class commit_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Runtime: immobile {
    AgentStore agents;

    struct SpecialAgents {
        Agent *runtime;
        Agent *journal;
        Agent *boot;
        Agent *user;
        Agent *root;

        static constexpr char const *ROOT_GROUP_NAME = "home";

        SpecialAgents (AgentStore &agents)
            : runtime(&agents.get(agents.spawn("runtime", NOT_AN_AGENT))),
              journal(&agents.get(agents.spawn("[journal]", NOT_AN_AGENT))),
              boot(&agents.get(agents.spawn("[boot]", NOT_AN_AGENT))),
              user(&agents.get(agents.spawn("user", NOT_AN_AGENT))),
              root(&agents.get(agents.spawn("root", NOT_AN_AGENT))) {
            CHECK(runtime->id == 0);
            CHECK(journal->id == 1);
            boot->permissions |= PERMISSION_SESSION;
            user->permissions |= PERMISSION_SESSION;
        };
    }  special;

    Journal    journal;
    Poller poller;
    bool stop_requested;
    AccessID last_processed_id = NO_ACCESS_ID;
    std::vector<std::unique_ptr<Session>> sessions;


    json dump () const {
        json j{
            {"agents", agents.dump()},
        };
        return j;
    }

    void dump (std::string const &path) const;

    void commit (json const &ops);

    void spawn (Message const &msg);
    void listAgents();
    int recv (Message const &msg);
    void process (Message &&msg, Agent *from);

    void updateMemory (Agent *);


    AgentID resolve(std::string const& address) const;

public:
    struct Config {
        std::string journal_path;
        std::string resume_path;
        std::string cli_output_path;   // the user driver will read this
        std::string cli_input_path;    // the user driver will write this
    };

    Runtime(Config const &config)
        : special(agents),
          journal(config.journal_path, config.resume_path, [this](Message &&msg) {
              if (msg.type() == "runtime:commit") {
                  protocol::runtime::Commit c(msg);
                  commit(c.ops);
              } else {
                  process(std::move(msg), special.journal);
              }
          }),
          stop_requested(false) {
        log::info("Initializing runtime");
        special.runtime->driver = std::make_unique<LoopDriver>(
                [this](Message const &msg) {
                    return recv(msg);
                });
        special.boot->driver = std::make_unique<LoopDriver>(
                [this](Message const &msg) {
                    return 0;
                });
        special.user->driver = std::make_unique<ShellDriver>(config.cli_input_path,
                                                             config.cli_output_path);
        poller.add(special.runtime->driver->read_fd(), special.runtime->id);
        poller.add(special.boot->driver->read_fd(), special.boot->id);
        poller.add(special.user->driver->read_fd(), special.user->id);
    }

    ~Runtime() = default;

    void enqueue (Message &&msg) {
        auto *driver = dynamic_cast<LoopDriver *>(special.runtime->driver.get());
        CHECK(driver);
        driver->enqueue(std::move(msg));
    }

    void enqueue_boot (Message &&msg) {
        auto *driver = dynamic_cast<LoopDriver *>(special.boot->driver.get());
        CHECK(driver);
        driver->enqueue(std::move(msg));
    }

    void run ();
};

} // namespace postline
