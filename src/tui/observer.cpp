#include "observer.h"

#include <postline/common.h>

namespace postline { namespace ui {

using namespace ftxui;

Observer::MessageHeader::MessageHeader(Message const &msg)
    : id(msg.access_id()),
      thread_id(msg.thread_id()),
      from(msg.from()),
      to(msg.to()),
      subject(msg.subject()) {
}

Observer::Thread::Thread()
    : trace(MAX_VISIBLE_MESSAGES,
          [](MessageHeader const &m) {
              return text(std::format("{} -> {}: {}", m.from, m.to, m.subject));
          }) {
}

Observer::Observer() {
    std::unique_ptr<Thread> thread = std::make_unique<Thread>();
    thread->id = 0;
    threads.push_back(std::move(thread));
}

void Observer::consume(Message &&m) {
    std::lock_guard lock(mutex);
    pendings.push_back(std::move(m));
}

void Observer::process() {
    std::deque<Message> local;
    {
        std::lock_guard lock(mutex);
        local.swap(pendings);
    }
    for (auto &m: local) {
        apply(std::move(m));
    }
}

void Observer::commit(json const &m) {
    std::string const &op = m.at("op").get_ref<std::string const &>();
    if (op == "create_agent") {
#if 0
        AgentParams params(m);
        DomainID domain_id = m.at("domain_id").get<DomainID>();
        CHECK(domain_id >= 0 && domain_id < domains.size());
        Domain *domain = domains[domain_id].get();
        result.agent = createAgent(params, domain);
        log::info("create agent {}: {}", result.agent->id, result.agent->name);
    }
    else if (op == "create_domain") {
        std::string name = m.at("name").get<std::string>();
        DomainID parent_id = m.at("parent_id").get<DomainID>();
        CHECK(parent_id >= 0 && parent_id < domains.size());
        Domain *parent = domains[parent_id].get();
        result.domain = createDomain(name, parent);
        log::info("create domain {}: {}", result.domain->id, result.domain->name);
    }
    else if (op == "create_domain_snapshot") {
        DomainID parent_id = m.at("parent_id").get<DomainID>();
        CHECK(parent_id >= 0 && parent_id < domains.size());
        Domain *parent = domains[parent_id].get();
        std::string snapshot = m.at("snapshot").get<std::string>();
        auto it = snapshots.find(snapshot);
        CHECK(it != snapshots.end());
        result.domain = createDomain(it->second, parent);
        log::info("create domain {}: {}", result.domain->id, result.domain->name);
#endif
    }
    else if (op == "create_thread") {
#if 0
        DomainID domain_id = m.at("domain_id").get<DomainID>();
        CHECK(domain_id >= 0 && domain_id < domains.size());
        Domain *domain = domains[domain_id].get();
        CHECK(!domain->detached);
        result.thread = createThread(domain);
        // We haven't handled entry.from/to yet
        log::info("create thread {} from domain {}", thread->id, domain->id);
#endif
        std::unique_ptr<Thread> thread = std::make_unique<Thread>();
        thread->id = threads.size();
        threads.push_back(std::move(thread));
    }
    else if (op == "begin_shutdown") {
    }
    else if (op == "end_shutdown") {
    }
}

void Observer::apply(Message &&msg) {
    std::string const &type = msg.type();
    if (type == "runtime:commit") {
        commit(json::parse(msg.body()));
    }
    else {
        MessageHeader header(msg);
        if (header.thread_id >= 0 && header.thread_id < int(threads.size())) {
            threads[header.thread_id]->trace.push_back(std::move(header));
        }
    }
}

}}
