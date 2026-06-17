#include "thread_nav.h"

#include <algorithm>
#include <format>

#include <postline/common.h>

namespace postline { namespace ui {

namespace {

void appendTreeEntries(std::vector<ThreadNavTreeEntry> *entries,
                       Domain const *domain,
                       size_t depth) {
    CHECK(entries);
    CHECK(domain);

    std::string label = std::string(depth * 2, ' ') + domain->name;
    bool pruned = depth > 0 && domain->detached;
    entries->push_back({std::move(label), pruned});

    if (pruned) {
        return;
    }

    std::vector<Domain const *> children;
    children.reserve(domain->children.size());
    for (auto const &[name, child]: domain->children) {
        CHECK(child);
        CHECK(name == child->name);
        children.push_back(child);
    }
    std::sort(
        children.begin(),
        children.end(),
        [](Domain const *lhs, Domain const *rhs) {
            return lhs->name < rhs->name;
        });

    for (Domain const *child: children) {
        appendTreeEntries(entries, child, depth + 1);
    }
}

}

Domain const *currentThreadDomain(Thread const *thread) {
    CHECK(thread);
    CHECK(thread->root);

    Domain const *domain = thread->root;
    if (!thread->stack.empty()) {
        domain = thread->stack.back().to.domain;
        CHECK(domain);
    }
    return domain;
}

std::vector<ThreadNavTreeEntry> buildThreadTreeEntries(Thread const *thread) {
    CHECK(thread);
    CHECK(thread->root);

    std::vector<ThreadNavTreeEntry> entries;
    appendTreeEntries(&entries, thread->root, 0);
    return entries;
}

std::vector<std::string> buildThreadStackEntries(Thread const *thread) {
    CHECK(thread);

    std::vector<std::string> entries;
    entries.reserve(thread->stack.size());

    for (Frame const &frame: thread->stack) {
        CHECK(frame.from.agent);
        CHECK(frame.from.domain);
        CHECK(frame.to.agent);
        CHECK(frame.to.domain);

        entries.push_back(std::format(
            "{}@{} -> {}@{}",
            frame.from.agent->name,
            frame.from.domain->name,
            frame.to.agent->name,
            frame.to.domain->name));
    }

    return entries;
}

std::vector<std::string> buildThreadMemberEntries(Thread const *thread) {
    Domain const *domain = currentThreadDomain(thread);

    std::vector<std::string> entries;
    entries.reserve(domain->members.size() + domain->children.size());

    for (auto const &[name, agent]: domain->members) {
        CHECK(agent);
        CHECK(name == agent->name);
        entries.push_back(agent->name);
    }
    std::sort(entries.begin(), entries.end());

    std::vector<std::string> child_entries;
    child_entries.reserve(domain->children.size());
    for (auto const &[name, child]: domain->children) {
        CHECK(child);
        CHECK(name == child->name);
        child_entries.push_back("@" + child->name);
    }
    std::sort(child_entries.begin(), child_entries.end());
    entries.insert(
        entries.end(),
        child_entries.begin(),
        child_entries.end());

    return entries;
}

std::vector<Agent *> buildThreadAddressAgents(Thread const *thread) {
    Domain const *domain = currentThreadDomain(thread);

    std::vector<Agent *> agents;
    agents.reserve(domain->members.size());
    for (auto const &[name, agent]: domain->members) {
        CHECK(agent);
        CHECK(name == agent->name);
        agents.push_back(agent);
    }
    return agents;
}

}}
