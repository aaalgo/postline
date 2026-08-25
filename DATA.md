# Agent Data Records

## Status

This file describes the implemented `agent:data` feature. Journals created
before its implementation contain no DATA records unless another change wrote
them independently.

Once DATA records are written, binaries that do not recognize the DATA action
will not be able to replay those journal entries through `Program::apply()`.
Deploy the reader/runtime support before allowing adapters to emit the new
record type. The physical journal framing itself does not need a version
change.

## Motivation

An AI agent may produce useful durable data while preparing its routed response.
Examples include notes, operation descriptions, and operation outcomes. This data
should become part of the producing agent's memory without being delivered to
another agent or appearing as communication in a thread trace.

Postline represents this output as a message whose protocol type is
`agent:data`. The existing journal record format remains unchanged: an
`agent:data` record has a JSON header, a raw body, and an `AccessID` assigned by
the journal.

## Inference Round Contract

A `LinearService` response contains, in order:

```text
zero or more agent:data records
exactly one routed CALL or RETURN message
```

The routed message is always the final item. `LinearService` enforces this
contract before returning the response records to its transport.

`agent:data` is not a replacement for the routed response. A response containing
only `agent:data` records is invalid because it would leave the current call
unresolved. A routed message followed by `agent:data` is also invalid because
the agent has yielded control once it produces the routed message.

There is no atomic batch or transport-level batch framing. The records are sent
through the existing transport one at a time and processed in order. In
particular, this design does not use `handshake:multi`.

If an agent or runtime crashes partway through the sequence, the journal may
contain a prefix of the response, including `agent:data` records without the
final routed message. Such an incomplete round is valid journal history and is
not rolled back.

## Record Semantics

An `agent:data` record:

- is appended to the journal;
- receives a normal journal `AccessID` and `Message-ID`;
- is appended to the producing agent's memory;
- is not appended to another agent's memory;
- is not marked as receiving in memory;
- is not appended to `Thread::trace`;
- is not dispatched to an agent driver;
- does not push, pop, or unwind a call frame;
- does not change thread pending state; and
- does not change any agent's obligation count.

The raw body and non-routing headers are opaque to the runtime. This permits AI
adapters to define structured data within the body without extending the
journal's physical format for each new data kind.

The intended persisted state for a round is:

```text
Record                 Journal   Sender memory   Receiver memory   Thread trace
agent:data                yes          yes              no              no
CALL/RETURN message       yes          yes              yes             yes
```

## Authoritative Context

The adapter identifies a data record with:

```json
{
  "type": "agent:data"
}
```

The runtime remains responsible for authoritative provenance. It derives the
producer from the driver that emitted the record and associates the record with
the active thread. An adapter must not be able to forge runtime-owned
`__context`, sender identity, or numeric program identifiers.

Although an `agent:data` record is omitted from `Thread::trace`, retaining its
thread association provides provenance and allows an AI adapter to select or
group its memory by thread.

`agent:data` has no destination and does not participate in the normal routing
headers `To`, `In-Reply-To`, or `In-Response-To`.

## Preprocessing and Apply

The runtime recognizes `agent:data` during preprocessing and saves an
authoritative context with a non-routing `DATA` action. Normal routed messages
continue to be preprocessed as `CALL`, `RETURN`, or `REWIND`.

After journal append, applying `DATA` performs only the replayable memory
transition:

```text
append record AccessID to the producing agent's memory
return no dispatch recipient
```

The runtime event loop treats a successful apply with no recipient as a
completed local record. It makes the journaled record available to observers
but skips recipient driver creation and delivery.

During journal replay, the same `DATA` apply path reconstructs the producing
agent's memory. It does not execute AI inference or any external operation.

## LinearService Behavior

`LinearService` distinguishes `agent:data` from the one final routed message.
It validates the entire response vector before returning any records.

Classification is deliberately simple: every record before the last must have
`type == "agent:data"`, and the last record must not. The final record is an
ordinary message; `LinearService` determines CALL versus RETURN using its
existing destination and reply-header rules. CALL and RETURN are authoritative
runtime context actions, not protocol `type` values supplied by the adapter.

