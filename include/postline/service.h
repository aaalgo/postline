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
        std::string peer_address;
        std::string message_id;
        bool is_incoming;
    };

    std::string address;
    std::string thread_id;
    std::vector<Entry> stack;
public:
    Service () {
    }

    virtual void on_memory (Message &&) {
    }

    virtual void on_exit () {
    }

    virtual std::vector<Message> on_connect () {
        Response resp;
        init(resp);
        std::vector<Message> msgs = resp.get();
        CHECK(msgs.size() <= 1);
        for (auto &msg: msgs) {
            std::string const &to = msg.to();
            std::string const &from = msg.from();
            CHECK(!to.empty());
            CHECK(!from.empty());
            address = from;
            stack.emplace_back();
            stack.back().peer_address = to;
            stack.back().message_id.clear();
            stack.back().thread_id.clear();
            stack.back().is_incoming = false;
        }
        return msgs;
    }

    virtual std::vector<Message> on_message (Message &&msg) {
        {   // before call, setup logic
            std::string const &to = msg.to();

            if (address.empty()) {
                address = to;
            }
            if (thread_id.empty()) {
                thread_id = msg.get("Thread-ID");
            }

            CHECK(address == msg.to(), "address {} != to {}", address, msg.to());

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
                stack.back().peer_address = from;
                stack.back().is_incoming = true;
                stack.back().thread_id = msg.get("Thread-ID");
                stack.back().message_id = msg.get("Message-ID");
            }
        }
        // call implementation
        Response resp;
        this->call(std::move(msg), resp);

        std::vector<Message> msgs = resp.get();
        CHECK(msgs.size() == 1);
        for (auto &msg: msgs) {
            if (stack.empty()) {
                msg.updateHeader([this](json &header) {
                    header["From"] = address;
                    CHECK(header.contains("To"));
                    header["Thread-ID"] = thread_id;
                });
                stack.emplace_back();
                stack.back().peer_address = msg.to();
                stack.back().message_id.clear();
                stack.back().thread_id = thread_id;
                stack.back().is_incoming = false;
            }
            else {
                auto const &e = stack.back();
                CHECK(e.is_incoming);
                msg.updateHeader([this, &e](json &header) {
                    header["From"] = address;
                    if (!header.contains("To")) {
                        header["To"] = e.peer_address;
                    }
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
                    stack.back().peer_address = to;
                    stack.back().message_id.clear();
                    stack.back().thread_id.clear();
                    stack.back().is_incoming = false;
                }
            }
        }
        return msgs;
    }

protected:
    virtual void init (Response &) {
    }

    virtual void call (Message &&, Response &) {
        // no need to fill the following of the resp message:
        //  From: set to self address
        //  To: by default set to caller
    }
};


} // namespace postline
