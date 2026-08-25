# Agent Data Records

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

A future agent-memory or journal-inspection view may display them independently.
Their absence from a thread trace is a presentation and state-model decision,
not an instruction to omit them from observer replay.

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
