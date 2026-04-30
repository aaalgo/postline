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
        Agent *user;
        Agent *root;
        SpecialAgents (AgentStore &agents)
            :runtime(&agents.get(agents.spawn(NOT_AN_AGENT))),
            journal(&agents.get(agents.spawn(NOT_AN_AGENT))),
            user(&agents.get(agents.spawn(NOT_AN_AGENT))),
            root(&agents.get(agents.spawn(NOT_AN_AGENT)))
        {
            CHECK(runtime->id == 0);
            CHECK(journal->id == 1);
            //runtime->driver = std::make_unique<QueueDriver>();
        }
    }  special;

    GroupStore groups;
    Journal    journal;
    Poller poller;
    bool stop_requested;

public:
    struct Config {
        std::string journal_path;
        std::string journal_chain_path;
        std::string cli_output_path;   // the user driver will read this
        std::string cli_input_path;    // the user driver will write this
    };

    Runtime(Config const &config)
        : special(agents),
        journal(config.journal_path,
                  config.journal_chain_path,
                  [this](Message &&msg) {
                        this->process(std::move(msg), special.journal);
                  }),
        stop_requested(false)
    {
        log::info("Initializing runtime");
        special.runtime->driver = std::make_unique<QueueDriver>();
        special.user->driver = std::make_unique<ShellDriver>(config.cli_input_path,
                                                     config.cli_output_path);
        poller.add(special.runtime->driver->read_fd(), special.runtime->id);
        poller.add(special.user->driver->read_fd(), special.user->id);
        log::info("Default setup");
        defaultSetup();
    }

    void defaultSetup () {
        // default setup involves
        //  - a default group
        //  - an agent "ai" inherited from root
        Group& default_group = groups.get(groups.create("local"));

        Agent& echo = agents.get(agents.spawn(special.root->id));
        echo.driver_name = "echo";
        default_group.hosts["echo"] = echo.id;

        Agent& ai = agents.get(agents.spawn(special.root->id));
        ai.driver_name = "openai";
        default_group.hosts["ai"] = ai.id;

        Agent& shell = agents.get(agents.spawn(special.root->id));
        shell.driver_name = "shell";
        default_group.hosts["shell"] = shell.id;

        Agent& mcp = agents.get(agents.spawn(special.root->id));
        mcp.driver_name = "mcp_bridge";
        default_group.hosts["mcp"] = mcp.id;

        default_group.hosts["user"] = special.user->id;
        //ai.driver_name = "openai";
        // to trigger off the conversation
        // send a message from runtime to user
        // with a ReplyTo set to ai@local
        json header{
            {"From", "runtime"},
            {"To", "user@local"},
            {"Cc", json::array({"shell@local", "echo@local", "mcp@local"})},
            {"Reply-To", "ai@local"},
            {"Subject", "hello"}
        };
        log::info("Sending message to user");
        process(Message(std::move(header)), special.runtime);
    }

    ~Runtime() {
    }

    void handle_runtime_message (Message &&msg) {

        json const &header = msg.header();
        if (header.contains("type") && header["type"].is_string() && header["type"].get_ref<std::string const &>() == "agent:bye") {
            stop_requested = true;
            log::info("Stop request received.");
            log::info("Runtime will shutdown.");
            return;
        }
        std::string subject;
        if (header.contains("Subject") && header["Subject"].is_string()) {
            subject = header["Subject"].get_ref<std::string const &>();
        }
        if (subject == "bye") {
            stop_requested = true;
            log::info("Stop request received.");
            log::info("Runtime will shutdown.");
            return;
        }
        if (subject == "list_agents") {
            std::cout << "Listing agents:" << std::endl;
            for (std::size_t i = 0; i < agents.size(); ++i) {
                std::cout << "[" << i << "]" << std::endl;
            }
        }
        json respHeader{{"From", header["To"]},
                    {"To", header["From"]},
                    {"Subject:", "done"}};
        special.runtime->driver->send(Message(std::move(respHeader)));
    }

    void process (Message &&msg, Agent *from) {
        std::cout << "--------" << std::endl;
        msg.formatEmail(std::cout);
        std::cout << std::endl;

        json const &header = msg.header();
        if ((!header.contains("To")) || header["To"].is_null()) {
            log::error("header doesn't contain To");
            return;
        }
        std::string to = header["To"].get<std::string>();
        Address addr = parse_address(to);
        if (addr.host == "runtime") {
            // no matter what is the group,
            // this is to runtime
            handle_runtime_message(std::move(msg));
            return;
        }

        GroupID group_id = resolve_domain(addr.domain);
        if (group_id == NOT_A_GROUP) {
            log::error("fail to resolve group of {}", to);
            return;
        }
        Group &group = groups.get(group_id);
        auto it = group.hosts.find(addr.host);
        if (it == group.hosts.end()) {
            log::error("found group, but fail to find agent {}", to);
            return;
        }
        AgentID agent_id = it->second;
        Agent *agent = &agents.get(agent_id);
        if (!agent->driver) {
            if (agent->driver_name.empty()) {
                log::error("agent {} {} has empty driver_name", agent_id, to);
                return;
            }
            fs::path cmd = POSTLINE_HOME / "bin" / "drivers" / agent->driver_name;
            log::info("Creating driver for agent {} {}: {}", agent_id, to, agent->driver_name);
            agent->driver = std::make_unique<ShellDriver>(
                    std::format("{} 2> agent-{}.log", cmd.string(), agent->id));
            CHECK(agent->driver);
            poller.add(agent->driver->read_fd(), agent->id);
        }
        // add to memory
        agent->driver->send(std::move(msg));
        agent->waiting_response = true;
    }

    void run () {

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
                std::vector<Message> tmp;
                int err = agent->driver->recv(tmp);
                CHECK(err == 0);
                agent->waiting_response = false;
                for (auto &msg: tmp) {
                    msg.set_access_id(journal.append(msg));
                    todo.emplace_back(std::move(msg), agent);
                }
            }
            for (auto &t: todo) {
                // all messages received
                process(std::move(t.message), t.agent);
                if (stop_requested) {
                    log::info("Stop requested, ignoring {} pending messages.", todo.size());
                    break;
                }
            }
            if (stop_requested) break;
        }
        // gracefully shutdown all pending
        for (std::size_t i = 0; i < agents.size(); ++i) {
            Agent *agent = &agents.get(i);
            if (agent->waiting_response) {
                CHECK(agent->driver);
                log::info("Waiting for agent {} to respond...", i);
                std::vector<Message> msg;
                agent->driver->recv(msg);  // TODO: add to journal?
                agent->waiting_response = false;
            }
            if (agent->driver) {    // bye
                log::info("Stopping agent {} driver...", i);
                agent->driver.reset();
            }
        }
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
        if (pos == std::string::npos) {
            return Address{
                .host = address.substr(0, pos)
            };
        }
        else {
            CHECK(pos > 0);
            CHECK(pos + 1 < address.size());
            return Address{
                .host = address.substr(0, pos),
                .domain = address.substr(pos + 1),
            };
        }
    }

    AgentID resolve(std::string const& address) const {
        return resolve(parse_address(address));
    }
};

} // namespace postline
