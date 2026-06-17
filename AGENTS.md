# Style

Always declare class in the following order:

class Dummy {
// private stuff
// it appears first because you need to read this first
// to understand the public stuff
public:
// public stuff
};

# Postline Runtime Overview

Postline is an AI agent runtime built around durable message passing. Agents,
the runtime itself, and the user interface all communicate by exchanging
`postline::Message` values. Every meaningful runtime transition is represented
as a message, appended to a journal, and replayed into an in-memory program
state.

This document summarizes the code in `include/postline/*` and top-level
`src/*.cpp`.

## Runtime Shape

The runtime has three major layers:

- `Message` and `Journal`: binary framing, JSON headers, raw bodies, durable
  append/read, and replay.
- `Program`: pure in-memory state for agents, domains, snapshots, threads, and
  call stacks.
- `Runtime`: the active event loop that owns drivers, polls agent file
  descriptors, preprocesses messages, writes the journal, applies messages, and
  dispatches them to the next agent.

The process entry point in `src/main.cpp` creates a UI service, constructs a
`Runtime`, starts `Runtime::run()` on a background thread, optionally initializes
the arena, and then runs the selected UI on the main thread.

## Messages

`postline::Message` is the fundamental data unit. It has:

- a JSON header;
- a raw string body;
- an `AccessID`, which is assigned only after journal append or pread replay.

The serialized representation is implemented in `src/common.cpp`:

```text
RecordHeader {
    magic       = "POST"
    header_size = byte length of JSON header
    body_size   = byte length of raw body
    crc         = CRC32(header + body)
}
header JSON bytes
body bytes
```

`Message::read(fd)` is used for stream I/O with adapters. `Message::read(fd,
offset, segment)` is used for journal reads and returns a message with a valid
`AccessID`.

Common logical headers include:

- `type`: protocol message type;
- `From`, `To`, `Cc`, `Subject`: email-like routing and presentation headers;
- `Thread-ID`: selected thread;
- `Message-ID`: assigned from the journal `AccessID`;
- `In-Reply-To`: marks a return to the caller at the top call frame;
- `In-Response-To`: marks a continued call sequence related to the top frame;
- `__context`: runtime-owned routing authority added during preprocessing;
- `Postline-Cost:*`: accounting values accumulated by `Accounting`.

The body is intentionally not parsed by the runtime for normal agent traffic.
Protocol structs parse only the pieces they own.

## Access IDs

`AccessID` identifies a message on disk. It encodes a journal segment and byte
offset. The current implementation uses:

- high bit: local "receiving" marker;
- segment bits;
- offset bits.

`mark_receiving()` and `unmark_receiving()` are bookkeeping helpers for memory
views. They do not change message identity on disk. Code that compares message
identity should unmark first or avoid comparing marked IDs directly.

## Journal

`Journal` is an append-only segmented log. Each segment starts with a
`journal:root` message that points to the previous segment path. On startup the
journal discovers the chain from the resume path backward, opens the segments in
chronological order, and replays every message after each segment root.

Two modes are supported:

- read/write mode when `journal_path` is provided: a new segment is created and
  linked to `resume_path`;
- read-only replay mode when only `resume_path` is provided.

Appending a message assigns its `AccessID`, writes the binary record, and
updates the message header's `Message-ID`.

## Protocol Messages

Protocol wrappers live in `include/postline/protocol.h`.

Journal-local messages:

- `journal:root`: first record in a segment; links to the previous segment.

Runtime state messages:

- `runtime:commit`: durable structural operation. Its body is a JSON object.
- `runtime:begin_shutdown` / `runtime:end_shutdown` / `runtime:flush`: declared
  protocol messages, though shutdown currently uses `runtime:commit` ops.

Handshake messages:

- `handshake:hello`: adapter/server startup handshake.
- `handshake:begin_memory` / `handshake:end_memory`: delimit memory replay sent
  to an adapter.
- `handshake:multi`: reserved but not implemented in `Server::run()`.
- `handshake:bye`: graceful adapter shutdown.

