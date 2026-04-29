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

namespace postline {

using AgentID = std::int32_t;
using GroupID = std::int32_t;

inline constexpr AgentID NOT_AN_AGENT = -1;
inline constexpr AgentID ROOT_AGENT = 0;

inline constexpr GroupID NOT_A_GROUP = -1;

struct AgentLink {
    AgentID parent;
    AccessID anchor;
};

struct Agent: immobile {
    AgentLink link;
    std::vector<AccessID> memory;
    std::unique_ptr<Driver> driver;

    explicit Agent(AgentLink link_)
        : link(std::move(link_)) {}


};

class AgentStore: immobile {
    std::vector<std::unique_ptr<Agent>> agents_;
public:
    AgentStore() {
        agents_.push_back(std::make_unique<Agent>(AgentLink{
            .parent = NOT_AN_AGENT,
            .anchor = NO_ACCESS_ID,
        }));
    }

    AgentID spawn(AgentID parent, AccessID anchor = NO_ACCESS_ID) {
        Agent& p = get(parent);

        if (anchor == NO_ACCESS_ID && !p.memory.empty()) {
            anchor = p.memory.back();
        }

        AgentID id = static_cast<AgentID>(agents_.size());

        agents_.push_back(std::make_unique<Agent>(AgentLink{
            .parent = parent,
            .anchor = anchor,
        }));

        return id;
    }

    Agent& get(AgentID id) {
        CHECK(exists(id));
        return *agents_[static_cast<std::size_t>(id)];
    }

    Agent const& get(AgentID id) const {
        CHECK(exists(id));
        return *agents_[static_cast<std::size_t>(id)];
    }

    bool exists(AgentID id) const {
        return id >= 0 &&
               static_cast<std::size_t>(id) < agents_.size() &&
               agents_[static_cast<std::size_t>(id)] != nullptr;
    }

    std::size_t size() const {
        return agents_.size();
    }
};

struct Group: immobile {
    std::string name; // empty => anonymous / temporary group
    std::unordered_map<std::string, AgentID> hosts;

    explicit Group(std::string name_ = "")
        : name(std::move(name_)) {}
};

class GroupStore: immobile {
    std::vector<std::unique_ptr<Group>> groups_;
    std::unordered_map<std::string, GroupID> names_;

public:
    GroupStore() {
    }

    GroupID create(std::string name = "") {
        if (!name.empty()) {
            CHECK(names_.find(name) == names_.end());
        }

        GroupID id = static_cast<GroupID>(groups_.size());

        groups_.push_back(std::make_unique<Group>(name));

        if (!name.empty()) {
            names_.emplace(std::move(name), id);
        }

        return id;
    }

    Group& get(GroupID id) {
        CHECK(exists(id));
        return *groups_[static_cast<std::size_t>(id)];
    }

    Group const& get(GroupID id) const {
        CHECK(exists(id));
        return *groups_[static_cast<std::size_t>(id)];
    }

    bool exists(GroupID id) const {
        return id >= 0 &&
               static_cast<std::size_t>(id) < groups_.size() &&
               groups_[static_cast<std::size_t>(id)] != nullptr;
    }

    GroupID find(std::string const& name) const {
        auto it = names_.find(name);
        if (it == names_.end()) {
            return NOT_A_GROUP;
        }
        return it->second;
    }

    std::size_t size() const {
        return groups_.size();
    }
};

struct Address {
    std::string host;
    std::string domain;
};

class Runtime: immobile {
    AgentStore agents;
    GroupStore groups;
    Journal    journal;

    int wake_fd;
    // for async interaction
    mutable std::mutex mutex;
    std::vector<Message> pending;
    bool stop_requested;

public:
    struct Config {
        std::string journal_path;
        std::string journal_chain_path;
    };

    Runtime(Config const &config)
        : journal(config.journal_path,
                  config.journal_chain_path,
                  [this](Message const&msg) {
                        this->process(msg, true);
                  }),
        stop_requested(false)
    {
        wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        CHECK(wake_fd >= 0);
    }

    ~Runtime() {
        ::close(wake_fd);
    }

    void stop () {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stop_requested = true;
        }
        uint64_t one = 1;
        write(wake_fd, &one, sizeof(one));
    }

    void enqueue (Message &msg) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            pending.push_back(std::move(msg));
        }
        uint64_t one = 1;
        write(wake_fd, &one, sizeof(one));
    }

    void process (Message const &msg, bool replay) {

    }

    void run () {
        Poller poller;
        poller.add(wake_fd, NOT_AN_AGENT);

        for (;;) {
            auto events = poller.wait();
            CHECK(!events.empty());
            std::vector<Message> todo;
            for (auto const &e: events) {
                if (e.token < 0) {  // wake_fd
                    std::lock_guard<std::mutex> lock(mutex);
                    todo.swap(pending);
                    continue;
                }
                Agent &agent = agents.get(e.token);
                CHECK(agent.driver);
                todo.emplace_back(agent.driver->recv());
            }
            // all messages received
            {   // check stop
                std::lock_guard<std::mutex> lock(mutex);
                if (stop_requested) {
                    log::info("Stop requested, ignoring {} pending messages.", todo.size());
                    break;
                }
            }
            for (Message const &msg: todo) {
                process(msg, false);
            }
        }
        // gracefully shutdown all pending
        log::info("Make sure you come back to close drivers.");
    }

    AgentID spawn_agent(AgentID parent, AccessID anchor = NO_ACCESS_ID) {
        return agents.spawn(parent, anchor);
    }

    GroupID create_group(std::string name = "") {
        return groups.create(std::move(name));
    }

    void bind(GroupID group_id, std::string host, AgentID agent_id) {
        CHECK(agents.exists(agent_id));

        Group& group = groups.get(group_id);
        auto [it, inserted] = group.hosts.emplace(std::move(host), agent_id);
        CHECK(inserted);
    }

    AgentID resolve(Address const& addr) const {
        GroupID group_id = resolve_domain(addr.domain);
        CHECK(group_id != NOT_A_GROUP);

        Group const& group = groups.get(group_id);

        auto it = group.hosts.find(addr.host);
        CHECK(it != group.hosts.end());

        AgentID agent_id = it->second;
        CHECK(agents.exists(agent_id));

        return agent_id;
    }

    GroupID resolve_domain(std::string const& domain) const {
        if (domain.starts_with("g.")) {
            return parse_group_id(domain);
        }

        return groups.find(domain);
    }

    static GroupID parse_group_id(std::string const& domain) {
        CHECK(domain.starts_with("g."));

        std::string_view s(domain);
        s.remove_prefix(2);

        CHECK(!s.empty());

        GroupID id = 0;
        for (char c : s) {
            CHECK(c >= '0' && c <= '9');
            id = id * 10 + static_cast<GroupID>(c - '0');
        }

        return id;
    }

    static Address parse_address(std::string const& address) {
        auto pos = address.find('@');
        CHECK(pos != std::string::npos);
        CHECK(pos > 0);
        CHECK(pos + 1 < address.size());

        return Address{
            .host = address.substr(0, pos),
            .domain = address.substr(pos + 1),
        };
    }

    AgentID resolve(std::string const& address) const {
        return resolve(parse_address(address));
    }
};

} // namespace postline
