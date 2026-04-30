#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

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

static inline Address parse_address(std::string const& address) {
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


struct GroupConfig {
    struct MemberConfig {
        Address from;
        std::string as;
        std::string service;
        bool clone;
    };
    std::string name;
    std::vector<MemberConfig> members;

    GroupConfig (Message const &msg) {
        json j(json::parse(msg.body()));
        // name
        CHECK(j.contains("name") && j["name"].is_string());
        name = j["name"].get<std::string>();

        // members
        CHECK(j.contains("members") && j["members"].is_array());
        auto const &arr = j["members"];
        CHECK(!arr.empty());

        members.reserve(arr.size());

        for (size_t i = 0; i < arr.size(); ++i) {
            auto const &m = arr[i];
            CHECK(m.is_object());

            MemberConfig mc;

            // from
            CHECK(m.contains("from") && m["from"].is_string());
            mc.from = parse_address(m["from"].get<std::string>());
            CHECK(!mc.from.host.empty());
            CHECK(!mc.from.domain.empty());
            // as
            CHECK(m.contains("as") && m["as"].is_string());
            mc.as = m["as"].get<std::string>();
            CHECK(!mc.as.empty());

            // service (optional)
            if (m.contains("service")) {
                CHECK(m["service"].is_string());
                mc.service = m["service"].get<std::string>();
            } else {
                mc.service.clear();
            }

            // clone (optional)
            if (m.contains("clone")) {
                CHECK(m["clone"].is_boolean());
                mc.clone = m["clone"].get<bool>();
            } else {
                mc.clone = true;
            }

            log::info("< {}@{} {} {} {}", mc.from.host, mc.from.domain, mc.as, mc.service, mc.clone);
            members.emplace_back(std::move(mc));
        }
    }
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
            :runtime(&agents.get(agents.spawn(NOT_AN_AGENT))),
            journal(&agents.get(agents.spawn(NOT_AN_AGENT))),
            user(&agents.get(agents.spawn(NOT_AN_AGENT))),
            root(&agents.get(agents.spawn(NOT_AN_AGENT)))
        {
            CHECK(runtime->id == 0);
            CHECK(journal->id == 1);
            //runtime->driver = std::make_unique<QueueDriver>();
            Group &group = groups.get(groups.create(ROOT_GROUP_NAME));
            group.hosts["runtime"] = runtime->id;
            group.hosts["user"] = user->id;
            group.hosts["agent"] = root->id;
        }
    }  special;

    Journal    journal;
    Poller poller;
    bool stop_requested;
    AccessID last_processed_id = NO_ACCESS_ID;

    void createGroup (GroupConfig const &conf) {
        CHECK(groups.find(conf.name) == NOT_A_GROUP);
        Group& group = groups.get(groups.create(conf.name));
        log::info("group names: {}", group.hosts.size());
        json cc = json::array();
        for (auto const &member: conf.members) {
            log::info("adding {} to {}", member.as, conf.name);
            //CHECK(group.hosts.find(member.as) == group.hosts.end());
            log::info("OK");
            AgentID from = resolve(member.from);
            log::info("{}", from);
            CHECK(from != NOT_AN_AGENT);
            AgentID id = from;
            if (member.clone) {
                id = agents.spawn(from);
                if (!member.service.empty()) {
                    Agent &agent = agents.get(from);
                    agent.service = member.service;
                    agent.address = std::format("{}@{}", member.as, conf.name);
                }
            }
            else {
                CHECK(member.service.empty(), "cannot relace service of existing agent");
            }
            group.hosts[member.as] = id;
            log::info("{}@{} = {}, {}", member.as, conf.name, id, member.service);
            cc.push_back(std::format("{}@{}", member.as, conf.name));
        }
        for (auto const &p: group.hosts) {
            log::info("{} => {}", p.first, p.second);
        }
        listAgents();

        json header{
            {"From", "runtime"},
            {"To", "user@home"},
            {"Cc", cc},
            {"Reply-To", "ai@local"},
            {"Subject", "hello"}
        };
        log::info("Sending message to user");
        enqueue(Message(std::move(header)));
    }

    void listAgents() {
            std::cout << "Listing agents:" << std::endl;
            for (std::size_t i = 0; i < agents.size(); ++i) {
                auto &agent = agents.get(i);
                log::info("{}: {} {}", i, agent.address, agent.service);
            }
    }

public:
    struct Config {
        std::string journal_path;
        std::string resume_path;
        std::string cli_output_path;   // the user driver will read this
        std::string cli_input_path;    // the user driver will write this
    };

    Runtime(Config const &config)
        : special(agents, groups),
        journal(config.journal_path,
                  config.resume_path,
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
    }

    ~Runtime() {
    }

    /*
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
    */

    void handle_runtime_message (Message &&msg) {

        std::string const &command = msg.subject();

        if (command == "exit") {
            stop_requested = true;
            log::info("Stop request received.");
            log::info("Runtime will shutdown.");
            // "exit" command doesn't reply
            return;
        }
        if (command == "list_agents") {
            listAgents();
            json respHeader{{"From", msg.to()},
                        {"To", msg.from()}};
            enqueue(Message(std::move(respHeader)));
            return;
        }
        else if (command == "create_group") {
            createGroup(GroupConfig(msg));
            return;
        }
        json respHeader{{"From", msg.to()},
                    {"To", msg.from()},
                    {"Subject", "Unknown command"}};
        enqueue(Message(std::move(respHeader)));
    }

    void process (Message &&msg, Agent *from) {
        if (from == special.journal) return;    // TODO FIX
        std::cout << "--------" << std::endl;
        msg.formatEmail(std::cout);
        std::cout << std::endl;

/*
        std::string const &from = msg.from();
        std::string const &to = msg.from();
        auto out = msg.cc();
        */
        
        // for each message
        // resolve
        // put save
        // for to and cc, send

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
            if (agent->service.empty()) {
                log::error("agent {} {} has empty service", agent_id, to);
                return;
            }
            log::info("Creating driver for agent {} {}: {}", agent_id, to, agent->service);
            agent->driver = create_driver(agent->service);
            CHECK(agent->driver);
            poller.add(agent->driver->read_fd(), agent->id);
        }
        // add to memory
        agent->driver->send(std::move(msg));
        agent->waiting_response = true;
    }

    void enqueue (Message &&msg) {
        special.runtime->driver->send(std::move(msg));
    }

    void run () {

        struct Todo: noncopyable {
            Message message;
            Agent *agent;
            Todo(Message&& m, Agent* a): message(std::move(m)), agent(a) {}
        };

        int trailing = 0;

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
            trailing = todo.size();
            for (auto &t: todo) {
                // all messages received
                last_processed_id = t.message.access_id();
                process(std::move(t.message), t.agent);
                --trailing;
                if (stop_requested) {
                    log::info("Stop requested, starting shutdown process.");
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
                std::vector<Message> tmp;
                agent->driver->recv(tmp);  // TODO: add to journal?
                agent->waiting_response = false;
                trailing += tmp.size();
                for (auto &msg: tmp) {
                    msg.set_access_id(journal.append(msg));
                }
            }
            if (agent->driver) {    // bye
                log::info("Stopping agent {} driver...", i);
                agent->driver.reset();
            }
        }
        log::info("{} messages unprocessed.", trailing);
        journal.append(protocol::runtime::Shutdown::make(last_processed_id));
        log::info("runtime shutdown.");
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
        if (addr.host == "runtime") return special.runtime->id;
        GroupID group_id = resolve_domain(addr.domain);
        CHECK(group_id != NOT_A_GROUP);

        Group const& group = groups.get(group_id);

        auto it = group.hosts.find(addr.host);
        if (it == group.hosts.end()) {
            log::error("Cannot resolve {}@{}", addr.host, addr.domain);
            return NOT_AN_AGENT;
        }
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

    AgentID resolve(std::string const& address) const {
        return resolve(parse_address(address));
    }
};

} // namespace postline
