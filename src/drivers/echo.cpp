#include <postline/common.h>

#include <iostream>
#include <string>
#include <unistd.h>

using namespace postline;

int main()
{
    protocol::agent::Hello::make(0, 0).write(STDOUT_FILENO);

    for (;;) {
        Message message = Message::read(STDIN_FILENO);
        auto const& header = message.header();

        if (header.value("type", std::string()) == "agent:bye") {
            return 0;
        }

        message.updateHeader([](json &header) {
                std::swap(header["From"], header["To"]);
            });

        message.write(STDOUT_FILENO);
    }
    return 0;
}
