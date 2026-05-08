#include <postline/common.h>
#include <postline/service.h>
#include <postline/server.h>


namespace postline {

class Benchmark : public Service {
    std::string constexpr TARGET("echo");
    bool running = false;
    int roundsTotal = 0;
    int roundsTodo = 0;
    std::string replyTo;
    // add begin time
protected:
    void call (Message&& msg, Response &resp) override
    {
        if (running = false) {
            // msg is a request
            replyTo = msg.from();
            
            roundsTotal = parse_i64(msg.subject());
            roundsTodo = roundsTotal;
            running = true;
            // record begin time
        }
        CHECK(running);
        if (roundsTodo == 0) {
            running = false;
            // measure time
            std::ostringstream report;
            // generate a report, you might want to replace the above
            // report with ostringstream
            // report number of messages done and messages per second
            resp.append(Message(json(), report.str()));
        }
        else {
            json header{{"To", TARGET}}
            std::string body;
            // generate a random body of 4096 letters
            resp.append(Message(json(), report));
            --roundsTodo;
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
        CLI11_PARSE(app, argc, argv);
    }
    Benchmark service;
    Server server(config);
    return server.run(&service);
}
