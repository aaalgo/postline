# TUI Design

The TUI is an FTXUI-based implementation of the `postline::ui::UI`
service.  It is a live observer of runtime traffic: messages are consumed from
the runtime journal stream, summarized into lightweight view state, and rendered
as tabs.  Thread traces store message headers only.  The active `ThreadTab`
loads the selected full message from the runtime journal into its
`current_message` cache before rendering the message reader.

## Major Pieces

`tui.cpp` owns the top-level `TUI` class.  `TUI` inherits both `UI` and
`Observer`, which gives it two responsibilities:

- implement the user-facing `UI` service boundary, including sending messages
  back to the runtime through `UI::send`;
- maintain a read-only observer model of runtime messages for rendering.

`observer.{h,cpp}` defines the observer model.  It keeps:

- a pending queue of incoming `Message` objects protected by a mutex;
- a vector of observed threads;
- for each thread, a bounded trace of message headers.

`tabs.h`, `global_tab.cpp`, and `thread_tab.cpp` define the screen-level views.
Every tab implements the small `Tab` interface:

- `label()` returns the tab label shown in the top menu;
- `component()` returns the FTXUI component tree for that tab;
- `threadId()` optionally maps the tab back to a runtime thread.

`MessageReader` and `MessageEditor` are concrete components owned by
`ThreadTab`.  The reader is constructed with a pointer to the tab's stable
`current_message` member and has no runtime, thread, trace, or journal
dependency.  The editor owns the current draft input.  This keeps the
right-hand thread pane open for richer reader/editor implementations without
moving message selection or journal loading out of `ThreadTab`.

`render.{h,cpp}` contains pure rendering helpers for logs, message headers,
message bodies, and the About dialog.  It intentionally does not own runtime or
selection state.

`../ftx_list.hpp` is a local FTXUI component used throughout the TUI for bounded
lists with tail-following behavior.

## Runtime Integration

The runtime is configured with `UI::consume()` as a sink for journaled messages.
For the TUI, `consume()` returns a callback that pushes each message into
`Observer::pendings` and posts an FTXUI custom event to wake the screen.

The runtime journal callback processes messages before forwarding them to the
UI observer.  As a result, a message with a valid `AccessID` can later be read
from the runtime through:

```cpp
runtime->readMessage(access_id)
```

Thread message detail rendering depends on that property.  The observer stores
only `MessageHeader`, including the `AccessID`.  `ThreadTab` compares the
selected header id with `current_message_id`; when it changes, the tab reads the
full message, swaps it into `current_message`, and resets the reader scroll.
`MessageReader` then renders through the pointer it received at construction.

The TUI sends user commands back to the runtime through the base `UI` API.  The
top-level Exit button sends a message on thread 0:

```json
{"To": "runtime", "Subject": "exit"}
```

The base `UI` class records this as an outstanding request for the target
thread and clears it when the runtime responds.

## Event Loop

`TUI::run()` constructs an FTXUI `Loop` over the top-level renderer and runs
FTXUI's default blocking loop.

The renderer drains pending observer messages with `Observer::process()`,
synchronizes observer thread state into visible tabs with `syncObserver()`, and
drains pending logs before drawing.

Incoming messages and log entries also call `screen.PostEvent(Event::Custom)`.
This wakes FTXUI promptly instead of waiting for keyboard or mouse input. In
the absence of input or posted events, the UI remains blocked and does not
redraw.

## Concurrency Model

The UI thread owns all FTXUI components and visible state.  Other runtime/logging
threads may enqueue data, but they do not mutate component state directly.

There are two protected queues:

- `Observer::pendings`, protected by `Observer::mutex`, for incoming messages;
- `TUI::pending_logs`, protected by `pending_log_mutex`, for spdlog messages.

The render loop drains both queues on the UI thread.  `Observer::process()`
swaps pending messages into a local deque and applies them without holding the
mutex.  `TUI::drainLogs()` uses the same swap-then-append pattern for log
entries.

## Observer State

The observer starts with thread 0.  Runtime commit messages with type
`runtime:commit` are parsed as JSON and applied to the observer.  At present,
only the `create_thread` operation mutates the TUI observer model: it appends a
new `Observer::Thread` with the next numeric id.

Non-commit messages are summarized into `Observer::MessageHeader`:

- `id`: the message `AccessID`;
- `thread_id`: `Thread-ID`;
- `from`: `From`;
- `to`: `To`, including `Postline-Cloned-To` handling from `Message::to()`;
- `subject`: `Subject`.

If the message's `thread_id` matches an observed thread, the header is appended
to that thread's trace.  Messages for unknown threads are ignored by the
observer.  Unexpected malformed message fields are expected to fail via
`CHECK`, matching the rest of Postline's fail-fast style.

## Top-Level Layout

The top-level screen is a vertical layout:

- a title/header row with the Postline title, tab menu, About button, and Exit
  button;
- a separator;
- the active tab content.

Tabs are implemented with `Container::Tab`, keyed by `tab_index`.  The first
tab is always `GlobalTab`.  Additional `ThreadTab` instances are created by
`syncObserver()` whenever the observer discovers new threads.

