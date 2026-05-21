#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
#include "common.h"
#include "journal.h"
#include "driver.h"
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
char constexpr ADDRESS_CHAR_DETACH = '&';
char constexpr ADDRESS_CHAR_CLONE  = '*';

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
    X(CLONE,   0x0000008)

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
    bool detached;      // detached domain are root of the subtree
                        // correspond to a thread
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
    Agent *agent;   // an agent might be communicating
    Domain *domain; // in a domain other than its home domain
                    // e.g. user, runtime
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
    Endpoint pending;
    // let's try this logic for now;
    // if stack is empty, pending is always (user, root)
    // later we might want to relax this to a user role rather than just user
    std::vector<Frame> stack;
    std::vector<AccessID> trace;

    Thread (ThreadID id_, Domain *root_, Agent *owner): id(id_), root(root_) {
        CHECK(!root->detached);
        pending.domain = root;
        pending.agent = owner;
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
                    {"pending_domain_id", pending.domain->id},
                    {"pending_agent_id", pending.agent->id},
                    {"stack", jstack},
                    {"trace", trace}};
    }
};

class Runtime;

struct Program: immobile {
    std::vector<std::unique_ptr<Domain>> domains;
    std::vector<std::unique_ptr<Agent>> agents;
    std::unordered_map<std::string, Snapshot> snapshots;
    std::vector<std::unique_ptr<Thread>> threads;

    Domain *global;
    Agent *zero;    // agent zero
    Agent *runtime;
    Agent *user;

    Program () {    // called in constructor via hook before journal replay
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

    json dump () const;

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

    struct ResolvedTo {
        enum class Tag: uint8_t {
            NONE = 0,       // only for rewind, or it is an error
            AGENT,          // resolved in the order below
            DOMAIN,
            SNAPSHOT,
        } tag;
        union {
            Agent *agent;
            Domain *domain;
            Snapshot *snapshot;
        };
        bool detach;
        bool clone;
        std::string error;
    };

    ResolvedTo resolve (std::string_view address, Domain *domain);

    enum class Action: uint8_t {
        YIELD = 0,          // not used for now
        CALL = 1,           // must have a to
        RETURN = 2,         // to not needed
        REWIND = 3          // to not needed
    };

    struct Context {
        Action action;
        Thread *thread;
        Endpoint from;
        Endpoint to;
        std::string error;
    };

    void saveContext (Message &msg, Context const &ctx) const;
    void loadContext (Message const &msg, Context &ctx);
    Message makeRewindMessage (Agent *fromAgent, Domain *fromDomain = nullptr, char const *error = "") const;

    void preprocess (Agent *from, Message &msg, Runtime *runtime);
    Agent *apply (Message const &msg);
};

class Runtime: public Program, public Service {

    Journal journal;
    Poller poller;
    Accounting accounting;
    bool stop_requested;

    struct SyscallResult {
        union {
        Agent *agent;
        Domain *domain;
        };
    };

    SyscallResult __commit (json const &op);

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

    void updateMemory (Agent *);

public:
    struct Config {
        std::string journal_path;
        std::string resume_path;
    };

    Runtime(Config const &config, std::unique_ptr<Driver> &&user_driver)
        : journal(config.journal_path, config.resume_path,
                  [this](Message &&msg) { 
                      if (msg.type() == "runtime:commit") {
                          protocol::runtime::Commit c(msg);
                          __commit(c.op);
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

    void call (Message &&msg, Response &) override;

    SyscallResult syscall (json const &op);

    void syscalls (json const &ops) {
        for (json const &j: ops.get_ref<json::array_t const &>()) {
            syscall(j);
        }
    }

    void run ();
};

} // namespace postline

