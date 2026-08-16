#pragma once

#include <unordered_set>

#include <postline/common.h>

namespace postline { namespace ui {

class ThreadUnreadState {
    std::unordered_set<AccessID> unread;

public:
    void observe(AccessID id) {
        CHECK(id != NO_ACCESS_ID);
        unread.insert(unmark_receiving(id));
    }

    void markRead(AccessID id) {
        if (id == NO_ACCESS_ID) {
            return;
        }
        unread.erase(unmark_receiving(id));
    }

    bool isUnread(AccessID id) const {
        if (id == NO_ACCESS_ID) {
            return false;
        }
        return unread.contains(unmark_receiving(id));
    }

    bool hasUnread() const {
        return !unread.empty();
    }
};

}}
