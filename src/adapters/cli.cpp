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

class Cli : public Server {
    /*
    struct Thread {
        ThreadID thread_id;
        DomainID domain_id;
    };
    */

    void recv_thread () {
        for (;;) {
            Message msg = Message::read(read_fd_);
            std::string type = msg.type();

            if (type == protocol::handshake::Bye::type) {
                break;
            }
        }
    }

public:
    Cli (Config const &config): Server(config) {
    }

    int run () {
        std::thread thread([this](){this->recv_thread();});
        protocol::handshake::Hello::make().write(write_fd_);
        std::string to = "runtime";
        std::string subject;
        std::string body;
        std::string thread_id = "0";
        std::string domain_id = "0";
        {
            json h{{"To", "runtime"},
                   {"Subject", "create_agents"},
                   {"Thread-ID", thread_id},
                   {"From-Domain-ID", domain_id}
                };
            Message(std::move(h),std::string(LOCAL_AGENTS)).write(write_fd_);
        }
        bool stop = false;
        while (!stop) {
            std::cout << "Thread: " << thread_id << "\t" << "Domain: " << domain_id << std::endl;
            std::cout << "To: " << to << "\t" << "Subject: " << subject << std::endl;
            if ((!std::getline(std::cin, body)) || body.starts_with("/x")) {
                json h{
                    {"To", "runtime"},
                    {"Subject", "exit"},
                    {"Thread-ID", thread_id},
                    {"From-Domain-ID", domain_id}
                };
                Message(std::move(h)).write(write_fd_);
                stop = true;
                continue;
            }
            // parse command
            if (body.starts_with("/s ")) {
                subject = trim(body.substr(3));
                continue;
            }
            if (body.starts_with("/t ")) {
                to = trim(body.substr(3));
                continue;
            }
            if (body.starts_with("/")) {
                std::cout << "Bad command." << std::endl;
                continue;
            }
            json h{
                {"type", "agent:message"},
                {"To", to},
                {"Subject", subject},
                {"Thread-ID", thread_id},
                {"From-Domain-ID", domain_id}
            };
            if (to == "runtime" && subject == "exit") {
                stop = true;
            }
            Message(std::move(h), std::move(body)).write(write_fd_);
        }
        std::cerr << "Stopping..." << std::endl;
        thread.join();
        return 0;
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
    Cli cli(config);;

    std::cout << "/s subject | /t to | /x or Ctrl-D to exit" << std::endl;

    return cli.run();
}
