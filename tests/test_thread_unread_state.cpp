#include <cassert>
#include <climits>

#include "tui/thread_unread_state.h"

using postline::make_access_id;
using postline::mark_receiving;
using postline::ui::ThreadUnreadState;

int main() {
    ThreadUnreadState state;
    auto first = make_access_id(1, 10);
    auto second = make_access_id(1, 20);

    assert(!state.hasUnread());
    assert(!state.isUnread(first));

    state.observe(first);
    state.observe(mark_receiving(second));
    assert(state.hasUnread());
    assert(state.isUnread(mark_receiving(first)));
    assert(state.isUnread(second));

    state.markRead(second);
    assert(state.hasUnread());
    assert(state.isUnread(first));
    assert(!state.isUnread(second));

    state.markRead(mark_receiving(first));
    assert(!state.hasUnread());

    return 0;
}
