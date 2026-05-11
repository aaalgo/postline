#pragma once
#include <array>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <postline/ansi.h>
#include <postline/common.h>

namespace postline {

using namespace std::chrono_literals;

struct TerminalContext {
    std::string delta;
    bool exited = false;
    int exit_status = -1;
};

class PtyDriver : immobile {
public:
    PtyDriver(std::string home,
              std::vector<std::string> argv,
              std::string log_path)
        : home_(std::move(home)),
          argv_(std::move(argv)),
          log_path_(std::move(log_path))
    {
        CHECK(!argv_.empty());
        CHECK(!log_path_.empty());
    }

    ~PtyDriver()
    {
        stop();

        if (log_fd_ >= 0) {
            ::close(log_fd_);
            log_fd_ = -1;
        }
    }

    void start()
    {
        CHECK(master_fd_ < 0);

        std::filesystem::create_directories(home_);

        log_fd_ = ::open(
            log_path_.c_str(),
            O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
            0644);

        CHECK(log_fd_ >= 0);

        int master_fd = -1;
        int slave_fd = -1;

        winsize ws{};
        ws.ws_row = 40;
        ws.ws_col = 120;

        CHECK(::openpty(&master_fd, &slave_fd, nullptr, nullptr, &ws) == 0);

        termios tio{};
        CHECK(::tcgetattr(slave_fd, &tio) == 0);
        tio.c_lflag &= ~ECHO;
        CHECK(::tcsetattr(slave_fd, TCSANOW, &tio) == 0);

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
            ::setenv("TERM", "dumb", 1);
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

    void stop()
    {
        if (child_pid_ > 0) {
            int status = 0;
            pid_t r = ::waitpid(child_pid_, &status, WNOHANG);

            if (r == 0) {
                ::kill(child_pid_, SIGHUP);
                ::waitpid(child_pid_, &status, 0);
            }

            child_pid_ = -1;
        }

        if (master_fd_ >= 0) {
            ::close(master_fd_);
            master_fd_ = -1;
        }
    }

    void write(std::string_view input)
    {
        CHECK(master_fd_ >= 0);

        append_log_input(input);

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

                int r = ::poll(&pfd, 1, 5000);
                CHECK(r > 0);
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
                        append_log_output(buf.data(), static_cast<size_t>(n));
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

private:
    void append_log_output(void const* buf, std::size_t n)
    {
        if (log_fd_ < 0 || n == 0) {
            return;
        }

        write_all(log_fd_, buf, n);
    }

    void append_log_input(std::string_view input)
    {
        if (log_fd_ < 0 || input.empty()) {
            return;
        }

        write_all(log_fd_, ansi::cyan, std::strlen(ansi::cyan));
        write_all(log_fd_, input.data(), input.size());
        write_all(log_fd_, ansi::reset, std::strlen(ansi::reset));
    }

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
    std::string log_path_;

    int master_fd_ = -1;
    int log_fd_ = -1;
    pid_t child_pid_ = -1;
};

}
