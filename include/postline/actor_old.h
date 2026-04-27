#pragma once

#include "common.h"

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/types.h>

#if !defined(__linux__)
#error "postline::Actor supports Linux only"
#endif

namespace postline {

inline constexpr std::uint32_t actor_protocol_version = 1;

enum class ActorSpawnType : std::uint16_t
{
    address = 0,
    scope   = 1
};

enum class ActorHistoryMode : std::uint16_t
{
    none = 0,
    all  = 1
};

class ActorProtocolError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

class Actor {
public:
    explicit Actor(std::string const &command);
    // This one should spawn the process
    // and setup the read/write pipes for stdin/stdout
    // It should then read a incoming message from the the subprocess' stdout, the
    // header should be
    // {
    //  "type": "actor-hello"   -- assert it
    //  "spawn_type": 0 or 1
    //  "history_mode": 0 or 1
    // }
    ~Actor(); // shutdown

    Actor(Actor const&) = delete;
    Actor& operator=(Actor const&) = delete;

    Actor(Actor&&) = delete;
    Actor& operator=(Actor&&) = delete;

    ActorSpawnType spawn_type() const noexcept { return spawn_; }
    ActorHistoryMode history_mode() const noexcept { return history_; }

    void send(Message const& msg);

    Message recv();

private:
    ActorSpawnType spawn_ = ActorSpawnType::address;
    ActorHistoryMode history_ = ActorHistoryMode::all;

    int input_fd_ = -1;   // runtime writes to actor stdin
    int output_fd_ = -1;  // runtime reads from actor stdout
};