Handshake messages are transport control messages; they are not journaled as
normal runtime traffic.

## Program State

`Program` is the replayable model. It owns:

- `domains`: hierarchical communication scopes;
- `agents`: named entities with service strings, flags, memory, lineage, and
  obligation counts;
- `snapshots`: reusable domain templates;
- `threads`: independent call stacks and traces.

The initial state is hard-coded:

- one global domain named `global`;
- three global agents: `zero`, `runtime`, and `user`;
- the user has `CATCH` and `THREAD` flags;
- the global entry route is `user -> zero`;
- thread 0 is rooted at the global domain.

### Agents

Agents are created from `AgentParams`:

- `link`: parent agent and anchor access ID for inherited memory;
- `name`;
- `comment`;
- `service`: driver spec such as `pipe:echo`;
- `flags`.

Supported flags are:

- `MEMORY`: send historical memory to the adapter on driver startup;
- `CATCH`: may catch rewinds;
- `THREAD`: permission marker for creating detached domains;
- `CLONE`: address resolution should clone this agent before delivery.

Each agent keeps a `memory` vector of access IDs. Sent messages are stored as
plain IDs. Received messages are stored with the receiving bit set.

### Domains and Snapshots

A `Domain` is a named scope with members and child domains. An attached domain
has entry agents `from` and `to`; when a message targets a domain, it is routed
to the domain's `entry.to`.

A detached domain becomes the root of a `Thread`. The code treats detached
domains as thread roots.

A `Snapshot` stores a domain template: member agent params plus the entry
agent names. Snapshot calls can create fresh domains from the template.

### Threads and Frames

A `Thread` owns:

- root domain;
- pending bit used by UI/user flow;
- stack of call `Frame`s;
- trace of message access IDs.

A `Frame` records the calling message ID plus from/to endpoints. `CALL` pushes a
frame. `RETURN` pops the top frame. `REWIND` unwinds until a catching agent or
an empty stack is reached.

## Address Resolution

`Program::resolve()` interprets `To` addresses relative to the sender's current
domain.

Resolution order:

1. agent in the current domain;
2. agent in the global domain;
3. child domain of the current domain;
4. snapshot by name.

Address prefixes modify behavior:

- `&name`: detach a domain or snapshot;
- `*name`: clone an agent or domain.

Agents with `AGENT_FLAG_CLONE` automatically request clone delivery even without
the `*` prefix.

Current constraints:

- detach is allowed only for domains and snapshots;
- clone is allowed only for agents and domains;
- cloned domains are recognized by resolution but not implemented in message
  preprocessing;
- snapshot delivery creates a new domain from the snapshot.

## Message Preprocessing and Apply

`Program::preprocess()` converts an incoming agent message into an authoritative
runtime context saved under the `__context` header. It validates the expected
sender, resolves the target, creates clones or snapshot domains when needed via
runtime syscalls, and chooses an action:

- `CALL`: target another agent/domain/snapshot;
- `RETURN`: reply to the current top frame;
- `REWIND`: error path or explicit synthetic rewind;
- `YIELD`: declared but not used.

`Program::apply()` mutates replayable state using the saved context:

- `runtime:commit` is passed to `Program::commit()`;
- `CALL` pushes a frame, increments target obligation count, and routes to the
  target agent;
- `RETURN` pops a frame, decrements the sender obligation count, and routes to
  the caller;
- `REWIND` walks back through frames until it finds an agent with `CATCH` or the
  stack empties.

Every applied normal message is appended to the thread trace, written into the
sender memory, and written into the receiver memory with the receiving bit set.

## Runtime Commit Operations

Structural mutations are made durable through `runtime:commit` messages. The
body is a JSON object with `op`.

Implemented ops:

- `create_agent`: add an agent to a domain;
- `create_domain`: add a child domain;
- `create_domain_snapshot`: instantiate a domain from a named snapshot;
- `create_snapshot`: store a snapshot from an existing domain;
- `create_thread`: detach a domain into a new thread;
- `begin_shutdown`, `end_shutdown`: recognized no-op structural markers.

