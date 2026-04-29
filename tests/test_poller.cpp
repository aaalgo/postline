#include <postline/driver.h>
#include <postline/poller.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <iostream>
#include <memory>
#include <string>

using namespace postline;

namespace {

constexpr std::size_t kDriverCount = 5;

bool parse_driver_id(const std::string& line, Poller::Token* driver_id)
{
    unsigned value = 0;
    const char* begin = line.data();
    const char* end = begin + line.size();
    auto [ptr, ec] = std::from_chars(begin, end, value);

    if (ec != std::errc() || ptr != end || value >= kDriverCount) {
        return false;
    }

    *driver_id = value;
    return true;
}

} // namespace

int main()
{
    setup_environ();
    Poller poller;
    std::array<std::unique_ptr<Driver>, kDriverCount> drivers;
    std::array<int, kDriverCount> registered_fds{};

    for (std::size_t i = 0; i < drivers.size(); ++i) {
        auto driver = std::make_unique<Driver>("./install/bin/drivers/echo");
        Poller::Token driver_id = static_cast<Poller::Token>(i);
        registered_fds[i] = driver->read_fd();
        poller.add(registered_fds[i], driver_id);
        drivers[i] = std::move(driver);
    }

    for (;;) {
        std::cout << "Driver ID (0-" << (kDriverCount-1) << "): " << std::flush;

        std::string driver_line;
        if (!std::getline(std::cin, driver_line)) {
            break;
        }
        if (driver_line.empty()) {
            break;
        }

        Poller::Token driver_id = 0;
        if (!parse_driver_id(driver_line, &driver_id)) {
            std::cerr << "invalid driver ID\n";
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

        const std::size_t driver_index = static_cast<std::size_t>(driver_id);
        Driver& driver = *drivers[driver_index];

        driver.send(Message(json{
            {"type", "user:message"},
            {"From", ""},
            {"To", ""},
            {"Subject", line}
        }));

        auto events = poller.wait();
        CHECK(!events.empty());

        auto it = std::find_if(events.begin(), events.end(), [driver_id](const Poller::Event& event) {
            return event.token == driver_id;
        });
        CHECK(it != events.end());

        const Poller::Event& event = *it;
        CHECK(event.token == driver_id);

        // Poller reports tokens, so use the token-to-fd registration map to
        // assert the ready event came from the driver we selected.
        CHECK(registered_fds[driver_index] == driver.read_fd());

        Message echoed = driver.recv();
        std::cout
            << "from driver " << driver_id
            << ": " << echoed.header().value("Subject", std::string())
            << '\n';
    }

    return 0;
}
