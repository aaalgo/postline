# Postline Tags

## Purpose

Tags are durable, replayable public knowledge carried by normal Postline
messages. All tags in the initial implementation may propagate through
conversation. Private tags are deferred.

The first use is a birth-giving user message carrying the tag `genesis`. A
newly contacted AI accepts that tag, and its response carries the tag back.
All agents, including `user`, otherwise start with no tags.

## Message Protocol

The tag header is named `Postline-Tags`. `X-Postline-Tags` is not used:
Postline already owns the `Postline-*` namespace, and an `X-` prefix would not
add useful meaning.

The value is a JSON array of unique strings:

```json
{
  "From": "user",
  "To": "ai",
  "Postline-Tags": ["genesis"]
}
```

Normal preprocessed messages always have `Postline-Tags`, including when the
array is empty. Tags are serialized in lexical order so journal dumps and
replay results are deterministic. A malformed tag header is an unexpected
protocol condition and fails with `CHECK`; the runtime does not convert it.
Messages from journals written before this feature have no tag header and are
replayed as offering an empty set. Absence is accepted only for this journal
compatibility; newly journaled normal messages are canonicalized with a header.

All currently supported tags appear in `Postline-Tags` and are public.

## Agent State

Each `Agent` owns one `tags` set containing tags the agent has originated or
accepted and may offer to other agents.

New agents and clones start with an empty tag set. Tags are live agent state,
not `AgentParams`: snapshots and genetic links do not copy them. Agent dumps
expose the set for inspection and future management tools.

In the initial implementation tags are monotonic. Adding an existing tag is a
no-op, and there is no automatic revocation of copies already learned by other
agents.

## Journal and Replay

The journal stores tags in `Postline-Tags` on normal messages. It does not emit
a separate record containing each agent's resulting tag set. During replay,
`Program::apply()` repeats the recorded unions and reconstructs `Agent::tags`.
Agent dumps show this reconstructed current state but are not the durable
record.

Future direct tag-management operations must be encoded as `runtime:commit`
messages so they cross the same replay boundary.

## Initial Propagation Policy

Receiving a tag is an offer. The receiver is conceptually free to accept any
subset, but the current AI framework has no syscall with which to record that
choice. The initial deterministic policy therefore accepts every offered tag.

For a successfully routed `CALL` or `RETURN` message:

1. Preprocessing makes `Postline-Tags` authoritative.
2. Applying the journaled message unions its tags into the receiving agent's
   `tags`.
3. A later message from that agent is stamped with its then-current tags, so
   accepted tags propagate onward.

Synthetic `REWIND` messages carry an empty tag list and do not transfer tags.
Runtime commit and transport handshake messages are not normal agent messages
and are outside this protocol.

The default acceptance belongs at the replay boundary in `Program::apply()`,
not in an individual OpenAI adapter. This makes C++ and Python agents behave
consistently and reconstructs the same tag state without calling an AI during
replay.

## Authority and Bootstrap

For ordinary agents, the runtime replaces any adapter-supplied
`Postline-Tags` with the agent's current tags. An adapter cannot claim a new
tag merely by writing a header.

The user is the bootstrap authority in the initial implementation. A user
message may offer additional tags supplied by the composer. Preprocessing
unions those proposed tags with the user's existing tags and writes the
canonical result to the message. Applying the message records those tags on
the user as well as applying the receiver's accept-all policy. This state
transition is fully represented by the journaled message.

The line-oriented CLI initializes its first draft with `genesis` and clears the
draft tags after sending it. The TUI starts with an empty tag editor so the user
can add bootstrap tags explicitly, then carries the sent list into the next
draft. Neither UI initializes the `user` agent with a hidden tag. Later user
messages are stamped from the user's durable tag set, so accepted tags continue
to propagate normally.

## Example

Initial state:

```text
user.tags = {}
ai.tags   = {}
```

The user sends the birth message:

```text
user -> ai    Postline-Tags: genesis
```

After apply:

```text
user.tags = {genesis}
ai.tags   = {genesis}
```

The AI response is stamped by the runtime:

```text
ai -> user    Postline-Tags: genesis
```

The second union is idempotent.

## Expansion Points

### Selective acceptance

A future agent-facing syscall can let the recipient select a subset of offered
tags. The selection itself must be journaled, most naturally as a
`runtime:commit` operation, before it changes agent state. Replay must apply the
recorded selection and must never ask the AI to decide again.

The accept-all merge in `Program::apply()` is therefore a policy placeholder,
not a claim that receivers can never reject tags.

### Tag management

Future runtime commands should add and remove tags on existing agents, for
example:

```json
{
  "op": "add_agent_tags",
  "agent_id": 7,
  "tags": ["research"]
}
```

Management mutations must use journaled `runtime:commit` operations.
Permission checks, naming rules, and whether removal needs tombstones are
intentionally deferred.

### Private tags

Private tags are not represented in the initial agent state or message
protocol. A later design must define who can read and manage them and how their
mutations are journaled. They must never appear in `Postline-Tags` or any other
ordinary agent-to-agent message header.

### Provenance and revocation

Plain string sets intentionally lose provenance: after B accepts A's tag, it
becomes one of B's tags and may be propagated as B's own. Removing the tag
from A cannot retract B's copy. If provenance, trust, expiry, or revocation
becomes necessary, a tag will need a stable identity and metadata rather than
only a string.

## Initial Non-Goals

- AI-selected subsets or an agent syscall.
- Tag-management commands or UI.
- Private tags, private-tag transport, encryption, or recipient-specific
  disclosure.
- Provenance, expiry, revocation, permissions, or tag namespaces.
- Copying tags through cloning, snapshots, or genetic links.
