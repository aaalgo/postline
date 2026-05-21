# ACTOR_MODEL.md

# Postline Actor / Thread / Domain Model

## Core Separation

The system separates:

```text
Domain tree
Thread               = communication scope + execution lineage
Agent                = execution entity
Endpoint             = an agent acting from a domain context
```

The runtime itself does not know about `Service`. `Service` is only one implementation pattern for adapters.

---

# Domain Tree

```cpp
struct Domain {
    Domain *parent;
    std::vector<Domain *> children;

    Thread *thread;

    std::vector<Agent *> members;
};
```

The domain tree is the primary static structure of the program.

Each domain belongs to exactly one thread.

## Thread Ownership Invariant

For every domain `d`:

```text
d->thread != nullptr
d->thread->root != nullptr
```

Following `d->parent` upward must eventually reach:

```text
d->thread->root
```

That root domain belongs to the same thread.

## Thread Creation

The initial root domain and root thread point to each other.

### Attached Domain

Creating an attached child domain:

```text
child.thread = parent.thread
```

### Thread Creation

Creating a detached child domain:

```text
child.thread = new_thread
new_thread.root = child
```

Detached domains therefore create new communication regions / threads.

---

# Thread

```cpp
struct Thread {
    Domain *root;

    std::vector<MessageID> trace;

    Endpoint continuation_owner;
};
```

`Thread::trace` is the complete ordered message history of the thread.

The ordering of messages inside a thread must be deterministic.

This implies:

```text
A thread may only have one continuation owner at a time.
```

---

# Endpoint

```cpp
struct Endpoint {
    Agent *agent;
    Domain *domain;
};
```

An endpoint represents:

```text
an agent acting from a specific domain context
```

The same agent may potentially appear in multiple domain contexts.

## Continuation Owner

`Thread::continuation_owner` specifies:

```text
the endpoint currently authorized to produce the next message
```

Invariant:

```text
continuation_owner.domain->thread == this_thread
```

---

# Agent Memory

```cpp
struct Agent {
    std::vector<MessageID> memory;
};
```

`Agent::memory` stores all messages visible to the agent.

---

# User Model

The user is represented at runtime as a special agent.

However:

```text
User is NOT implemented as a Service.
```

The runtime protocol supports asynchronous ingress from the user.

`Service` remains a strict ping-pong request/response abstraction.

## Continuation Ownership

A thread may legitimately have:

```text
thread.continuation_owner.agent == user
```

This means:

```text
the thread is paused waiting for user input
```

This is NOT a Service call.

---

# Thread Lifecycle

## Initial State

Newly created threads begin with:

```text
continuation_owner = user
```

Meaning:

```text
the thread is waiting for initial user input
```

## Active Thread

A thread is considered active when:

```text
continuation_owner is a normal service agent
```

The runtime is then waiting for a ping-pong response from that adapter.

## User-Paused Thread

A thread is considered paused/dormant when:

```text
continuation_owner == user
```

The runtime is waiting for user ingress.

---

# User Ingress Rules

User messages entering the runtime must:

```text
1. specify thread_id
2. target a thread whose continuation_owner == user
```

The runtime must verify this invariant.

After user ingress:

```text
continuation ownership transfers to the next executing agent
```

---

# Stack Model

The opening frame of every stack originates from user ingress.

Because only the user may initiate messages spontaneously.

Agents may later call the user and suspend execution awaiting user input.

A paused stack therefore has:

```text
bottom frame = user-origin frame
top frame    = waiting-for-user frame
```

Intermediate frames may contain ordinary agent-to-agent calls.

---

# Runtime Processing Pipeline

The runtime pipeline is:

```text
Runtime::preprocess(msg)
    -> resolve addresses
    -> determine endpoint domains
    -> fill From-Domain-ID / To-Domain-ID
    -> emit ontology micro-ops if necessary
    -> commit micro-ops

journal.append(msg)

Runtime::apply(msg)
    -> update traces
    -> update memories
    -> update continuation ownership
    -> dispatch next adapter call if needed
```

## Important Consequence

Ontology changes occur entirely through micro-ops emitted during preprocessing.

`apply()` no longer creates agents or domains.

This unifies ontology mutation handling and ensures the journal contains fully resolved messages.

# deduction.md

# Deduction of the Postline Actor Model

This document records how the current actor/thread/domain model was derived from a small number of generic constraints rather than invented arbitrarily.

The intent is to preserve the reasoning process, not merely the final structures.

---

# 1. The Need for a Static Spatial Structure

The first requirement is that the runtime must store a persistent world state.

That state contains:

