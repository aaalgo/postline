// poller.h - Thin Linux epoll wrapper
#pragma once

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <array>

namespace postline {

class Poller: immobile {
    static constexpr std::uint32_t default_events =
        EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP;
    static constexpr std::size_t wait_capacity = 64;
    int epoll_fd_ = -1;
public:
    using Token = std::int64_t;

    struct Event {
        Token token = 0;
        std::uint32_t flags = 0;  // Raw epoll flags.
    };

    Poller()
        : epoll_fd_(::epoll_create1(EPOLL_CLOEXEC))
    {
        CHECK_FD(epoll_fd_);
    }

    ~Poller() noexcept
    {
        ::close(epoll_fd_);
    }

    void add(int fd, Token t)
    {
        epoll_event ev{};
        ev.events = default_events;
        ev.data.u64 = static_cast<std::uint64_t>(t);

        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
            CHECK_ERRNO(0);
        }
    }

    void remove(int fd)
    {
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr) < 0) {
            CHECK_ERRNO(0);
        }
    }

    std::vector<Event> wait(int timeout_ms = -1)
    {
        std::array<epoll_event, wait_capacity> ready{};

        int count = 0;
        do {
            count = ::epoll_wait(
                epoll_fd_,
                ready.data(),
                static_cast<int>(ready.size()),
                timeout_ms);
        } while (count < 0 && errno == EINTR);

        CHECK(count >= 0);

        std::vector<Event> out;
        out.reserve(static_cast<std::size_t>(count));

        for (int i = 0; i < count; ++i) {
            out.push_back(Event{
                .token = static_cast<Token>(ready[static_cast<std::size_t>(i)].data.u64),
                .flags = ready[static_cast<std::size_t>(i)].events,
            });
        }

        return out;
    }

private:

};

} // namespace postline
