#include <postline/observer.h>

#include <postline/common.h>

namespace postline { namespace ui {

Observer::MessageHeader::MessageHeader(Message const &msg)
    : id(msg.access_id()),
      thread_id(msg.thread_id()),
      from(msg.from()),
      to(msg.to()),
      subject(msg.subject()) {
}


Observer::Observer() {
}

Message const *Observer::getMessage (AccessID id) {
    static Message null;
    auto it = cache.find(unmark_receiving(id));
    if (it == cache.end()) return &null;
    return &it->second;

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
        Program::apply(m);
        cache[m.access_id()] = std::move(m);
    }
}


}}
