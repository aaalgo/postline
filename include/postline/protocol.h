#pragma once
namespace postline {
namespace protocol {

struct View {
    View(Message const& msg, std::string_view expected_type)
    {
        auto const &header = msg.header();
        CHECK(header.contains("type"));
        auto type = header["type"].get<std::string>();
        CHECK(type == expected_type,
              "expected message type {}, got {}",
              expected_type, type);
    }
};

namespace agent {

struct Data : public View {
    static constexpr std::string_view type = "agent:data";

    explicit Data(Message const& msg)
        : View(msg, type)
    {
    }

    static Message make(std::string body = "") {
        return Message(json{{"type", type}}, std::move(body));
    }
};

}

namespace journal {

    // these messages never go above Journal
    // Runtime doesn't see these messages
    // these messages do not have From/To/... like typical messages

struct Root : public View {
    static constexpr std::string_view type = "journal:root";
    std::string prev;

    explicit Root(Message const& msg)
        : View(msg, type)
    {
        auto const &header = msg.header();
        if (header.contains("prev") && !header["prev"].is_null()) {
            prev = header["prev"].get<std::string>();
        }
    }

    static Message make(std::string const& prev = "") {
        return Message(json{
            {"type", type},
            {"prev", prev.empty() ? json(nullptr) : json(prev)}
        });
    }
};

}

namespace runtime {
    // These are the runtime data structure modifying messages
    // They are processed by Runtime::commit and should never fail.
    // these messages do not have From/To/... like typical messages

struct Commit : public View {
    static constexpr std::string_view type = "runtime:commit";
    json op;    // operations

    explicit Commit(Message const& msg)
        : View(msg, type),
        op(json::parse(msg.body()))
    {
        CHECK(op.is_object());
    }

    static Message make(json const &op) {
        json header{{"type", type}};
        std::string body = op.dump();
        return Message(std::move(header), std::move(body));
    }
};

struct BeginShutdown: public View {
    static constexpr std::string_view type = "runtime:begin_shutdown";

    static Message make(json const &ops) {
        json header{{"type", type}};
        return Message(std::move(header));
    }
};

struct EndShutdown: public View {
    static constexpr std::string_view type = "runtime:end_shutdown";

    static Message make(json const &ops) {
        json header{{"type", type}};
        return Message(std::move(header));
    }
};

struct Flush: public View {
    static constexpr std::string_view type = "runtime:flush";

    static Message make(json const &ops) {
        json header{{"type", type}};
        return Message(std::move(header));
    }
};

}

namespace handshake {

    // these messages shouldn't go into journal
    // therefore they don't go through Runtime::process
    // and they shouldn't go into agent memory either

struct Hello : public View {
    static constexpr std::string_view type = "handshake:hello";

    explicit Hello(Message const& msg)
        : View(msg, type)
    {
        auto const &header = msg.header();
    }

    static Message make() {
        return Message(json{
            {"type", type},
        });
    }
};

struct BeginMemory {
    static constexpr std::string_view type = "handshake:begin_memory";

    static Message make() {
        return Message(json{
            {"type", type},
        });
    }
};

struct EndMemory {
    static constexpr std::string_view type = "handshake:end_memory";

    static Message make() {
        return Message(json{
            {"type", type},
        });
    }
};

struct Multi : public View {
    static constexpr std::string_view type = "handshake:multi";
    size_t count;

    explicit Multi(Message const& msg)
        : View(msg, type)
    {
        count = msg.header()["count"].get<size_t>();
    }

    static Message make (size_t count) {
        return Message(json{
            {"type", type},
            {"count", count}
        });
    }
};

struct Bye : public View {
    static constexpr std::string_view type = "handshake:bye";

    explicit Bye(Message const& msg)
        : View(msg, type)
    {
    }

    static Message make () {
        return Message(json{
            {"type", type},
        });
    }
};

}
}} // namespaces