`Runtime::syscall()` wraps an op in `runtime:commit`, appends it to the journal,
applies it immediately, and forwards the committed message to observers.

## Runtime Event Loop

`Runtime` extends both `Program` and `LinearService`. It is therefore both the
owner of active drivers and an addressable service named `runtime`.

Startup:

1. construct `Program` initial state;
2. construct `Journal`, which replays prior messages into `Runtime::apply()`;
3. create loopback drivers for `runtime` and `user`;
4. poll the runtime and user driver fds.

Main loop:

1. `Poller` waits on agent driver fds;
2. ready drivers return one or more messages;
3. each message is preprocessed, unless it is a synthetic rewind;
4. the message is appended to the journal;
5. accounting headers are accumulated;
6. the message is applied to program state;
7. if the recipient has no driver yet, the runtime creates one from the
   recipient's `service` string and optionally sends memory;
8. the recipient driver receives the message;
9. observers receive the journaled message via `consume`.

If a driver reaches EOF, the agent is marked dead. User EOF stops the runtime.
Other agent EOF creates a rewind message.

Shutdown:

- append `begin_shutdown`;
- wait for outstanding agent obligations and append trailing responses without
  processing them further;
- call each driver's `shutdown()`;
- append `end_shutdown`.

## Drivers and Services

`Driver` is the runtime side of an agent connection:

- `send(Message const&)`;
- `recv(std::vector<Message>&)`;
- `shutdown()`;
- optional `read_fd()` for polling.

Implemented drivers:

- `ShellDriver`: forks `/bin/sh -c <command>`, speaks Postline records over
  pipes, and requires a `handshake:hello` from the child.
- `LoopDriver`: in-process service adapter backed by `eventfd`; used for the
  runtime service and UI/user service.

`create_driver()` currently supports `pipe:<adapter>`, resolved under
`$POSTLINE_HOME/bin/adapters/<adapter>`.

`Service` is the adapter-side interface. `LinearService` is a helper for
request/response style services. It maintains its own stack, fills standard
reply headers, and requires each call to append exactly one response.

`Server` is the standalone adapter transport harness. It sends
`handshake:hello`, forwards messages to a `Service`, handles memory replay
blocks, and exits on `handshake:bye`. It can read/write stdio, files, or a
single TCP connection.

## Runtime Message API

Messages addressed to `runtime` are parsed in `Runtime::call()` with CLI11.
The command is taken from the message `Subject`.

Supported commands:

- `exit`: request runtime stop;
- `cost`: return accumulated accounting JSON;
- `list_agents [-a|--all]`: list agents in the caller's domain or all agents;
- `create_agents`: create agents from a JSON array body;
- `create_domain`: create a child domain, optionally detached;
- `create_snapshot`: snapshot a child domain;
- `dump [path]`: return or write the full runtime dump.

Responses are normal messages generated through `LinearService`.

## UI Layer

`UI` is also a `Service`. It represents the user agent. `UI::send()` and
`UI::syscall()` stamp `Thread-ID`, enqueue a message into the runtime's user
loopback driver, and track per-thread pending state.

Implemented top-level UIs:

- `null`: sends `runtime` `exit` and waits for shutdown;
- `cli`: simple line-based UI with `/t`, `/s`, and `/x` commands;
- `tui`: implemented under `src/tui`;
- `web`: HTTP API exposing `/api/program/dump/` and `/api/exit/`.

`Observer` is a replay mirror used by UIs. It derives from `Program`, consumes
journaled messages, applies them to its own copy, and caches messages by
unmarked access ID for display.

## Python Adapter API

`src/python-api.cpp` builds the `_postline` Python extension. It exposes:

- `Message`: construction, read, parse, updateHeader, get, write, format, and
  isReceiving;
- `Response`: append;
- `Service`: Python subclass hook for `on_call()` and `on_memory()`, backed by
  `Server::run()`.

This is the bridge used by Python adapters to speak the same binary Postline
protocol over stdio/files/sockets.

## Email Formatting and Parsing

