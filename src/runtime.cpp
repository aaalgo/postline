#include <postline/runtime.h>

#include <format>
#include <fstream>
#include <iostream>
#include <unordered_set>
#include <CLI/CLI.hpp>

namespace postline {

class CommitError : public Error {
public:
    using Error::Error;
};


std::string const &commit_get_string(json const &j, std::string const &key) {
    if (!j.contains(key)) {
        throw CommitError("missing {}", key);
    }
    if (!j[key].is_string()) {
        throw CommitError("{} is not string", key);
    }
    return j[key].get_ref<std::string const &>();
}

bool commit_get_bool(json const &j, std::string const &key) {
    if (!j.contains(key)) {
        throw CommitError("missing {}", key);
    }
    if (!j[key].is_boolean()) {
        throw CommitError("{} is not bool", key);
    }
    return j[key].get<bool>();
}

int64_t commit_get_int(json const &j, std::string const &key) {
    if (!j.contains(key)) {
        throw CommitError("missing {}", key);
    }
    if (!j[key].is_number_integer()) {
        throw CommitError("{} is not bool", key);
    }
    return j[key].get<int64_t>();
}

json dump_agent_flags (AgentFlags flags) {
    json flag_strings = json::array();
#define X(flag, value) if (flags & AGENT_FLAG_##flag) { flag_strings.push_back(#flag); }
    AGENT_FLAG_LIST(X)
#undef X
    return flag_strings;
}

json AgentParams::dump () const {
    return json{
        {"link", {{"parent", link.parent},
                  {"anchor", link.anchor}
                  }},
        {"name", name},
        {"comment", comment},
        {"service", service},
        {"flags", dump_agent_flags(flags)},
    };
    return json();
}

json Agent::dump () const {
    return json{
        {"link", {{"parent", link.parent},
                  {"anchor", link.anchor}
                  }},
        {"name", name},
        {"comment", comment},
        {"service", service},
        {"flags", dump_agent_flags(flags)},
        {"id", id},
        {"domain_id", domain->id},
        {"dead", dead},
        {"oblication_count", obligation_count},
        {"memory", memory}
    };
    return json();
}


json Snapshot::dump () const {
    json jmembers = json::array();
    for (auto const &member: members) {
        jmembers.push_back(member.dump());
    }

    return json{
        {"name", name},
        {"entry", {{"from", entry.from},
                   {"to", entry.to}
                   }},
        {"members", jmembers}
    };
}

json Domain::dump () const {
    json jmembers = json::object();
    for (auto const &[name, agent]: members) {
        CHECK(agent);
        jmembers[name] = agent->id;
    }

    json jchildren = json::object();
    for (auto const &[name, domain]: children) {
        CHECK(domain);
        jchildren[name] = domain->id;
    }

    return json{
        {"id", id},
        {"name", name},
        {"parent_id", parent ? json(parent->id) : json(nullptr)},
        {"thread_id", thread ? json(thread->id) : json(nullptr)},
        {"entry", {{"from", entry.from ? json(entry.from->id) : json(nullptr)},
                   {"to", entry.to ? json(entry.to->id) : json(nullptr)}
                   }},
        {"members", jmembers},
        {"children", jchildren}
    };
}

json Program::dump () const {
    json jdomains = json::array();
    for (auto const &domain: domains) {
        CHECK(domain);
        jdomains.push_back(domain->dump());
    }

    json jagents = json::array();
    for (auto const &agent: agents) {
        CHECK(agent);
        jagents.push_back(agent->dump());
    }

    json jsnapshots = json::object();
    for (auto const &[name, snapshot]: snapshots) {
        jsnapshots[name] = snapshot.dump();
    }

    json jthreads = json::array();
    for (auto const &thread: threads) {
        CHECK(thread);
        jthreads.push_back(thread->dump());
    }

    return json{
        {"domains", jdomains},
        {"agents", jagents},
        {"snapshots", jsnapshots},
        {"threads", jthreads},
    };
}

#if 0
void Runtime::dump (std::string const &path) const {
    std::ofstream ofs(path);
    //ofs << dump().dump(4);
}

void Runtime::spawn(Message const &msg) {
    json ops = json::array();
    json arr(json::parse(msg.body()));

    if (!arr.is_array()) {
        throw CommitError("agent list is not array");
    }

    for (size_t i = 0; i < arr.size(); ++i) {
        auto const &m = arr[i];
        if (!m.is_object()) {
            throw CommitError("member is not object");
        }

        std::string const &from = commit_get_string(m, "from");
        std::string const &address = commit_get_string(m, "address");
        std::string comment;
        std::string service;
        AgentFlags flags = 0;

        if (m.contains("flags")) {
            json fs = m["flags"];
            if (!fs.is_array()) throw CommitError("flags is not array");
            for (auto const &f: fs) {
                if (!f.is_string()) throw CommitError("unknown flag, is not string");
                std::string const &s = f.get_ref<std::string const&>();
                if (s == "clone") {
                    flags |= AGENT_FLAG_CLONE;
                }
                else if (s == "thread") {
                    flags |= AGENT_FLAG_THREAD;
                }
                else if (s == "catch") {
                    flags |= AGENT_FLAG_CATCH;
                }
                else if (s == "history") {
                    flags |= AGENT_FLAG_HISTORY;
                }
                else {
                    throw CommitError("unknown flag");
                }
            }
        }


        if (address.find('_') != address.npos) {
            throw CommitError(std::format("address cannot contain _: {}", address));
        }

        if (m.contains("comment")) {
            comment = commit_get_string(m, "comment");
        }

        if (m.contains("service")) {
            service = commit_get_string(m, "service");
        }

        AgentID from_id = resolve(from);
        if (from_id == NOT_AN_AGENT) {
            throw CommitError(std::format("cannot resolve from {}", from));
        }
        else {
            Agent const &p = agents.get(from_id);
            if ((p.flags & AGENT_FLAG_CLONE) && (flags & AGENT_FLAG_CLONE)) {
                throw CommitError(std::format("cannot double clone"));
            }
        }

        AgentID new_id = resolve(address);
        if (new_id != NOT_AN_AGENT) {
            throw CommitError(std::format("{} already used", address));
        }

        ops.push_back(json{{"op", "spawn"},
                           {"address", address},
                           {"comment", comment},
                           {"from", from},
                           {"service", service},
                           {"flags", flags},
                           {"is_clone", false},
                           });
    }

    Message entry = protocol::runtime::Commit::make(ops);
    journal.append(entry);
    commit(ops);
}
#endif

void Runtime::commit(json const &ops) {
#if 0
    CHECK(ops.is_array());
    for (size_t i = 0; i < ops.size(); ++i) {
        auto const &m = ops[i];
        CHECK(m.is_object());
        std::string const &op = m["op"].get_ref<std::string const &>();

        if (op == "spawn") {
            std::string const &address = commit_get_string(m, "address");
            std::string const &comment = commit_get_string(m, "comment");
            std::string const &from = commit_get_string(m, "from");
            std::string const &service = commit_get_string(m, "service");
            AgentFlags flags = commit_get_int(m, "flags");
            bool is_clone = commit_get_bool(m, "is_clone");
            AgentID from_id = resolve(from);
            CHECK(from_id != NOT_AN_AGENT);
            Agent &parent = agents.get(from_id);
            if (is_clone) {
                CHECK(parent.flags & AGENT_FLAG_CLONE);
                CHECK(!(flags & AGENT_FLAG_CLONE));
                std::string suffix = std::format("_{}", parent.next_clone_id);
                CHECK(address.ends_with(suffix));
                ++parent.next_clone_id;
            }
            AgentID id = agents.spawn(address, comment, from_id, NO_ACCESS_ID, service, flags);
            Agent &agent = agents.get(id);
            log::info("create agent {}: {}", id, agent.address);
        } else if (op == "shutdown") {
        } else {
            CHECK(0, "UNKNOWN OP");
        }
    }
#endif
}

void Runtime::call (Message &&msg, Response &resp) {

    json respHeader;
    std::string respBody;

    do {

        CLI::App app{"Postline Runtime Message API"};

        app.require_subcommand(1);
        app.allow_extras(false);

        auto cmd_exit   = app.add_subcommand("exit");
        auto cmd_list_agents   = app.add_subcommand("list_agents");
        /*
        auto cmd_spawn = app.add_subcommand("spawn");
        auto cmd_dump = app.add_subcommand("dump");
        auto cmd_account = app.add_subcommand("account");
        std::string dump_path;
        cmd_dump->add_option("path", dump_path)->required();
        */

        try {
            app.parse(msg.subject(), false);
        } catch (CLI::ParseError const& e) {
            respHeader["Subject"] = std::string("Error: ") + e.what();
            break;
        }

        if (*cmd_exit) {
            stop_requested = true;
            respHeader["Subject"] = "Re: exit";
            log::info("Stop request received.");
            log::info("Runtime will shutdown.");
            break;
        }
        if (*cmd_list_agents) {
            respHeader["Subject"] = "Re: list_agents";
            json j = json::array();
            for (auto const &a: program.agents) {
                j.push_back(a->dump());
            }
            respBody = j.dump();
            break;
        }
#if 0
        if (*cmd_spawn) {
            try {
                spawn(msg);
                respHeader["Subject"] = "Re: spawn";
                respBody = agents.dump().dump();
            } catch (CommitError &e) {
                log::info("COMMIT ERROR: {}", e.what());
                respHeader["Subject"] = e.what();
            }
            break;
        }
        if (*cmd_dump) {
            respHeader["Subject"] = "Re: exit";
            dump(dump_path);
            break;
        }
        if (*cmd_account) {
            respHeader["Subject"] = "Re: account";
            respBody = accounting.dump().dump();
            break;
        }
#endif
    } while (0);

    resp.append(Message(std::move(respHeader), std::move(respBody)));
}

void Runtime::updateMemory (Agent *agent) {
    if (agent->flags & AGENT_FLAG_HISTORY == 0) return;
    std::vector<AgentLink> links;
    links.emplace_back(agent->id, agent->anchor());
    Agent *cur = agent;
    while (cur) {
        if (cur->link.parent == NOT_AN_AGENT) {
            cur = nullptr;
        }
        else {
            links.emplace_back(cur->link);
            cur = program.agents[cur->link.parent].get();
        }
    }
    
    agent->driver->send(protocol::handshake::BeginMemory::make());
    for (auto it = links.rbegin(); it != links.rend(); ++it) {
        auto link = *it;
        cur = program.agents[link.parent].get();
        for (AccessID id: cur->memory) {
            if (id > link.anchor) break;
            Message msg = journal.read(id);
            //std::string const &type = msg.type();
            agent->driver->send(msg);
        }
    }
    agent->driver->send(protocol::handshake::EndMemory::make());
}

void Runtime::run() {

    while (!stop_requested) {
        auto events = poller.wait();
        CHECK(!events.empty());

        std::vector<Message> todo;
        for (auto const &e : events) {
            CHECK(e.token >= 0 && e.token < program.agents.size());
            Agent *agent = program.agents[e.token].get();
            CHECK(agent->driver);

            std::vector<Message> tmp;
            try {
                int err = agent->driver->recv(tmp);
                for (auto &msg : tmp) {
                    program.preprocess(agent, msg);
                    todo.emplace_back(std::move(msg));
                }
            }
            catch (eof_error const &) {
                // driver has crashed
                agent->dead = true;
                // TODO: record agent died
                agent->driver.reset();
                // construct response messages to waiting parties
                todo.emplace_back(program.makeRewindMessage(agent));
            }
        }

        for (auto &msg : todo) {
            int64_t flags = msg.flags();
            if ((flags & MESSAGE_QUIET) == 0) {
                std::cerr << "PROCESS " << flags << std::endl;
                std::cerr << "====" << std::endl;
                msg.formatEmail(std::cerr);
                std::cerr << std::endl;
            }
            journal.append(msg);
            accounting.update(msg);
            Agent *agent = program.apply(msg);
            if (!agent->driver) {
                if (agent->dead) {
                    log::error("Agent is in error status.");
                }
                if (agent->service.empty()) {
                    log::error("agent {} {} has empty service", agent->id, agent->name);
                    return;
                }
                log::info("Creating driver for agent {} {}: {}",
                          agent->id,
                          agent->name,
                          agent->service);
                agent->driver = create_driver(agent->service);
                CHECK(agent->driver);
                updateMemory(agent);
                poller.add(agent->driver->read_fd(), agent->id);
            }
            agent->driver->send(msg);
        }
    }

    {
        json ops = json::array();
        json op{{"op", "shutdown-request"}};
        ops.push_back(op);
        Message msg = protocol::runtime::Commit::make(ops);
        journal.append(msg);
    }

    size_t trailing = 0;
    for (std::size_t i = 0; i < program.agents.size(); ++i) {
        Agent *agent = program.agents[i].get();
        while (agent->obligation_count > 0) {
            CHECK(agent->driver);
            log::info("Waiting for agent {} (oc: {}) to respond...", i, agent->obligation_count);

            std::vector<Message> tmp;
            agent->driver->recv(tmp);
            for (auto &msg: tmp) {
                --agent->obligation_count;
                journal.append(msg);
            }
            trailing += tmp.size();
        }
        if (agent->driver) {
            log::info("Stopping agent {} driver...", i);
            agent->driver->shutdown();
            agent->driver.reset();
        }
    }

    log::info("{} messages unprocessed.", trailing);
    {
        json ops = json::array();
        json op{{"op", "shutdown"}};
        ops.push_back(op);
        Message msg = protocol::runtime::Commit::make(ops);
        journal.append(msg);
        log::info("runtime shutdown.");
    }
}

}  // namespace postline
