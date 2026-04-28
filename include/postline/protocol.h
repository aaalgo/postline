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

namespace journal {
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

namespace driver {

struct Hello : public View {
    static constexpr std::string_view type = "driver:hello";
    int spawn_type;
    int history_mode;

    explicit Hello(Message const& msg)
        : View(msg, type)
    {
        auto const &header = msg.header();
        spawn_type = header["spawn_type"].get<int>();
        history_mode = header["history_mode"].get<int>();
    }

    static Message make(int spawn_type, int history_mode) {
        return Message(json{
            {"type", type},
            {"spawn_type", spawn_type},
            {"history_mode", history_mode}
        });
    }
};

struct Bye : public View {
    static constexpr std::string_view type = "driver:bye";

    explicit Bye(Message const& msg)
        : View(msg, type)
    {
    }

    static Message make() {
        return Message(json{
            {"type", type},
        });
    }
};

}
}} // namespaces
