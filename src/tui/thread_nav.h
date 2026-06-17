#pragma once

#include <string>
#include <vector>

#include <postline/program.h>

namespace postline { namespace ui {

struct ThreadNavTreeEntry {
    std::string label;
    bool pruned;
};

Domain const *currentThreadDomain(Thread const *thread);
std::vector<ThreadNavTreeEntry> buildThreadTreeEntries(Thread const *thread);
std::vector<std::string> buildThreadStackEntries(Thread const *thread);
std::vector<std::string> buildThreadMemberEntries(Thread const *thread);
std::vector<Agent *> buildThreadAddressAgents(Thread const *thread);

}}
