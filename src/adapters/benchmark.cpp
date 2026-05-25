#include <postline/common.h>
#include <postline/service.h>
#include <postline/server.h>

#include <chrono>
#include <random>
#include <sstream>


namespace postline {

class Benchmark : public LinearService {
    static constexpr char const* TARGET = "echo";
    static constexpr size_t BODY_SIZE = 4096;

    bool running = false;
    int64_t roundsTotal = 0;
    int64_t roundsTodo = 0;
    std::chrono::steady_clock::time_point begin;
    std::mt19937 random{std::random_device{}()};

    std::string make_body ()
    {
        std::string body;
        body.resize(BODY_SIZE);

        std::uniform_int_distribution<int> letters('a', 'z');
        for (char &ch: body) {
            ch = static_cast<char>(letters(random));
        }

        return body;
    }

protected:
    void call (Message&& msg, Response &resp) override
    {
        if (!running) {
            // msg is a request
            roundsTotal = parse_i64(msg.subject());
            CHECK(roundsTotal >= 0);
            roundsTodo = roundsTotal;
            running = true;
            begin = std::chrono::steady_clock::now();
        }
        else {
            CHECK(msg.from() == TARGET);
            CHECK(msg.header().contains("In-Reply-To"));
        }

        CHECK(running);

        if (roundsTodo <= 0) {
            running = false;
            auto end = std::chrono::steady_clock::now();
            std::chrono::duration<double> elapsed = end - begin;
            double seconds = elapsed.count();
            int64_t messages = roundsTotal * 2;

            std::ostringstream report;
            report << "round trips: " << roundsTotal << "\n"
                   << "messages: " << messages << "\n"
                   << "seconds: " << seconds << "\n"
                   << "messages per second: ";
            if (seconds > 0) {
                report << static_cast<double>(messages) / seconds;
            }
            else {
                report << "inf";
            }
            report << "\n";

            resp.append(Message(json(), report.str()));
        }
        else {
            int64_t flags = MESSAGE_QUIET;
            json header{{"To", TARGET},
                        {"Postline-Flags", std::format("{}", flags)}};
            resp.append(Message(std::move(header), make_body()));
            --roundsTodo;
        }
    }
};

}

using namespace postline;

int main(int argc, char** argv)
{
    Server::Config config;
    {
        CLI::App app{"Postline benchmark server"};
        config.configure(app);
        CLI11_PARSE(app, argc, argv);
    }
    Benchmark service;
    Server server(config);
    return server.run(&service);
}
