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


class commit_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/*
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
*/

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

    static std::string const &commit_get_string (json const &j, std::string const &key) {
        if (!j.contains(key)) throw commit_error(std::format("missing {}", key));
        if (!j[key].is_string()) throw commit_error(std::format("{} is not string", key));
        return j[key].get_ref<std::string const &>();
    }

    static bool commit_get_bool (json const &j, std::string const &key) {
        if (!j.contains(key)) throw commit_error(std::format("missing {}", key));
        if (!j[key].is_boolean()) throw commit_error(std::format("{} is not string", key));
        return j[key].get<bool>();
    }

    void createGroup (Message &&msg, std::vector<std::string> *addrs) {
        json ops = json::array();

        json j(json::parse(msg.body()));

        std::string const &group_name = commit_get_string(j, "name");
        if (groups.find(group_name) != NOT_A_GROUP) throw commit_error("group already exists");

        ops.push_back(json{{"op", "group_create"}, {"group", group_name}});

        if (!j.contains("members")) throw commit_error("missing members");
        auto const &arr = j["members"];
        if (!arr.is_array()) throw commit_error("members is not array");

        std::unordered_set<std::string> seen;
        for (size_t i = 0; i < arr.size(); ++i) {
            auto const &m = arr[i];
            if (!m.is_object()) throw commit_error("member is not object");
            std::string const &from = commit_get_string(m, "from");
            std::string const &as = commit_get_string(m, "as");
            std::string service;
            bool clone = true;
            if (m.contains("service")) {
                service = commit_get_string(m, "service");
            }
            if (m.contains("clone")) {
                clone = commit_get_bool(m, "clone");
            }

            if ((!clone) && (!service.empty())) throw commit_error("cannot rewrite service");

            AgentID from_id = resolve(from);
            if (from_id == NOT_AN_AGENT) throw commit_error(std::format("cannot resolve from {}", from));
            if (seen.find(as) != seen.end()) throw commit_error(std::format("{} appears twice", as));

            ops.push_back(json{{"op", "group_add"},
                               {"group", group_name},
                               {"from", from},
                               {"as", as},
                               {"service", service},
                               {"clone", clone}});
            addrs->emplace_back(std::format("{}@{}", as, group_name));
        }
        journal.append(protocol::runtime::Commit::make(ops));
        commit(ops);
    }

    void listAgents() {
            std::cout << "Listing agents:" << std::endl;
            for (std::size_t i = 0; i < agents.size(); ++i) {
                auto &agent = agents.get(i);
                log::info("{}: {} {}", i, agent.address, agent.service);
            }
    }

    void commit (json const &ops) {
        CHECK(ops.is_array());
        for (size_t i = 0; i < ops.size(); ++i) {
            auto const &m = ops[i];
            CHECK(m.is_object());
            std::string const &op = commit_get_string(m, "op");
            if (op == "group_create") {
                std::string const &group_name = commit_get_string(m, "group");
                groups.create(group_name);
                log::info("create group {}", group_name);
            }
            else if (op == "group_add") {
                std::string const &group_name = commit_get_string(m, "group");
                Group &group = groups.get(groups.find(group_name));
                std::string const &from = commit_get_string(m, "from");
                std::string const &as = commit_get_string(m, "as");
                std::string const &service = commit_get_string(m, "service");
                bool clone = commit_get_bool(m, "clone");
                AgentID from_id = resolve(from);
                AgentID id = from_id;
                if (clone) {
                    id = agents.spawn(from_id);
                    Agent &agent = agents.get(id);
                    agent.service = service;
                    agent.address = std::format("{}@{}", as, group_name);
                    log::info("create agent {}: {}", id, agent.address);
                }
                group.hosts[as] = id;
            }
            else if (op == "shutdown") {
                ;
            }
            else CHECK(0, "UNKNOWN OP");
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
                        if (msg.type().starts_with("agent:")) {
                            this->process(std::move(msg), special.journal);
                        }
                        else {
                            protocol::runtime::Commit c(msg);
                            commit(c.ops);
                        }
                  }),
        stop_requested(false)
    {
        log::info("Initializing runtime");
        special.runtime->driver = std::make_unique<LoopDriver>();
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

    void recv (Message &&msg) {

        json respHeader{{"From", msg.to()},
                    {"To", msg.from()},
                    {"Subject", "OK"},
                    };
        std::string respBody;
        bool reply = true;

        std::string const &command = msg.subject();

        if (command == "exit") {
            stop_requested = true;
            log::info("Stop request received.");
            log::info("Runtime will shutdown.");
            reply = false;
        }
        if (command == "list_agents") {
            listAgents();
        }
        else if (command == "create_group") {
            try {
                std::vector<std::string> members;
                createGroup(std::move(msg), &members);
                json cc = json::array();
                for (auto const &addr: members) {
                    cc.push_back(addr);
                }
                respHeader["Cc"] = cc;
            }
            catch (commit_error &e) {
                respHeader["Subjet"] = e.what();
            }
        }

        if (reply) {
            send(Message(std::move(respHeader), std::move(respBody)));
        }
    }

    void process (Message &&msg, Agent *from) {
        // if from == journal, then it is journal replay
        CHECK(msg.has_access_id());
        bool is_replay = (from == special.journal);

        std::cout << "--------" << std::endl;
        msg.formatEmail(std::cout);
        std::cout << std::endl;

        int constexpr LEVEL_FROM = 0;   // save, don't deliver
        int constexpr LEVEL_CC = 1;     // send, don't expect return
        int constexpr LEVEL_TO = 2;     // send, expect return
                                        // the numbers cannot change
        std::vector<std::pair<Agent *, int>> todo;

        {   // process from
            std::string const &addr  = msg.from();
            Agent *agent = &agents.get(resolve(addr));
            if (!is_replay) {   // if not replay, we cannot
                                // let agent add to other's mailbox
                //CHECK(agent == from); // TODO: handle the initial message from main
                                        // which is from user but handled via runtime
                ;
            }
            todo.emplace_back(agent, LEVEL_FROM);
        }
        {   // process to
            std::string const &addr  = msg.to();
            Agent *agent = &agents.get(resolve(addr));
            todo.emplace_back(agent, LEVEL_TO);
        }

        for (auto const &addr: msg.cc()) {
            Agent *agent = &agents.get(resolve(addr));
            todo.emplace_back(agent, LEVEL_CC);
        }

        for (auto [agent, level]: todo) {
            agent->memory.push_back(msg.access_id());   // save to memory
            if ((level >= LEVEL_CC) && !is_replay) {
                if (agent == special.runtime) {
                    recv(std::move(msg));
                }
                else {  // deliver
                    if (!agent->driver) {
                        if (agent->service.empty()) {
                            log::error("agent {} {} has empty service", agent->id, agent->address);
                            return;
                        }
                        log::info("Creating driver for agent {} {}: {}", agent->id, agent->address, agent->service);
                        agent->driver = create_driver(agent->service);
                        CHECK(agent->driver);
                        poller.add(agent->driver->read_fd(), agent->id);
                    }
                    // add to memory
                    agent->driver->send(std::move(msg));
                }
                if (level == LEVEL_TO) {
                    agent->waiting_response = true;
                }
            }
        }
    }

    void send (Message &&msg) {
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
        json ops = json::array();
        json op{{"op", "shutdown"},
                {"last_processed_id", last_processed_id}};
        ops.push_back(op);
        journal.append(protocol::runtime::Commit::make(ops));
        log::info("runtime shutdown.");
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
