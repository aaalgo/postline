# Python Adapters

A Python adapter is an executable service that exchanges Postline messages over
standard input and standard output. It subclasses `postline.Service`, handles
each request in `on_call`, appends one reply to the supplied `Response`, and
calls `run()`.

The Python binding is implemented in `src/python-api.cpp`. The Python package is
in `python/postline`, and example adapters are in `python/adapters`.

For ordinary adapter work, this document describes the complete public Python
API and routing behavior. Reading the C++ binding or parser should only be
necessary when extending that API.

## Minimal adapter

Create an executable file under `python/adapters`, for example
`python/adapters/uppercase.py`:

```python
#!/usr/bin/env python3

from postline import Message, Service


class Uppercase(Service):

    def on_call(self, msg, resp):
        text = msg.get("Subject")

        if not text:
            resp.append(Message({
                "Status": "error",
                "Subject": "missing Subject",
            }))
            return

        resp.append(Message({
            "Status": "ok",
            "Subject": text.upper(),
        }))


def main():
    Uppercase().run()


if __name__ == "__main__":
    main()
```

Make it executable:

```sh
chmod +x python/adapters/uppercase.py
```

The shebang is required because the pipe driver launches the adapter directly,
not through `python3`.

## Service lifecycle

`Service` is a linear request/reply service.

```python
class Adapter(Service):

    def __init__(self):
        super().__init__()
        # Initialize clients and adapter state here.

    def on_memory(self, msg):
        # Optional: receive one historical message at startup.
        pass

    def on_call(self, msg, resp):
        # Required: process one message and append exactly one response.
        resp.append(Message({"Subject": "done"}))
```

Call `super().__init__()` when overriding `__init__`.

`on_memory(msg)` is called before normal requests when the agent has the
`history` flag. It does not receive a `Response`.

`on_call(msg, resp)` must append exactly one `Message`. Returning no message or
multiple messages fails a runtime `CHECK`.

`Service.run()` performs the Postline handshake and then blocks, reading
messages until the runtime sends the exit handshake. Do not write application
data to standard output because stdout carries the binary Postline protocol.
Diagnostic output may be written to standard error.

## Reading messages

The currently exposed `Message` methods are:

```python
value = msg.get("Subject")
receiving = msg.isReceiving()
text = msg.format()
compact_text = msg.format(True)
msg.updateHeader({"Status": "ok"})
```

`get(key)` returns an empty string when a header is absent or null. It expects
an existing value to be a JSON string; using it on an array, number, object, or
boolean fails a `CHECK`.

`isReceiving()` reports whether a replayed memory message was received by this
agent rather than sent by it.

`format(compact=False)` renders the message in email form. Compact mode omits
runtime-specific headers and compacts multipart content. For a plain-text
message, the body can be obtained from the compact form when needed:

```python
headers, separator, body = msg.format(True).partition("\n\n")
assert separator
```

The current Python API has no direct raw-body or complete-header accessor.
Consequently, `format(True)` is suitable for plain text but is not a lossless
way to read multipart bodies. Extend the pybind11 binding if an adapter requires
exact raw body or structured header access.

`updateHeader(fields)` merges a Python dictionary into the message header.
Dictionary keys must be strings. Supported values are `None`, booleans,
integers, floats, strings, dictionaries, lists, and tuples composed of those
types.

## Creating replies

Construct a message from a header dictionary and an optional string body:

```python
reply = Message({
    "Status": "ok",
    "Content-Type": "json",
}, '{"result":42}')

resp.append(reply)
```

The body must be a Python string. Encode structured data explicitly:

```python
import json

body = json.dumps(result, ensure_ascii=False, separators=(",", ":"))
resp.append(Message({
    "Status": "ok",
    "Content-Type": "json",
}, body))
```

For an ordinary reply, omit `From`, `To`, `Thread-ID`, and `In-Reply-To`.
Postline fills them from the request. Set `To` only when the response should
call another agent instead of replying directly to the caller. The linear
service tracks that call and treats the later `In-Reply-To` message as the next
`on_call` input.

The incoming message's `From` header identifies the agent that sent it. An
adapter that calls other agents may compare `msg.get("From")` with its pending
destination to distinguish a reply from a new request.

