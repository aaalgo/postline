#include <iostream>
#include <string>

#include "postline/common.h"
#include "postline/journal.h"

using namespace postline;

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: inspect_journal path_to_journal\n";
        return 1;
    }

    Journal journal(std::string(), argv[1], [](Message&&) {});

    AccessID access_id;
    while (std::cin >> access_id) {
        Message message = journal.read(access_id);
        std::cout << "======== access_id=" << access_id << '\n';
        if (message.header().contains("To")) {
            message.formatEmail(std::cout);
        }
        else {
            std::cout << message.header().dump(4) << std::endl;
            if (!message.body().empty()) {
                std::cout << message.body() << std::endl;
            }
        }
        std::cout << std::endl;
    }

    return 0;
}
