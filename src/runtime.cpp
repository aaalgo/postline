#include <string.h>
#include <format>
#include <fstream>
#include <iostream>
#include <unordered_set>
#include <CLI/CLI.hpp>
#include <postline/runtime.h>

namespace postline {


class SyscallError : public Error {
public:
    using Error::Error;
};

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

json Runtime::dump () const {
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

#if 0
        AgentID from_id = resolve(from);
        if (from_id == NOT_AN_AGENT) {
            throw SyscallError(std::format("cannot resolve from {}", from));
        }
        else {
            Agent const &p = agents.get(from_id);
            if ((p.flags & AGENT_FLAG_CLONE) && (flags & AGENT_FLAG_CLONE)) {
                throw SyscallError(std::format("cannot double clone"));
            }
        }

        AgentID new_id = resolve(address);
        if (new_id != NOT_AN_AGENT) {
            throw SyscallError(std::format("{} already used", address));
        }
#endif
        json op = params.dump();
        op["op"] = "create_agent";
        op["domain_id"] = domain_id;
        ops.emplace_back(std::move(op));
    }

    Message entry = protocol::runtime::Commit::make(ops);
    journal.append(entry);
    commit(ops);
    return 0;
}

void Runtime::commit(json const &ops) {
    CHECK(ops.is_array());
    for (size_t i = 0; i < ops.size(); ++i) {
        auto const &m = ops[i];
        CHECK(m.is_object());
        std::string const &op = m.at("op").get_ref<std::string const &>();
        if (op == "create_agent") {
            AgentParams params(m);
            DomainID domain_id = m.at("domain_id").get<DomainID>();
            CHECK(domain_id >= 0 && domain_id < domains.size());
            Domain *domain = domains[domain_id].get();
            Agent *agent = createAgent(params, domain);
            log::info("create agent {}: {}", agent->id, agent->name);
        } 
        else if (op == "create_domain") {
            std::string name = m.at("name").get<std::string>();
            DomainID parent_id = m.at("parent_id").get<DomainID>();
            CHECK(parent_id >= 0 && parent_id < domains.size());
            Domain *parent = domains[parent_id].get();
            Domain *domain = createDomain(name, parent);
            log::info("create domain {}: {}", domain->id, domain->name);
        }
        else if (op == "begin_shutdown") {
        }
        else if (op == "end_shutdown") {
        } else {
            CHECK(0, "UNKNOWN OP");
        }
    }
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
                    preprocess(agent, msg);
                    todo.emplace_back(std::move(msg));
                }
            }
            catch (eof_error const &) {
                // driver has crashed
                agent->dead = true;
                // TODO: record agent died
                agent->driver.reset();
                // construct response messages to waiting parties
                todo.emplace_back(makeRewindMessage(agent));
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
