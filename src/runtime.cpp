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

json Agent::dump () const {
    json flag_strings = json::array();
#define X(flag, value) if (flags & AGENT_FLAG_##flag) { flag_strings.push_back(#flag); }
    AGENT_FLAG_LIST(X)
#undef X
    return json{
        {"link", {{"parent", link.parent},
                  {"anchor", link.anchor}
                  }},
        {"name", name},
        {"comment", comment},
        {"service", service},
        {"flags", flag_strings},
        {"id", id},
        {"domain_id", domain->id},
        {"dead", dead},
        {"oblication_count", obligation_count},
        {"memory", memory}
    };
    return json();
}

json Snapshot::dump () const {
    // TODO
    return json();
}

json Domain::dump () const {
    // TODO
    return json();
}

json Program::dump () const {
}

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

void Runtime::commit(json const &ops) {
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
        auto cmd_spawn = app.add_subcommand("spawn");
        auto cmd_dump = app.add_subcommand("dump");
        auto cmd_account = app.add_subcommand("account");
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
            respHeader["Subject"] = "Re: exit";
            log::info("Stop request received.");
            log::info("Runtime will shutdown.");
            break;
        }
        if (*cmd_list_agents) {
            respHeader["Subject"] = "Re: list_agents";
            respBody = agents.dump().dump();
            break;
        }
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
            cur = &agents.get(cur->link.parent);
        }
    }
    
    agent->driver->send(protocol::handshake::BeginMemory::make());
    for (auto it = links.rbegin(); it != links.rend(); ++it) {
        auto link = *it;
        cur = &agents.get(link.parent);
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
    struct Todo : noncopyable {
        Message message;
        Agent *agent;

        Todo(Message &&m, Agent *a) : message(std::move(m)), agent(a) {
        }
    };

    int trailing = 0;

    // send a message to user

    while (!stop_requested) {
        auto events = poller.wait();
        CHECK(!events.empty());

        std::vector<Message> todo;
        for (auto const &e : events) {
            CHECK(e.token >= 0 && e.token < agents.size());
            Agent *agent = agents[e.token].get();
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

        trailing = todo.size();
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
#if 0
                if (ctx.thread) {
                    std::cerr << "Stack" << std::endl;
                    for (auto const &e: ctx.thread->stack) {
                        std::cerr << std::format("{} -> {}: {}",
                                e.access_id, e.agent_id, e.agent_address) << std::endl;
                    }
                }
                CHECK(ctx.thread);
                while (!ctx.thread->stack.empty()) {
                    auto &e = ctx.thread->stack.back();
                    Agent *notify = &agents.get(e.reply_to_id);
                    if (notify->flags & AGENT_FLAG_CATCH == 0) {
                        ctx.thread->stack.pop_back();
                        continue;
                    }
                    // we'll notify this one
                    json respHeader{{"From", "runtime"},
                                    {"To", notify->address},
                                    {"Subject", "error"},
                                    {"In-Reply-To", e.access_id},
                                    {"Thread-ID", ctx.thread->id}
                                    };
                    std::string respBody;
                    enqueue(Message(std::move(respHeader), std::move(respBody)));
                    // TODO
                    // construct the body with stack dump
                    return;
                }
#endif
            if (!agent->driver) {
                if (agent->dead) {
                    log::error("Agent is in error status.");
                }
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
                updateMemory(agent);
                poller.add(agent->driver->read_fd(), agent->id);
            }
            agent->driver->send(msg);
        }
    }

    json ops = json::array();
    json op{{"op", "shutdown-request"}};
    ops.push_back(op);
    Message msg = protocol::runtime::Commit::make(ops);
    journal.append(msg);

    for (std::size_t i = 0; i < agents.size(); ++i) {
        Agent *agent = &agents.get(i);
        while (agent->obligation_count > 0) {
            CHECK(agent->driver);
            log::info("Waiting for agent {} (oc: {}) to respond...", i, agent->obligation_count);

            std::vector<Message> tmp;
            agent->driver->recv(tmp);
            for (auto &msg: tmp) {
                --agent->obligation_count;
            }
            journal.append(msg);
            trailing += tmp.size();
        }
        if (agent->driver) {
            log::info("Stopping agent {} driver...", i);
            agent->driver->shutdown();
            agent->driver.reset();
        }
    }

    log::info("{} messages unprocessed.", trailing);
    json ops = json::array();
    json op{{"op", "shutdown"}};
    ops.push_back(op);
    Message msg = protocol::runtime::Commit::make(ops);
    journal.append(msg);
    log::info("runtime shutdown.");
}

}  // namespace postline
