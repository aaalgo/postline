#include <string.h>
#include <format>
#include <fstream>
#include <iostream>
#include <unordered_set>
#include <CLI/CLI.hpp>
#include <postline/runtime.h>

namespace postline {


json dump_agent_flags (AgentFlags flags) {
    json flag_strings = json::array();
#define X(flag, value) if (flags & AGENT_FLAG_##flag) { flag_strings.push_back(#flag); }
    AGENT_FLAG_LIST(X)
#undef X
    return flag_strings;
}

static bool compare_flag_string (json const &j, char const *str) {
    CHECK(str);
    auto const &s = j.get_ref<std::string const &>();
    return strcasecmp(s.c_str(), str) == 0;
}

AgentFlags load_agent_flags (json const &jflags) {
    // use get_ref to check type
    AgentFlags flags = 0;
    for (auto const &f: jflags.get_ref<json::array_t const &>()) {
#define X(flag, value) if (compare_flag_string(f, #flag)) { flags |= value; }
        AGENT_FLAG_LIST(X)
#undef X
    }
    return flags;
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

AgentParams::AgentParams (json const &j)
    : link{
          .parent = j.at("link").at("parent").get<AgentID>(),
          .anchor = j.at("link").at("anchor").get<AccessID>(),
      },
      name(j.at("name").get<std::string>()),
      comment(j.at("comment").get<std::string>()),
      service(j.at("service").get<std::string>()),
      flags(load_agent_flags(j.at("flags")))
{
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

    CHECK(thread);

    return json{
        {"id", id},
        {"name", name},
        {"parent_id", parent ? json(parent->id) : json(nullptr)},
        {"thread_id", json(thread->id)},
        {"detached", detached},
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

Program::ResolvedTo Program::resolve (std::string_view address, Domain *domain) {
    // we might want to expand address to a more generic form
    Program::ResolvedTo r;
    r.tag = ResolvedTo::Tag::NONE;
    r.detach = false;
    r.clone = false;
    if (address.starts_with(ADDRESS_CHAR_DETACH)) {
        r.detach = true;
        address.remove_prefix(1);
    }
    if (address.starts_with(ADDRESS_CHAR_CLONE)) {
        r.clone = true;
        address.remove_prefix(1);
    }
    do { // try alternative lookup layers
        std::string addr(address);
        if (domain) {
            r.agent = domain->getMember(addr);
            if (r.agent) {
                if (r.agent->flags & AGENT_FLAG_CLONE) {
                    r.clone = true;
                }
                r.tag = ResolvedTo::Tag::AGENT;
                break;
            }
        }   // cannot find agent in provided domain
        {   // look in global domain
            r.agent = global->getMember(addr);
            if (r.agent) {
                if (r.agent->flags & AGENT_FLAG_CLONE) {
                    r.clone = true;
                }
                r.tag = ResolvedTo::Tag::AGENT;
                break;
            }
        }   // cannot find address as an agent, look in domains
        if (domain) {
            r.domain = domain->getChild(addr);
            if (r.domain) {
                r.tag = ResolvedTo::Tag::DOMAIN;
                break;
            }
        }
        {
            auto it = snapshots.find(addr);
            if (it != snapshots.end()) {
                r.tag = ResolvedTo::Tag::SNAPSHOT;
                r.snapshot = &it->second;
                break;
            }
        }
    } while (false);
    // check constraint
    if (r.detach) {
        // can only detach domain or snapshot
        if (!(r.tag == ResolvedTo::Tag::DOMAIN || r.tag == ResolvedTo::Tag::SNAPSHOT)) {
            r.tag = ResolvedTo::Tag::NONE;
            r.error = "Can only detach domain or snapshot";
        }
    }
    if (r.clone) {
        if (!(r.tag == ResolvedTo::Tag::AGENT || r.tag == ResolvedTo::Tag::DOMAIN)) {
            r.tag = ResolvedTo::Tag::NONE;
            r.error = "Can only clone agent or domain";
        }
    }
    return r;
}


void Program::saveContext (Message &msg, Context const &ctx) const {
    msg.updateHeader([this, &ctx](json &h) {
        json j{
            {"action", static_cast<int>(ctx.action)},
            {"thread_id", ctx.thread->id},
            {"from_agent_id", ctx.from.agent->id},
            {"from_domain_id", ctx.from.domain->id},
            {"to_agent_id", ctx.to.agent->id},
            {"to_domain_id", ctx.to.domain->id},
            {"error", ctx.error},
            // below are fields allow canonical formatting of mail headers
            // this is the authority information
            {"from_agent_name", ctx.from.agent->name},
            {"from_agent_home_domain_id", ctx.from.agent->domain->id},
            {"from_domain_name", ctx.from.domain->name},
            {"to_agent_name", ctx.to.agent->id},
            {"to_agent_home_domain_id", ctx.to.agent->domain->id},
            {"to_domain_name", ctx.to.domain->id},
            };
        h[CONTEXT_HEADER_NAME] = j;
    });
}

void Program::loadContext (Message const &msg, Context &ctx) {
    json j = msg.header()[CONTEXT_HEADER_NAME];
    ThreadID thread_id = j.at("thread_id").get<ThreadID>();
    CHECK(thread_id >= 0 && thread_id < threads.size());
    ctx.thread = threads[thread_id].get();
    ctx.action = static_cast<Action>(j.at("action").get<int>());
    AgentID from_agent_id = j.at("from_agent_id").get<AgentID>();
    CHECK(from_agent_id >= 0 && from_agent_id < agents.size());
    ctx.from.agent = agents[from_agent_id].get();
    DomainID from_domain_id = j.at("from_domain_id").get<DomainID>();
    CHECK(from_domain_id >= 0 && from_domain_id < domains.size());
    ctx.from.domain = domains[from_domain_id].get();
    ctx.to.agent = nullptr;
    ctx.to.domain = nullptr;
    if (ctx.action != Action::RETURN && ctx.action != Action::REWIND) {
        AgentID to_agent_id = j.at("to_agent_id").get<AgentID>();
        CHECK(to_agent_id >= 0 && to_agent_id < agents.size());
        ctx.to.agent = agents[to_agent_id].get();
        DomainID to_domain_id = j.at("to_domain_id").get<DomainID>();
        CHECK(to_domain_id >= 0 && to_domain_id < domains.size());
        ctx.to.domain = domains[to_domain_id].get();
    }
    ctx.error = j.at("error").get<std::string>();
}

Message Program::makeRewindMessage (Agent *fromAgent, Domain *fromDomain, char const *error) const {
    if (!fromDomain) {
        fromDomain = fromAgent->domain;
    }
    CHECK(fromDomain != global);
    // global:
    //      - runtime: should never EOF
    //      - user: user EOF should not a trigger a rewind, it should just
    //              put the thread in paused state
    Message msg;
    Context ctx;
    ctx.thread = fromDomain->thread;   // cannot rewind from global agents
    CHECK(ctx.thread, "global agents should not EOF");
    ctx.action = Action::REWIND;
    ctx.from.agent = fromAgent;
    ctx.from.domain = fromDomain;
    ctx.to.agent = nullptr;
    ctx.to.domain = nullptr;
    ctx.error = error;
    saveContext(msg, ctx);
    return msg;
}

void Program::preprocess (Agent *from, Message &msg, Runtime *runtime) {
    // by the time a message is received
    // we expect the agent implement make sure that
    // the following fields are correct:
    //      - thread_id
    //      - from_domain_id
    // create context & save to msg
    // update headers when necessary
    Context ctx;
    ctx.action = Action::REWIND;    // fail by default
    int thread_id = msg.thread_id();
    if (!(thread_id >= 0 && thread_id < threads.size())) {
        log::error("Bad thread id {}", thread_id);
        CHECK(0);
    }
    ctx.thread = threads[thread_id].get();
    if (from->domain != global) {
        if (ctx.thread != from->domain->thread) {
            log::error("In this version only global agents are allowed to message across thread");
            CHECK(0);
        }
    }
    int domain_id = msg.from_domain_id();
    if (!(domain_id >= 0 && domain_id < domains.size())) {
        log::error("Bad domain id {}", domain_id);
        CHECK(0);
    }
    ctx.from.agent = from;
    ctx.from.domain = domains[domain_id].get();
    if (from->domain != global) {
        // this can be relaxed later, but for now we
        // want all non-global agent to send from their own domains
        CHECK(from->domain == ctx.from.domain);
    }

    do {
        // do all error checking and add additional field to msg
        // determine op
        AccessID in_reply_to = msg.in_reply_to();
        AccessID in_response_to = msg.in_response_to();

        if (in_reply_to != NO_ACCESS_ID) {
            if (ctx.thread->stack.empty()) {
                ctx.error = "reply to an empty stack";
                break;
            }
            auto const &f = ctx.thread->stack.back();
            if (f.opening_message_id != in_reply_to) {
                ctx.error = std::format("msg in reply to {} doesn't match stack {}", in_reply_to, f.opening_message_id);
                break;
            }
            // CHECK matching of TO
            ctx.to = f.opening_endpoint;
            ctx.action = Action::RETURN;
        }
        else {
            if (in_response_to != NO_ACCESS_ID) {
                if (ctx.thread->stack.empty()) {
                    ctx.error = "Resopnd to empty stack";
                    break;
                }
                auto const &f = ctx.thread->stack.back();
                if (f.opening_message_id != in_response_to) {
                    ctx.error = "Resopnd not matching";
                    break;
                }
            }
            else {
                /*
                if (!ctx.thread->stack.empty()) {
                    if (!ctx.thread->pending.agent != ctx.from.agent) {
                        ctx.error = "from unexpected agent";
                    }
                    break;
                }
                */
            }
            ResolvedTo to = resolve(msg.to(), ctx.from.domain);
            // now create necessary group & agents
            // and setup ctx.to
            if (to.tag == ResolvedTo::Tag::NONE) {
                ctx.error = "Fail to resolve";
                break;
            }
            else if (to.tag == ResolvedTo::Tag::AGENT) {
                if (to.clone) {
                    std::string name = std::format("agent_{}", agents.size());
                    if (ctx.from.domain->getMember(name) != nullptr) {
                        ctx.error = "cannot create agent";
                        break;
                    }
                    AgentParams params = to.agent->snapshot(name);
                    json op = params.dump();
                    op["op"] = "create_againt";
                    op["domain_id"] = ctx.from.domain->id;
                    ctx.to.domain = ctx.from.domain;
                    ctx.to.agent = runtime->syscall(op).agent;
                }
                else {
                    ctx.to.domain = ctx.from.domain;
                    ctx.to.agent = to.agent;
                }
                break;
            }
            else if (to.tag == ResolvedTo::Tag::DOMAIN) {
                if (to.clone) {
                    ctx.error = "Not supported.";
                }
                else {
                    ctx.to.domain = to.domain;
                    ctx.to.agent = to.domain->entry.to;
                }
                break;
            }
            else if (to.tag == ResolvedTo::Tag::SNAPSHOT) {
                json op{{"op", "create_domain_snapshot"},
                        {"parent_id", ctx.from.domain->id},
                        {"snapshot", to.snapshot->name}};
                ctx.to.domain = runtime->syscall(op).domain;
                ctx.to.agent = ctx.to.domain->entry.to;
            }
            ctx.action = Action::CALL;
        }
    }
    while (false);
    if (ctx.action == Action::REWIND) {
        ;
    }
    saveContext(msg, ctx);
}

// same as journal apply
Agent *Program::apply (Message const &msg) {
    // now msg has an id
    Context ctx;
    Agent *to = nullptr;
    loadContext(msg, ctx);
    if (ctx.action == Action::RETURN) {
        ctx.thread->stack.pop_back();
        --ctx.from.agent->obligation_count;
        to = ctx.to.agent;
    }
    else if (ctx.action == Action::CALL) {
        ctx.thread->stack.emplace_back();
        auto &f = ctx.thread->stack.back();
        f.opening_message_id = msg.access_id();
        f.opening_endpoint = ctx.from;
        to = ctx.to.agent;
        ++to->obligation_count;
    }
    else if (ctx.action == Action::REWIND) {
        --ctx.from.agent->obligation_count;
        while (ctx.thread->stack.size()) {
            auto const &f = ctx.thread->stack.back();
            if (f.opening_endpoint.agent->flags & AGENT_FLAG_CATCH) {
                to = f.opening_endpoint.agent;
                ctx.thread->stack.pop_back();
                break;
            }
            --f.opening_endpoint.agent->obligation_count;
            ctx.thread->stack.pop_back();
        }
    }
    else {
        CHECK(0);
    }
    CHECK(ctx.from.agent);
    AccessID message_id = msg.access_id();
    ctx.thread->trace.push_back(message_id);
    ctx.from.agent->memory.push_back(message_id);
    to->memory.push_back(mark_receiving(message_id));
    return to;
}

int Runtime::cmd_create_agents (Message const &msg, json *resp) {

    json jagents = json::parse(msg.body());
    json ops = json::array();

    for (auto const &m: jagents.get_ref<json::array_t const &>()) {
        AgentParams params(m);
        DomainID domain_id = m.at("domain_id").get<DomainID>();

        if (!(params.link.parent >= 0 && params.link.parent < agents.size())) {
            throw std::runtime_error("bad params");
        }
        if (!(domain_id >= 0 && domain_id < domains.size())) {
            throw std::runtime_error("bad params");
        }

        Domain *domain = domains[domain_id].get();
        if (domain->getChild(params.name)) {
            throw std::runtime_error("bad params");
        }

        json op = params.dump();
        op["op"] = "create_agent";
        op["domain_id"] = domain_id;
        ops.emplace_back(std::move(op));
    }

    syscalls(ops);
    return 0;
}

Runtime::SyscallResult Runtime::syscall (json const &op) {
    Message entry = protocol::runtime::Commit::make(op);
    journal.append(entry);
    return __commit(op);
}

Runtime::SyscallResult Runtime::__commit (json const &m) {
    Runtime::SyscallResult result;
    CHECK(m.is_object());
    std::string const &op = m.at("op").get_ref<std::string const &>();
    if (op == "create_agent") {
        AgentParams params(m);
        DomainID domain_id = m.at("domain_id").get<DomainID>();
        CHECK(domain_id >= 0 && domain_id < domains.size());
        Domain *domain = domains[domain_id].get();
        result.agent = createAgent(params, domain);
        log::info("create agent {}: {}", result.agent->id, result.agent->name);
    } 
    else if (op == "create_domain") {
        std::string name = m.at("name").get<std::string>();
        DomainID parent_id = m.at("parent_id").get<DomainID>();
        CHECK(parent_id >= 0 && parent_id < domains.size());
        Domain *parent = domains[parent_id].get();
        result.domain = createDomain(name, parent);
        log::info("create domain {}: {}", result.domain->id, result.domain->name);
    }
    else if (op == "create_domain_snapshot") {
        DomainID parent_id = m.at("parent_id").get<DomainID>();
        CHECK(parent_id >= 0 && parent_id < domains.size());
        Domain *parent = domains[parent_id].get();
        std::string snapshot = m.at("snapshot").get<std::string>();
        auto it = snapshots.find(snapshot);
        CHECK(it != snapshots.end());
        result.domain = createDomain(it->second, parent);
        log::info("create domain {}: {}", result.domain->id, result.domain->name);
    }
    else if (op == "begin_shutdown") {
    }
    else if (op == "end_shutdown") {
    } else {
        CHECK(0, "UNKNOWN OP");
    }
    return result;
}

void Runtime::call (Message &&msg, Response &resp) {

    json respHeader;
    json respBody;

    do {

        CLI::App app{"Postline Runtime Message API"};

        app.require_subcommand(1);
        app.allow_extras(false);

        using Handler = int (Runtime::*)(Message const &, json *resp);

        struct Command {
            CLI::App *cmd;
            Handler handler;
        };

        std::vector<Command> commands;

        auto add_command = [&](const std::string &name, Handler handler) {
            auto *cmd = app.add_subcommand(name);
            commands.push_back({cmd, handler});
            return cmd;
        };

        add_command("exit",   &Runtime::cmd_exit);
        add_command("list_agents",   &Runtime::cmd_list_agents);
        add_command("create_agents", &Runtime::cmd_create_agents);
        //add_command("create_group",  &RuntimeCli::cmd_create_group);
        add_command("cost",  &Runtime::cmd_cost);

        try {
            app.parse(msg.subject(), false);
        } catch (CLI::ParseError const& e) {
            respHeader["Subject"] = std::string("Error: ") + e.what();
            break;
        }

        for (auto &c : commands) {
            if (*c.cmd) {
                try {
                    int ret = (this->*c.handler)(msg, &respBody);
                } catch (json::exception const &e) {
                    log::info("COMMIT ERROR: {}", e.what());
                    respHeader["Subject"] = e.what();
                } catch (std::exception const &e) {
                    log::info("COMMIT ERROR: {}", e.what());
                    respHeader["Subject"] = e.what();
                }

                break;
            }
        }
    } while (0);

    resp.append(Message(std::move(respHeader), respBody.dump()));
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
            cur = agents[cur->link.parent].get();
        }
    }
    
    agent->driver->send(protocol::handshake::BeginMemory::make());
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
            CHECK(e.token >= 0 && e.token < agents.size());
            Agent *agent = agents[e.token].get();
            CHECK(agent->driver);

            std::vector<Message> tmp;
            try {
                int err = agent->driver->recv(tmp);
                for (auto &msg : tmp) {
                    preprocess(agent, msg, this);
                    todo.emplace_back(std::move(msg));
                }
            }
            catch (eof_error const &) {
                // driver has crashed
                agent->dead = true;
                // TODO: record agent died
                agent->driver.reset();
                // construct response messages to waiting parties
                if (agent == user) {
                    log::error("User agent has died, stopping...");
                    stop_requested = true;
                }
                else {
                    todo.emplace_back(makeRewindMessage(agent, nullptr, "agent has died."));
                }
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
            Agent *agent = apply(msg);
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
        json op{{"op", "begin_shutdown"}};
        ops.push_back(op);
        Message msg = protocol::runtime::Commit::make(ops);
        journal.append(msg);
    }

    size_t trailing = 0;
    for (std::size_t i = 0; i < agents.size(); ++i) {
        Agent *agent = agents[i].get();
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
        json op{{"op", "end_shutdown"}};
        ops.push_back(op);
        Message msg = protocol::runtime::Commit::make(ops);
        journal.append(msg);
        log::info("runtime shutdown.");
    }
}


}  // namespace postline
