#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "common.h"
#include "agent.h"

namespace postline {

using SessionID = int64_t;
static constexpr SessionID NOT_A_SESSION = -1;

struct CallStackEntry {

    AccessID access_id; // present message
    AgentID  agent_id;  // sent to agent_id, waiting for its reply
                        //
    bool operator==(CallStackEntry const& other) const noexcept {
        return access_id == other.access_id &&
               agent_id   == other.agent_id;
    }

    json dump() const {
        return json{access_id, agent_id};
    }
};

struct Session: noncopyable {
    std::vector<CallStackEntry> stack;
    std::vector<AccessID> trace;

    // a session can only have at most one outstanding token
    // and it's owner is stack.back().agent_id

    json dump () const {
        json j;
        {
            json arr = json::array();
            for (auto const &e: stack) {
                arr.push_back(e.dump());
            }
            j["stack"] = std::move(arr);
        }
        {
            json arr = json::array();
            for (auto id: trace) {
                arr.push_back(id);
            }
            j["trace"] = std::move(arr);
        }
        return j;
    }
};


} // namespace postline
