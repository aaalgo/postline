#include <iostream>
#include <string>

#include <postline/common.h>
#include <postline/service.h>
#include <postline/server.h>


namespace postline {

std::string trim(const std::string& s) {
    auto start = std::find_if_not(s.begin(), s.end(),
        [](unsigned char c){ return std::isspace(c); });

    auto end = std::find_if_not(s.rbegin(), s.rend(),
        [](unsigned char c){ return std::isspace(c); }).base();

    return (start < end) ? std::string(start, end) : std::string();
}

class Cli : public Service {
public:
    Cli () {
    }
protected:
    std::vector<Message> on_connect () override {
        json h{{"From", "user"},
               {"To", "runtime"},
               {"Subject", "list_agents"},
               {"Thread-ID", "0"},
               {"From-Domain-ID", "0"}}
               ;
        std::vector<Message> resp;
        resp.emplace_back(std::move(h)); //, std::string(LOCAL_AGENTS));
        return resp;
    }

    void call (Message&& message, Response &resp) override
    {
        std::cout << "======== " << message.type() << std::endl;
        if (message.header().contains("To")) {
            message.formatEmail(std::cout);
        }
        else {
            std::cout << message.header().dump(4) << std::endl;
            std::cout << message.body() << std::endl;
        }
        std::cout << "========" << std::endl;

        auto const& header = message.header();

        std::string to;
        std::string subject;
        std::string body;

        if (header.contains("From")) {
            to = header["From"].get<std::string>();
        }

        if (header.contains("Reply-To")) {
            to = header["Reply-To"].get<std::string>();
        }

        for (;;) {
            std::cout << "To: " << to << "\t" << "Subject: " << subject << std::endl;
            if ((!std::getline(std::cin, body)) || body == "/x") {
                json h{
                    {"To", "runtime"},
                    {"Subject", "exit"}
                };
                resp.append(Message(std::move(h)));
                return;
            }
            // parse command
            if (body.starts_with('/') && body.size() > 3) {
                if (body[1] == 's') {
                    // change subject
                    subject = trim(body.substr(3));
                }
                else if (body[1] == 't') {
                    // change to
                    to = trim(body.substr(3));
                }
            }
            else {
                break;
            }
        }

        json h{
            {"type", "agent:message"},
            {"To", to},
            {"Subject", subject}
        };

        resp.append(Message(std::move(h), std::move(body)));
    }
};
}

using namespace postline;

int main(int argc, char** argv)
{
    Server::Config config;

    CLI::App app{"Postline CLI server"};
    config.configure(app);
    CLI11_PARSE(app, argc, argv);
    Server server(config);
    Cli cli;

    std::cout << "/s subject | /t to | /x or Ctrl-D to exit" << std::endl;

    return server.run(&cli);
}
