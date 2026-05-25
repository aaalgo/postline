#include <array>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <postline/common.h>
#include <postline/service.h>
#include <postline/server.h>

using namespace postline;

static std::string read_all_from_fd(int fd)
{
    std::string out;
    std::array<char, 4096> buf{};

    for (;;) {
        ssize_t n = ::read(fd, buf.data(), buf.size());

        if (n > 0) {
            out.append(buf.data(), n);
        } else if (n == 0) {
            break;
        } else {
            if (errno == EINTR) continue;
            CHECK(false);
        }
    }

    return out;
}

static void write_all_to_fd(int fd, std::string const& body)
{
    char const* data = body.data();
    size_t remaining = body.size();

    while (remaining > 0) {
        ssize_t n = ::write(fd, data, remaining);

        if (n > 0) {
            data += n;
            remaining -= n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && errno == EPIPE) {
            break;
        } else {
            CHECK(false);
        }
    }
}

struct CommandResult {
    int exit_status = -1;
    std::string stdout_;
    std::string stderr_;
};

static CommandResult run_command(char const* cmd, std::string const &body)
{
    int in_pipe[2] = {-1, -1};
    int out_pipe[2];
    int err_pipe[2];

    if (!body.empty()) {
        CHECK(::pipe(in_pipe) == 0);
    }

    CHECK(::pipe(out_pipe) == 0);
    CHECK(::pipe(err_pipe) == 0);

    pid_t pid = ::fork();
    CHECK(pid >= 0);

    if (pid == 0) {
        ::close(out_pipe[0]);
        ::close(err_pipe[0]);

        if (!body.empty()) {
            ::close(in_pipe[1]);
            CHECK(::dup2(in_pipe[0], STDIN_FILENO) >= 0);
            ::close(in_pipe[0]);
        }

        CHECK(::dup2(out_pipe[1], STDOUT_FILENO) >= 0);
        CHECK(::dup2(err_pipe[1], STDERR_FILENO) >= 0);

        ::close(out_pipe[1]);
        ::close(err_pipe[1]);

        ::execl("/bin/sh", "sh", "-c", cmd, nullptr);
        _exit(127);
    }

    ::close(out_pipe[1]);
    ::close(err_pipe[1]);

    pid_t writer_pid = -1;

    if (!body.empty()) {
        ::close(in_pipe[0]);

        writer_pid = ::fork();
        CHECK(writer_pid >= 0);

        if (writer_pid == 0) {
            ::close(out_pipe[0]);
            ::close(err_pipe[0]);

            write_all_to_fd(in_pipe[1], body);
            ::close(in_pipe[1]);
            _exit(0);
        }

        ::close(in_pipe[1]);
    }

    CommandResult result;
    result.stdout_ = read_all_from_fd(out_pipe[0]);
    result.stderr_ = read_all_from_fd(err_pipe[0]);

    ::close(out_pipe[0]);
    ::close(err_pipe[0]);

    int status = 0;
    CHECK(::waitpid(pid, &status, 0) >= 0);
    if (writer_pid >= 0) {
        CHECK(::waitpid(writer_pid, nullptr, 0) >= 0);
    }

    if (WIFEXITED(status)) {
        result.exit_status = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_status = 128 + WTERMSIG(status);
    }

    return result;
}

class Shell : public LinearService {
protected:
    void call (Message&& message, Response &resp) override
    {
        auto const& header = message.header();
        auto it = header.find("Subject");
        if (it == header.end()) {
            json header{{"Subject", "not run"}};
            
            resp.append(Message(std::move(header),
                 std::string{"Please send command in Subject.\n"}));
        }

        std::string const& cmd = it->get_ref<std::string const&>();
        std::string const &body = message.body();
        auto result = run_command(cmd.c_str(), body);

        std::vector<Message> parts;

        if (!result.stdout_.empty()) {
            parts.emplace_back(
                json{{"Content-Disposition",
                      "attachment; filename=\"stdout\""}},
                std::move(result.stdout_));
        }

        if (!result.stderr_.empty()) {
            parts.emplace_back(
                json{{"Content-Disposition",
                      "attachment; filename=\"stderr\""}},
                std::move(result.stderr_));
        }

        std::string subject = std::format("exit status {}", result.exit_status);

        resp.append(Message(json{{"Subject", subject}}, std::move(parts)));
    }
};

int main(int argc, char** argv)
{
    Server::Config config;
    {
        CLI::App app{"Postline shell server"};
        config.configure(app);
        CLI11_PARSE(app, argc, argv);
    }
    Shell shell;
    Server server(config);
    return server.run(&shell);
}
