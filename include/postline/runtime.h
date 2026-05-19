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

    AgentParams (json const &j) {
        CHECK(0);
    }

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
    Domain *parent;
    Thread *thread;
    struct Entry {
        Agent *from;
        Agent *to;
    } entry;
    std::unordered_map<std::string, Agent *> members;
    std::unordered_map<std::string, Domain *> children;

    Domain (DomainID id_,
            std::string const &name_,
            Domain *parent_ = nullptr,
            Thread *thread_ = nullptr)
        : id(id_),
        name(name_),
        parent(parent_),
        thread(thread_)
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

struct Frame {
    AccessID opening_message_id = 0;
    Agent *opening_agent;

    json dump () const {
        return json{{"opening_message_id", opening_message_id},
                    {"opening_agent_id", opening_agent->id},
                    {"opening_agent_name", opening_agent->name}};
    }
};

struct Thread {
    ThreadID id;
    std::string name;
    std::vector<Frame> stack;
    std::vector<AccessID> trace;

    Thread (ThreadID id_): id(id_) {}

    json dump () const {
        json jstack = json::array();
        for (auto const &f: stack) {
            jstack.push_back(f.dump());
        }
        return json{{"id", id},
                    {"name", name},
                    {"stack", jstack},
                    {"trace", trace}};
    }
};

struct Program {
    std::vector<std::unique_ptr<Domain>> domains;
    std::vector<std::unique_ptr<Agent>> agents;
    std::unordered_map<std::string, Snapshot> snapshots;
    std::vector<std::unique_ptr<Thread>> threads;

    Domain *global;
    Agent *zero;    // agent zero
    Agent *runtime;
    Agent *user;

    Program () {
        // hard-coded initial state:
        //      thread-0  --  domain-0 -- {runtime, user, agent}
        Thread *thread = createThread();
        // root cannot be created with createDomain
        // all other domains must be created with createDomain
        domains.emplace_back(std::make_unique<Domain>(domains.size(), DOMAIN_NAME_GLOBAL, nullptr, thread));
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
    }

    json dump () const;

    Thread *createThread () {
        threads.emplace_back(std::make_unique<Thread>(threads.size()));
        return threads.back().get();
    }

    Agent *createAgent (AgentParams const &params, Domain *domain) {
        CHECK(domain->getMember(params.name) == nullptr);
        agents.emplace_back(std::make_unique<Agent>(params, agents.size(), domain));
        Agent *agent = agents.back().get();
        domain->members[agent->name] = agent;
        return agent;
    }

    Domain *createDomain (Snapshot const &snapshot, Domain *parent) {
        DomainID id = domains.size();
        std::string name = std::format("domain-{}", id);
        CHECK(parent->getChild(name) == nullptr);
        domains.emplace_back(std::make_unique<Domain>(id, name, parent, parent->thread));
        Domain *domain = domains.back().get();
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
        Agent *from;
        ResolvedTo to;
        std::string error;
    };

    void saveContext (Message &msg, Context const &ctx) const {
        msg.updateHeader([this, &ctx](json &h) {
            json j{
                {"thread_id", ctx.thread->id},
                {"action", static_cast<int>(ctx.action)},
                {"from_agent_id", ctx.from->id},
                {"to_tag", static_cast<int>(ctx.to.tag)},
                {"error", ctx.error}
                };
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
            h[CONTEXT_HEADER_NAME] = j;
        });
    }

    void loadContext (Message const &msg, Context &ctx) {
        json j = msg.header()[CONTEXT_HEADER_NAME];
        ThreadID thread_id = j["thread_id"].get<ThreadID>();
        CHECK(thread_id >= 0 && thread_id < threads.size());
        ctx.thread = threads[thread_id].get();
        ctx.action = static_cast<Action>(j["action"].get<int>());
        AgentID from_agent_id = j["from_agent_id"].get<AgentID>();
        CHECK(from_agent_id >= 0 && from_agent_id < agents.size());
        ctx.from = agents[from_agent_id].get();
        ctx.to.tag = static_cast<ResolvedTo::Tag>(j["to_tag"].get<int>());
        if (ctx.to.tag == ResolvedTo::Tag::AGENT || ctx.to.tag == ResolvedTo::Tag::AGENT_CLONE) {
            AgentID id = j["to_agent_id"].get<AgentID>();
            CHECK(id >= 0 && id < agents.size());
            ctx.to.agent = agents[id].get();
        }
        else if (ctx.to.tag == ResolvedTo::Tag::DOMAIN || ctx.to.tag == ResolvedTo::Tag::DOMAIN_CLONE) {
            DomainID id = j["to_domain_id"].get<DomainID>();
            CHECK(id >= 0 && id < domains.size());
            ctx.to.domain = domains[id].get();
        }
        else if (ctx.to.tag == ResolvedTo::Tag::SNAPSHOT) {
            std::string name = j["to_snapshot"].get<std::string>();
            auto it = snapshots.find(name);
            CHECK(it != snapshots.end());
            ctx.to.snapshot = &it->second;
        }
        ctx.error = j["error"].get<std::string>();
    }

    Message makeRewindMessage (Agent *from) const {
        Message msg;
        Context ctx;
        ctx.thread = from->domain->thread;   // cannot rewind from global agents
        CHECK(ctx.thread, "global agents should not EOF");
        ctx.action = Action::REWIND;
        ctx.from = from;
        ctx.to.tag = ResolvedTo::Tag::NONE;
        ctx.error = "adapter of agent has died.";
        saveContext(msg, ctx);
        return msg;
    }