For each `agent:data` record, `LinearService` supplies or validates the current
sender and thread provenance, but it does not add routing or reply headers. It
does not modify its service-side call stack for a data record.

For the final routed message, `LinearService` retains its existing behavior:
it supplies the routing headers, determines whether the message is a call or a
return, and updates its service-side stack accordingly.

## Obligations

Obligation counts describe routed control flow, not the number of journal
records emitted by an agent. The invariant is:

> Obligation counts are changed only by routed control-flow actions. Persisting
> agent-local data never creates or fulfills an obligation.

Consequently, shutdown code that waits for an obligated agent must ignore
`agent:data` for obligation counting and continue waiting for the final routed
message. The current shutdown path decrements an obligation for every raw
record it receives and must be adjusted when this design is implemented.

The shutdown path also currently journals trailing responses without normal
preprocessing. How authoritative context should be added to those trailing
records is a related existing replay concern, but atomic response batches remain
outside this design.

## Memory Replay

Agent memory remains an ordered vector of journal `AccessID` values. It does
not require a new container type: an ID may refer to either an ordinary routed
message or an `agent:data` record.

The existing memory handshake can transmit both kinds of records. An adapter's
`on_memory()` implementation distinguishes them using the `type` header. An
`agent:data` record is sent as an owned memory record rather than a
receiving-marked record.

## Observer and Presentation Behavior

Observers should consume and cache journaled `agent:data` records so the
mirrored `Program` reconstructs agent memory correctly. Because these records
are absent from `Thread::trace`, they do not appear in the existing thread
message list by default.

There is no natural place in the current UI to display `agent:data`. The TUI is
organized primarily around `Thread::trace`, and `agent:data` is deliberately
excluded from that trace. The initial UI therefore ignores these records for
presentation. A future agent-memory or journal-inspection view may display them
independently.

Ignoring `agent:data` in the UI does not mean ignoring it during observer
replay. The expected observer flow is:

```text
observer receives agent:data
observer applies it to reconstruct agent memory
observer may cache it by AccessID
no Thread::trace entry is created
the TUI never selects or renders it
```

The user-facing `UI::on_message()` also does not receive `agent:data`, because
the record has no dispatch recipient. It therefore cannot be mistaken for a
routed reply or alter UI pending state.

The implementation must preserve the following safety boundaries:

- `Program::apply()` recognizes `DATA` before executing code that expects a
  destination;
- the runtime skips recipient lookup and delivery when apply returns no
  recipient;
- `Observer::process()` applies and may cache `agent:data`;
- applying `DATA` never appends the record to `Thread::trace`; and
- UI synchronization code does not interpret entries in `Agent::memory` as
  routed messages without first checking their type.

Current thread-message components load records exclusively through
`Thread::trace`. They may continue to require routing context, including a
destination, because the trace invariant excludes `agent:data`. The UI should
not add broad exception handling or synthetic routing fields to accommodate a
record that is prohibited from appearing there.

Tests should verify that observer processing of `agent:data` succeeds, updates
only the producing agent's memory, leaves the thread trace unchanged, and does
not cause UI delivery.

## Deferred Scope

This design deliberately does not introduce:

- a new physical journal record format;
- atomic inference batches;
- `handshake:multi` transport framing;
- rollback of incomplete inference rounds;
- receiver access to `agent:data`;
- insertion of `agent:data` into thread traces; or
- runtime interpretation of the record body.

Operation-specific schemas, retention policy, model-facing formatting, and any
future system-generated data records can be designed after experience with the
basic `agent:data` mechanism.

## Implementation Handoff

The following is the expected implementation map in the current codebase. It
records code locations, not a requirement to preserve their present APIs
exactly.

### Protocol and program state

- Declare the `agent:data` protocol type in `include/postline/protocol.h`.
- Add `DATA` to `Program::Action` in `include/postline/program.h`.
- Ensure `Context::to_needed(Action::DATA)` is false.
- Extend `Program::preprocess()` in `src/program.cpp` to recognize
  `agent:data`, validate the actual producer and current thread, avoid address
  resolution, and save an authoritative DATA context.
