#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <postline/common.h>
#include <postline/protocol.h>

namespace postline {

class Service {
public:
    Service () {}

    ~Service () {}

    virtual void on_memory (Message &&) {
    }

    virtual void on_exit () {
    }

    virtual std::vector<Message> on_connect () {
        return {};
    }

    virtual std::vector<Message> on_message (Message &&msg) {
        return {};
    }
};

class Response: noncopyable {
    std::vector<Message> messages;
public:
    void append(Message &&msg) {
        messages.emplace_back(std::move(msg));
    }
    std::vector<Message> get () {
        return std::move(messages);
    }
};


class LinearService: public Service {
protected:
    struct Entry {
        std::string thread_id;
        std::string my_address;
        std::string peer_address;
        std::string message_id;
        bool is_incoming;
    };
    std::vector<Entry> stack;
public:
    virtual std::vector<Message> on_message (Message &&msg) {
        {   // before call, setup logic
            std::string const &to = msg.to();
            std::string const &from = msg.from();

            if (msg.header().contains("In-Reply-To")) {
                CHECK(!stack.empty());
                auto const &e = stack.back();
                CHECK(!e.is_incoming);
                std::string const &on_behalf_of = msg.get("On-Behalf-Of");
                if (on_behalf_of.empty()) {
                    CHECK(e.peer_address == from);
                }
                else {
                    CHECK(e.peer_address == on_behalf_of);    // clone behavior
                }
                stack.pop_back();
            }
            else {
                stack.emplace_back();
                auto &e = stack.back();
                e.thread_id = msg.get("Thread-ID");
                e.my_address = to;
                e.peer_address = from;
                e.message_id = msg.get("Message-ID");
                e.is_incoming = true;
            }
        }
        // call implementation
        Response resp;
        this->call(std::move(msg), resp);

        std::vector<Message> msgs = resp.get();
        CHECK(!msgs.empty());
        CHECK(!stack.empty());
        auto const &e = stack.back();
        CHECK(e.is_incoming);
        for (std::size_t i = 0; i + 1 < msgs.size(); ++i) {
            auto const &header = msgs[i].header();
            CHECK(msgs[i].type() == protocol::agent::Data::type);
            CHECK(!header.contains("To"));
            CHECK(!header.contains("In-Reply-To"));
            CHECK(!header.contains("In-Response-To"));
            CHECK(!header.contains(CONTEXT_HEADER_NAME));
            if (header.contains("From")) {
                CHECK(msgs[i].from() == e.my_address);
            }
            if (header.contains("Thread-ID")) {
                CHECK(msgs[i].get("Thread-ID") == e.thread_id);
            }
        }
        CHECK(msgs.back().type() != protocol::agent::Data::type);

        for (std::size_t i = 0; i + 1 < msgs.size(); ++i) {
            msgs[i].updateHeader([&e](json &header) {
                header["From"] = e.my_address;
                header["Thread-ID"] = e.thread_id;
            });
        }

        Message &routed = msgs.back();
        routed.updateHeader([&e](json &header) {
            header["From"] = e.my_address;
            if (!header.contains("To")) {
                header["To"] = e.peer_address;
            }
            // we are not responsible to set To-Domain-ID
            header["Thread-ID"] = e.thread_id;
        });
        std::string const &to = routed.to();
        if (e.peer_address == to) {   // this is a reply
            routed.updateHeader([&e](json &header) {
                header["In-Reply-To"] = e.message_id;
            });
            stack.pop_back();
        }
        else {
            routed.updateHeader([&e](json &header) {
                header["In-Response-To"] = e.message_id;
            });
            stack.emplace_back();
            auto &outgoing = stack.back();
            outgoing.is_incoming = false;
            outgoing.peer_address = to;
        }
        return msgs;
    }

protected:

    virtual void call (Message &&, Response &) {
        // no need to fill the following of the resp message:
        //  From: set to self address
        //  To: by default set to caller
    }
};


} // namespace postline
