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
