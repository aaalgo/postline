# Style

Always declare class in the following order:

class Dummy {
// private stuff
// it appears first because you need to read this first
// to understand the public stuff
public:
// public stuff
};

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



