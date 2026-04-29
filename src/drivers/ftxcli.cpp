#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>
#include <semaphore>
#include <thread>

#include <fcntl.h>
#include <unistd.h>

#include <postline/common.h>
#include <postline/ui/cli.hpp>

using namespace postline;

int main(int argc, char** argv) {
    CHECK(argc == 3, "usage: {} READ_PATH WRITE_PATH", argv[0]);
    std::cout << "[cli] " << argv[1] << " " << argv[2] << std::endl;

    int write_fd = ::open(argv[2], O_WRONLY | O_CLOEXEC);
    CHECK_FD(write_fd);

    int read_fd = ::open(argv[1], O_RDONLY | O_CLOEXEC);
    CHECK_FD(read_fd);

    std::binary_semaphore can_receive{0};

    ftxui::CLI cli([](Message &&msg) {
        msg.write(write_fd);
        can_receive.release();
    });

    std::cout << "Sending hello" << std::endl;
    protocol::agent::Hello::make(0, 0).write(write_fd);
    std::cout << "Hello sent, waiting for reply" << std::endl;

    std::thread t([&cli]() {
      for (;;) {
        Message message = Message::read(read_fd);
        auto const& header = message.header();

        if (header.contains("type") && !header["type"].is_null()) {
            if (header.value("type", std::string()) == "agent:bye") {
                break;
            }
        }
        cli.recv(std::move(msg));
        can_receive.acquire();
    });

    cli.run();

    ::close(write_fd);
    ::close(read_fd);

    return 0;
}
