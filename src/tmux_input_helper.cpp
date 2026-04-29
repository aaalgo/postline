#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>

#include <fcntl.h>
#include <unistd.h>

#include <postline/common.h>

using namespace postline;

int main(int argc, char** argv) {
    CHECK(argc == 2, "usage: {} FIFO_PATH", argv[0]);

    char const * fifo_path = argv[1];

    int fifo_fd = ::open(fifo_path, O_WRONLY | O_CLOEXEC);
    CHECK_FD(fifo_fd);

  for (;;) {
        std::string subject, body;
        std::cout << "Subject: " << std::flush;
        if (!std::getline(std::cin, subject)) {
            subject.clear();
        }
        if (subject.empty()) break;
        std::cout << "Body: " << std::flush;
        std::getline(std::cin, body);

        json header{
            {"type", "agent:message"},
            {"From", "user"},
            {"To", "runtime"},
            {"Subject", subject},
        };
        Message to(std::move(header), std::move(body));
        to.write(fifo_fd);
    }

    json header{
        {"type", "agent:bye"},
        {"From", "user"},
        {"To", "runtime"}
    };
    Message(std::move(header)).write(fifo_fd);
    ::close(fifo_fd);

    std::cerr << "Requsted runtime to stop." << std::endl;
    std::cerr << "Bye." << std::endl;

    return 0;
}
