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

void Runtime::spawn(Message const &msg) {
    json ops = json::array();
    json arr(json::parse(msg.body()));

    if (!arr.is_array()) {
        throw commit_error("agent list is not array");
    }

    for (size_t i = 0; i < arr.size(); ++i) {
        auto const &m = arr[i];
        if (!m.is_object()) {
            throw commit_error("member is not object");
        }

        std::string const &from = commit_get_string(m, "from");
        std::string const &address = commit_get_string(m, "address");
        std::string service;
        bool clone = false;

        if (address.find('_') != address.npos) {
            throw commit_error(std::format("address cannot contain _: {}", address));
        }

        if (m.contains("service")) {
            service = commit_get_string(m, "service");
        }

        if (m.contains("clone")) {
            clone = commit_get_bool(m, "clone");
        }


        AgentID from_id = resolve(from);
        if (from_id == NOT_AN_AGENT) {
            throw commit_error(std::format("cannot resolve from {}", from));
        }
        else {
            Agent const &p = agents.get(from_id);
            if (p.clone && clone) {
                throw commit_error(std::format("cannot double clone"));
            }
        }

        AgentID new_id = resolve(address);
        if (new_id != NOT_AN_AGENT) {
            throw commit_error(std::format("{} already used", address));
        }


        ops.push_back(json{{"op", "spawn"},
                           {"address", address},
                           {"from", from},
                           {"service", service},
                           {"clone", clone},
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
            std::string const &from = commit_get_string(m, "from");
            std::string const &service = commit_get_string(m, "service");
            bool clone = commit_get_bool(m, "clone");
            bool is_clone = commit_get_bool(m, "is_clone");
            AgentID from_id = resolve(from);
            CHECK(from_id != NOT_AN_AGENT);
            Agent &parent = agents.get(from_id);
            if (is_clone) {
                CHECK(parent.clone);
                CHECK(!clone);
                std::string suffix = std::format("_{}", parent.next_clone_id);
                CHECK(address.ends_with(suffix));
                ++parent.next_clone_id;
            }
            AgentID id = agents.spawn(address, from_id, NO_ACCESS_ID, service, clone);
            Agent &agent = agents.get(id);
            log::info("create agent {}: {}", id, agent.address);
        } else if (op == "shutdown") {
        } else {
            CHECK(0, "UNKNOWN OP");
        }
    }
}


int Runtime::recv(Message const &msg) {

    json respHeader{{"From", msg.to()},
                    {"To", msg.replyTo()},
                    {"Subject", "OK"},
                    {"Session-ID", msg.header()["Session-ID"]},
                    {"In-Reply-To", msg.header()["Message-ID"]}
                    };
    std::string respBody;

    bool reply = true;
    if (msg.to().empty()) reply = false;    // INVESTIGATE WHY IT DOESN'T WORK
    if (resolve(msg.to()) == NOT_AN_AGENT) reply = false;

    do {

        CLI::App app{"Postline Runtime Message API"};

        app.require_subcommand(1);
        app.allow_extras(false);

        auto cmd_exit   = app.add_subcommand("exit");
        auto cmd_list_agents   = app.add_subcommand("list_agents");
        auto cmd_spawn = app.add_subcommand("spawn");
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
            --special.runtime->obligation_count;
            break;
        }
        if (*cmd_list_agents) {
            respHeader["type"] = "ui:update_agents";
            respBody = agents.dump().dump();
            break;
        }
        if (*cmd_spawn) {
            try {
                spawn(msg);
            } catch (commit_error &e) {
                log::info("COMMIT ERROR: {}", e.what());
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
    return 0;
}

void Runtime::process(Message &&msg, Agent *from) {
    CHECK(msg.has_access_id());
    bool is_replay = (from == special.journal);

    std::cout << "--------" << std::endl;
    msg.formatEmail(std::cout);
    std::cout << std::endl;

    // handle session
    SessionID session_id = msg.session_id();
    if (session_id == NOT_A_SESSION) {
        // allocate session if not set
        if ((from->permissions & PERMISSION_SESSION) == 0) {
            CHECK(0, "agent not permitted to create session");
        }
        session_id = sessions.size();
        msg.updateHeader([session_id](json &header) {header["Session-ID"] = std::format("{}", session_id);});
        sessions.push_back(std::make_unique<Session>());
    }
    CHECK(session_id != NOT_A_SESSION);
    CHECK(session_id >= 0 && session_id < sessions.size());
    Session &session = *sessions[session_id].get();

    if (!is_replay) {   // handle session top
        session.trace.push_back(msg.access_id());

        AccessID in_reply_to = msg.in_reply_to();
        AccessID in_response_to = msg.in_response_to();
        if (in_reply_to != NO_ACCESS_ID) {
            CHECK(session.stack.size());
            auto const &e = session.stack.back();
            CHECK(e.agent_id == from->id);
            CHECK(e.access_id == in_reply_to);
            session.stack.pop_back();
            --from->obligation_count;
        }
        else {
            std::string const &addr = msg.to();
            AgentID to_id = resolve(addr);
            CHECK(to_id != NOT_AN_AGENT);
            if (in_response_to != NO_ACCESS_ID) {
                CHECK(session.stack.size());
                auto const &e = session.stack.back();
                CHECK(e.agent_id == from->id);
                CHECK(e.access_id == in_response_to);
                --from->obligation_count;
            }
            else {
                Agent *to_agent = &agents.get(to_id);
                CHECK(session.stack.empty());
                ++to_agent->obligation_count;
            }
            session.stack.emplace_back();
            session.stack.back().access_id = msg.access_id();
            session.stack.back().agent_id = to_id;
            // we need correct error handling
        }
        std::cout << "<<< stack" << std::endl;
        std::cout << session.dump() << std::endl;
    }

    int constexpr LEVEL_FROM = 0;
    int constexpr LEVEL_CC = 1;
    int constexpr LEVEL_TO = 2;
    std::vector<std::pair<Agent *, int>> todo;
    std::unordered_set<std::string> seen;

    {
        std::string const &addr = msg.from();
        if (!addr.empty()) {
            seen.insert(addr);
            AgentID id = resolve(addr);
            if (id != NOT_AN_AGENT) {
                Agent *agent = &agents.get(id);
                todo.emplace_back(agent, LEVEL_FROM);
            }
        }
    }
    {
        std::string const &addr = msg.to();
        if ((!addr.empty()) && (!seen.contains(addr))) {
            AgentID id = resolve(addr);
            if (id != NOT_AN_AGENT) {
                seen.insert(addr);
                Agent *agent = &agents.get(id);
                todo.emplace_back(agent, LEVEL_TO);
            }
        }
    }

    for (auto const &addr : msg.cc()) {
        if ((!addr.empty()) && (!seen.contains(addr))) {
            AgentID id = resolve(addr);
            if (id != NOT_AN_AGENT) {
                seen.insert(addr);
                Agent *agent = &agents.get(id);
                todo.emplace_back(agent, LEVEL_CC);
            }
        }
    }

    for (auto [agent, level] : todo) {
        if ((level >= LEVEL_TO) && !is_replay) {
            if (agent == special.runtime) {
                recv(std::move(msg));
            } else {
                if (agent->clone) {
                    // clone agent
                    std::string address = std::format("{}_{}", agent->address, agent->next_clone_id);
                    log::info("cloning {} to {}", agent->address, address);
                    json ops = json::array();
                    ops.push_back(json{{"op", "spawn"},
                                       {"address", address},
                                       {"from", agent->address},
                                       {"service", agent->service},
                                       {"clone", false},
                                       {"is_clone", true}
                                       });
                    Message entry = protocol::runtime::Commit::make(ops);
                    journal.append(entry);
                    commit(ops);
                    AgentID clone_id = resolve(address);
                    CHECK(clone_id != NOT_AN_AGENT);
                    agent = &agents.get(clone_id);
                }
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

                    updateMemory(agent);

                    poller.add(agent->driver->read_fd(), agent->id);
                }
                agent->driver->send(msg);
            }
        }
        agent->memory.push_back(msg.access_id());
    }
}

void Runtime::updateMemory (Agent *agent) {
    if (agent->driver->history_mode() == DriverHistoryMode::NONE) return;
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
            for (auto &msg : tmp) {
                std::string const &from = msg.from();
                if (from != agent->address) {
                    msg.updateHeader([agent](json &header){
                        header["Original-From"] = header["From"];
                        header["From"] = agent->address;
                    });
                }
                journal.append(msg);
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
        while (agent->obligation_count > 0) {
            CHECK(agent->driver);
            log::info("Waiting for agent {} (oc: {}) to respond...", i, agent->obligation_count);

            std::vector<Message> tmp;
            agent->driver->recv(tmp);
            for (auto &msg: tmp) {
                --agent->obligation_count;
            }
            trailing += tmp.size();
            for (auto &msg : tmp) {
                journal.append(msg);
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
    Message msg = protocol::runtime::Commit::make(ops);
    journal.append(msg);
    log::info("runtime shutdown.");
}

AgentID Runtime::resolve(std::string const &address) const {
    if (address.empty()) return NOT_AN_AGENT;
    return agents.find(address);
}

}  // namespace postline
