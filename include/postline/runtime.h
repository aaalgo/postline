#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common.h"
#include "agent.h"
#include "journal.h"
#include "driver.h"
#include "poller.h"

namespace postline {


struct Address {
    std::string host;
    std::string domain;
};

class Runtime: immobile {
    AgentStore agents;

    struct SpecialAgents {
        Agent *runtime;
        Agent *journal;
        Agent *root;
        SpecialAgents (AgentStore &agents)
            :runtime(&agents.get(agents.spawn(NOT_AN_AGENT))),
            journal(&agents.get(agents.spawn(NOT_AN_AGENT))),
            root(&agents.get(agents.spawn(NOT_AN_AGENT)))
        {
            CHECK(runtime->id == 0);
            CHECK(journal->id == 1);
            runtime->driver = std::make_unique<QueueDriver>();
        }
    }  special;

    GroupStore groups;
    Journal    journal;
    bool stop_requested;

public:
    struct Config {
        std::string journal_path;
        std::string journal_chain_path;
    };

    Runtime(Config const &config)
        : special(agents),
        journal(config.journal_path,
                  config.journal_chain_path,
                  [this](Message &&msg) {
                        this->process(msg, special.journal);
                  }),
        stop_requested(false)
    {
    }

    ~Runtime() {
    }

    void stop () {
        special.runtime->driver->send(protocol::agent::Bye::make());
    }

    void enqueue (Message &&msg) {
        special.runtime->driver->send(std::move(msg));
    }

    void handle_runtime_message (Message const &msg) {
        if (msg.header()["type"].get_ref<std::string const &>() == "agent:bye") {
            stop_requested = true;
        }
    }

    void process (Message const &msg, Agent *from) {
        //std::cout << "======== replay: " <<  replay << std::endl;
        if (from == special.runtime) {
            log::info("processing runtime message");
            handle_runtime_message(msg);
        }
        else if (from == special.journal) {
            // don't process
        }
        else {  // common messages
            json const &header = msg.header();
            if (!header.contains("To")) CHECK(0, "No header: To");
            std::string to = header["To"].get<std::string>();
#if 0           // process logic
                step 1. expand to_address to canonical form
                step 2. resolve the address
                step 3. send the message to agent
#endif
        }
    }

    void run () {
        Poller poller;
        poller.add(special.runtime->driver->read_fd(), special.runtime->id);

        struct Todo: noncopyable {
            Message message;
            Agent *agent;
            Todo(Message&& m, Agent* a): message(std::move(m)), agent(a) {}
        };

        for (;;) {
            auto events = poller.wait();
            CHECK(!events.empty());
            std::vector<Todo> todo;
            for (auto const &e: events) {
                Agent *agent = &agents.get(e.token);
                CHECK(agent->driver);
                std::vector<Message> tmp = agent->driver->recv();
                for (auto &msg: tmp) {
                    msg.set_access_id(journal.append(msg));
                    todo.emplace_back(std::move(msg), agent);
                }
            }
            for (auto &t: todo) {
                // all messages received
                process(t.message, t.agent);
                if (stop_requested) {
                    log::info("Stop requested, ignoring {} pending messages.", todo.size());
                    break;
                }
            }
            if (stop_requested) break;
        }
        // gracefully shutdown all pending
        log::info("Make sure you come back to close drivers.");
    }

    AgentID spawn_agent(AgentID parent, AccessID anchor = NO_ACCESS_ID) {
        return agents.spawn(parent, anchor);
    }

    GroupID create_group(std::string name = "") {
        return groups.create(std::move(name));
    }

    void bind(GroupID group_id, std::string host, AgentID agent_id) {
        CHECK(agents.exists(agent_id));

        Group& group = groups.get(group_id);
        auto [it, inserted] = group.hosts.emplace(std::move(host), agent_id);
        CHECK(inserted);
    }

    AgentID resolve(Address const& addr) const {
        GroupID group_id = resolve_domain(addr.domain);
        CHECK(group_id != NOT_A_GROUP);

        Group const& group = groups.get(group_id);

        auto it = group.hosts.find(addr.host);
        CHECK(it != group.hosts.end());

        AgentID agent_id = it->second;
        CHECK(agents.exists(agent_id));

        return agent_id;
    }

    GroupID resolve_domain(std::string const& domain) const {
        if (domain.starts_with("g.")) {
            return parse_group_id(domain);
        }

        return groups.find(domain);
    }

    static GroupID parse_group_id(std::string const& domain) {
        CHECK(domain.starts_with("g."));

        std::string_view s(domain);
        s.remove_prefix(2);

        CHECK(!s.empty());

        GroupID id = 0;
        for (char c : s) {
            CHECK(c >= '0' && c <= '9');
            id = id * 10 + static_cast<GroupID>(c - '0');
        }

        return id;
    }

    static Address parse_address(std::string const& address) {
        auto pos = address.find('@');
        CHECK(pos != std::string::npos);
        CHECK(pos > 0);
        CHECK(pos + 1 < address.size());

        return Address{
            .host = address.substr(0, pos),
            .domain = address.substr(pos + 1),
        };
    }

    AgentID resolve(std::string const& address) const {
        return resolve(parse_address(address));
    }
};

} // namespace postline
