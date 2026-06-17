#include <string.h>
#include <format>
#include <fstream>
#include <iostream>
#include <unordered_set>
#include <CLI/CLI.hpp>
#include <postline/runtime.h>

namespace postline {

Runtime::AgentState &Runtime::agentState (Agent const *agent) {
    CHECK(agent);
    CHECK(agent->id >= 0);
    CHECK(static_cast<std::size_t>(agent->id) < agent_states.size());
    return agent_states[agent->id];
}

Runtime::AgentState const &Runtime::agentState (Agent const *agent) const {
    CHECK(agent);
    CHECK(agent->id >= 0);
    CHECK(static_cast<std::size_t>(agent->id) < agent_states.size());
    return agent_states[agent->id];
}

void Runtime::syncAgentStates () {
    agent_states.resize(agents.size());
}

json Runtime::dumpAgent (Agent const *agent) const {
    json ret = agent->dump();
    auto const &state = agentState(agent);
    ret["driver"] = state.driver
        ? json{{"read_fd", state.driver->read_fd()}}
        : json(nullptr);
    return ret;
}

json Runtime::dump () const {
    json ret = Program::dump();
    json &jagents = ret.at("agents");
    CHECK(jagents.size() == agents.size());
    for (std::size_t i = 0; i < agents.size(); ++i) {
        jagents[i] = dumpAgent(agents[i].get());
    }
    return ret;
}

EntityRef Runtime::apply (Message const &msg) {
    EntityRef ret = Program::apply(msg);
    syncAgentStates();
    return ret;
}

void Runtime::regularizeAgentParams (json &m, Domain *domain) {
    if (m.contains("from")) {
        ResolvedAddress from = resolve(m.at("from").get_ref<std::string const &>(), domain);
        if (from.detach || from.clone) {
            throw Error("from cannot contain detach & clone");
        }
        if (from.tag != ResolvedAddress::Tag::AGENT) {
            throw Error("from must be agent");
        }

        m["link"] = json{{"parent", from.agent->id},
                         {"anchor", from.agent->anchor()}};

        if (!m.contains("comment")) {
            m["comment"] = from.agent->comment;
        }

        if (!m.contains("service")) {
            m["service"] = from.agent->service;
        }

        if (!m.contains("flags")) {
            m["flags"] = dump_agent_flags(from.agent->flags);
        }
    }

    if (!m.contains("comment")) {
        m["comment"] = "";
    }

    if (!m.contains("service")) {
        m["service"] = "";
    }

    if (!m.contains("flags")) {
        m["flags"] = json::array();
    }
}

int Runtime::cmd_create_agents (Message const &msg, json *resp) {
    Context ctx;
    loadContext(msg, ctx);

    json jagents = json::parse(msg.body());
    json ops = json::array();
    std::unordered_set<std::string> seen;

    Domain *domain = ctx.from.domain;

    for (auto &m: jagents.get_ref<json::array_t&>()) {

        regularizeAgentParams(m, domain);
        AgentParams params(m);

        if (!(params.link.parent >= 0 && params.link.parent < agents.size())) {
            throw Error("bad params");
        }

        if (domain->getMember(params.name)) {
            throw Error("{} already exists in domain {}", params.name, domain->name);
        }
        if (seen.contains(params.name)) {
            throw Error("{} is duplicate", params.name);
        }
        seen.insert(params.name);

        json op = params.dump();
        op["op"] = "create_agent";
        op["domain_id"] = domain->id;
        ops.emplace_back(std::move(op));
    }

    syscalls(ops);
    return 0;
}

int Runtime::cmd_create_domain (Message const &msg, json *resp) {
    Context ctx;
    loadContext(msg, ctx);

    json params = json::parse(msg.body());
    Domain *domain = ctx.from.domain;
    std::string name;
    if (params.contains("name")) {
        name = params.at("name").get_ref<std::string const &>();
    }
    else {
        name = std::format("domain-{}", domains.size());
    }
    if (domain->getChild(name) != nullptr) {
        throw Error("Domain {} already exists.", name);
    }
    bool detach = false;
    if (params.contains("detach")) {
        detach = params.at("detach").get<bool>();
    }

    json op{{"op", "create_domain"},
            {"name", name},
            {"parent_id", domain->id}};
    auto r = syscall(op);
    CHECK(r.tag == EntityRef::Tag::DOMAIN);
    *resp = json{{"domain_id", r.domain->id}};

    if (detach) {
        json op{{"op", "create_thread"},
                {"domain_id", r.domain->id}};
        auto r = syscall(op);
        (*resp)["thread_id"] = r.thread->id;
    }

    return 0;
}

int Runtime::cmd_create_snapshot (Message const &msg, json *resp) {
    Context ctx;
    loadContext(msg, ctx);

    json params = json::parse(msg.body());
    std::string name = params.at("name").get_ref<std::string const &>();
    if (snapshots.find(name) != snapshots.end()) {
        throw Error("snapshot {} already exists.", name);
    }
    std::string domain_name = params.at("domain").get_ref<std::string const &>();
    Domain *parent_domain = ctx.from.domain;
    Domain *domain = parent_domain->getChild(domain_name);
    if (!domain) {
        throw Error("Domain {} not found.", domain_name);
    }
    json op{{"op", "create_snapshot"},
            {"name", name},
            {"domain_id", domain->id}};
    auto r = syscall(op);
    CHECK(r.tag == EntityRef::Tag::SNAPSHOT);
    *resp = json{{"snapshot", r.snapshot->name}};

    return 0;
}

EntityRef Runtime::syscall (json const &op) {
    Message entry = protocol::runtime::Commit::make(op);
    journal.append(entry);
    auto r = apply(entry);
    if (consume) consume(std::move(entry));
    return r;
}

void Runtime::call (Message &&msg, Response &resp) {

    int status = 0;
    json respHeader;
    json respBody;

    CLI::App app{"Postline Runtime Message API"};

    app.require_subcommand(1);
    app.allow_extras(false);

    app.add_subcommand("exit")
        ->callback([this] {
           stop_requested = true;
        });

    app.add_subcommand("cost")
        ->callback([&] {
            respBody = accounting.dump();
        });

    bool list_all = false;
    auto sub_list_agents = app.add_subcommand("list_agents");
    sub_list_agents->add_flag("-a,--all", list_all);
    sub_list_agents->callback([&] {
            json j = json::array();
            if (list_all) {
                for (auto const &a: agents) {
                    j.push_back(dumpAgent(a.get()));
                }
            }
            else {
                // only list domain
                Context ctx;
                loadContext(msg, ctx);
                for (auto const &p: ctx.from.domain->members) {
                    j.push_back(dumpAgent(p.second));
                }
            }
            respBody.swap(j);
        });

    app.add_subcommand("create_agents")
        ->callback([&] {
            status = cmd_create_agents(msg, &respBody);
        });

    app.add_subcommand("create_domain")
        ->callback([&] {
            status = cmd_create_domain(msg, &respBody);
        });

    app.add_subcommand("create_snapshot")
        ->callback([&] {
            status = cmd_create_snapshot(msg, &respBody);
        });

    std::string dump_path;
    auto sub_dump = app.add_subcommand("dump");
    sub_dump->add_option("path", dump_path);
    sub_dump->callback([&] {
        if (dump_path.empty()) {
            respBody = dump();
        }
        else {
            std::ofstream f(dump_path);
            f << dump().dump(2);
        }});

    try {
        app.parse(msg.subject(), false);
    }
    catch (CLI::ParseError const& e) {
        respHeader["Subject"] = std::string("Error: ") + e.what();
    } catch (json::exception const &e) {
        log::info("COMMIT ERROR: {}", e.what());
        respHeader["Subject"] = e.what();
    } catch (std::exception const &e) {
        log::info("COMMIT ERROR: {}", e.what());
        respHeader["Subject"] = e.what();
    }

    resp.append(Message(std::move(respHeader), respBody.dump()));
}

void Runtime::updateMemory (Agent *agent, AccessID end) {
    if ((agent->flags & AGENT_FLAG_MEMORY) == 0) return;
    auto &state = agentState(agent);
    CHECK(state.driver);
    std::vector<AgentLink> links;
    links.emplace_back(agent->id, agent->anchor());
    Agent *cur = agent;
    while (cur) {
        if (cur->link.parent == NOT_AN_AGENT) {
            cur = nullptr;
        }
        else {
            links.emplace_back(cur->link);
            cur = agents[cur->link.parent].get();
        }
    }
    
    state.driver->send(protocol::handshake::BeginMemory::make());
    for (auto it = links.rbegin(); it != links.rend(); ++it) {
        auto link = *it;
        cur = agents[link.parent].get();
        for (AccessID id: cur->memory) {
            if (id > link.anchor) break;
            Message msg = journal.read(id);
            //std::string const &type = msg.type();
            if (is_receiving(id)) {
                msg.updateHeader([id](json &h) {
                    h["Is-Receiving"] = "1";
                });
            }
            if (msg.access_id() < end) {
                state.driver->send(msg);
            }
        }
    }
    state.driver->send(protocol::handshake::EndMemory::make());
}

void Runtime::run() {
    struct Todo {
        Message message;
        Agent *agent;
    };

    CHECK(agentState(user).driver, "Must attachUser first.");
    poller.add(agentState(runtime).driver->read_fd(), runtime->id);
    poller.add(agentState(user).driver->read_fd(), user->id);

    while (!stop_requested) {
        auto events = poller.wait();
        CHECK(!events.empty());

        std::vector<Todo> todos;
        for (auto const &e : events) {
            CHECK(e.token >= 0 && e.token < agents.size());
            Agent *agent = agents[e.token].get();
            auto &state = agentState(agent);
            CHECK(state.driver);

            std::vector<Message> tmp;
            try {
                int err = state.driver->recv(tmp);
                for (auto &msg : tmp) {
                    //preprocess(agent, msg, this);
                    todos.emplace_back(Todo{.message = std::move(msg), .agent = agent});
                }
            }
            catch (eof const &) {
                // driver has crashed
                agent->dead = true;
                // TODO: record agent died
                state.driver.reset();
                // construct response messages to waiting parties
                if (agent == user) {
                    log::error("User agent has died, stopping...");
                    stop_requested = true;
                }
                else {
                    todos.emplace_back(Todo{.message = makeRewindMessage(agent, nullptr, "agent has died."), .agent = nullptr});
                }
            }
        }

        for (auto &todo: todos) {
            Message &msg = todo.message;
            if (todo.agent) {   // is not rewind message
                preprocess(todo.agent, msg, this);
            }
            int64_t flags = msg.flags();
#if 0
            if ((flags & MESSAGE_QUIET) == 0) {
                std::cerr << "PROCESS " << flags << std::endl;
                std::cerr << "====" << std::endl;
                msg.formatEmail(std::cerr);
                std::cerr << std::endl;
            }
#endif
            journal.append(msg);
            accounting.update(msg);
            EntityRef r = apply(msg);
            Agent *agent = r.agent;
            CHECK(agent != nullptr);
            auto &state = agentState(agent);
            if (!state.driver) {
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
                std::string err_file = std::format("stderr.{}.{}", agent->name, agent->id);
                state.driver = create_driver(agent->service, err_file);
                CHECK(state.driver);
                updateMemory(agent, msg.access_id());
                poller.add(state.driver->read_fd(), agent->id);
            }
            state.driver->send(msg);
            if (consume) consume(std::move(msg));
        }
    }

    // from this point on we don't call consume anymore
    // as we are in shutdown process
    {
        json op{{"op", "begin_shutdown"}};
        Message msg = protocol::runtime::Commit::make(op);
        journal.append(msg);
    }

    size_t trailing = 0;
    for (std::size_t i = 0; i < agents.size(); ++i) {
        Agent *agent = agents[i].get();
        auto &state = agentState(agent);
        while (agent->obligation_count > 0) {
            CHECK(state.driver);
            log::info("Waiting for agent {} (oc: {}) to respond...", i, agent->obligation_count);

            std::vector<Message> tmp;
            state.driver->recv(tmp);
            for (auto &msg: tmp) {
                --agent->obligation_count;
                journal.append(msg);
            }
            trailing += tmp.size();
        }
        if (state.driver) {
            log::info("Stopping agent {} driver...", i);
            state.driver->shutdown();
            state.driver.reset();
        }
    }

    log::info("{} messages unprocessed.", trailing);
    {
        json op{{"op", "end_shutdown"}};
        Message msg = protocol::runtime::Commit::make(op);
        journal.append(msg);
        log::info("runtime shutdown.");
    }
}


}  // namespace postline
