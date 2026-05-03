#include <postline/common.h>
#include <postline/server.h>

using namespace postline;

class EchoServer : public ServerBase {
    bool remember;
    json memory;

protected:

    virtual DriverHistoryMode history_mode() const noexcept {
        return remember ? DriverHistoryMode::ALL : DriverHistoryMode::NONE;
    }

    void updateMemory(Message&& message) override
    {
        if (remember) {
            memory.emplace_back(message.header());
        }
    }

    void recv(Message&& message) override
    {
        if (remember) {
            memory.emplace_back(message.header());
            json header{{"From", message.to()},
                        {"To", message.from()},
                        {"Subject", "memory"},
                        {"In-Reply-To", message.header()["Message-ID"]},
            };
            std::string body = memory.dump(4);
            send(Message(std::move(header), std::move(body)));
        }
        else {
            message.updateHeader([](json& header) {
                std::swap(header["From"], header["To"]);
                header["In-Response-To"] = header["Message-ID"];
                header.erase("Message-ID");
            });
            send(std::move(message));
        }
        return;
    }
public:
    EchoServer (): remember(false), memory(json::array()) {
    }

    void configure(CLI::App& app) override {
        ServerBase::configure(app);
        app.add_flag("-m,--memory", remember, "with memory");
    }


};

int main(int argc, char** argv)
{
    EchoServer server;

    CLI::App app{"Postline echo server"};
    server.configure(app);
    CLI11_PARSE(app, argc, argv);

    return server.run();
}