```text
agents
domains
relationships between domains
```

A tree structure naturally emerges because:

```text
- domains contain subdomains
- containment is hierarchical
- ownership relationships are directional
```

This gives:

```text
Domain tree
```

The domain tree is therefore the static spatial structure of the system.

---

# 2. Parallelism Requires Communication Isolation

If the entire domain tree belonged to one communication space, then:

```text
- any agent could message any other agent
- parallel execution would interfere globally
- message ordering would become ambiguous
```

Therefore:

```text
communication must be partitioned
```

This leads to the idea of detached subtrees.

A detached subtree creates a new communication region.

Originally this was described as a partitioning of domains into threads.

Later the concept was simplified:

```text
Each domain belongs to exactly one thread.
Detached domains create new threads.
```

This yields:

```cpp
Domain::thread
Thread::root
```

The thread therefore represents:

```text
a communication scope + execution lineage
```

while the domain tree remains the static ownership structure.

---

# 3. Deterministic Message Ordering Implies Thread Linearity

A fundamental invariant was desired:

```text
Thread::trace must have deterministic ordering.
```

If multiple agents were simultaneously allowed to respond inside one thread, then:

```text
response ordering becomes nondeterministic
```

Therefore:

```text
a thread may only have one outstanding continuation at a time
```

This leads directly to:

```cpp
Thread::continuation_owner
```

This is one of the strongest deductions in the model:

```text
deterministic trace
    =>
single continuation owner
```

The thread therefore behaves as a linear execution authority even though the overall system is concurrent.

---

# 4. Why Continuation Ownership Cannot Be Null

Initially dormant threads were represented using:

```text
continuation_owner == null
```

However this produced conceptual problems:

```text
Who owns the right to resume the thread?
```

The answer is:

```text
the user
```

Therefore dormant threads are not truly ownerless.

Instead:

```text
continuation_owner == user
```

This unifies:

```text
- newly created threads
- paused threads
- user-resumable threads
```

under a single mechanism.

---

# 5. User Is an Agent But Not a Service

The runtime protocol already allowed asynchronous user ingress.

However the `Service` abstraction was strictly ping-pong:

```cpp
call(request) -> response
```

The user does not naturally fit this model because:

```text
- user may initiate the first message
- user may resume dormant threads asynchronously
- runtime may notify user without expecting an immediate response
```

This revealed that:

```text
runtime protocol != Service abstraction
```

The correct conclusion became:

```text
user exists at runtime level as an agent
but is not implemented as a Service
```

This preserves purity of ordinary service adapters.

---

# 6. Why Endpoint Is Necessary

At first continuation ownership used only:

```cpp
Agent *
```

However this became insufficient because:

```text
the same agent may act from different domain contexts
```

This leads to:

```cpp
struct Endpoint {
    Agent *agent;
    Domain *domain;
};
```

Continuation ownership therefore represents:

```text
who may continue
from where
```

rather than merely:

```text
which agent
```

---

# 7. User-Thread Multiplexing

Once the user became asynchronous, multiple dormant threads became possible.

This created the requirement:

```text
user ingress must specify thread_id
```

The user/client must only resume threads whose:

```text
continuation_owner == user
```

This naturally allows:

```text
multiple dormant threads
```

while preserving thread linearity.

---

# 8. Stack Opening and User Frames

Only the user may spontaneously initiate a new message chain.

Therefore every stack originates from user ingress.

Later, agents may suspend execution waiting for the user.

Thus a paused stack has the structure:

```text
bottom frame = user-origin frame
top frame    = waiting-for-user frame
```

This explains why user interaction naturally brackets stack execution.

---

# 9. Runtime Preprocessing and Ontology Mutation

Originally:

```text
Runtime::apply(msg)
```

performed ontology mutations such as:

```text
creating agents
creating domains
```

However messages must contain fully resolved:

```text
From-Domain-ID
To-Domain-ID
```

before journal append.

This forces ontology creation to occur earlier:

```text
Runtime::preprocess(msg)
```

The result is a cleaner unification:

```text
preprocess:
    resolve addresses
    emit ontology micro-ops
    commit ontology mutations

journal.append(resolved message)

apply:
    update runtime state only
```

This deduction was forced by actual execution requirements rather than aesthetic preference.

---

# 10. General Pattern of the Design Process

Most structures in the model were not invented directly.

Instead they emerged from constraints such as:

```text
deterministic message ordering
communication isolation
persistent replayability
single continuation authority
user-driven ingress
fully resolved journal entries
```

Whenever two independent constraints force the same structure, that structure is likely fundamental rather than accidental.

