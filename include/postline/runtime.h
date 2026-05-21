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
#include "journal.h"
#include "poller.h"
#include "service.h"
#include "accounting.h"

namespace postline {

using AgentID = std::int32_t;
using DomainID = std::int32_t;
using ThreadID = std::int32_t;
constexpr AgentID NOT_AN_AGENT = -1;
constexpr DomainID NOT_A_DOMAIN = -1;
constexpr ThreadID NOT_A_THREAD = -1;

constexpr char const *DOMAIN_NAME_GLOBAL = "global";
constexpr char const *GLOBAL_ENTRY_FROM = "user";
constexpr char const *GLOBAL_ENTRY_TO = "zero";
constexpr char const *AGENT_NAME_ZERO = "zero";
constexpr char const *AGENT_NAME_RUNTIME = "runtime";
constexpr char const *AGENT_NAME_USER = "user";

struct Domain;
struct Thread;

struct AgentLink {
    AgentID parent;
    AccessID anchor;
};

using AgentFlags = std::uint64_t;

#define AGENT_FLAG_LIST(X)  \
    X(HISTORY,  0x00000001) \
    X(CATCH,    0x00000002) \
    X(THREAD,   0x00000004) \
    X(CLONE,   0x0000008)  \

#define X(flag, value) AgentFlags constexpr AGENT_FLAG_##flag = value;
    AGENT_FLAG_LIST(X)
#undef X

struct AgentParams {
    AgentLink link;
    std::string name;
    std::string comment;
    std::string service;
    AgentFlags flags;

    AgentParams (std::string const &name_, AgentFlags flags_)
        : link{.parent=NOT_AN_AGENT, .anchor=NO_ACCESS_ID},
        name(name_),
        flags(flags_)
    {
    }

    AgentParams (json const &j);

    json dump () const;
};

struct Agent: immobile, AgentParams {
    AgentID id;
    Domain *domain;
    std::vector<AccessID> memory;
    std::unique_ptr<Driver> driver;
    bool dead;                 // whether the agent has died
    int obligation_count;      // sanity check

    explicit Agent(AgentParams const &params, AgentID id_, Domain *domain_)
        : AgentParams(params),
        id(id_),
        domain(domain_),
        dead(false),
        obligation_count(0)
    {}

    AccessID anchor () const {
        if (memory.empty()) {
            return link.anchor;
        }
        else {
            return memory.back();
        }
    }

    AgentParams snapshot (std::string const &name = "") const {
        AgentParams p = *this;
        if (!name.empty()) {
            p.name = name;
        }
        // however we need to change link to point to this instead of this->link
        p.link.parent = id;
        p.link.anchor = anchor();
        return p;
    }

    json dump () const;

    ~Agent() {
        if (driver) {
            log::error("Deleting agent {} with open driver.", id);
        }
    }
};

// A template used to create domain
// it is created from taking a snapshot of a domain
struct Snapshot {
    std::string name;
    std::vector<AgentParams> members;
    struct {
        std::string from, to;
    } entry;

    json dump () const;
};

struct Domain: immobile {
    DomainID id;
    std::string name;
    Domain *parent;     // only global has null parent
    Thread *thread;     // thread must always be not null
    bool detached;
    struct Entry {      // only attached domain have from/to
        Agent *from;    // detached domains cannot be called
        Agent *to;      // so they don't have from/to
    } entry;
    std::unordered_map<std::string, Agent *> members;
    std::unordered_map<std::string, Domain *> children;

    Domain (DomainID id_,
            std::string const &name_,
            Domain *parent_ = nullptr)
        : id(id_),
        name(name_),
        parent(parent_),
        thread(parent_ ? parent_->thread : nullptr),
        detached(false)
    {
        entry.from = nullptr;
        entry.to = nullptr;
    }

    Snapshot snapshot (std::string const &name) const {
        Snapshot s;
        s.name = name;
        for (auto const &p: members) {
            s.members.emplace_back(p.second->snapshot());
        }
        s.entry.from = entry.from->name;
        s.entry.to = entry.to->name;
        return s;
    }

    Agent *getMember (std::string const &name) {
        auto it = members.find(name);
        if (it == members.end()) return nullptr;
        return it->second;
    }

