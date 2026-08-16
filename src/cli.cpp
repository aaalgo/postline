#include <iostream>
#include <string>
#include <postline/ui.h>


namespace postline { namespace ui {

std::string trim(const std::string& s) {
    auto start = std::find_if_not(s.begin(), s.end(),
        [](unsigned char c){ return std::isspace(c); });

    auto end = std::find_if_not(s.rbegin(), s.rend(),
        [](unsigned char c){ return std::isspace(c); }).base();

    return (start < end) ? std::string(start, end) : std::string();
}

class CLI : public UI {
    std::vector<std::string> tags{"genesis"};

    struct Thread {
        ThreadID thread_id;
        std::string to;
        std::string subject;
    };

    std::vector<std::unique_ptr<Thread>> threads;
    Thread *current;

public:
    CLI () {
    }

    void run () {
        bool stop = false;
        threads.push_back(std::make_unique<Thread>());
        threads.push_back(std::make_unique<Thread>());
        threads.push_back(std::make_unique<Thread>());
        current = threads[2].get();
        current->thread_id = 2;
        current->to = "runtime";
        current = threads[1].get();
        current->thread_id = 1;
        current->to = "runtime";
        current = threads[0].get();
        current->thread_id = 0;
        current->to = "runtime";

        while (!stop) {
            std::cout << "Thread: " << current->thread_id << std::endl;
            std::cout << "To: " << current->to << "\t" << "Subject: " << current->subject << std::endl;
            std::string body;
            if ((!std::getline(std::cin, body)) || body.starts_with("/x")) {
                waitUntilIdle(0);
                json h{
                    {"To", "runtime"},
                    {"Subject", "exit"},
                };
                send(0, Message(std::move(h)));
                stop = true;
                continue;
            }
            // parse command
            if (body.starts_with("/s ")) {
                current->subject = trim(body.substr(3));
                continue;
            }
            if (body.starts_with("/t ")) {
                current->to = trim(body.substr(3));
                continue;
            }
            if (body == "/0") {
                current = threads[0].get();
                continue;
            }
            if (body == "/1") {
                current = threads[1].get();
                continue;
            }
            if (body == "/2") {
                current = threads[1].get();
                continue;
            }
            if (body.starts_with("/")) {
                std::cout << "Unknown command." << std::endl;
                continue;
            }
            json h{
                {"type", "agent:message"},
                {"To", current->to},
                {"Subject", current->subject},
                {POSTLINE_TAGS_HEADER_NAME, tags},
            };
            if (current->to == "runtime" && current->subject == "exit") {
                stop = true;
            }
            send(current->thread_id, Message(std::move(h), std::move(body)));
            tags.clear();
        }
        std::cerr << "Stopping..." << std::endl;
    }
};

std::unique_ptr<UI> make_cli () {
    return std::make_unique<CLI>();
}

}}