`src/parser.cpp` provides an email-like textual representation for messages.
It parses canonical headers into JSON and leaves the body raw. Formatting can
be full or compact:

- full format includes noncanonical headers and runtime context;
- compact format hides runtime-only context and summarizes multipart bodies.

Multipart messages are represented by a generated boundary named
`========== POSTLINE MESSAGE ==========`. The runtime still stores the body as
raw text.

## Environment and Logging

`setup_environ()` sets `POSTLINE_HOME` from the environment or derives it from
`/proc/self/exe`, then prepends `$POSTLINE_HOME/python` to `PYTHONPATH`.

`init_logging()` installs a custom spdlog sink that buffers log messages until a
UI attaches, plus a rotating `postline.log` file sink. `CHECK` failures restore
the terminal, print a stack trace, stop the process with `SIGSTOP`, and then
abort if continued.

## Utility Binaries

Top-level sources also provide journal inspection tools:

- `dump_journal`: replay a journal and print every message;
- `inspect_journal`: read access IDs from stdin and print those messages.

Both use the same `Journal` and `Message::formatEmail()` paths as the runtime.

## Replay Contract

The important design constraint is that runtime state is reconstructible from
the journal. `Program::apply()` and `Program::commit()` are the replay boundary.
Any durable change to agents, domains, snapshots, threads, call stacks, traces,
or memory should either be encoded in a normal preprocessed message or in a
`runtime:commit` message.

Transport handshakes, UI pending state, driver file descriptors, live child
processes, and log sinks are runtime process state. They are rebuilt after
replay rather than stored in the journal.

# Utilities

## Error Checking

The system is designed to fail upon unexpected conditions.  Use CHECK
generously.  DO NOT try to save an unexpected situation, e.g. by
converting formats.

CHECK(fd >= 0);
CHECK(fd >= 0, "errno: {} ({})", errno, std::strerror(errno));

In particular, CHECK(0) is OK.

## Logging

We use spdlog, with `namespace log = spdlog` already done.

log::info("Welcome to spdlog!");
log::error("Some error message with arg: {}", 1);

## JSON

We use json, with `using json = nlohmann::json` already done.

# Messages

Postline is essentially a system for message passing.  A serialized
message for I/O has three parts:

- A record header, which is only used by the I/O code and not exposed to
  runtime.
- A json header, always automatically decoded.
- A json body, never touched by runtime.

Below are from include/postline/common.h (src/common.cpp):

    using AccessID = int64_t;   // Identifies a message on disk
    static constexpr AccessID NO_ACCESS_ID = -1;
    // AccessID is 15-bit segment_id and 48 bit within file offset
    AccessID make_access_id(uint32_t segment, uint64_t offset);
    void split_access_id(AccessID access_id, uint32_t *segment, uint64_t *offset);

    class Message {
    public:
        Message (json &&header,
                 std::string &&body_raw = "");

        void write(int fd) const;
        static Message read(int fd);    // stream version
        static Message read(int fd, off_t offset, unsigned segment); // pread version

        AccessID access_id() const;
        bool has_access_id() const;
        json const& header() const; // returns the header
        size_t serialized_size () const;
    };

A message has access_id of -1 by default.  Only the message returned by
the pread version of Message::read has a valid access_id.

## Message Protocols

Actual messages follow a protocol and are defined in
include/postline/protocol.h .  The protocol structs are defined in
namespaces like

namespace protocol {
    namespace journal {
        struct Root;
    }
    namespace actor {
        struct Hello;
        struct Bye;
    }
}

Each struct has a constructor that takes in a Message and parses
necessary fields into the present struct.  It also defines a static
method `make` that constructs a Message. E.g.

// upon start of actor it sends the Hello message
protocol::actor::Hello::make(0, 0).write(STDOUT_FILENO);

The same message is then read on the other side by

// inside Actor::Actor, where recv() returns a Message
protocol::actor::Hello hello(recv());


# 



# TUI Code Map

The TUI is the FTXUI implementation of `postline::ui::UI`.  It is also an
`Observer`, so it owns a local `Program` mirror that is updated from journaled
runtime messages.  Rendering code reads that mirror on the UI thread after
pending messages have been drained.

