#include <iostream>
#include <string>

#include <postline/common.h>
#include <postline/server.h>

using namespace postline;

class CliServer : public ServerBase {
public:
    CliServer()
    {}

protected:
    void exit () {
        std::cout << "Exiting..." << std::endl;
        json h{
            {"type", "agent:bye"},
            {"To", "runtime"},
            {"From", "from"}
        };
        send(Message(std::move(h)));
    }

    void recv(Message&& message) override
    {
        auto const& header = message.header();

        std::string to;
        std::string from;
        std::string subject;
        std::string body;

        if (header.contains("From")) {
            to = header["From"].get<std::string>();
        }

        if (header.contains("Reply-To")) {
            to = header["Reply-To"].get<std::string>();
        }

        if (header.contains("To")) {
            from = header["To"].get<std::string>();
        }

        for (;;) {
            std::cout << "To: " << to << "\t" << "Subject: " << subject << std::endl;
            if ((!std::getline(std::cin, body)) || body == "/x") {
                exit();
                return;
            }
            // parse command
            if (body.starts_with('/') && body.size() > 3) {
                if (body[1] == 's') {
                    // change subject
                    subject = body.substr(3);
                }
                else if (body[1] == 't') {
                    // change to
                    to = body.substr(3);
                }
            }
            else {
                break;
            }
        }

        json h{
            {"type", "agent:message"},
            {"From", from},
            {"To", to},
            {"Subject", subject}
        };

        send(Message(std::move(h), std::move(body)));
    }
};

int main(int argc, char** argv)
{
    CliServer server;

    CLI::App app{"Postline CLI server"};
    server.configure(app);
    CLI11_PARSE(app, argc, argv);

    std::cout << "/s subject | /t to | /x or Ctrl-D to exit" << std::endl;

    return server.run();
}
