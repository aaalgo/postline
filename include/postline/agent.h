#pragma once
#include <vector>
#include <memory>
#include <unordered_map>

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

class Driver;

struct Agent: immobile {
    AgentID id;
    AgentLink link;
    std::vector<AccessID> memory;
    std::unique_ptr<Driver> driver;

    explicit Agent(AgentID id_, AgentLink link_)
        : id(id_), link(std::move(link_)) {}
};

class AgentStore: immobile {
    std::vector<std::unique_ptr<Agent>> agents_;
public:
    AgentStore() {
    }

    AgentID spawn(AgentID parent, AccessID anchor = NO_ACCESS_ID) {

        if (parent != NOT_AN_AGENT && anchor == NO_ACCESS_ID) {
            Agent& p = get(parent);
            if (!p.memory.empty()) {
                anchor = p.memory.back();
            }
        }

        AgentID id = static_cast<AgentID>(agents_.size());

        agents_.push_back(std::make_unique<Agent>(agents_.size(), AgentLink{
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
}
