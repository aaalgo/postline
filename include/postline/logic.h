#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "common.h"
#include "agent.h"

namespace postline {

int constexpr LEVEL_FROM = 0;
int constexpr LEVEL_CC = 1;
int constexpr LEVEL_TO = 2;

struct Thread;

struct MessageContext {
    Thread *thread;
    Agent *received_from;
    Agent *from;
    Agent *to;
    int logic_op;
    std::vector<std::pair<Agent *, int>> targets;
};

struct CallStackEntry {

    AccessID access_id; // present message
    AgentID  agent_id;  // sent to agent_id, waiting for its reply
    std::string const &agent_address;

    CallStackEntry (AccessID access_id_,
                    AgentID agent_id_,
                    std::string const &agent_address_)
        : access_id(access_id_),
        agent_id(agent_id_),
        agent_address(agent_address_) {
    }
                        //
    json dump() const {
        return json{access_id, agent_id};
    }
};

class logic_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;

    template <typename... Args>
    logic_error(std::format_string<Args...> fmt, Args&&... args)
        : std::runtime_error(std::format(fmt, std::forward<Args>(args)...)) {}
};

struct Thread {
    std::vector<CallStackEntry> stack;
    std::vector<AccessID> trace;

    json dump () const {
        json j;
        {
            json arr = json::array();
            for (auto const &e: stack) {
                arr.push_back(e.dump());
            }
            j["stack"] = std::move(arr);
        }
        {
            json arr = json::array();
            for (auto id: trace) {
                arr.push_back(id);
            }
            j["trace"] = std::move(arr);
        }
        return j;
    }

    // a session can only have at most one outstanding token
    // and it's owner is stack.back().agent_id
    int check (Message &msg, MessageContext *ctx) {

        // logic.trace.push_back(msg.access_id());

        AccessID in_reply_to = msg.in_reply_to();
        AccessID in_response_to = msg.in_response_to();
        if (in_reply_to != NO_ACCESS_ID) {
            if (stack.empty()) throw logic_error("stack is empty upon replying msg {}", msg.access_id());
            auto const &e = stack.back();
            if (e.access_id != in_reply_to) throw logic_error("msg {} in reply to {} doesn't match stack {}", msg.access_id(), in_reply_to, e.access_id);
            // CHECK(e.agent_id == from->id); // doesn't have to
            // or clone agents will fail
            // in future we need to implement reply on behalf of
            if (e.agent_id != ctx->from->id) {
                // from is replying on behalf of e.agent_id
                // we need to set that
                msg.updateHeader([&e](json &header) {
                        header["On-Behalf-Of"] = e.agent_address;
                        });
            }
            return 0;
        }
        else {
            //std::string const &addr = msg.to();
            //AgentID to_id = resolve(addr);
            //CHECK(to_id != NOT_AN_AGENT, "cannot resolve {}", addr);
            if (in_response_to != NO_ACCESS_ID) {
                if (stack.empty()) throw logic_error("stack is empty upon responding msg {}", msg.access_id());
                auto const &e = stack.back();
                if (e.access_id != in_response_to) throw logic_error("msg {} in response to {} doesn't match stack {}", msg.access_id(), in_response_to, e.access_id);
                if (e.agent_id != ctx->from->id) {
                    // from is replying on behalf of e.agent_id
                    // we need to set that
                    msg.updateHeader([&e](json &header) {
                            header["On-Behalf-Of"] = e.agent_address;
                            });
                }
                return 1;
            }
            else {
                if (!stack.empty()) throw logic_error("stack is not empty upon fresh message", msg.access_id());
                return 2;
            }
        }
        CHECK(0);
    }

    void process (Message &msg, MessageContext *ctx, int op) {
        if (op == 0) {
            stack.pop_back();
            --ctx->from->obligation_count;
        }
        else {
            if (op == 1) {
                --ctx->from->obligation_count;
            }
            else {
                ++ctx->to->obligation_count;
            }
            stack.emplace_back(msg.access_id(), ctx->to->id, ctx->to->address);
        }
    }
};

struct Logic: noncopyable {
    std::vector<std::unique_ptr<Thread>> threads;

    void check (Message &msg, MessageContext *ctx) {
        auto thread_id = msg.thread_id();
        ctx->thread = nullptr;
        if (thread_id < 0) {
            // create_thread
            thread_id = threads.size();
            threads.push_back(std::make_unique<Thread>());
            msg.updateHeader([thread_id](json &header) {
                    header["Thread-ID"] = std::format("{}", thread_id);
                    });
        }
        else {
            if (thread_id >= threads.size()) {
                throw logic_error("Bad thread_id: {}", thread_id);
            }
        }
        ctx->thread = threads[thread_id].get();
        ctx->logic_op = ctx->thread->check(msg, ctx);
    }

    void process (Message &msg, MessageContext *ctx) {
        ctx->thread->process(msg, ctx, ctx->logic_op);
    }

    json dump () const {
        json j = json::array();
        for (auto const &p: threads) {
            j.push_back(p->dump());
        }
        return j;
    }
};


} // namespace postline
