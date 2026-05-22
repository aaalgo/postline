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
    struct Thread {
        std::string thread_id;
        std::string to;
        std::string subject;
    };

    void recv_thread () {
        for (;;) {
            Message msg = Message::read(read_fd_);
            std::string type = msg.type();

            if (type == protocol::handshake::Bye::type) {
                break;
            }
        }
    }

    std::vector<std::unique_ptr<Thread>> threads;
    Thread *current;

public:
    Cli (Config const &config): Server(config) {
    }

    int run () {
        std::thread thread([this](){this->recv_thread();});
        protocol::handshake::Hello::make().write(write_fd_);
        {
            json h{{"To", "runtime"},
                   {"Subject", "create_domain"},
                   {"Thread-ID", "0"}
                };
            json op{{"detach", true}};
            Message(std::move(h),op.dump()).write(write_fd_);
        }
        {
            json h{{"To", "runtime"},
                   {"Subject", "create_agents"},
                   {"Thread-ID", "1"},
                };
            Message(std::move(h),std::string(LOCAL_AGENTS)).write(write_fd_);
        }
        {
            json h{{"To", "runtime"},
                   {"Subject", "create_domain"},
                   {"Thread-ID", "0"}
                };
            json op{{"detach", true}};
            Message(std::move(h),op.dump()).write(write_fd_);
        }
        {
            json h{{"To", "runtime"},
                   {"Subject", "create_agents"},
                   {"Thread-ID", "2"},
                };
static char const *LOCAL_AGENTS_2 = R"(
[
{"from": "zero", "name": "echo", "service": "pipe:echo", "flags": []},
{"from": "zero", "name": "ai1", "comment": "openai", "service": "pipe:openai", "flags": ["history"]},
{"from": "zero", "name": "ai2", "comment": "anthropic", "service": "pipe:claude", "flags": ["history"]},
{"from": "zero", "name": "ai3", "comment": "openrouter", "service": "pipe:v1", "flags": ["history"]},
{"from": "zero", "name": "shell", "service": "pipe:shell", "flags": []},
{"from": "zero", "name": "mcp", "service": "pipe:mcp_bridge", "flags": []},
{"from": "zero", "name": "memory", "service": "pipe:echo -m", "flags": ["history"]},
{"from": "zero", "name": "login", "service": "pipe:login", "flags": ["clone"]},
{"from": "zero", "name": "benchmark", "service": "pipe:benchmark", "flags": []}
]
)";
            Message(std::move(h),std::string(LOCAL_AGENTS_2)).write(write_fd_);
        }
        bool stop = false;
        threads.push_back(std::make_unique<Thread>());
        threads.push_back(std::make_unique<Thread>());
        threads.push_back(std::make_unique<Thread>());
        current = threads[2].get();
        current->thread_id = "2";
        current->to = "runtime";
        current = threads[1].get();
        current->thread_id = "1";
        current->to = "runtime";
        current = threads[0].get();
        current->thread_id = "0";
        current->to = "runtime";

        while (!stop) {
            std::cout << "Thread: " << current->thread_id << std::endl;
            std::cout << "To: " << current->to << "\t" << "Subject: " << current->subject << std::endl;
            std::string body;
            if ((!std::getline(std::cin, body)) || body.starts_with("/x")) {
                json h{
                    {"To", "runtime"},
                    {"Subject", "exit"},
                    {"Thread-ID", "0"},
                };
                Message(std::move(h)).write(write_fd_);
                stop = true;
                continue;
            }
            // parse command
            if (body.starts_with("/s ")) {
                current->subject = trim(body.substr(3));
                continue;
            }
            if (body.starts_with("/t ")) {
                current->to = trim(body.substr(3));
                continue;
            }
            if (body == "/0") {
                current = threads[0].get();
                continue;
            }
            if (body == "/1") {
                current = threads[1].get();
                continue;
            }
            if (body == "/2") {
                current = threads[1].get();
                continue;
            }
            if (body.starts_with("/")) {
                std::cout << "Unknown command." << std::endl;
                continue;
            }
            json h{
                {"type", "agent:message"},
                {"To", current->to},
                {"Subject", current->subject},
                {"Thread-ID", current->thread_id},
            };
            if (current->to == "runtime" && current->subject == "exit") {
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
