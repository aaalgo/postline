#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <postline/common.h>
#include <postline/protocol.h>

namespace postline {

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

class Service {
protected:
    struct Entry {
        std::string thread_id;
        std::string my_address;
        std::string my_domain_id;
        std::string peer_address;
        std::string peer_domain_id;
        std::string message_id;
        bool is_incoming;
    };
    std::vector<Entry> stack;
public:
    Service () {
    }

    virtual void on_memory (Message &&) {
    }

    virtual void on_exit () {
    }

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
                e.my_domain_id = msg.get("To-Domain-ID");
                e.peer_address = from;
                e.peer_domain_id = msg.get("From-Domain-ID");
                e.message_id = msg.get("Message-ID");
                e.is_incoming = true;
            }
        }
        // call implementation
        Response resp;
        this->call(std::move(msg), resp);

        std::vector<Message> msgs = resp.get();
        CHECK(msgs.size() == 1);
        for (auto &msg: msgs) {
            CHECK(!stack.empty());
            auto const &e = stack.back();
            CHECK(e.is_incoming);
            msg.updateHeader([this, &e](json &header) {
                header["From"] = e.my_address;
                header["From-Domain-ID"] = e.my_domain_id;
                if (!header.contains("To")) {
                    header["To"] = e.peer_address;
                }
                // we are not responsible to set To-Domain-ID
                header["Thread-ID"] = e.thread_id;
            });
            std::string const &to = msg.to();
            if (e.peer_address == to) {   // this is a reply
                msg.updateHeader([&e](json &header) {
                    header["In-Reply-To"] = e.message_id;
                        });
                stack.pop_back();
            }
            else {
                msg.updateHeader([&e](json &header) {
                    header["In-Response-To"] = e.message_id;
                        });
                stack.emplace_back();
                auto &e = stack.back();
                e.is_incoming = false;
                e.peer_address = to;

            }
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
