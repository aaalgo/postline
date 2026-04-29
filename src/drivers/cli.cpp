#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>

#include <fcntl.h>
#include <unistd.h>

#include <postline/common.h>

using namespace postline;

int main(int argc, char** argv) {
    CHECK(argc == 3, "usage: {} READ_PATH WRITE_PATH", argv[0]);
    std::cout << "[cli] " << argv[1] << " " << argv[2] << std::endl;

    // must open write and then read
    // or there will be deadlock
    int write_fd = ::open(argv[2], O_WRONLY | O_CLOEXEC);
    CHECK_FD(write_fd);

    int read_fd = ::open(argv[1], O_RDONLY | O_CLOEXEC);
    CHECK_FD(read_fd);

    std::cout << "Sending hello" << std::endl;
    protocol::agent::Hello::make(0, 0).write(write_fd);
    std::cout << "Hello sent, waiting for reply" << std::endl;

  std::string subject, body = "Hello, Agent.  Please respond in email format.";
  for (;;) {
      std::string to, from;
          {
            Message message = Message::read(read_fd);
            auto const& header = message.header();

            if (header.contains("type") && !header["type"].is_null()) {
                if (header.value("type", std::string()) == "agent:bye") {
                    break;
                }
            }

            if (header.contains("From")) {
                to = header["From"].get<std::string>();
            }
            if (header.contains("Reply-To")) {
                to = header["Reply-To"].get<std::string>();
            }
            if (header.contains("To")) {
                from = header["To"].get<std::string>();
            }
          }

        std::cout << "To: " << to << std::endl;
        std::cout << "Body: " << std::flush;
        if (body.empty()) {
            if (!std::getline(std::cin, body)) {
                body.clear();
            }
        }
        if (body.empty()) {
            // send exit message
            json header{
                {"type", "agent:bye"},
                {"From", from},
                {"To", "runtime"}
            };
            Message(std::move(header)).write(write_fd);
        }
        else {
            json header{
                {"type", "agent:message"},
                {"From", from},
                {"To", to},
                {"Subject", subject}
            };
            Message to(std::move(header), std::move(body));
            to.write(write_fd);
        }
        body.clear();
    }

    ::close(write_fd);
    ::close(read_fd);

    return 0;
}