```text
TUI
|- UI        sends user messages to Runtime and tracks pending UI requests
`- Observer  queues journal messages, applies them to Program, caches Messages
   `- Program
```

Prefer explicit qualification in TUI code when reading mirrored runtime state:
use `Program::domains`, `Program::agents`, `Observer::threads`, and
`Program::global`.  This avoids confusion with `TUI::global`, which is the
presentation state for the Global tab.

## Runtime Flow

`src/tui/tui.cpp` builds the top-level `TUI` object.

- `TUI::consume()` returns the journal sink installed by the runtime.
- The sink calls `Observer::consume()` and posts `ftxui::Event::Custom`.
- The top-level renderer calls `Observer::process()`, `syncObserver()`, then
  `drainLogs()` before drawing.
- `Observer::process()` applies each message with `Program::apply()` and stores
  the full message in `Observer::cache`.
- `ThreadTab` renders traces from `Thread::trace`, whose entries are
  `AccessID`s produced by `Program::apply()`.
- `MessageListDataRef` and `ThreadTab::reloadCurrentMessage()` load full
  messages through `Observer::getMessage()`.

Only the UI thread should touch FTXUI components and rendered state.  Other
threads enqueue messages or logs, then wake the screen with a custom event.

## Main Files

- `src/tui/tui.cpp`
  Owns `TUI`, the screen, top-level chrome, modal, tab container, log queue,
  runtime wakeups, and dynamic creation of thread tabs.

- `include/postline/ui.h`
  Defines the UI service boundary.  `UI::send()` and `UI::syscall()` add
  `Thread-ID`, enqueue user messages into the runtime, and track per-thread
  pending state until runtime replies arrive.

- `include/postline/observer.h`, `src/observer.cpp`
  Define the observer queue and message cache.  `Observer` inherits `Program`;
  `process()` is the point where queued runtime messages become visible to the
  TUI mirror.

- `src/tui/tabs.h`
  Declares only the abstract `Tab` interface shared by top-level tabs.

- `src/tui/global_tab.h`, `src/tui/global_tab.cpp`
  Declare and implement the Global tab: thread list on the left, runtime log
  list and log detail on the right.  `GlobalData` lives here.

- `src/tui/stat_tab.h`, `src/tui/stat_tab.cpp`
  Declare and implement the Stat tab.  It reads Linux `/proc` files and
  `pstree` once per second and renders process, system, and process-tree panes.

- `src/tui/message_reader.h`
  Declares `MessageReader`, the scrollable selected-message viewer.
  `src/tui/message_reader.cpp` implements it.

- `src/tui/message_editor.h`
  Declares `MessageEditor`, the composer for user messages.
  `src/tui/message_editor.cpp` implements it.

- `src/tui/message_list_data_ref.h`
  Declares `MessageListDataRef`, the list adapter that renders a thread trace
  from `AccessID`s in `Thread::trace`.

- `src/tui/split_pane.h`, `src/tui/split_pane.cpp`
  Declare and implement the borderless draggable split-pane component used to
  size the thread tab columns.

- `src/tui/thread_nav.h`, `src/tui/thread_nav.cpp`
  Build thread navigation view-model data from `Program` objects: tree entries,
  stack entries, member entries, current-domain derivation, and composer
  address candidates.

- `src/tui/thread_tab.h`, `src/tui/thread_tab.cpp`
  Declare and implement `ThreadTab`.  The implementation owns thread-screen
  layout, focus, shortcuts, selected-message loading, and callbacks into the
  message reader, composer, split panes, and navigation builders.

- `src/tui/thread_focus.h`
  Small focus-state helper for cycling the four thread panes with Tab and
  Shift-Tab.

- `src/tui/render.h`, `src/tui/render.cpp`
  Pure rendering helpers for colors, pane frames, logs, message headers, body
  text, and the About dialog.