`Response.append` moves the message into the response. Do not reuse a
`Message` after appending it.

Messages can also be parsed from email-formatted text:

```python
reply = Message.parse("""\
Subject: completed
Content-Type: text/plain

The operation completed.
""")
resp.append(reply)
```

## Forwarding and delegation

To forward a request, return a `Message` with `To` set to the destination. To
pass the eventual answer back to the original caller, return a normal reply
without `To`.

Nested forwarding can be tracked with a stack of destination addresses:

```python
#!/usr/bin/env python3

from postline import Message, Service


def message_body(msg):
    _, separator, body = msg.format(True).partition("\n\n")
    assert separator
    return body


class Delegate(Service):

    def __init__(self):
        super().__init__()
        self.peer_addresses: list[str] = []

    def on_call(self, msg, resp):
        from_address = msg.get("From")
        body = message_body(msg)

        if self.peer_addresses and from_address == self.peer_addresses[-1]:
            self.peer_addresses.pop()
            resp.append(Message({
                "Subject": msg.get("Subject"),
            }, body))
            return

        to_address, separator, subject = msg.get("Subject").partition(":")
        assert separator
        assert to_address

        self.peer_addresses.append(to_address)
        resp.append(Message({
            "To": to_address,
            "Subject": subject,
        }, body))


def main():
    Delegate().run()


if __name__ == "__main__":
    main()
```

This example accepts new request subjects in the form
`destination:new subject`. It splits only at the first colon, preserving any
later colons in the forwarded subject. A matching reply pops the stack and
preserves the reply's subject and body. It deliberately does not set `To` on
that reply, allowing Postline to route it back to the caller.

Push the destination before appending the forwarded message. Treat a message
as a reply only when its `From` exactly matches the top of the stack. Unexpected
or malformed protocol input should fail with `assert` or `CHECK` semantics
rather than be guessed or repaired.

## Errors

Expected operational failures should still produce one normal response:

```python
def on_call(self, msg, resp):
    try:
        result = perform_operation(msg.get("Subject"))
    except Exception as error:
        resp.append(Message({
            "Status": "error",
            "Subject": error.__class__.__name__,
        }, str(error)))
        return

    resp.append(Message({"Status": "ok"}, result))
```

Unexpected protocol or type errors are intended to fail the adapter rather than
be silently repaired.

## Build and registration

Build Postline after adding the adapter:

```sh
cmake --build build
```

The build copies `python/postline` to `build/install/python/postline` and
`python/adapters` to `build/install/bin/adapters`. The installed adapter imports
the installed `postline` extension module.

Register the adapter in an arena entry:

```json
{
  "from": "zero",
  "name": "uppercase",
  "service": "pipe:uppercase.py",
  "flags": []
}
```

The `pipe:` value is resolved relative to
`$POSTLINE_HOME/bin/adapters`. Arguments may follow the executable name, for
example `"service": "pipe:uppercase.py --mode strict"`.

Use `"flags": ["history"]` only when the adapter implements `on_memory` and
needs prior messages replayed at startup.

Add the entry to the appropriate agent list in `arena.json`, rebuild, and start
Postline normally. Messages addressed with `To: uppercase` are then delivered
to this adapter.

For a quick syntax and executable check before rebuilding:

```sh
chmod +x python/adapters/example.py
python3 -m py_compile python/adapters/example.py
```

## API summary

- `Message()` creates an empty message.
- `Message(header, body="")` creates a message from JSON-compatible headers and
  a string body.
- `Message.parse(value)` parses email-formatted text.
- `Message.read(fd)` reads the binary message format from a file descriptor;
  adapters normally use `Service.run()` instead.
- `msg.get(key)` returns a string header or `""`.
- `msg.updateHeader(fields)` merges header fields.
- `msg.isReceiving()` identifies received messages during memory replay.
- `msg.format(compact=False)` renders email-formatted text.
- `msg.write(fd)` writes the binary message format; adapters normally append to
  `Response` instead.
- `resp.append(msg)` appends the single response for the current call.
- `service.run()` serves requests until shutdown.