    Domain *getChild (std::string const &name) {
        auto it = children.find(name);
        if (it == children.end()) return nullptr;
        return it->second;
    }

    json dump () const;
};

struct Endpoint {
    Agent *agent;
    Domain *domain;
};

struct Frame {
    AccessID opening_message_id = 0;
    Endpoint opening_endpoint;

    json dump () const {
        return json{{"opening_message_id", opening_message_id},
                    {"opening_agent_id", opening_endpoint.agent->id},
                    {"opening_agent_name", opening_endpoint.agent->name},
                    {"opening_domain_id", opening_endpoint.domain->id},
                    {"opening_domain_name", opening_endpoint.domain->name}};
    }
};

struct Thread {
    ThreadID id;
    std::string name;
    Domain *root;
    //Agent *continuation_owner;
    std::vector<Frame> stack;
    std::vector<AccessID> trace;

    Thread (ThreadID id_, Domain *root_, Agent *owner): id(id_), root(root_) {
        CHECK(!root->detached);
        root->thread = this;
        root->detached = true;
        root->entry.from = nullptr;
        root->entry.to = nullptr;
    }

    json dump () const {
        json jstack = json::array();
        for (auto const &f: stack) {
            jstack.push_back(f.dump());
        }
        return json{{"id", id},
                    {"name", name},
                    {"root", root->id},
                    //{"continuation_owner", continuation_owner->id},
                    {"stack", jstack},
                    {"trace", trace}};
    }
};

struct InitHook {
    template<typename F>
    InitHook(F&& f) {
        f();
    }
};

class Runtime: immobile, public Service {

    std::vector<std::unique_ptr<Domain>> domains;
    std::vector<std::unique_ptr<Agent>> agents;
    std::unordered_map<std::string, Snapshot> snapshots;
    std::vector<std::unique_ptr<Thread>> threads;

    Domain *global;
    Agent *zero;    // agent zero
    Agent *runtime;
    Agent *user;

    bool allowUnsolicited (Agent *agent) const {
        return agent == user;
    }

    InitHook initHook;
    Journal journal;
    Poller poller;
    Accounting accounting;
    bool stop_requested;

// Section: program state manipulation

    void initGlobal () {    // called in constructor via hook before journal replay
        // hard-coded initial state:
        //      thread-0  --  domain-0 -- {runtime, user, agent}
        // root cannot be created with createDomain
        // all other domains must be created with createDomain
        domains.emplace_back(std::make_unique<Domain>(domains.size(), DOMAIN_NAME_GLOBAL, nullptr));
        global  = domains.back().get();
        zero    = createAgent(AgentParams(AGENT_NAME_ZERO, 0), global);
        runtime = createAgent(zero->snapshot(AGENT_NAME_RUNTIME), global);
        user    = createAgent(zero->snapshot(AGENT_NAME_USER), global);

        user->flags |= AGENT_FLAG_CATCH | AGENT_FLAG_THREAD;

        global->entry.from = global->getMember(GLOBAL_ENTRY_FROM);
        global->entry.to = global->getMember(GLOBAL_ENTRY_TO);
        if (!global->entry.from) {
            log::warn("Global entry.from {} not found.", GLOBAL_ENTRY_FROM);
        }
        if (!global->entry.to) {
            log::warn("Global entry.to {} not found.", GLOBAL_ENTRY_TO);
        }

        createThread(global, user);
    }

    Thread *createThread (Domain *root, Agent *owner) {
        threads.emplace_back(std::make_unique<Thread>(threads.size(), root, owner));
        return threads.back().get();
    }

    Agent *createAgent (AgentParams const &params, Domain *domain) {
        CHECK(domain->getMember(params.name) == nullptr);
        agents.emplace_back(std::make_unique<Agent>(params, agents.size(), domain));
        Agent *agent = agents.back().get();
        domain->members[agent->name] = agent;
        return agent;
    }

    Domain *createDomain (std::string const &name, Domain *parent) {
        CHECK(parent->getChild(name) == nullptr);
        domains.emplace_back(std::make_unique<Domain>(domains.size(), name, parent));
        return domains.back().get();
    }

