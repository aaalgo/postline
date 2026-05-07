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

#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <postline/common.h>
#include <postline/service.h>
#include <postline/server.h>

namespace postline {
using namespace std::chrono_literals;

constexpr std::string DEFAULT_HOME = "./home";

struct TerminalContext {
    std::string delta;
    bool exited = false;
    int exit_status = -1;
};

static std::string safe_name(std::string const& s)
{
    std::string out;

    for (char c : s) {
        unsigned char u = static_cast<unsigned char>(c);

        if (std::isalnum(u) || c == '-' || c == '_' || c == '.') {
            out.push_back(c);
        } else {
            out.push_back('_');
        }
    }

    CHECK(!out.empty());
    return out;
}

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

class AnsiTerminalDriver {
public:
    AnsiTerminalDriver(std::string home,
                       std::vector<std::string> argv)
        : home_(std::move(home)),
          argv_(std::move(argv))
    {
        CHECK(!argv_.empty());
    }

    ~AnsiTerminalDriver()
    {
        close();
    }

    AnsiTerminalDriver(AnsiTerminalDriver const&) = delete;
    AnsiTerminalDriver& operator=(AnsiTerminalDriver const&) = delete;

    void start()
    {
        CHECK(master_fd_ < 0);

        std::filesystem::create_directories(home_);

        int master_fd = -1;
        int slave_fd = -1;

        CHECK(::openpty(&master_fd, &slave_fd, nullptr, nullptr, nullptr) == 0);

        pid_t pid = ::fork();
        CHECK(pid >= 0);

        if (pid == 0) {
            ::close(master_fd);

            if (::setsid() < 0) {
                _exit(127);
            }

            if (::ioctl(slave_fd, TIOCSCTTY, 0) < 0) {
                _exit(127);
            }

            if (::dup2(slave_fd, STDIN_FILENO) < 0) {
                _exit(127);
            }

            if (::dup2(slave_fd, STDOUT_FILENO) < 0) {
                _exit(127);
            }

            if (::dup2(slave_fd, STDERR_FILENO) < 0) {
                _exit(127);
            }

            if (slave_fd > STDERR_FILENO) {
                ::close(slave_fd);
            }

            ::setenv("HOME", home_.c_str(), 1);
            ::setenv("SHELL", argv_[0].c_str(), 1);

            if (::chdir(home_.c_str()) < 0) {
                _exit(127);
            }

            std::vector<char*> args;
            args.reserve(argv_.size() + 1);

            for (auto& s : argv_) {
                args.push_back(s.data());
            }

            args.push_back(nullptr);

            ::execvp(args[0], args.data());
            _exit(127);
        }

        ::close(slave_fd);

        master_fd_ = master_fd;
        child_pid_ = pid;

        int flags = ::fcntl(master_fd_, F_GETFL, 0);
        CHECK(flags >= 0);
        CHECK(::fcntl(master_fd_, F_SETFL, flags | O_NONBLOCK) == 0);
    }

    void write(std::string_view input)
    {
        CHECK(master_fd_ >= 0);

        char const* p = input.data();
        size_t left = input.size();

        while (left > 0) {
            ssize_t n = ::write(master_fd_, p, left);

            if (n > 0) {
                p += n;
                left -= static_cast<size_t>(n);
                continue;
            }

            if (n < 0 && errno == EINTR) {
                continue;
            }

            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                pollfd pfd{};
                pfd.fd = master_fd_;
                pfd.events = POLLOUT;

                int r = ::poll(&pfd, 1, -1);
                if (r < 0 && errno == EINTR) {
                    continue;
                }

                CHECK(r >= 0);
                continue;
            }

            CHECK(false);
        }
    }

    TerminalContext read_until_quiet(
        std::chrono::milliseconds first_timeout = 2000ms,
        std::chrono::milliseconds quiet_timeout = 200ms)
    {
        CHECK(master_fd_ >= 0);

        TerminalContext ctx;
        bool seen_data = false;

        for (;;) {
            int timeout_ms = static_cast<int>(
                (seen_data ? quiet_timeout : first_timeout).count());

            pollfd pfd{};
            pfd.fd = master_fd_;
            pfd.events = POLLIN | POLLHUP | POLLERR;

            int r = ::poll(&pfd, 1, timeout_ms);

            if (r < 0) {
                if (errno == EINTR) {
                    continue;
                }

                CHECK(false);
            }

            if (r == 0) {
                ctx.exited = check_child_exited(ctx.exit_status);
                return ctx;
            }

            if (pfd.revents & (POLLIN | POLLHUP | POLLERR)) {
                for (;;) {
                    std::array<char, 4096> buf{};

                    ssize_t n = ::read(master_fd_, buf.data(), buf.size());

                    if (n > 0) {
                        seen_data = true;
                        ctx.delta.append(buf.data(), static_cast<size_t>(n));
                        continue;
                    }

                    if (n == 0) {
                        ctx.exited = true;
                        check_child_exited(ctx.exit_status);
                        return ctx;
                    }

                    if (errno == EINTR) {
                        continue;
                    }

                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }

                    if (errno == EIO) {
                        ctx.exited = true;
                        check_child_exited(ctx.exit_status);
                        return ctx;
                    }

                    CHECK(false);
                }
            }
        }
    }

    void close()
    {
        if (master_fd_ >= 0) {
            ::close(master_fd_);
            master_fd_ = -1;
        }

        if (child_pid_ > 0) {
            int status = 0;
            pid_t r = ::waitpid(child_pid_, &status, WNOHANG);

            if (r == 0) {
                ::kill(child_pid_, SIGHUP);
                ::waitpid(child_pid_, &status, 0);
            }

            child_pid_ = -1;
        }
    }

private:
    bool check_child_exited(int& exit_status)
    {
        if (child_pid_ <= 0) {
            return true;
        }

        int status = 0;
        pid_t r = ::waitpid(child_pid_, &status, WNOHANG);

        if (r == 0) {
            return false;
        }

        CHECK(r == child_pid_);

        if (WIFEXITED(status)) {
            exit_status = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            exit_status = 128 + WTERMSIG(status);
        }

        child_pid_ = -1;
        return true;
    }

private:
    std::string home_;
    std::vector<std::string> argv_;

    int master_fd_ = -1;
    pid_t child_pid_ = -1;
};

class Login : public Service {
public:
    void configure(CLI::App& app)
    {
        app.add_option("--home", home,
                       "Base HOME directory for login sessions");

        app.add_option("--shell", shell,
                       "Shell executable");
    }

protected:
    void call (Message&& message, Response &resp) override
    {
        if (!terminal_) {
            std::string const& user = message.from();
            std::string user_home = home + "/" + safe_name(user);

            terminal_.emplace(
                std::move(user_home),
                std::vector<std::string>{
                    shell,
                    "--noprofile",
                    "--norc"
                });

            terminal_->start();

            TerminalContext ctx = terminal_->read_until_quiet();
            transcript_.append(strip_ansi(ctx.delta));

            resp.append(send_transcript("login"));
        }

        std::string input = message.body();

        if (input.empty() || input.back() != '\n') {
            input.push_back('\n');
        }

        transcript_.append(input);

        terminal_->write(input);

        TerminalContext ctx = terminal_->read_until_quiet();
        transcript_.append(strip_ansi(ctx.delta));

        resp.append(send_transcript("terminal"));
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

    std::optional<AnsiTerminalDriver> terminal_;
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
