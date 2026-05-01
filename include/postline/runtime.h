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

namespace postline {

struct Address {
    std::string host;
    std::string domain;
};

Address parse_address(std::string const& address);

class commit_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Runtime: immobile {
    AgentStore agents;
    GroupStore groups;

    struct SpecialAgents {
        Agent *runtime;
        Agent *journal;
        Agent *user;
        Agent *root;

        static constexpr char const *ROOT_GROUP_NAME = "home";

        SpecialAgents (AgentStore &agents, GroupStore &groups)
            : runtime(&agents.get(agents.spawn(NOT_AN_AGENT))),
              journal(&agents.get(agents.spawn(NOT_AN_AGENT))),
              user(&agents.get(agents.spawn(NOT_AN_AGENT))),
              root(&agents.get(agents.spawn(NOT_AN_AGENT))) {
            runtime->address = "runtime@home";
            user->address = "user@home";
            root->address = "agent@home";
            CHECK(runtime->id == 0);
            CHECK(journal->id == 1);

            Group &group = groups.get(groups.create(ROOT_GROUP_NAME));
            group.hosts["runtime"] = runtime->id;
            group.hosts["user"] = user->id;
            group.hosts["agent"] = root->id;
        };
    }  special;

    Journal    journal;
    Poller poller;
    bool stop_requested;
    AccessID last_processed_id = NO_ACCESS_ID;


    json dump () const {
        json j{
            {"agents", agents.dump()},
            {"groups", groups.dump()},
        };
        return j;
    }

    void dump (std::string const &path) const;

    void commit (json const &ops);

    void createGroup (Message &&msg, std::vector<std::string> *addrs);
    void listAgents();
    void recv (Message &&msg);
    void process (Message &&msg, Agent *from);


    AgentID resolve(Address const& addr) const;
    GroupID resolve_domain(std::string const& domain) const;
    static GroupID parse_group_id(std::string const& domain);
    AgentID resolve(std::string const& address) const;

public:
    struct Config {
        std::string journal_path;
        std::string resume_path;
        std::string cli_output_path;   // the user driver will read this
        std::string cli_input_path;    // the user driver will write this
    };

    Runtime(Config const &config)
        : special(agents, groups),
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
        special.runtime->driver = std::make_unique<LoopDriver>();
        special.user->driver = std::make_unique<ShellDriver>(config.cli_input_path,
                                                             config.cli_output_path);
        poller.add(special.runtime->driver->read_fd(), special.runtime->id);
        poller.add(special.user->driver->read_fd(), special.user->id);
    }

    ~Runtime() = default;

    void enqueue (Message &&msg) {
        auto *driver = dynamic_cast<LoopDriver *>(special.runtime->driver.get());
        CHECK(driver);
        driver->enqueue(std::move(msg));
    }

    void run ();
};

} // namespace postline
