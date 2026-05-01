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
    struct Config {
        std::string name;
        std::string params;
    };
    virtual ~Driver () {}
    virtual int send(Message const &msg) = 0;
    virtual int recv(std::vector<Message> &out) = 0;
    Message recv_one () {
        std::vector<Message> all;
        int err = recv(all);
        CHECK(err == 0);
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

    void handshake () {
        // read hello
        protocol::agent::Hello hello(recv_one());

        spawn_ = static_cast<DriverSpawnType>(hello.spawn_type);
        history_ = static_cast<DriverHistoryMode>(hello.history_mode);
    }
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
        handshake();

    }

    explicit ShellDriver(std::string const &cli_input_path,
                         std::string const &cli_output_path)
        : input_fd_(::open(cli_input_path.c_str(), O_WRONLY | O_CLOEXEC)),
        output_fd_(::open(cli_output_path.c_str(), O_RDONLY | O_CLOEXEC))
    {
        log::info("shell driver read from {} and write to {}", cli_output_path, cli_input_path);
        handshake();
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

    int send(Message const &msg) override
    {
        msg.write(input_fd_);
        return 0;
    }

    int recv(std::vector<Message> &out) override
    {
        out.clear();
        out.emplace_back(Message::read(output_fd_));
        return 0;
    }

    int read_fd () const override { return output_fd_; }
};

class LoopDriver: public Driver {
    // LoopDriver is a local API
    // to send messages to the runtime
    // There are two cases:
    // - Runtime sends message to itself
    //      via LoopDriver so that the message
    //      goes through standard processing
    // - Other entities in the same process as Runtime
    //      to send message to runtime
    // 
    // The send is not exposed; use
    // Runtime::send
    //
    int wake_fd;
    std::mutex mutex;
    std::vector<Message> pending;
public:
    LoopDriver ()
        : wake_fd(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC))
    {
        CHECK(wake_fd >= 0);
    }

    ~LoopDriver () {
        ::close(wake_fd);
    }

    int send(Message const &msg) override {
        CHECK(0);
    }

    int enqueue (Message && msg) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            pending.emplace_back(std::move(msg));
        }
        uint64_t one = 1;
        ssize_t rc = ::write(wake_fd, &one, sizeof(one));
        CHECK(rc == static_cast<ssize_t>(sizeof(one)));
        return 0;
    }

    int recv(std::vector<Message> &out) override {
        out.clear();
        {
            std::lock_guard<std::mutex> lock(mutex);
            out.swap(pending);
        }
        uint64_t count;
        ssize_t rc = ::read(wake_fd, &count, sizeof(count));
        CHECK(rc == static_cast<ssize_t>(sizeof(count)));
        return 0;
    }

    int read_fd () const override {
        return wake_fd;
    }

    DriverSpawnType spawn_type() const noexcept override { return DriverSpawnType::ADDRESS;}
    DriverHistoryMode history_mode() const noexcept override { return DriverHistoryMode::NONE; }
};

static inline std::unique_ptr<Driver> create_driver (std::string const &service) {
    std::string driver;
    std::string params;
    auto off = service.find(':');
    if (off == service.npos) {
        driver = service;
    }
    else {
        driver = service.substr(0, off);
        params = service.substr(off + 1);
    }
    if (driver == "shell") {
        std::string command = (POSTLINE_HOME / "bin" / "servers").string() + "/" + params;
        return std::make_unique<ShellDriver>(std::string(command));
    }
    else CHECK(0, "dynamically createing driver {} not supported", driver);
    return nullptr;
}


} // namespace postline
