#include <iostream>
#include <string>
#include <postline/actor.h>
#include <CLI/CLI.hpp>

using namespace postline;

int main(int argc, char *argv[])
{
    std::string address;
    std::string from = "user";
    {
        CLI::App app{"Postline actor testor"};
        argv = app.ensure_utf8(argv);
        app.add_option("-a,--address", address, "actor address");
        app.add_option("--from", from, "from address");
        CLI11_PARSE(app, argc, argv);
    }

    setup_environ();

    Actor* actor = new Actor("./actors/echo");

    for (;;) {
        std::string subject, body;
        std::cout << "Subject: " << std::flush;
        if (!std::getline(std::cin, subject)) {
            subject.clear();
        }
        if (subject.empty()) break;
        std::cout << "Body: " << std::flush;
        std::getline(std::cin, body);

        json header{
            {"From", from},
            {"To", address},
            {"Subject", subject},
            {"Body", body}
        };

        actor->send(Message(std::move(header)));

        Message echoed = actor->recv();
        std::cout << echoed.header().dump() << '\n';
    }
    delete actor;

    return 0;
}

