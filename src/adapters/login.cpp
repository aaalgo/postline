#include <array>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <cerrno>
#include <cstdlib>

#include <postline/common.h>
#include <postline/service.h>
#include <postline/server.h>
#include "pty.h"

namespace postline {
using namespace std::chrono_literals;

constexpr std::string DEFAULT_HOME = "./home";

static std::string strip_ansi(std::string const& in)
{
    std::string out;
    out.reserve(in.size());

    for (size_t i = 0; i < in.size();) {
        unsigned char c = static_cast<unsigned char>(in[i]);

        if (c == 0x1b) {
            // ESC [ ... final-byte
            if (i + 1 < in.size() && in[i + 1] == '[') {
                i += 2;

                while (i < in.size()) {
                    char x = in[i++];
                    if (x >= '@' && x <= '~') {
                        break;
                    }
                }

                continue;
            }

            // Other ESC sequence: drop ESC itself.
            ++i;
            continue;
        }

        // Ignore CR. This is approximate, not a real screen emulator.
        if (c == '\r') {
            ++i;
            continue;
        }

        // Drop other C0 control chars except common whitespace.
        if (c < 0x20 && c != '\n' && c != '\t') {
            ++i;
            continue;
        }

        out.push_back(static_cast<char>(c));
        ++i;
    }

    return out;
}

static std::string last_n_lines(std::string const& s, size_t n)
{
    size_t pos = s.size();
    size_t count = 0;

    while (pos > 0 && count <= n) {
        --pos;
        if (s[pos] == '\n') {
            ++count;
        }
    }

    if (count > n) {
        ++pos;
    } else {
        pos = 0;
    }

    return s.substr(pos);
}

class Login : public Service {
public:
    void configure(CLI::App& app)
    {
        app.add_option("--home", home,
                       "Base HOME directory for login sessions");

        app.add_option("--shell", shell,
                       "Shell executable");

        app.add_option("--read-first-timeout-ms", read_first_timeout_ms,
                       "Milliseconds to wait for first terminal output");

        app.add_option("--read-quiet-timeout-ms", read_quiet_timeout_ms,
                       "Milliseconds of terminal silence before responding");
    }

protected:
    void call (Message&& message, Response &resp) override
    {
        std::string const &my_addr = message.to();
        if (!terminal_) {
            std::string const& user = message.from();
            std::string user_home = home + "/" + user;
            std::string log_path = my_addr + ".log";

            terminal_.emplace(
                std::move(user_home),
                std::vector<std::string>{
                    shell,
                    "--noprofile",
                    "--norc"
                },
                log_path
                );

            terminal_->start();

            TerminalContext ctx = terminal_->read_until_quiet(
                std::chrono::milliseconds(read_first_timeout_ms),
                std::chrono::milliseconds(read_quiet_timeout_ms));
            transcript_.append(strip_ansi(ctx.delta));

            resp.append(send_transcript("login"));
            return;
        }

        std::string input_buffer = message.body();

        if (input_buffer.empty() || input_buffer.back() != '\n') {
            input_buffer.push_back('\n');
        }

        for (;;) {
            size_t nl = input_buffer.find('\n');

            if (nl == std::string::npos) {
                break;
            }

            std::string input = input_buffer.substr(0, nl + 1);
            std::string remainder = input_buffer.substr(nl + 1);

            transcript_.append(input);

            terminal_->write(input);

            TerminalContext ctx = terminal_->read_until_quiet(
                std::chrono::milliseconds(read_first_timeout_ms),
                std::chrono::milliseconds(read_quiet_timeout_ms));

            std::string delta = strip_ansi(ctx.delta);
            transcript_.append(delta);

            if (ctx.exited) {
                transcript_.append("\n[terminal exited, status ");
                transcript_.append(std::to_string(ctx.exit_status));
                transcript_.append("]\n");
                transcript_.append("[Next messsage will restart terminal.]\n");

                terminal_.reset();

                resp.append(send_transcript("exit"));
                break;
            }

            if (!remainder.starts_with(delta)) {
                resp.append(send_transcript("terminal"));
                break;
            }

            remainder.erase(0, delta.size());
            input_buffer = std::move(remainder);

            if (input_buffer.empty()) {
                resp.append(send_transcript("terminal"));
                break;
            }
        }
    }

    void on_exit() override
    {
        terminal_.reset();
    }

private:
    Message send_transcript(std::string const& subject)
    {
        std::string body = last_n_lines(transcript_, 50);
        return Message(json{{"Subject", subject}}, std::move(body));
    }

private:
    std::string home = DEFAULT_HOME;
    std::string shell = "/bin/bash";
    int read_first_timeout_ms = 2000;
    int read_quiet_timeout_ms = 200;

    std::optional<PtyDriver> terminal_;
    std::string transcript_;
};

}

using namespace postline;

int main(int argc, char** argv)
{
    Login login;
    Server::Config config;

    CLI::App app{"Postline login server"};
    config.configure(app);
    login.configure(app);
    CLI11_PARSE(app, argc, argv);

    Server server(config);


    return server.run(&login);
}
