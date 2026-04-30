#include <postline/common.h>
#include <postline/server.h>

using namespace postline;

class EchoServer : public ServerBase {
protected:
    void recv(Message&& message) override
    {
        message.updateHeader([](json& header) {
            std::swap(header["From"], header["To"]);
        });

        send(std::move(message));
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
