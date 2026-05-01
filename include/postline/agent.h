#pragma once
#include <vector>
#include <memory>
#include <unordered_map>
#include "driver.h"

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
    std::string service;
    std::string address;
    std::vector<AccessID> memory;
    std::unique_ptr<Driver> driver;
    bool waiting_response;

    explicit Agent(AgentID id_, AgentLink link_, std::string const &service_)
        : id(id_),
        link(std::move(link_)),
        service(service_),
        waiting_response(false)
    {}

    AccessID anchor () const {
        if (memory.empty()) return NO_ACCESS_ID;
        return memory.back();
    }

    json dump () const {
        return json{
            {"id", id},
            {"link", {
                {"parent", link.parent},
                {"anchor", link.anchor}
            }},
            {"service", service},
            {"address", address},
            {"memory", memory}
        };
    }

    ~Agent() {
        if (driver) {
            log::error("Deleting agent {} with open driver.", id);
        }
    }
};

class AgentStore: immobile {
    std::vector<std::unique_ptr<Agent>> agents_;
public:
    AgentStore() {
    }

    json dump () const {
        json j = json::array();
        for (auto const &p: agents_) {
            j.push_back(p->dump());
        }
        return j;
    }

    AgentID spawn(AgentID parent, AccessID anchor = NO_ACCESS_ID) {

        std::string service;
        if (parent != NOT_AN_AGENT && anchor == NO_ACCESS_ID) {
            Agent& p = get(parent);
            service = p.service;
            anchor = p.anchor();
        }

        AgentID id = static_cast<AgentID>(agents_.size());

        agents_.push_back(std::make_unique<Agent>(id, AgentLink{
            .parent = parent,
            .anchor = anchor,
        }, service));

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

    json dump () const {
        json j;
        j["name"] = name;

        std::vector<std::pair<std::string, AgentID>> items(
            hosts.begin(), hosts.end()
        );

        std::sort(items.begin(), items.end(),
            [](auto const& a, auto const& b) {
                return a.first < b.first;
            });

        nlohmann::ordered_json h = nlohmann::ordered_json::object();
        for (auto const& [k, v] : items) {
            h[k] = v;
        }

        j["hosts"] = std::move(h);

        return j;
    }


    explicit Group(std::string name_ = "")
        : name(std::move(name_)) {}
};

class GroupStore: immobile {
    std::vector<std::unique_ptr<Group>> groups_;
    std::unordered_map<std::string, GroupID> names_;

public:
    GroupStore() {
    }

    json dump () const {
        json j = json::array();
        for (auto const &p: groups_) {
            j.push_back(p->dump());
        }
        return j;
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
