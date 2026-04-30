#include <atomic>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <semaphore>
#include <string>
#include <thread>

#include <fcntl.h>
#include <unistd.h>

#include <postline/common.h>
#include <postline/ui/cli.hpp>

using namespace postline;

int main(int argc, char** argv) {
    CHECK(argc == 3, "usage: {} READ_PATH WRITE_PATH", argv[0]);
    std::cout << "[ftxcli] " << argv[1] << " " << argv[2] << std::endl;

    int write_fd = ::open(argv[2], O_WRONLY | O_CLOEXEC);
    CHECK_FD(write_fd);

    int read_fd = ::open(argv[1], O_RDONLY | O_CLOEXEC);
    CHECK_FD(read_fd);

    std::atomic<bool> stop_reader = false;
    std::binary_semaphore can_receive{0};

    ftxui::CLI cli([&](Message &&msg) {
        msg.write(write_fd);
        can_receive.release();
    });

    std::cout << "Sending hello" << std::endl;
    protocol::agent::Hello::make(0, 0).write(write_fd);
    std::cout << "Hello sent, waiting for reply" << std::endl;

    std::thread reader([&] {
        for (;;) {
            Message message = Message::read(read_fd);
            cli.recv(std::move(message));
            can_receive.acquire();
            if (stop_reader.load()) {
                break;
            }
        }
    });

    cli.run();
    stop_reader = true;
    can_receive.release();
    reader.join();

    ::close(write_fd);
    ::close(read_fd);

    return 0;
}
