# Accessing Program State From the TUI

 src/tui/tui.cpp

`TUI` is a `Program`, indirectly through `Observer`:

```text
TUI
|- UI
`- Observer
   `- Program
```

The declarations that establish this relationship are:

```cpp
class TUI: public UI, public Observer {
    // ...
};

class Observer: public Program {
    // ...
};
```

Consequently, code in a `TUI` member function can directly inspect the public
state and call the public methods declared by `Program`. This includes:

- `domains`, `agents`, `snapshots`, and `threads`;
- `global`, `zero`, `runtime`, and `user`;
- `dump()` and `resolve()`;
- the state reachable through `Domain`, `Agent`, and `Thread` objects.

## What the Observer Mirrors

The inherited `Program` is a local replica of program state, not the
`Program` base subobject inside the live `Runtime`.

The runtime sends journaled messages to the callback returned by
`TUI::consume()`. That callback enqueues each message with
`Observer::consume()`. On the FTXUI thread, the top-level renderer calls
`Observer::process()`, which applies every queued message to the inherited
program:

```cpp
for (auto &m: local) {
    Program::apply(m);
    cache[m.access_id()] = std::move(m);
}
```

`Program::apply()` updates the replica in the same way that journal messages
update the runtime's program model. Commit messages create agents, domains, or
threads, including domains instantiated from an existing snapshot. Ordinary
messages update thread stacks, traces, pending status, agent memories, and
obligation counts.

This gives the TUI a coherent program snapshot after `Observer::process()`
returns. It has two important consequences:

1. The replica can lag behind incoming traffic while messages remain in
   `Observer::pendings`.
2. Pointers in the replica refer to observer-owned objects. They must not be
   compared with, or passed as substitutes for, pointers owned by the live
   `Runtime`.

The render callback already calls `Observer::process()` before
`syncObserver()` and before rendering tab content, so TUI rendering code can
read the inherited state without taking `Observer::mutex`. Background
callbacks must only enqueue messages; they must not read or mutate the
replica.

## Name Qualification

Prefer explicit `Program::` or `Observer::` qualification in TUI code. It
makes the inheritance visible and avoids confusing unrelated state.

In particular, `TUI` declares:

```cpp
GlobalData global;
```

This hides `Program::global`, which is a `Domain *`. Therefore:

```cpp
global                 // TUI::global, the Global tab's presentation data
Program::global        // the observer replica's global Domain
Observer::threads      // the observer replica's Program::threads
```

`UI` also maintains private per-thread request bookkeeping. It is separate
from `Program::threads`, which contains the mirrored runtime threads.

## Access Examples

### Program-wide counts

Inside any `TUI` member function:

```cpp
size_t domain_count = Program::domains.size();
size_t agent_count = Program::agents.size();
size_t thread_count = Observer::threads.size();
size_t snapshot_count = Program::snapshots.size();
```

The vectors use their object ids as indexes. Check an externally supplied id
before indexing:

```cpp
CHECK(thread_id >= 0);
CHECK(thread_id < int(Observer::threads.size()));
Thread const *thread = Observer::threads[thread_id].get();
CHECK(thread);
```

### Thread status

`Thread` exposes its current call stack, trace, root domain, and whether it is
waiting for the user:

```cpp
Thread const *thread = Observer::threads[thread_id].get();

bool pending = thread->pending;
size_t stack_depth = thread->stack.size();
size_t message_count = thread->trace.size();
Domain const *root = thread->root;
std::string const &name = thread->name;
```

For example, a compact status label can be built directly from the mirror:

```cpp
Thread const *thread = Observer::threads[thread_id].get();
std::string status = std::format(
    "thread {}: {}, stack={}, messages={}",
    thread->id,
    thread->pending ? "pending" : "idle",
    thread->stack.size(),
    thread->trace.size());
```

The top frame describes the active call when the stack is non-empty:

```cpp
if (!thread->stack.empty()) {
    Frame const &frame = thread->stack.back();
    CHECK(frame.from.agent);
    CHECK(frame.from.domain);
    CHECK(frame.to.agent);
    CHECK(frame.to.domain);

    std::string call = std::format(
        "{}@{} -> {}@{}",
        frame.from.agent->name,
        frame.from.domain->name,
        frame.to.agent->name,
        frame.to.domain->name);
}
```

### Global agents and domains

Use qualification for the global domain because of `TUI::global`:

```cpp
Domain const *global_domain = Program::global;
CHECK(global_domain);

Agent const *user_agent = Program::user;
Agent const *runtime_agent = Program::runtime;
Agent const *zero_agent = Program::zero;
CHECK(user_agent);
CHECK(runtime_agent);
CHECK(zero_agent);
```

Domain membership and child domains are available from the mirrored domain:

```cpp
Agent *member = Program::global->getMember("user");
CHECK(member == Program::user);

Domain *child = Program::global->getChild("worker-domain");
if (child) {
    size_t member_count = child->members.size();
    size_t child_count = child->children.size();
}
```

### Agent status

Agents expose their identity, home domain, lifecycle state, flags, and
mirrored message history:

```cpp
CHECK(agent_id >= 0);
CHECK(agent_id < int(Program::agents.size()));
Agent const *agent = Program::agents[agent_id].get();
CHECK(agent);
CHECK(agent->domain);

std::string const &name = agent->name;
std::string const &domain_name = agent->domain->name;
bool dead = agent->dead;
bool has_memory = (agent->flags & AGENT_FLAG_MEMORY) != 0;
size_t memory_size = agent->memory.size();
int obligations = agent->obligation_count;
```

The `driver` field is runtime-only. It is not meaningful in the observer
replica and should not be used by the TUI.

### Address resolution

The inherited `resolve()` method resolves an address against the mirrored
state:

```cpp
Program::ResolvedAddress result =
    Program::resolve(address, thread->root);

if (result.error.empty()
    && result.tag == Program::ResolvedAddress::Tag::AGENT) {
    CHECK(result.agent);
    std::string const &resolved_name = result.agent->name;
}
```

Callers should handle every `ResolvedAddress::Tag` they accept. An unresolved
or invalid address has tag `NONE`; constraint failures also populate
`result.error`.

### JSON snapshot

For diagnostics or a state inspector, `Program::dump()` serializes the whole
mirrored model:

```cpp
json state = Program::dump();
log::info("TUI program state: {}", state.dump());
```

This is convenient but constructs the complete JSON representation. Direct
field access is preferable for normal rendering paths that need only a small
part of the state.

## Recommended TUI Pattern

Read program state from renderers or helper methods invoked after
`Observer::process()` on the FTXUI thread:

```cpp
main_renderer = Renderer(main_container, [&] {
    Observer::process();
    syncObserver();
    drainLogs();

    CHECK(Program::global);
    size_t agent_count = Program::agents.size();
    size_t domain_count = Program::domains.size();

    return renderMain(agent_count, domain_count);
});
```

When a tab needs program state, either pass it stable pointers owned by the
observer replica, as `ThreadTab` does with `Thread`, or pass a callback that
reads the state on the UI thread. Raw pointers to domains, agents, and threads
remain stable for the lifetime of the replica because the vectors hold
`unique_ptr` objects.
