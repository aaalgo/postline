#pragma once
#include <sys/types.h>
#include <sys/eventfd.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include "common.h"

namespace postline {


enum class DriverSpawnType : std::uint16_t
{
    ADDRESS = 0,
    SCOPE  = 1
};

enum class DriverHistoryMode : std::uint16_t
{
    NONE = 0,
    ALL  = 1
};

class Driver: noncopyable {
public:
    virtual ~Driver () {}
    virtual void send(Message &&msg) = 0;
    virtual std::vector<Message> recv() = 0;
    Message recv_one () {
        std::vector<Message> all = recv();
        CHECK(all.size() == 1);
        return std::move(all[0]);
    }
    virtual int read_fd () const {
        return -1;
    }
    virtual DriverSpawnType spawn_type() const noexcept = 0;
    virtual DriverHistoryMode history_mode() const noexcept = 0;
};

class ShellDriver: public Driver {
    DriverSpawnType spawn_;
    DriverHistoryMode history_;

    int input_fd_ = -1;
    int output_fd_ = -1;
    pid_t pid_ = -1;
public:
    explicit ShellDriver(std::string const &command)
    {
        int stdin_pipe[2] = {-1, -1};
        int stdout_pipe[2] = {-1, -1};

        if (pipe(stdin_pipe) < 0) throw std::runtime_error("pipe failed");
        if (pipe(stdout_pipe) < 0) {
            close(stdin_pipe[0]);
            close(stdin_pipe[1]);
            throw std::runtime_error("pipe failed");
        }

        pid_ = fork();
        if (pid_ < 0) {
            close(stdin_pipe[0]);
            close(stdin_pipe[1]);
            close(stdout_pipe[0]);
            close(stdout_pipe[1]);
            throw std::runtime_error("fork failed");
        }

        if (pid_ == 0) {
            // child
            dup2(stdin_pipe[0], STDIN_FILENO);
            dup2(stdout_pipe[1], STDOUT_FILENO);

            close(stdin_pipe[0]);
            close(stdin_pipe[1]);
            close(stdout_pipe[0]);
            close(stdout_pipe[1]);

            execl("/bin/sh", "sh", "-c", command.c_str(), (char*)nullptr);
            _exit(127);
        }

        // parent
        close(stdin_pipe[0]);
        close(stdout_pipe[1]);

        input_fd_ = stdin_pipe[1];
        output_fd_ = stdout_pipe[0];

        // read hello
        protocol::agent::Hello hello(recv_one());

        spawn_ = static_cast<DriverSpawnType>(hello.spawn_type);
        history_ = static_cast<DriverHistoryMode>(hello.history_mode);
    }

    ~ShellDriver()
    {
        try {
            if (input_fd_ >= 0) {
                send(protocol::agent::Bye::make());
            }
        } catch (...) {
        }

        if (input_fd_ >= 0) {
            close(input_fd_);
            input_fd_ = -1;
        }

        if (output_fd_ >= 0) {
            close(output_fd_);
            output_fd_ = -1;
        }

        if (pid_ > 0) { int status = 0;
            waitpid(pid_, &status, 0);
        }
    }

    DriverSpawnType spawn_type() const noexcept override { return spawn_; }
    DriverHistoryMode history_mode() const noexcept override { return history_; }

    void send(Message &&msg) override
    {
        msg.write(input_fd_);
    }

    std::vector<Message> recv() override
    {
        std::vector<Message> out;
        out.emplace_back(Message::read(output_fd_));
        return out;
    }

    int read_fd () const override { return output_fd_; }
};

class QueueDriver: public Driver {
    int wake_fd;
    std::mutex mutex;
    std::vector<Message> pending;
public:
    QueueDriver ()
        : wake_fd(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC))
    {
        CHECK(wake_fd >= 0);
    }

    ~QueueDriver () {
        ::close(wake_fd);
    }

    void send(Message && msg) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            pending.emplace_back(std::move(msg));
        }
        uint64_t one = 1;
        (void)::write(wake_fd, &one, sizeof(one));
    }

    std::vector<Message> recv() {
        std::vector<Message> out;
        {
            std::lock_guard<std::mutex> lock(mutex);
            out.swap(pending);
        }
        uint64_t count;
        (void)::read(wake_fd, &count, sizeof(count));
        return out;
    }

    int read_fd () const {
        return wake_fd;
    }

    DriverSpawnType spawn_type() const noexcept { return DriverSpawnType::ADDRESS;}
    DriverHistoryMode history_mode() const noexcept { return DriverHistoryMode::NONE; }
};


} // namespace postline
