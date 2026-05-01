#include <postline/runtime.h>

#include <format>
#include <fstream>
#include <iostream>
#include <unordered_set>
#include <CLI/CLI.hpp>

namespace postline {

std::string const &commit_get_string(json const &j, std::string const &key) {
    if (!j.contains(key)) {
        throw commit_error(std::format("missing {}", key));
    }
    if (!j[key].is_string()) {
        throw commit_error(std::format("{} is not string", key));
    }
    return j[key].get_ref<std::string const &>();
}

bool commit_get_bool(json const &j, std::string const &key) {
    if (!j.contains(key)) {
        throw commit_error(std::format("missing {}", key));
    }
    if (!j[key].is_boolean()) {
        throw commit_error(std::format("{} is not bool", key));
    }
    return j[key].get<bool>();
}


void Runtime::dump (std::string const &path) const {
    std::ofstream ofs(path);
    ofs << dump().dump(4);
}

void Runtime::createGroup(Message &&msg, std::vector<std::string> *addrs) {
    json ops = json::array();
    json j(json::parse(msg.body()));

    std::string const &group_name = commit_get_string(j, "name");
    if (groups.find(group_name) != NOT_A_GROUP) {
        throw commit_error("group already exists");
    }

    ops.push_back(json{{"op", "group_create"}, {"group", group_name}});

    if (!j.contains("members")) {
        throw commit_error("missing members");
    }
    auto const &arr = j["members"];
    if (!arr.is_array()) {
        throw commit_error("members is not array");
    }

    std::unordered_set<std::string> seen;
    for (size_t i = 0; i < arr.size(); ++i) {
        auto const &m = arr[i];
        if (!m.is_object()) {
            throw commit_error("member is not object");
        }

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
        if ((!clone) && (!service.empty())) {
            throw commit_error("cannot rewrite service");
        }

        AgentID from_id = resolve(from);
        if (from_id == NOT_AN_AGENT) {
            throw commit_error(std::format("cannot resolve from {}", from));
        }
        if (seen.find(as) != seen.end()) {
            throw commit_error(std::format("{} appears twice", as));
        }
        seen.insert(as);

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

void Runtime::listAgents() {
    std::cout << "Listing agents:" << std::endl;
    for (std::size_t i = 0; i < agents.size(); ++i) {
        auto &agent = agents.get(i);
        log::info("{}: {} {}", i, agent.address, agent.service);
    }
}

void Runtime::commit(json const &ops) {
    CHECK(ops.is_array());
    for (size_t i = 0; i < ops.size(); ++i) {
        auto const &m = ops[i];
        CHECK(m.is_object());

        std::string const &op = commit_get_string(m, "op");
        if (op == "group_create") {
            std::string const &group_name = commit_get_string(m, "group");
            groups.create(group_name);
            log::info("create group {}", group_name);
        } else if (op == "group_add") {
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
        } else if (op == "shutdown") {
        } else {
            CHECK(0, "UNKNOWN OP");
        }
    }
}


void Runtime::recv(Message &&msg) {

    json respHeader{{"From", msg.to()},
                    {"To", msg.from()},
                    {"Subject", "OK"}};
    std::string respBody;

    bool reply = true;

    do {

        CLI::App app{"Postline Runtime Message API"};

        app.require_subcommand(1);
        app.allow_extras(false);

        auto cmd_exit   = app.add_subcommand("exit");
        auto cmd_list_agents   = app.add_subcommand("list_agents");
        auto cmd_create_group = app.add_subcommand("create_group");
        auto cmd_dump = app.add_subcommand("dump");
        std::string dump_path;
        cmd_dump->add_option("path", dump_path)->required();

        try {
            app.parse(msg.subject(), false);
        } catch (CLI::ParseError const& e) {
            respHeader["Subject"] = std::string("Error: ") + e.what();
            break;
        }

        if (*cmd_exit) {
            stop_requested = true;
            log::info("Stop request received.");
            log::info("Runtime will shutdown.");
            reply = false;
            break;
        }
        if (*cmd_list_agents) {
            listAgents();
            break;
        }
        if (*cmd_create_group) {
            try {
                std::vector<std::string> members;
                createGroup(std::move(msg), &members);
                json cc = json::array();
                for (auto const &addr : members) {
                    cc.push_back(addr);
                }
                respHeader["Cc"] = cc;
            } catch (commit_error &e) {
                respHeader["Subject"] = e.what();
            }
            break;
        }
        if (*cmd_dump) {
            dump(dump_path);
            break;
        }
    } while (0);

    if (reply) {
        log::info("Replying...");
        enqueue(Message(std::move(respHeader), std::move(respBody)));
    }
}

void Runtime::process(Message &&msg, Agent *from) {
    CHECK(msg.has_access_id());
    bool is_replay = (from == special.journal);

    std::cout << "--------" << std::endl;
    msg.formatEmail(std::cout);
    std::cout << std::endl;

    int constexpr LEVEL_FROM = 0;
    int constexpr LEVEL_CC = 1;
    int constexpr LEVEL_TO = 2;
    std::vector<std::pair<Agent *, int>> todo;

    {
        std::string const &addr = msg.from();
        Agent *agent = &agents.get(resolve(addr));
        if (!is_replay) {
        }
        todo.emplace_back(agent, LEVEL_FROM);
    }
    {
        std::string const &addr = msg.to();
        Agent *agent = &agents.get(resolve(addr));
        todo.emplace_back(agent, LEVEL_TO);
    }

    for (auto const &addr : msg.cc()) {
        Agent *agent = &agents.get(resolve(addr));
        todo.emplace_back(agent, LEVEL_CC);
    }

    for (auto [agent, level] : todo) {
        agent->memory.push_back(msg.access_id());
        if ((level >= LEVEL_TO) && !is_replay) {
            if (agent == special.runtime) {
                recv(std::move(msg));
            } else {
                if (!agent->driver) {
                    if (agent->service.empty()) {
                        log::error("agent {} {} has empty service", agent->id, agent->address);
                        return;
                    }
                    log::info("Creating driver for agent {} {}: {}",
                              agent->id,
                              agent->address,
                              agent->service);
                    agent->driver = create_driver(agent->service);
                    CHECK(agent->driver);
                    poller.add(agent->driver->read_fd(), agent->id);
                }
                agent->driver->send(std::move(msg));
                if (level == LEVEL_TO) {
                    agent->waiting_response = true;
                }
            }
        }
    }
}

void Runtime::run() {
    struct Todo : noncopyable {
        Message message;
        Agent *agent;

        Todo(Message &&m, Agent *a) : message(std::move(m)), agent(a) {
        }
    };

    int trailing = 0;

    for (;;) {
        auto events = poller.wait();
        CHECK(!events.empty());

        std::vector<Todo> todo;
        for (auto const &e : events) {
            Agent *agent = &agents.get(e.token);
            CHECK(agent->driver);

            std::vector<Message> tmp;
            int err = agent->driver->recv(tmp);
            CHECK(err == 0);
            agent->waiting_response = false;

            for (auto &msg : tmp) {
                msg.set_access_id(journal.append(msg));
                todo.emplace_back(std::move(msg), agent);
            }
        }

        trailing = todo.size();
        for (auto &t : todo) {
            last_processed_id = t.message.access_id();
            process(std::move(t.message), t.agent);
            --trailing;
            if (stop_requested) {
                log::info("Stop requested, starting shutdown process.");
                break;
            }
        }
        if (stop_requested) {
            break;
        }
    }

    for (std::size_t i = 0; i < agents.size(); ++i) {
        Agent *agent = &agents.get(i);
        if (agent->waiting_response) {
            CHECK(agent->driver);
            log::info("Waiting for agent {} to respond...", i);

            std::vector<Message> tmp;
            agent->driver->recv(tmp);
            agent->waiting_response = false;
            trailing += tmp.size();
            for (auto &msg : tmp) {
                msg.set_access_id(journal.append(msg));
            }
        }
        if (agent->driver) {
            log::info("Stopping agent {} driver...", i);
            agent->driver.reset();
        }
    }

    log::info("{} messages unprocessed.", trailing);
    json ops = json::array();
    json op{{"op", "shutdown"}, {"last_processed_id", last_processed_id}};
    ops.push_back(op);
    journal.append(protocol::runtime::Commit::make(ops));
    log::info("runtime shutdown.");
}

AgentID Runtime::resolve(Address const &addr) const {
    if (addr.host == "runtime") {
        return special.runtime->id;
    }

    GroupID group_id = resolve_domain(addr.domain);
    CHECK(group_id != NOT_A_GROUP);

    Group const &group = groups.get(group_id);
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

GroupID Runtime::resolve_domain(std::string const &domain) const {
    if (domain.starts_with("g.")) {
        return parse_group_id(domain);
    }
    return groups.find(domain);
}

GroupID Runtime::parse_group_id(std::string const &domain) {
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

AgentID Runtime::resolve(std::string const &address) const {
    return resolve(parse_address(address));
}

Address parse_address(std::string const& address) {
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



}  // namespace postline
