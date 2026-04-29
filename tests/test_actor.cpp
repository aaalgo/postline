#include <iostream>
#include <string>
#include <postline/driver.h>
#include <CLI/CLI.hpp>

using namespace postline;

int main(int argc, char *argv[])
{
    std::string address;
    std::string from = "user";
    {
        CLI::App app{"Postline driver tester"};
        argv = app.ensure_utf8(argv);
        app.add_option("-a,--address", address, "driver address");
        app.add_option("--from", from, "from address");
        CLI11_PARSE(app, argc, argv);
    }

    setup_environ();

    Driver* driver = new ShellDriver("./install/bin/drivers/echo");

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

        driver->send(Message(std::move(header)));

        Message echoed = driver->recv_one();
        std::cout << echoed.header().dump() << '\n';
    }
    delete driver;

    return 0;
}