    Domain *createDomain (Snapshot const &snapshot, Domain *parent) {
        std::string name = std::format("domain-{}", domains.size());
        Domain *domain = createDomain(name, parent);
        parent->children[name] = domain;
        // now setup the children
        for (auto const &params: snapshot.members) {
            createAgent(params, domain);
        }
        domain->entry.from = domain->getMember(snapshot.entry.from);
        CHECK(domain->entry.from);
        domain->entry.to = domain->getMember(snapshot.entry.to);
        CHECK(domain->entry.to);
        return domain;
    }

#if 0
    void publishSnapshot (std::string const &name, Domain const *domain) {
        CHECK(snapshots.find(name) == snapshots.end());
        snapshots.emplace(std::make_pair(name, domain->snapshot(name)));
    }

    /*
    Thread *detach (Domain *domain) {
        domain->thread = createThread();
        return domain->thread;
    }
    */
#endif

// Section: message routing

    struct ResolvedTo {
        enum class Tag: uint8_t {
            NONE = 0,       // only for rewind, or it is an error
            AGENT,          // resolved in the order below
            AGENT_CLONE,
            DOMAIN,
            DOMAIN_CLONE,
            SNAPSHOT,
        } tag;
        union {
            Agent *agent;
            Domain *domain;
            Snapshot *snapshot;
        };
    };

    ResolvedTo resolve (std::string const &address, Domain *domain) {
        ResolvedTo r;
        r.tag = ResolvedTo::Tag::NONE;
        if (address.starts_with("&")) { // clone request
            // clone request
            std::string deref = address.substr(1);
            if (deref.starts_with("&")) return r;    // fail, cannot start with &&
            ResolvedTo r2 = resolve(deref, domain);
            if (r2.tag == ResolvedTo::Tag::AGENT || r2.tag == ResolvedTo::Tag::AGENT_CLONE) {
                r.tag = ResolvedTo::Tag::AGENT_CLONE;
                r.agent = r2.agent;
            }
            else if (r2.tag == ResolvedTo::Tag::DOMAIN) {
                r.tag = ResolvedTo::Tag::DOMAIN_CLONE;
                r.domain = r2.domain;
            }
            else {
                // we cannot clone others
                return r;
            }
        }
        if (domain) {
            r.agent = domain->getMember(address);
            if (r.agent) {
                if (r.agent->flags & AGENT_FLAG_CLONE) {
                    r.tag = ResolvedTo::Tag::AGENT_CLONE;
                }
                else {
                    r.tag = ResolvedTo::Tag::AGENT;
                }
                return r;
            }
        }
        {
            r.agent = global->getMember(address);
            if (r.agent) {
                r.tag = ResolvedTo::Tag::AGENT;
                return r;
            }
        }
        if (domain) {
            r.domain = domain->getChild(address);
            if (r.domain) {
                r.tag = ResolvedTo::Tag::DOMAIN;
                return r;
            }
        }
        {
            auto it = snapshots.find(address);
            if (it != snapshots.end()) {
                r.tag = ResolvedTo::Tag::SNAPSHOT;
                r.snapshot = &it->second;
                return r;
            }
        }
        return r;
    }

    enum class Action: uint8_t {
        YIELD = 0,
        CALL = 1,
        RETURN = 2,
        REWIND = 3
    };

    struct Context {
        Thread *thread;
        Action action;
        Endpoint from;
        Endpoint to;
        //ResolvedTo to;
        std::string error;
    };

    void saveContext (Message &msg, Context const &ctx) const {
        msg.updateHeader([this, &ctx](json &h) {
            json j{
                {"thread_id", ctx.thread->id},
                {"action", static_cast<int>(ctx.action)},
                {"from_agent_id", ctx.from.agent->id},
                {"from_domain_id", ctx.from.domain->id},
                {"to_agent_id", ctx.to.agent->id},
                {"to_domain_id", ctx.to.domain->id},
                {"error", ctx.error}
                };
#if 0
            if (ctx.to.tag == ResolvedTo::Tag::AGENT || ctx.to.tag == ResolvedTo::Tag::AGENT_CLONE) {
                j["to_agent_id"] = int64_t(ctx.to.agent->id);
            }
            else if (ctx.to.tag == ResolvedTo::Tag::DOMAIN || ctx.to.tag == ResolvedTo::Tag::DOMAIN_CLONE) {
                j["to_domain_id"] = int64_t(ctx.to.domain->id);
            }
            else if (ctx.to.tag == ResolvedTo::Tag::SNAPSHOT) {
                j["to_snapshot"] = ctx.to.snapshot->name;
            }
            else {
                CHECK(0, "Unknown to tag: {}", static_cast<int>(ctx.to.tag));
            }
#endif
            h[CONTEXT_HEADER_NAME] = j;
            h["To-Domain-ID"] = std::format("{}", ctx.to.domain->id);
        });
    }

