#pragma once
#include <vector>
#include <memory>
#include <stack>
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

using AgentFlags = std::uint64_t;

AgentFlags constexpr AGENT_FLAG_CLONE = 0x00000001;
AgentFlags constexpr AGENT_FLAG_THREAD = 0x00000002;
AgentFlags constexpr AGENT_FLAG_CATCH = 0x00000004;
AgentFlags constexpr AGENT_FLAG_HISTORY = 0x00000008;

struct Agent: immobile {
    AgentID id;
    AgentLink link;
    std::string address;
    std::string service;
    AgentFlags flags;
    int next_clone_id;
    std::vector<AccessID> memory;
    std::unique_ptr<Driver> driver;
    int obligation_count;
    // important:
    // expecting is the protocol with driver


    explicit Agent(AgentID id_, AgentLink link_, std::string const &address_, std::string const &service_, AgentFlags flags_)
        : id(id_),
        link(std::move(link_)),
        address(address_),
        service(service_),
        flags(flags_),
        next_clone_id(0),
        obligation_count(0)
    {}

    AccessID anchor () const {
        if (memory.empty()) return NO_ACCESS_ID;
        return memory.back();
    }

    json dump () const {
        json flag_strings = json::array();
        if (flags & AGENT_FLAG_CLONE) {
            flag_strings.push_back("clone");
        }
        if (flags & AGENT_FLAG_THREAD) {
            flag_strings.push_back("thread");
        }
        if (flags & AGENT_FLAG_CATCH) {
            flag_strings.push_back("catch");
        }
        if (flags & AGENT_FLAG_HISTORY) {
            flag_strings.push_back("history");
        }
        return json{
            {"id", id},
            {"link", {
                {"parent", link.parent},
                {"anchor", link.anchor}
            }},
            {"address", address},
            {"service", service},
            {"flags", flag_strings},
            {"oblication_count", obligation_count},
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
    std::unordered_map<std::string, AgentID> lookup_;
public:
    AgentStore() {
    }

    json dump () const {
	std::cerr << "lookup" << std::endl;
	for (auto const &p: lookup_) {
		std::cerr << p.first  << ": " << p.second << std::endl;
	}
        json j = json::array();
        for (auto const &p: agents_) {
            j.push_back(p->dump());
        }
        return j;
    }

    AgentID spawn(std::string const &address, AgentID parent = NOT_AN_AGENT, AccessID anchor = NO_ACCESS_ID, std::string service = std::string(), AgentFlags flags = 0) {
        AgentID id = static_cast<AgentID>(agents_.size());
        auto [it, inserted] = lookup_.insert(std::make_pair(address, id));
        if (!inserted) return NOT_AN_AGENT;   // already exists
                                              //

        if (parent != NOT_AN_AGENT) { // && anchor == NO_ACCESS_ID) {
            Agent& p = get(parent);
            if (service.empty()) {
                service = p.service;
            }
            if (anchor == NO_ACCESS_ID) {
                anchor = p.anchor();
            }
            if (flags & AGENT_FLAG_CLONE) {
                CHECK(!p.flags & AGENT_FLAG_CLONE, "a clone of a cloning parent cannot be cloning");
            }
        }

        agents_.push_back(std::make_unique<Agent>(id, AgentLink{
            .parent = parent,
            .anchor = anchor,
        }, address, service, flags));

        return id;
    }

    AgentID find(std::string const& name) const {
        auto it = lookup_.find(name);
        if (it == lookup_.end()) {
            return NOT_AN_AGENT;
        }
        return it->second;
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

#if 0
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
#endif
}