- `src/tui/ftx_list.hpp`
  Local FTXUI list component.  It supports stable logical indexes, bounded
  `ListData<T>`, keyboard/mouse navigation, and tail-follow mode.

- `src/tui/limits.h`
  Retention limits for visible threads, logs, and messages.

## Component Ownership

`TUI` owns all tabs through `std::vector<std::unique_ptr<Tab>>`.  It also keeps
the parallel FTXUI tab labels and components required by `Container::Tab`.

`GlobalData` is shared presentation state owned by `TUI` and passed to
`GlobalTab`.  It contains bounded lists for visible thread labels and logs plus
an `onOpenThread` callback back into `TUI`.

Each `ThreadTab` is bound to one mirrored `Thread *` from `Observer::threads`.
The tab does not own the thread.  It owns:

- generated navigation labels for tree, stack, and member views;
- navigation metadata produced by `thread_nav`;
- a `MessageListDataRef` over the thread's `trace`;
- a selected message pointer loaded from `Observer::cache`;
- `MessageReader` and `MessageEditor` child components;
- split-pane widths, focus state, and maximized-right-pane mode.

`MessageReader` renders the selected `Message` through a pointer supplied by
`ThreadTab`.  `MessageEditor` refreshes its address choices from
`thread_nav`, then asks its address provider callback for extra entries such as
the runtime agent.  `ThreadTab` owns the send callback that turns editor input
into a `Message`.

## Rendering Model

FTXUI component construction and data refresh are currently mixed.  Several
render lambdas call sync methods immediately before drawing:

- `TUI` drains observer messages and logs.
- `ThreadTab` rebuilds navigation labels through `thread_nav`.
- `ThreadTab` reloads the selected full message.
- `MessageEditor` refreshes available addresses through `thread_nav` and its
  address provider callback.
- `StatTab` refreshes its `/proc` snapshot.

This keeps the UI current, but it also means rendering has side effects.
Refactors should preserve the one-thread ownership model while moving these
updates toward explicit "sync model, then render model" phases.

## Message Detail

Message header rendering in `render.cpp` uses this order:

1. `Thinking`, if present, as body-like text before headers.
2. `From`
3. `To`
4. `Cc`
5. `Subject`
6. `Content-Type`
7. `Content-Disposition`
8. Remaining headers in JSON iteration order.

Internal fields hidden from the reader are `type`, `Thinking`, `Trash`, and
`CONTEXT_HEADER_NAME`.  Body rendering is plain text split on newlines.

## Maintenance Notes

The header split has already moved concrete declarations out of `tabs.h`; keep
that direction.  `src/tui/thread_tab.cpp` delegates reusable split-pane
plumbing, message-reading behavior, composer behavior, and navigation model
derivation to smaller implementation files.  Keep it focused on layout, focus,
shortcuts, selected-message loading, and callbacks.

The next useful boundary is a broader TUI view-model layer.  Thread navigation
and composer address derivation now live in `thread_nav`, but components still
often read `Program` objects directly and mutate cached labels while rendering.
Explicit view-model builders for trace rows and other tab state would make the
code easier to test without FTXUI and would reduce friend access to `Observer`.

The observer cache is unbounded even though visible lists have retention
limits.  If long-running sessions are expected, add a retention policy for
`Observer::cache`, `Thread::trace`, and agent memory, or document that the
observer intentionally mirrors the full journal for the session.

`MessageListDataRef::Render()` currently catches every exception and returns
`"no context"`.  That is unlike the rest of Postline's fail-fast style.  Prefer
explicit presence checks plus `CHECK` for malformed messages, or keep a narrow
fallback only for genuinely missing cached messages.

`StatTab` is Linux-specific and shells out to `pstree`.  That is acceptable for
Postline's Linux target, but the command execution and `/proc` parsing would be
easier to test if moved behind small free functions or a snapshot provider.

There is currently little test coverage for TUI behavior beyond
`ThreadPaneFocus`.  Good low-cost tests would cover `ftxui::ListData` index
retention, list tail-follow selection transitions, navigation view-model
generation in `thread_nav`, address selection preservation, and message header
ordering.