- Extend `Program::apply()` in `src/program.cpp` so DATA appends the journaled
  ID only to `ctx.from.agent->memory` and returns `EntityRef::Tag::NONE`.
- DATA apply must return before the common code that updates a destination,
  thread trace, pending state, call frames, and obligation counts. Handle any
  eventual DATA tag policy separately because DATA has no destination.

The journal must append the record before apply, as it does for normal
messages, because DATA memory stores the resulting journal `AccessID`.

### Runtime dispatch

- In the normal event loop in `src/runtime.cpp`, accept
  `EntityRef::Tag::NONE` as the successful result of applying DATA.
- Still run observer consumption for DATA.
- Skip recipient validation, driver creation, memory replay to a new recipient,
  and `driver->send()` for DATA.
- Preserve transport order. `Server` already writes a service response vector
  in order. `ShellDriver` may continue reading one record per readiness event;
  no batch read or batch boundary is required.

### LinearService

- Replace the current `CHECK(msgs.size() == 1)` in
  `include/postline/service.h` with validation of the full response vector.
- Require a nonempty vector, require every item except the last to be
  `agent:data`, and require the last item not to be `agent:data`.
- Stamp the current `From` and `Thread-ID` provenance on each DATA record.
- Reject or remove routing headers on DATA according to the validation policy
  selected below.
- Run the existing `To`, `In-Reply-To`, `In-Response-To`, and service-stack
  logic only for the last routed message.
- Perform all vector validation before returning any records to the transport.

### Shutdown

- The shutdown receive loop in `src/runtime.cpp` currently decrements an
  obligation for every raw record. It must not decrement for `agent:data` and
  must continue reading until the routed response is received.
- Shutdown processing must preserve DATA's sender-only memory semantics.
- The existing shutdown behavior of appending trailing responses without
  preprocessing is already a replay concern. It is not solved by batch
  atomicity, which remains out of scope, but the DATA implementation must not
  silently append unreplayable DATA records.

### Observer and tests

- `Observer::process()` should continue applying and caching every consumed
  record. DATA becomes safe there through `Program::apply()`.
- No thread-list or message-reader rendering path should be added for DATA.
- Add focused tests for LinearService ordering, preprocessing authority,
  sender-only memory, no trace entry, no obligation change, no dispatch,
  replay, observer processing, and a partial sequence ending before its routed
  message.
- Add a shutdown test in which an obligated agent emits DATA before its final
  response.

## Settled Rules

The following choices were explicitly settled during design and should not be
reopened merely because the first implementation could be made simpler by
changing them:

- The protocol type is exactly `agent:data`.
- A round is `agent:data*` followed by exactly one routed message.
- `LinearService`, not the runtime, enforces the round shape and ordering.
- The runtime understands individual DATA semantics but has no batch model.
- There is no atomicity, rollback, or `handshake:multi` support.
- A crash may leave durable DATA without a final routed message.
- DATA is journaled and stored only in the producing agent's memory.
- DATA is never receiving-marked, dispatched, or inserted into a thread trace.
- DATA has thread provenance despite being absent from the thread trace.
- DATA never changes pending state, call frames, or obligation counts.
- The current UI does not display DATA, but observers still replay it.
- The existing physical journal format and `Agent::memory` vector remain in
  use.

## Implementation Policies

The initial implementation makes these choices:

- DATA records containing `To`, `In-Reply-To`, or `In-Response-To` are rejected.
- DATA records participate in the existing accounting pass.
- Adapter-proposed Postline tags on DATA are discarded; DATA neither requires a
  destination nor changes agent tags.
- DATA bodies remain opaque and have no runtime schema.
- Observers retain DATA in the existing unbounded cache.
- Every service may emit DATA; there is no AI-only agent flag.

Shutdown now preprocesses and applies trailing records before journaling them,
so DATA retains authoritative replay context and a routed response, rather than
DATA itself, fulfills the outstanding obligation.
