#include <string.h>
#include <format>
#include <fstream>
#include <iostream>
#include <CLI/CLI.hpp>
#include <postline/runtime.h>

namespace postline {

static std::set<std::string> load_tags(json const &value) {
    CHECK(value.is_array(), "{} must be an array", POSTLINE_TAGS_HEADER_NAME);
    std::set<std::string> tags;
    for (json const &tag: value.get_ref<json::array_t const &>()) {
        CHECK(tag.is_string(), "{} entries must be strings", POSTLINE_TAGS_HEADER_NAME);
        auto const &name = tag.get_ref<std::string const &>();
        CHECK(tags.insert(name).second, "duplicate tag {}", name);
    }
    return tags;
}

static json dump_tags(std::set<std::string> const &tags) {
    json result = json::array();
    for (std::string const &tag: tags) {
        result.push_back(tag);
    }
    return result;
}

static std::set<std::string> load_message_tags(json const &header) {
    auto it = header.find(POSTLINE_TAGS_HEADER_NAME);
    if (it == header.end()) {
        return {};
    }
    return load_tags(*it);
}


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
    CHECK(domain);
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
        {"domain_name", domain->name},
        {"memory", memory},
        {"tags", dump_tags(tags)},
        {"dead", dead},
        {"obligation_count", obligation_count}
    };
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
        jmembers[name] = json{{"id", agent->id},
                              {"name", agent->name}};
    }

    json jchildren = json::object();
    for (auto const &[name, domain]: children) {
        CHECK(domain);
        jchildren[name] = json{{"id", domain->id},
                               {"name", domain->name}};
    }

    CHECK(thread);

    return json{
        {"id", id},
        {"name", name},
        {"parent_id", parent ? json(parent->id) : json(nullptr)},
        {"thread_id", json(thread->id)},
        {"detached", detached},
        {"entry", {{"from", entry.from ? json(entry.from->name) : json(nullptr)},
                   {"to", entry.to ? json(entry.to->name) : json(nullptr)}}},
        {"members", jmembers},
        {"children", jchildren}
    };
}

json Frame::dump () const {
    CHECK(from.agent);
    CHECK(from.domain);
    CHECK(to.agent);
    CHECK(to.domain);

    return json{{"message_id", message_id},
                {"from_agent_id", from.agent->id},
                {"from_agent_name", from.agent->name},
                {"from_domain_id", from.domain->id},
                {"from_domain_name", from.domain->name},
                {"to_agent_id", to.agent->id},
                {"to_agent_name", to.agent->name},
                {"to_domain_id", to.domain->id},
                {"to_domain_name", to.domain->name}};
}

json Thread::dump () const {
    CHECK(root);

    json jstack = json::array();
    for (auto const &f: stack) {
        jstack.push_back(f.dump());
    }

    return json{{"id", id},
                {"name", name},
                {"root", root->id},
                {"root_name", root->name},
                {"pending", pending},
                {"stack", jstack},
                {"trace", trace}};
}

json Program::dump () const {
    CHECK(global);
    CHECK(zero);
    CHECK(runtime);
    CHECK(user);

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
        {"global_id", global->id},
        {"zero_id", zero->id},
        {"runtime_id", runtime->id},
        {"user_id", user->id},
    };
}

