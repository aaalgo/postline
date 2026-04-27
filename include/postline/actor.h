#pragma once
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include "common.h"

namespace postline {


enum class ActorSpawnType : std::uint16_t
{
    ADDRESS = 0,
    SCOPE  = 1
};

enum class ActorHistoryMode : std::uint16_t
{
    NONE = 0,
    ALL  = 1
};

class Actor {
    ActorSpawnType spawn_;
    ActorHistoryMode history_;

    int input_fd_ = -1;
    int output_fd_ = -1;
    pid_t pid_ = -1;
public:
    explicit Actor(std::string const &command)
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
        protocol::actor::Hello hello(recv());

        spawn_ = static_cast<ActorSpawnType>(hello.spawn_type);
        history_ = static_cast<ActorHistoryMode>(hello.history_mode);
    }

    ~Actor()
    {
        try {
            if (input_fd_ >= 0) {
                send(protocol::actor::Bye::make());
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

    Actor(Actor const&) = delete;
    Actor& operator=(Actor const&) = delete;

    Actor(Actor&&) = delete;
    Actor& operator=(Actor&&) = delete;

    ActorSpawnType spawn_type() const noexcept { return spawn_; }
    ActorHistoryMode history_mode() const noexcept { return history_; }

    void send(Message const& msg)
    {
        msg.write(input_fd_);
    }

    Message recv()
    {
        return Message::read(output_fd_);
    }

    int read_fd () const { return output_fd_; }
};

} // namespace postline