    void loadContext (Message const &msg, Context &ctx) {
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

    Message makeRewindMessage (Agent *fromAgent, Domain *fromDomain = nullptr) const {
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
        ctx.error = "adapter of agent has died.";
        saveContext(msg, ctx);
        return msg;
    }

    void preprocess (Agent *from, Message &msg) {
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
        /*
        if (from->domain->thread) {
            if (ctx.thread != from->domain->thread) {
                log::warn("thread not matching");
            }
        }
        */
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
                    if (!ctx.thread->stack.empty()) {
                        if (!allowUnsolicited(ctx.from.agent)) {
                            ctx.error = "Spontaneus message on non empty stack";
                        }
                        break;
                    }
                }
                ResolvedTo to = resolve(msg.to(), ctx.from.domain);
                // now create necessary group & agents
                // and setup ctx.to
                if (to.tag == ResolvedTo::Tag::NONE) {
                    ctx.error = "Fail to resolve";
                    break;
                }
                else if (to.tag == ResolvedTo::Tag::AGENT) {
                    ctx.to.domain = ctx.from.domain;
                    ctx.to.agent = to.agent;
                }
                else if (to.tag == ResolvedTo::Tag::AGENT_CLONE) {
                    ctx.error = "Not supported.";
                    break;
                }
                else if (to.tag == ResolvedTo::Tag::DOMAIN) {
                    ctx.error = "Not supported.";
                    break;
                }
                else if (to.tag == ResolvedTo::Tag::DOMAIN_CLONE) {
                    ctx.error = "Not supported.";
                    break;
                }
                else if (to.tag == ResolvedTo::Tag::SNAPSHOT) {
                    ctx.error = "Not supported.";
                    break;
                }
                ctx.action = Action::CALL;
            }
        }
        while (false);
        if (ctx.action == Action::REWIND) {
        }
        saveContext(msg, ctx);
    }

    Agent *apply (Message const &msg) {
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
        to->memory.push_back(message_id);
        return to;
    }

// Runtime

    void updateMemory (Agent *);

#if 0
    void enqueue (Message &&msg) {
        auto *driver = dynamic_cast<LoopDriver *>(runtime->driver.get());
        CHECK(driver);
        driver->enqueue(std::move(msg));
    }
#endif

    void commit(json const &ops);

    // commands
    int cmd_exit (Message const &, json *resp) {
        stop_requested = true;
        return 0;
    }

    int cmd_list_agents (Message const &, json *resp) {
        json j = json::array();
        for (auto const &a: agents) {
            j.push_back(a->dump());
        }
        *resp = j;
        return 0;
    }

    int cmd_create_agents (Message const &, json *resp);

    int cmd_cost (Message const &, json *resp) {
        *resp = accounting.dump();
        return 0;
    }

public:
    struct Config {
        std::string journal_path;
        std::string resume_path;
    };

    Runtime(Config const &config, std::unique_ptr<Driver> &&user_driver)
        : initHook([this]() { initGlobal(); }),
        journal(config.journal_path, config.resume_path,
                  [this](Message &&msg) { 
                      if (msg.type() == "runtime:commit") {
                          protocol::runtime::Commit c(msg);
                          commit(c.ops);
                      } else {
                          apply(msg);
                      }
                }),
          stop_requested(false) {
        log::info("Initializing runtime");
        runtime->driver = std::make_unique<LoopDriver>(this);
        user->driver = std::move(user_driver);
        poller.add(runtime->driver->read_fd(), runtime->id);
        poller.add(user->driver->read_fd(), user->id);
    }

    ~Runtime() = default;

    json dump () const;

    void call (Message &&msg, Response &) override;

    void run ();
};

} // namespace postline

