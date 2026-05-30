#include <postline/common.h>
#include <postline/service.h>
#include <postline/server.h>


namespace postline {

class Echo : public Server, public LinearService {

    int savedError;

    int getError (Message const &m, int e = 0) {
        try {
            return int(parse_i64(m.subject()));
        }
        catch (...) {
        }
        return e;
    }

protected:
    virtual void call (Message &&msg, Response &resp) {
        int error = getError(msg);
        resp.append(Message(json{{"Subject", "error"}}));
    }

public:
    Echo (Config const &c): Server(c), savedError(0) {
    }

    int run () {

        protocol::handshake::Hello::make().write(write_fd_);

        {
            std::vector<Message> resp = this->on_connect();
            for (auto &msg: resp) {
                msg.write(write_fd_);
            }
        }

        for (;;) {
            int error = savedError;
            Message msg = Message::read(read_fd_);
            error = getError(msg, error);

#if 0       // TODO
            try {
                // parse string msg.subject() into error;
            }
            catch (// complete this) {
            }
#endif

            std::string type = msg.type();

            if (type == protocol::handshake::Bye::type) {
                break;
            }

            if (type == protocol::handshake::BeginMemory::type) {
                for (;;) {
                    msg = Message::read(read_fd_);
                    if (msg.type() == protocol::handshake::EndMemory::type) {
                        break;
                    }
                    else {
                        this->on_memory(std::move(msg));
                    }
                }
                continue;
            }

            if (type == protocol::handshake::Multi::type) {
                CHECK(0);
#if 0
                protocol::handshake::Multi multi(msg);
                for (size_t i = 0; i < multi.count; ++i) {
                    Message msg = Message::read(read_fd_);
                    //recv(std::move(msg));
                }
                continue;
#endif
            }

            std::vector<Message> resp = this->on_message(std::move(msg));
            for (auto &msg: resp) {
                msg.write(write_fd_);
            }
        }
        this->on_exit();
        return 0;
    }
};

}

using namespace postline;

int main(int argc, char** argv)
{
    Server::Config config;
    {
        CLI::App app{"Postline echo server"};
        config.configure(app);
        CLI11_PARSE(app, argc, argv);
    }
    Echo echo(config);
    return echo.run();
}
