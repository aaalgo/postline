#include <cstdlib>
#include <unistd.h>

#include <postline/common.h>
#include <postline/service.h>
#include <postline/server.h>


namespace postline {

class Chaos : public LinearService {
private:
    void call (Message&& message, Response &resp) override
    {
        int64_t mode = parse_i64(message.subject());

        switch (mode) {
        case 0:
            std::abort();
        case 1:
            resp.append(protocol::handshake::Hello::make());
            return;
        case 2:
            resp.append(Message(json{{"Subject", 0}}));
            return;
        case 3:
            for (;;) {
                ::pause();
            }
        default:
            resp.append(Message(json{{"Subject", "chaos mode exhausted"}}));
            return;
        }
    }

public:
    Chaos () {
    }
};

}

using namespace postline;

int main(int argc, char** argv)
{
    Server::Config config;
    {
        CLI::App app{"Postline chaos server"};
        config.configure(app);
        CLI11_PARSE(app, argc, argv);
    }
    Chaos chaos;
    Server server(config);
    return server.run(&chaos);
}
