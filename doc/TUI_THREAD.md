# Updating the TUI Thread Tab

The thread tab is implemented by `ThreadTab` in:

- `src/tui/tabs.h`
- `src/tui/thread_tab.cpp`

Each `ThreadTab` is bound to a stable `Thread *` owned by the observer's
mirrored `Program`. The tab must read this observer-owned state, not state
owned by the live `Runtime`.

## State and Rendering

The top-level TUI renderer calls `Observer::process()` before rendering tab
content. Consequently, `ThreadTab` render callbacks may read `Thread`,
`Frame`, `Endpoint`, `Domain`, and `Agent` objects directly without locking.

Do not read or mutate observer state from background callbacks. Background
code should only enqueue messages for the observer.

Pointers stored in the mirrored program remain stable because program object
vectors hold `unique_ptr` objects. Do not compare these pointers with, or pass
them as substitutes for, pointers owned by the live runtime.

## Class Layout

Follow the repository class-order convention:

```cpp
class ThreadTab : public Tab {
private:
    // state, components, and helper methods

public:
    // constructor and Tab interface
};
```

Keep navigation data and synchronization helpers private. Add public methods
only when they are part of the tab interface.

## Left Navigation

The left pane has three modes:

- `tree`
- `stack`
- `members`

FTXUI `Menu` components hold pointers to their entry vectors. Keep the vectors
as `ThreadTab` members and update their contents in place. Do not replace the
vectors or create temporary vectors whose addresses would become invalid.

All three menus share `nav_selected`. After rebuilding the active mode's
entries, normalize this index:

```cpp
if (entries.empty()) {
    nav_selected = 0;
}
else {
    nav_selected = std::clamp(
        nav_selected, 0, int(entries.size()) - 1);
}
```

Only clamp for the active mode. Otherwise, rebuilding an inactive list could
unexpectedly move the selection in the visible list.

Run navigation synchronization from the left-pane render callback. At that
point observer messages have already been applied and the list reflects the
current program state.

## Domain Tree

The tree is rooted at:

```cpp
data->root
```

Traverse only `Domain::children`. Do not use domain members, parents, entries,
or other fields to construct the tree.

Render domains in preorder, with the root first and indentation indicating
depth. Sort each domain's children by name because `Domain::children` is an
unordered map.

Validate assumptions:

```cpp
CHECK(data->root);
CHECK(child);
CHECK(name == child->name);
```

## Call Stack

The stack is:

```cpp
data->stack
```

Iterate in vector order so the oldest frame appears first and the active frame
appears last. Render each frame as:

```text
from-agent@from-domain -> to-agent@to-domain
```

Both sides are `Endpoint` values. Check every pointer before formatting:

```cpp
CHECK(frame.from.agent);
CHECK(frame.from.domain);
CHECK(frame.to.agent);
CHECK(frame.to.domain);
```

The stack may grow or shrink whenever observer messages are applied. Rebuild
the entries during rendering and clamp the active selection.

## Current Domain and Members

The current domain follows the runtime's dispatch model:

```cpp
Domain const *domain = data->root;
if (!data->stack.empty()) {
    domain = data->stack.back().to.domain;
}
CHECK(domain);
```

The members mode displays:

- each `Domain::members` agent as `Agent::name`;
- each `Domain::children` domain as `@` followed by `Domain::name`.

Sort agent names and child-domain entries for stable display. Keep agents
before child domains. Validate map values and verify that map keys match
object names.

## Message Trace and Reader

The middle pane displays the thread's `trace`. The tab uses an observer-backed
list adapter so trace updates become visible after observer processing.

`reloadCurrentMessage()` derives an `AccessID` from the selected trace entry.
It avoids reloading when the id has not changed, retrieves the message through
the supplied callback, and resets reader scrolling after a change.

Keep journal access behind the callback. Do not make the thread tab depend on
the live runtime or journal implementation directly.

## Message Editor

The editor is bound to the same stable `Thread *`. Sending constructs a
message and invokes the supplied callback with the thread id. Preserve this
callback boundary when extending editor behavior.

Use `CHECK` for invalid selected indexes and other states that should be
impossible. Do not silently recover from violated internal invariants.

## Adding Thread-Tab State

When adding another live view:

1. Store its FTXUI entry vector and component in `ThreadTab`.
2. Add a private synchronization helper.
3. Derive entries directly from the observer-owned `Thread` and reachable
   program objects.
4. Validate required pointers and invariants with `CHECK`.
5. Sort unordered-map data when stable ordering matters.
6. Refresh from the appropriate render callback.
7. Clamp selection only when that view is active.
8. Update `src/tui/DESIGN.md`.

Avoid copying the full program with `Program::dump()` for normal rendering.
Use direct field access for the specific state the view needs.

## Verification

Build the affected target:

```sh
cmake --build build --target postline -j2
```

Also run:

```sh
git diff --check
```

For interactive changes, verify:

- empty, single-entry, and multi-entry lists;
- stack push and pop updates;
- domain creation and nested children;
- selection behavior when a list shrinks;
- stable ordering across repeated renders;
- narrow left-pane rendering and long names.