Program::ResolvedAddress Program::resolve (std::string_view address, Domain *domain) const {
    // we might want to expand address to a more generic form
    Program::ResolvedAddress r;
    r.tag = ResolvedAddress::Tag::NONE;
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
                r.tag = ResolvedAddress::Tag::AGENT;
                break;
            }
        }   // cannot find agent in provided domain
        {   // look in global domain
            r.agent = global->getMember(addr);
            if (r.agent) {
                if (r.agent->flags & AGENT_FLAG_CLONE) {
                    r.clone = true;
                }
                r.tag = ResolvedAddress::Tag::AGENT;
                break;
            }
        }   // cannot find address as an agent, look in domains
        if (domain) {
            r.domain = domain->getChild(addr);
            if (r.domain) {
                r.tag = ResolvedAddress::Tag::DOMAIN;
                break;
            }
        }
        {
            auto it = snapshots.find(addr);
            if (it != snapshots.end()) {
                r.tag = ResolvedAddress::Tag::SNAPSHOT;
                r.snapshot = &it->second;
                break;
            }
        }
    } while (false);
    // check constraint
    if (r.detach) {
        // can only detach domain or snapshot
        if (!(r.tag == ResolvedAddress::Tag::DOMAIN || r.tag == ResolvedAddress::Tag::SNAPSHOT)) {
            r.tag = ResolvedAddress::Tag::NONE;
            r.error = "Can only detach domain or snapshot";
        }
    }
    if (r.clone) {
        if (!(r.tag == ResolvedAddress::Tag::AGENT || r.tag == ResolvedAddress::Tag::DOMAIN)) {
            r.tag = ResolvedAddress::Tag::NONE;
            r.error = "Can only clone agent or domain";
        }
    }
    return r;
}


void Program::saveContext (Message &msg, Context const &ctx) const {
    msg.updateHeader([this, &ctx](json &h) {
        CHECK(ctx.thread);
        CHECK(ctx.from.agent);
        CHECK(ctx.from.domain);
        json to_agent_id = nullptr;
        json to_domain_id = nullptr;
        json to_agent_name = nullptr;
        json to_agent_home_domain_id = nullptr;
        json to_domain_name = nullptr;
        if (Context::to_needed(ctx.action)) {
            CHECK(ctx.to.agent);
            CHECK(ctx.to.domain);
            to_agent_id = ctx.to.agent->id;
            to_domain_id = ctx.to.domain->id;
            to_agent_name = ctx.to.agent->name;
            to_agent_home_domain_id = ctx.to.agent->domain->id;
            to_domain_name = ctx.to.domain->name;
        }
        json j{
            {"action", static_cast<int>(ctx.action)},
            {"thread_id", ctx.thread->id},
            {"from_agent_id", ctx.from.agent->id},
            {"from_domain_id", ctx.from.domain->id},
            {"to_agent_id", to_agent_id},
            {"to_domain_id", to_domain_id},
            {"error", ctx.error},
            // below are fields allow canonical formatting of mail headers
            // this is the authority information
            {"from_agent_name", ctx.from.agent->name},
            {"from_agent_home_domain_id", ctx.from.agent->domain->id},
            {"from_domain_name", ctx.from.domain->name},
            {"to_agent_name", to_agent_name},
            {"to_agent_home_domain_id", to_agent_home_domain_id},
            {"to_domain_name", to_domain_name},
            };
        h[CONTEXT_HEADER_NAME] = j;
    });
}