    void preprocess (Agent *from, Message &msg) {
        // create context & save to msg
        // update headers when necessary
        Context ctx;
        ctx.action = Action::REWIND;    // fail by default
        int thread_id = msg.thread_id();
        if (!(thread_id >= 0 && thread_id < threads.size())) {
            log::error("Bad thread id {}", thread_id);
            thread_id = 0;
        }
        ctx.thread = threads[thread_id].get();
        if (from->domain->thread) {
            if (ctx.thread != from->domain->thread) {
                log::warn("thread not matching");
            }
        }
        ctx.from = from;
        ctx.to = resolve(msg.to(), from->domain);

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
                ctx.to.tag = ResolvedTo::Tag::AGENT;
                ctx.to.agent = f.opening_agent;
                ctx.action = Action::RETURN;
            }
            else {
                if (ctx.to.tag == ResolvedTo::Tag::NONE) {
                    ctx.error = "Fail to resolve";
                    break;
                }
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
                        ctx.error = "Spontaneus message on non empty stack";
                        break;
                    }
                }
                ctx.action = Action::CALL;
            }
        }
        while (false);
        saveContext(msg, ctx);
    }

    Agent *apply (Message const &msg) {
        // now msg has an id
        Context ctx;
        Agent *to = nullptr;
        loadContext(msg, ctx);
        if (ctx.action == Action::RETURN) {
            ctx.thread->stack.pop_back();
            --ctx.from->obligation_count;
            CHECK(ctx.to.tag == ResolvedTo::Tag::AGENT);
            to = ctx.to.agent;
        }
        else if (ctx.action == Action::CALL) {
            ctx.thread->stack.emplace_back();
            auto &f = ctx.thread->stack.back();
            f.opening_message_id = msg.access_id();
            f.opening_agent = ctx.from;
            // realize cloning and resolving
            if (ctx.to.tag == ResolvedTo::Tag::AGENT) {
                to = ctx.to.agent;
            }
            else if (ctx.to.tag == ResolvedTo::Tag::AGENT_CLONE) {
                AgentParams params = ctx.to.agent->snapshot();
                params.name = std::format("{}_{}", ctx.to.agent->name, agents.size());
                to = createAgent(params, ctx.from->domain);
            }
            else if (ctx.to.tag == ResolvedTo::Tag::DOMAIN) {
                to = ctx.to.domain->entry.to;
            }
            else if (ctx.to.tag == ResolvedTo::Tag::DOMAIN_CLONE) {
                std::string name = std::format("{}_{}", ctx.to.domain->name, domains.size());
                Snapshot snapshot = ctx.to.domain->snapshot(name);
                Domain *domain = createDomain(snapshot, ctx.from->domain);
                to = ctx.to.domain->entry.to;
            }
            else if (ctx.to.tag == ResolvedTo::Tag::SNAPSHOT) {
                Domain *domain = createDomain(*ctx.to.snapshot, ctx.from->domain);
                to = domain->entry.to;
            }
            else CHECK(0);
            CHECK(to);
            ++to->obligation_count;
        }
        else if (ctx.action == Action::REWIND) {
            --ctx.from->obligation_count;
            while (ctx.thread->stack.size()) {
                auto const &f = ctx.thread->stack.back();
                if (f.opening_agent->flags & AGENT_FLAG_CATCH) {
                    to = f.opening_agent;
                    ctx.thread->stack.pop_back();
                    break;
                }
                --f.opening_agent->obligation_count;
                ctx.thread->stack.pop_back();
            }
        }
        else {
            CHECK(0);
        }
        CHECK(ctx.from);
        AccessID message_id = msg.access_id();
        ctx.thread->trace.push_back(message_id);
        ctx.from->memory.push_back(message_id);
        to->memory.push_back(message_id);
        return to;
    }

};

class Runtime: immobile, public Service {
    Program program;
    Journal journal;
    Poller poller;
    Accounting accounting;
    bool stop_requested;


#if 0
    void dump (std::string const &path) const;
    void createAgent (Message const &msg);
#endif

    void updateMemory (Agent *);

    void enqueue (Message &&msg) {
        auto *driver = dynamic_cast<LoopDriver *>(program.runtime->driver.get());
        CHECK(driver);
        driver->enqueue(std::move(msg));
    }

    void commit (json const &ops);

    void replay (Message &&msg) {
      if (msg.type() == "runtime:commit") {
          protocol::runtime::Commit c(msg);
          commit(c.ops);
      } else {
          program.apply(msg);
      }
    }

public:
    struct Config {
        std::string journal_path;
        std::string resume_path;
    };

    Runtime(Config const &config, std::unique_ptr<Driver> &&user_driver)
        : journal(config.journal_path, config.resume_path,
                  [this](Message &&msg) { replay(std::move(msg));
                }),
          stop_requested(false) {
        log::info("Initializing runtime");
        program.runtime->driver = std::make_unique<LoopDriver>(this);
        program.user->driver = std::move(user_driver);
        poller.add(program.runtime->driver->read_fd(), program.runtime->id);
        poller.add(program.user->driver->read_fd(), program.user->id);
    }

    ~Runtime() = default;

    void call (Message &&msg, Response &) override;

    void run ();
};

} // namespace postline
