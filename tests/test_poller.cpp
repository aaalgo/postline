#include <postline/actor.h>
#include <postline/poller.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <iostream>
#include <memory>
#include <string>

using namespace postline;

namespace {

constexpr std::size_t kActorCount = 5;

bool parse_actor_id(const std::string& line, Poller::Token* actor_id)
{
    unsigned value = 0;
    const char* begin = line.data();
    const char* end = begin + line.size();
    auto [ptr, ec] = std::from_chars(begin, end, value);

    if (ec != std::errc() || ptr != end || value >= kActorCount) {
        return false;
    }

    *actor_id = value;
    return true;
}

} // namespace

int main()
{
    setup_environ();
    Poller poller;
    std::array<std::unique_ptr<Actor>, kActorCount> actors;
    std::array<int, kActorCount> registered_fds{};

    for (std::size_t i = 0; i < actors.size(); ++i) {
        auto actor = std::make_unique<Actor>("./actors/echo");
        Poller::Token actor_id = static_cast<Poller::Token>(i);
        registered_fds[i] = actor->read_fd();
        poller.add(registered_fds[i], actor_id);
        actors[i] = std::move(actor);
    }

    for (;;) {
        std::cout << "Actor ID (0-" << (kActorCount-1) << "): " << std::flush;

        std::string actor_line;
        if (!std::getline(std::cin, actor_line)) {
            break;
        }
        if (actor_line.empty()) {
            break;
        }

        Poller::Token actor_id = 0;
        if (!parse_actor_id(actor_line, &actor_id)) {
            std::cerr << "invalid actor ID\n";
            continue;
        }

        std::cout << "Message: " << std::flush;

        std::string line;
        if (!std::getline(std::cin, line)) {
            break;
        }
        if (line.empty()) {
            break;
        }

        const std::size_t actor_index = static_cast<std::size_t>(actor_id);
        Actor& actor = *actors[actor_index];

        actor.send(Message(json{
            {"type", "user:message"},
            {"From", ""},
            {"To", ""},
            {"Subject", line}
        }));

        auto events = poller.wait();
        CHECK(!events.empty());

        auto it = std::find_if(events.begin(), events.end(), [actor_id](const Poller::Event& event) {
            return event.t == actor_id;
        });
        CHECK(it != events.end());

        const Poller::Event& event = *it;
        CHECK(event.t == actor_id);

        // Poller reports tokens, so use the token-to-fd registration map to
        // assert the ready event came from the actor we selected.
        CHECK(registered_fds[actor_index] == actor.read_fd());

        Message echoed = actor.recv();
        std::cout
            << "from actor " << actor_id
            << ": " << echoed.header().value("Subject", std::string())
            << '\n';
    }

    return 0;
}
