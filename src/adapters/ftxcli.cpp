#include <semaphore>

#include <postline/common.h>
#include <postline/service.h>
#include <postline/server.h>
#include <postline/ui/cli.hpp>


using namespace postline;

char const *LOCAL = R"(
[
{"from": "root", "address": "echo", "service": "pipe:echo", "flags": []},
{"from": "root", "address": "ai", "service": "pipe:claude", "flags": ["history"]},
{"from": "root", "address": "shell", "service": "pipe:shell", "flags": []},
{"from": "root", "address": "mcp", "service": "pipe:mcp_bridge", "flags": []},
{"from": "root", "address": "memory", "service": "pipe:echo -m", "flags": ["history"]},
{"from": "root", "address": "login", "service": "pipe:login", "flags": ["clone"]},
{"from": "root", "address": "benchmark", "service": "pipe:benchmark", "flags": []}
]
)";

class MessageHolder {
    std::mutex mutex_;
    std::condition_variable cv_;

    bool has_message_ = false;
    Message message_;

public:
    void put(Message message)
    {
        std::unique_lock lock(mutex_);

        cv_.wait(lock, [&]{
            return !has_message_;
        });

        message_ = std::move(message);
        has_message_ = true;

        cv_.notify_all();
    }

    Message get()
    {
        std::unique_lock lock(mutex_);

        cv_.wait(lock, [&]{
            return has_message_;
        });

        Message ret = std::move(message_);
        has_message_ = false;

        cv_.notify_all();

        return ret;
    }
};


class FTXCli : public Service {
public:
    FTXCli (Server::Config const &config)
        : server_(config),
        cli_([this](Message &&msg) {
            outgoing_.put(std::move(msg));
        })
    {
        json h{{"From", "user"},
               {"To", "runtime"},
               {"Subject", "spawn"}};
        outgoing_.put(Message(std::move(h), std::string(LOCAL)));
    }

    void init (Response &resp) {
        resp.append(outgoing_.get());
    }

    void call (Message &&msg, Response &resp) {
        cli_.recv(std::move(msg));
        resp.append(outgoing_.get());
    }

    void run () {

        server_thread_ = std::thread([this] {
            server_.run(this);
            cli_.request_exit();
        });
        cli_.run();
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
    }
private:
    std::thread server_thread_;
    Server server_;
    ftxui::CLI cli_;
    MessageHolder outgoing_;
};

int main(int argc, char **argv)
{
    Server::Config config;
    CLI::App app{"Postline FTXUI CLI"};
    config.configure(app);
    CLI11_PARSE(app, argc, argv);
    FTXCli cli(config);
    cli.run();
    std::cerr << "ftxcli exit" << std::endl;
    return 0;
}