void Program::loadContext (Message const &msg, Context &ctx) const {
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
    if (Context::to_needed(ctx.action)) {
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
    CHECK(fromAgent != runtime);
    CHECK(fromAgent != user);
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
    msg.updateHeader([&ctx](json &h) {
            h["Thread-ID"] = std::format("{}", ctx.thread->id);
            h[POSTLINE_TAGS_HEADER_NAME] = json::array();
            });
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

    do {
        Endpoint expected;
        if (ctx.thread->stack.empty()) {
            expected.domain = ctx.thread->root;
            expected.agent = expected.domain->entry.from;
        }
        else {
            expected = ctx.thread->stack.back().to;
        }
        ctx.from = expected;
        CHECK(ctx.from.agent);
        CHECK(ctx.from.domain);
        if (expected.agent != from) {
            ctx.error = std::format("expected from agent {} but got {}", expected.agent->name, from->name);
            break;
        }
        if (from->domain != global) {
            // this can be relaxed later, but for now we
            // want all non-global agent to send from their own domains
            CHECK(from->domain == ctx.from.domain);
        }
        if (msg.type() == protocol::agent::Data::type) {
            protocol::agent::Data data(msg);
            auto const &header = msg.header();
            CHECK(!header.contains("To"));
            CHECK(!header.contains("In-Reply-To"));
            CHECK(!header.contains("In-Response-To"));
            msg.updateHeader([&ctx](json &h) {
                if (h.contains("From") && h["From"] != ctx.from.agent->name) {
                    h["Original-From"] = h["From"];
                }
                h["From"] = ctx.from.agent->name;
                h["Thread-ID"] = std::format("{}", ctx.thread->id);
                h[POSTLINE_TAGS_HEADER_NAME] = json::array();
            });
            ctx.action = Action::DATA;
            break;
        }
        msg.updateHeader([&ctx](json &header) {
            if (header["From"] != ctx.from.agent->name) {
                header["Original-From"] = header["From"];
                header["From"] = ctx.from.agent->name;
            }
        });
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
            if (f.message_id != in_reply_to) {
                ctx.error = std::format("msg in reply to {} doesn't match stack {}", in_reply_to, f.message_id);
                break;
            }
            // CHECK matching of TO
            ctx.to.domain = nullptr;
            ctx.to.agent = nullptr;
            ctx.action = Action::RETURN;
        }
        else {
            if (in_response_to != NO_ACCESS_ID) {
                if (ctx.thread->stack.empty()) {
                    ctx.error = "Resopnd to empty stack";
                    break;
                }
                auto const &f = ctx.thread->stack.back();
                if (f.message_id != in_response_to) {
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
            ResolvedAddress to = resolve(msg.to(), ctx.from.domain);
            // now create necessary group & agents
            // and setup ctx.to
            if (to.tag == ResolvedAddress::Tag::NONE) {
                ctx.error = "Fail to resolve";
                break;
            }
            else if (to.tag == ResolvedAddress::Tag::AGENT) {
                if (to.clone) {
                    std::string name = std::format("{}_{}", to.agent->name, agents.size());
                    if (ctx.from.domain->getMember(name) != nullptr) {
                        ctx.error = "cannot create agent";
                        break;
                    }
                    AgentParams params = to.agent->snapshot(name);
                    params.flags &= ~AGENT_FLAG_CLONE;
                    json op = params.dump();
                    op["op"] = "create_agent";
                    op["domain_id"] = ctx.from.domain->id;
                    ctx.to.domain = ctx.from.domain;
                    ctx.to.agent = runtime->syscall(op).agent;
                }
                else {
                    ctx.to.domain = ctx.from.domain;
                    ctx.to.agent = to.agent;
                }
            }
            else if (to.tag == ResolvedAddress::Tag::DOMAIN) {
                if (to.clone) {
                    ctx.error = "Not supported.";
                    break;
                }
                else {
                    ctx.to.domain = to.domain;
                    ctx.to.agent = to.domain->entry.to;
                }
            }
            else if (to.tag == ResolvedAddress::Tag::SNAPSHOT) {
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
    msg.updateHeader([this, from, &ctx](json &header) {
        std::set<std::string> proposed_tags = load_message_tags(header);

        std::set<std::string> message_tags;
        if (ctx.action != Action::REWIND) {
            message_tags = from->tags;
            if (from == user) {
                message_tags.insert(proposed_tags.begin(), proposed_tags.end());
            }
        }
        header[POSTLINE_TAGS_HEADER_NAME] = dump_tags(message_tags);
    });
    if (ctx.action == Action::REWIND) {
        ;
    }
    saveContext(msg, ctx);
}

// same as journal apply
EntityRef Program::apply (Message const &msg) {
    // now msg has an id
    if (msg.type() == "runtime:commit") {
        return commit(msg);
    }
    Context ctx;
    Agent *to = nullptr;
    loadContext(msg, ctx);
    if (ctx.action == Action::DATA) {
        CHECK(ctx.from.agent);
        CHECK(msg.access_id() != NO_ACCESS_ID);
        ctx.from.agent->memory.push_back(msg.access_id());
        EntityRef r;
        r.tag = EntityRef::Tag::NONE;
        return r;
    }
    if (ctx.action == Action::RETURN) {
        auto const &f = ctx.thread->stack.back();
        to = f.from.agent;
        ctx.thread->stack.pop_back();
        --ctx.from.agent->obligation_count;
    }
    else if (ctx.action == Action::CALL) {
        ctx.thread->stack.emplace_back();
        auto &f = ctx.thread->stack.back();
        f.message_id = msg.access_id();
        f.from = ctx.from;
        f.to = ctx.to;
        to = ctx.to.agent;
        ++to->obligation_count;
    }
    else if (ctx.action == Action::REWIND) {
        to = ctx.from.agent;
        do {
            if (to->flags & AGENT_FLAG_CATCH) break;
            if (ctx.thread->stack.empty()) break;
            auto const &f = ctx.thread->stack.back();
            to = f.from.agent;
            --f.to.agent->obligation_count;
            ctx.thread->stack.pop_back();
        } while (true);
    }
    else {
        CHECK(0);
    }
    CHECK(ctx.from.agent);
    CHECK(to);
    if (ctx.action == Action::CALL || ctx.action == Action::RETURN) {
        auto tags = load_message_tags(msg.header());
        ctx.from.agent->tags.insert(tags.begin(), tags.end());
        // Initial policy: the receiver accepts every offered tag.
        to->tags.insert(tags.begin(), tags.end());
    }
    AccessID message_id = msg.access_id();
    ctx.thread->trace.push_back(message_id);
    if (ctx.from.agent == user && to != user) {
        ctx.thread->pending = true;
    }
    else if (to == user) {
        ctx.thread->pending = false;
    }
    ctx.from.agent->memory.push_back(message_id);
    to->memory.push_back(mark_receiving(message_id));
    EntityRef r;
    r.tag = EntityRef::Tag::AGENT;
    r.agent = to;
    return r;
}

EntityRef Program::commit (Message const &msg) {
    json m = json::parse(msg.body());
    EntityRef result;
    CHECK(m.is_object());
    std::string const &op = m.at("op").get_ref<std::string const &>();
    if (op == "create_agent") {
        AgentParams params(m);
        DomainID domain_id = m.at("domain_id").get<DomainID>();
        CHECK(domain_id >= 0 && domain_id < domains.size());
        Domain *domain = domains[domain_id].get();
        result.tag = EntityRef::Tag::AGENT;
        result.agent = createAgent(params, domain);
        log::info("create agent {}: {}", result.agent->id, result.agent->name);
    } 
    else if (op == "create_domain") {
        std::string name = m.at("name").get<std::string>();
        DomainID parent_id = m.at("parent_id").get<DomainID>();
        CHECK(parent_id >= 0 && parent_id < domains.size());
        Domain *parent = domains[parent_id].get();
        result.tag = EntityRef::Tag::DOMAIN;
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
        result.tag = EntityRef::Tag::DOMAIN;
        result.domain = createDomain(it->second, parent);
        log::info("create domain {}: {}", result.domain->id, result.domain->name);
    }
    else if (op == "create_snapshot") {
        DomainID domain_id = m.at("domain_id").get<DomainID>();
        CHECK(domain_id >= 0 && domain_id < domains.size());
        Domain *domain = domains[domain_id].get();
        std::string name = m.at("name").get<std::string>();
        auto it = snapshots.find(name);
        CHECK(it == snapshots.end());
        result.tag = EntityRef::Tag::SNAPSHOT;
        result.snapshot = createSnapshot(name, domain);
        log::info("create snapshot {} from domain {}", name, domain->name);
    }
    else if (op == "create_thread") {
        DomainID domain_id = m.at("domain_id").get<DomainID>();
        CHECK(domain_id >= 0 && domain_id < domains.size());
        Domain *domain = domains[domain_id].get();
        CHECK(!domain->detached);
        result.tag = EntityRef::Tag::THREAD;
        result.thread = createThread(domain);
        // We haven't handled entry.from/to yet
        log::info("create thread {} from domain {}", result.thread->id, domain->id);
    }
    // the two below have to go to runtime
    else if (op == "begin_shutdown") {
    }
    else if (op == "end_shutdown") {
    } else {
        CHECK(0, "UNKNOWN OP");
    }
    return result;
}


}  // namespace postline
