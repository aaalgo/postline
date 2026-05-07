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
#include "service.h"
#include "logic.h"

namespace postline {


int constexpr LEVEL_FROM = 0;
int constexpr LEVEL_CC = 1;
int constexpr LEVEL_TO = 2;

class commit_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class resolve_error : public std::runtime_error {
public:
    template <typename... Args>
    resolve_error(std::format_string<Args...> fmt, Args&&... args)
        : std::runtime_error(std::format(fmt, std::forward<Args>(args)...)) {}
};

class Runtime: immobile, public Service {
    AgentStore agents;

    struct SpecialAgents {
        Agent *runtime;
        Agent *journal;
        Agent *user;
        Agent *root;

        static constexpr char const *ROOT_GROUP_NAME = "home";

        SpecialAgents (AgentStore &agents)
            : runtime(&agents.get(agents.spawn("runtime"))),
              journal(&agents.get(agents.spawn("[journal]"))),
              user(&agents.get(agents.spawn("user"))),
              root(&agents.get(agents.spawn("root"))) {
            CHECK(runtime->id == 0);
            CHECK(journal->id == 1);
            user->flags |= AGENT_FLAG_THREAD | AGENT_FLAG_CATCH;
        };
    }  special;

    Journal    journal;
    Poller poller;
    bool stop_requested;
    AccessID last_processed_id = NO_ACCESS_ID;
    Logic logic;

    json dump () const {
        json j{
            {"agents", agents.dump()},
        };
        return j;
    }


    void commit (json const &ops);

    void dump (std::string const &path) const;
    void spawn (Message const &msg);

    void call (Message &&msg, Response &) override;

    void replay (Message &&msg);
    void process (Message &&msg, Agent *from);

    void updateMemory (Agent *);

    AgentID resolve(std::string const& address) const;

    void enqueue (Message &&msg) {
        auto *driver = dynamic_cast<LoopDriver *>(special.runtime->driver.get());
        CHECK(driver);
        driver->enqueue(std::move(msg));
    }

    void resolve (Message const &msg, MessageContext *);

public:
    struct Config {
        std::string journal_path;
        std::string resume_path;
        std::string cli_output_path;   // the user driver will read this
        std::string cli_input_path;    // the user driver will write this
    };

    Runtime(Config const &config, std::unique_ptr<Driver> &&user_driver)
        : special(agents),
          journal(config.journal_path, config.resume_path,
                  [this](Message &&msg) { replay(std::move(msg));
                }),
          stop_requested(false) {
        log::info("Initializing runtime");
        special.runtime->driver = std::make_unique<LoopDriver>(this);
        special.user->driver = std::move(user_driver);
        poller.add(special.runtime->driver->read_fd(), special.runtime->id);
        poller.add(special.user->driver->read_fd(), special.user->id);
    }

    ~Runtime() = default;


    void run ();
};

} // namespace postline
