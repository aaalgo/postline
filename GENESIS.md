# Welcome to Postline

You are entering Postline, an AI agent runtime built around durable message
passing. This document is your starting point: it explains the system's mental
model, identifies the code that gives that model behavior, and suggests how to
work safely in the repository.

Read `AGENTS.md` next. It contains the detailed architecture map and the local
coding and workflow rules. Read `TAGS.md` before changing tag propagation.

## The One-Sentence Model

Postline receives a message, gives it authoritative routing context, appends it
to a journal, applies it to replayable program state, and delivers it to the
next agent.

Messages are not merely payloads travelling through the runtime. They are the
record of what happened. If a durable state transition cannot be reconstructed
from journaled messages, it does not belong in the replayable model yet.

```text
agent or user
    |
    v
Runtime::run
    |
    +-- Program::preprocess  resolve and authorize routing
    +-- Journal::append      make the event durable
    +-- Program::apply       update replayable state
    `-- Driver::send         deliver to the recipient
```

On restart, the journal is read in chronological order and the same
`Program::apply()` boundary reconstructs agents, domains, threads, call stacks,
traces, memory, and public tags.

## Your First Reading Path

Start with these files in order:

1. `include/postline/common.h` and `src/common.cpp` — `Message`, `AccessID`, and
   binary record framing.
2. `include/postline/program.h` and `src/program.cpp` — the replayable state,
   address resolution, preprocessing, commits, calls, returns, and rewinds.
3. `include/postline/journal.h` — append, segmented history, and replay.
4. `include/postline/runtime.h` and `src/runtime.cpp` — the active polling and
   delivery loop.
5. `include/postline/driver.h`, `include/postline/service.h`, and
   `include/postline/server.h` — the runtime/adapter boundary.
6. `include/postline/ui.h`, `src/ui.cpp`, and `src/observer.cpp` — user messages
   and the UI's replay mirror.
7. `src/tui/tui.cpp` and `src/tui/thread_tab.cpp` — the interactive UI.

Use `src/main.cpp` when you want to see how the pieces are assembled rather
than how any one piece works.

## The Durable Model

`Program` is the heart of Postline's state. It owns:

- agents, which have names, services, flags, memory, tags, and obligations;
- domains, which provide hierarchical communication scopes;
- snapshots, which are reusable domain templates;
- threads, which contain a root domain, call frames, and a message trace.

Normal traffic changes that state through a preprocessed message. Structural
operations such as creating agents, domains, snapshots, or detached threads
are encoded as `runtime:commit` messages. Both forms cross the journal before
they become durable state.

Keep process-only resources outside this model. File descriptors, drivers,
child processes, UI pending state, and log sinks are rebuilt when the process
starts and are not journal state.

## Routing and Conversation

Messages use email-like headers such as `From`, `To`, `Cc`, `Subject`,
`Thread-ID`, `Message-ID`, `In-Reply-To`, and `In-Response-To`. The runtime adds
`__context`; adapters must not treat that internal field as their authority.

A `CALL` pushes a frame and transfers an obligation to the recipient. A
`RETURN` pops the frame and routes back to its caller. A `REWIND` unwinds an
error path until it reaches a catching agent or empties the stack.

Addresses are resolved relative to the sender's domain. The global domain is a
fallback. Domains and snapshots may be detached with `&`; agents and domains
may be cloned with `*`, subject to the implementation limits documented in
`AGENTS.md`.

## Tags and Genesis

`Postline-Tags` is a JSON array of unique public tag strings. Tags travel on
normal journaled messages and are replayed into agent state. The current policy
accepts every offered tag and treats accepted tags as monotonic knowledge.

The tag `genesis` has a convention, not hidden runtime privilege: a user may
place it on the message that brings a new AI into a conversation. The TUI tag
editor starts empty, so the user adds it explicitly. Receiving `genesis` does
not change protocol rules; it tells you that this message is intended as an
entrance or beginning.

Do not invent tags in an adapter and expect the runtime to trust them. The
runtime stamps ordinary agents' messages from their accepted tag state. The
user is the initial authority allowed to propose additional public tags.

## Runtime and Adapter Boundary

The runtime owns `Driver` instances. A shell driver launches an adapter and
speaks Postline's framed message format over pipes. An adapter uses `Server`
and a `Service`; `LinearService` is the request/response helper used by simple
services.

Handshake messages control transport startup, memory replay, and shutdown.
They are not normal agent traffic and are not journaled as replayable program
events.

Python adapters reach the same protocol through the `_postline` extension in
`src/python-api.cpp`. Protocol behavior should normally live in the runtime,
not in only one adapter language.

## UI Mental Model

The UI does not read live runtime objects directly. `Observer` consumes
journaled messages and applies them to its own `Program` mirror. The TUI renders
that mirror on the UI thread.

Background activity may enqueue messages or logs and wake the screen, but only
the UI thread should mutate FTXUI components or presentation state. In TUI
code, qualify mirrored state explicitly with `Program::` or `Observer::` when
that avoids confusion with presentation fields.

For detailed ownership and rendering boundaries, read `src/tui/DESIGN.md` and
the TUI section of `AGENTS.md`.

## Changing Postline Safely

Before editing, identify which side of the replay boundary owns the change:

- message serialization belongs in `Message` and journal I/O;
- durable behavior belongs in a normal message or `runtime:commit` operation;
- reconstructed state belongs in `Program::apply()` or `Program::commit()`;
- live transport behavior belongs in `Runtime`, drivers, or servers;
- presentation-only behavior belongs in UI code.

Preserve fail-fast behavior. Unexpected protocol states use `CHECK`; do not
silently coerce malformed durable data. Preserve old-journal compatibility only
where the protocol explicitly defines it.

After a code change, build and run the relevant tests, then stage and commit the
completed change without including unrelated working-tree files. The standard
commands are:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The runnable installation tree is created under `build/install`; the main
launcher is `build/install/bin/pl`.

## Questions to Ask of Every Change

Before considering a change complete, ask:

1. Is the state durable, and if so, which journaled message represents it?
2. Will replay produce exactly the same state without contacting an AI?
3. Who is authorized to set each routing or protocol field?
4. Does the change preserve call-stack and obligation accounting?
5. Does UI work remain on the UI thread?
6. Does malformed protocol data fail clearly instead of being guessed at?
7. Are tests exercising the durable behavior rather than only its surface?

If you can answer those questions, you are no longer merely browsing
Postline—you are ready to change it.
