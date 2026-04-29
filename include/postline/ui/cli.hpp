#pragma once

namespace ftxui {
    using namespace postline;
    // CLI should display a user interface for the user to
    // fill in the fields of an email
    //
    // Root layout: top (1 row) - bottom (Input, textarea occupying rest)
    //
    // top: left: To: [select box]  Subject: [input box]
    //
    // When enter is hit, it should check `canSend`
    // if true, collect the fields
    // and makeMessage and call the sendCallback,
    // after sending, set canSend = false;
    // 
    // If enter is hit when canSend is false,
    // it should do nothing.
    //

    class CLI {
        std::string from;
        std::unordered_set<std::string> toList;
        bool canSend;

        Message makeMessage (std::string &&to,
                             std::string &&subject,
                             std::string &&body) {
            json header{
            };
            return Message(std::move(header), std::move(body));
        }

        void process (Message &&msg) {
            // leave it there for me to implement
            json const &header = msg.header();
            if (header.contains("type") && !header["type"].is_null()) {
                if (header.value("type", std::string()) == "agent:bye") {
                    // request the FTXUI to quit
                    return;
                }
            }

            std::string to;

            if (header.contains("From")) {
                to = header["From"].get<std::string>();
                toList.insert(to);

            }
            if (header.contains("Reply-To")) {
                to = header["Reply-To"].get<std::string>();
                toList.insert(to);
            }
            if (header.contains("To")) {
                from = header["To"].get<std::string>();
                toList.insert(to);
            }

            canSend = true;

            // set To: field to `to`
            // sort toList and supply that to the To: field select list
        }
    public:
        CLI (std::function<void(json &&)> sendCallback): canSend(false) {
            // save sendCallback
        }

        void recv (Message &&msg) {
            // this is called async
            // use FTXUI's post to pass msg to this->process
        
        }

        void run () {
            // this should run the loop
            // it's supposed to block while the UI runs
        }
    };
}