The About dialog is an FTXUI modal.  It displays the ASCII banner, repository
URL, version, commit, build type, and build time from `build_info.hpp`.  It can
be closed with the Close button or Escape.

## Global Tab

`GlobalTab` shows process-wide state in two panes:

- left pane: observed threads;
- right pane: runtime log list and selected log detail.

The thread list is backed by `GlobalData::threads`.  Pressing Return on a
thread invokes `GlobalData::onOpenThread`, which asks the top-level `TUI` to
switch to the corresponding `ThreadTab`.

The log list is backed by `GlobalData::log_entries`.  It follows the tail by
default, so fresh logs remain visible until the user navigates away from the
tail.  Selecting a log renders a five-line detail box with level, logger, time,
source location, function name, thread id, and payload.

## Thread Tab

Each `ThreadTab` is bound to one `Observer::Thread`.  The layout has three
columns:

- left: navigation mode selector plus navigation list;
- middle: message trace for the thread;
- right: selected message detail plus a placeholder new-message editor.

The left navigation has three modes: `tree`, `stack`, and `members`.  The
`tree` list displays the domain hierarchy rooted at the thread's root domain.
The `stack` list reflects the current thread call stack and renders each frame
as `from-agent@from-domain -> to-agent@to-domain`.  The `members` list shows
the agents and child domains in the current domain; agents use their names and
child domains render as `@domain-name`.

The middle trace is a bounded list of message summaries rendered as:

```text
From -> To: Subject
```

Agent-generated trace entries observed after a thread tab is created are
tracked as unread presentation state.  Unread rows have a leading `*`, and the
thread's top-level tab and Global thread-list entry also have a trailing `*`.
Displaying a selected message in the active thread tab marks only that message
read.  User-authored messages and trace entries present when the tab is created
are treated as read.  Unread state is owned by the TUI and is neither journaled
nor added to `Program`.

The right pane is split into two components:

- `MessageReader` renders `ThreadTab::current_message`;
- `MessageEditor` renders the placeholder new-message input.

`ThreadTab::reloadCurrentMessage()` runs before the right pane renders.  It
derives the selected `AccessID` from the trace selection, skips work if that id
matches `current_message_id`, otherwise reads the message from the runtime
journal and swaps it into `current_message`.  If there is no selected message,
it swaps in a default-constructed `Message`.

`MessageReader` renders selected header fields, then renders the raw body split
into paragraph lines.  Changing the selected message resets the message scroll
position to the top.  The reader supports Arrow Up/Down, `j`/`k`, Page Up/Down,
Home, and End.

The message editor's `To:` dropdown lists the agents in the current domain by
`Agent::name`, plus additional agents supplied by the thread tab.  The current
domain is the thread root when the stack is empty, or the top frame's
destination domain when the stack is non-empty.  The list is refreshed during
rendering so stack and membership changes are reflected.  The TUI currently
adds `runtime` to this list.

## Rendering Rules

Message header rendering follows a canonical order:

1. `Thinking`, if present, is rendered as text lines before headers;
2. `From`;
3. `To`;
4. `Cc`;
5. `Subject`;
6. `Content-Type`;
7. `Content-Disposition`;
8. remaining headers in JSON iteration order.

The renderer hides internal fields that are not useful in the current message
detail view: `type`, `Thinking`, `Trash`, and `CONTEXT_HEADER_NAME`.

Header values are expected to be strings, except `Cc`, which may be a list of
strings.  Arrays are rendered as comma-separated strings.  Unexpected types
fail with `CHECK`.

Message body rendering is intentionally plain.  `appendTextLines()` splits on
newline and renders each line with `paragraph()`.  There is no markdown,
MIME-aware, or JSON-specific body renderer at this layer.

## Bounded Lists and Tail Following

The TUI uses `ftxui::ListData<T>` and `ftxui::List()` from `../ftx_list.hpp`
instead of raw FTXUI menus for runtime-sized streams.

`ListData<T>` stores items in a deque with a fixed retention limit.  When the
limit is exceeded, old items are dropped from the front and `first_valid_`
advances.  This gives stable logical indexes for retained entries while bounding
memory use.

List selection uses `-1` to mean "no selected item; follow the tail".  When
`follow_tail` is true, the viewport stays pinned to the newest retained items.
Manual navigation disables tail following.  Pressing End, scrolling past the
last item, or paging past the last item restores tail-follow mode.

Current limits are defined in `limits.h`:

- `MAX_VISIBLE_THREADS = 4096`;
- `MAX_VISIBLE_LOGS = 16384`;
- `MAX_VISIBLE_MESSAGES = 16384`.

## Current Limitations

Several parts of the TUI are scaffolding for the intended runtime model:

- observer commit handling only creates threads; agent/domain/snapshot handling
  is present as disabled code;
- thread names are available in `Observer::Thread::name`, but no current commit
  path populates them;
- the thread navigation panes use static placeholder entries;
- the new-message editor does not send messages;
- message bodies are rendered as plain text only;
- messages for unknown threads are dropped from the observer view.

These limitations are local to the TUI presentation layer.  The runtime and
journal remain the source of truth for full message data and runtime state.
