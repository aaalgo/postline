#include <semaphore>

#include <postline/common.h>
#include <postline/server.h>
#include <postline/ui/cli.hpp>

using namespace postline;

class FXTCliServer : public ServerBase {
public:
    FXTCliServer()
        : cli_([this](Message &&msg) {
              this->send(std::move(msg));
              can_receive_.release();
          })
    {}

    int run()
    {
        server_thread_ = std::thread([this] {
            ServerBase::run();
            cli_.request_exit();
        });

        cli_.run();

        can_receive_.release();

        if (server_thread_.joinable()) {
            server_thread_.join();
        }

        return 0;
    }

protected:
    void recv(Message &&msg) override
    {
        cli_.recv(std::move(msg));
        can_receive_.acquire();
    }

private:
    ftxui::CLI cli_;
    std::binary_semaphore can_receive_{0};
    std::thread server_thread_;
};

int main(int argc, char **argv)
{
    FXTCliServer server;

    CLI::App app{"Postline FTXUI CLI"};
    server.configure(app);
    CLI11_PARSE(app, argc, argv);

    server.run();
    std::cerr << "ftxcli exit" << std::endl;
    return 0;
}
