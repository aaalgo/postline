#include <postline/common.h>
#include <postline/service.h>
#include <postline/server.h>


namespace postline {

class Echo : public LinearService {
    json memory;
public:
    Echo (bool memory_) {
        if (memory_) {
            memory = json::array();
        }
    }

    void on_memory (Message&& message) override
    {
        if (memory.is_array()) {
            memory.emplace_back(message.header());
        }
    }
protected:
    void call (Message&& message, Response &resp) override
    {
        if (memory.is_array()) {
            memory.emplace_back(message.header());
            std::string body = memory.dump(4);
            resp.append(Message(json(), std::move(body)));
        }
        else {
            message.updateHeader([](json& header) {
                header.erase("From");
                header.erase("To");
                header.erase("Message-ID");
            });
            resp.append(std::move(message));
        }
    }
};

}

using namespace postline;

int main(int argc, char** argv)
{
    bool memory = false;
    Server::Config config;
    {
        CLI::App app{"Postline echo server"};
        config.configure(app);
        app.add_flag("-m,--memory", memory, "with memory");
        CLI11_PARSE(app, argc, argv);
    }
    Echo echo(memory);
    Server server(config);
    return server.run(&echo);
}

