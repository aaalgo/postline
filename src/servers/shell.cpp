#include <array>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <postline/common.h>
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

struct CommandResult {
    int exit_status = -1;
    std::string stdout_;
    std::string stderr_;
};

static CommandResult run_command(char const* cmd)
{
    int out_pipe[2];
    int err_pipe[2];

    CHECK(::pipe(out_pipe) == 0);
    CHECK(::pipe(err_pipe) == 0);

    pid_t pid = ::fork();
    CHECK(pid >= 0);

    if (pid == 0) {
        ::close(out_pipe[0]);
        ::close(err_pipe[0]);

        CHECK(::dup2(out_pipe[1], STDOUT_FILENO) >= 0);
        CHECK(::dup2(err_pipe[1], STDERR_FILENO) >= 0);

        ::close(out_pipe[1]);
        ::close(err_pipe[1]);

        ::execlp(cmd, cmd, nullptr);
        _exit(127);
    }

    ::close(out_pipe[1]);
    ::close(err_pipe[1]);

    CommandResult result;
    result.stdout_ = read_all_from_fd(out_pipe[0]);
    result.stderr_ = read_all_from_fd(err_pipe[0]);

    ::close(out_pipe[0]);
    ::close(err_pipe[0]);

    int status = 0;
    CHECK(::waitpid(pid, &status, 0) >= 0);

    if (WIFEXITED(status)) {
        result.exit_status = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_status = 128 + WTERMSIG(status);
    }

    return result;
}

class ShellServer : public ServerBase {
protected:
    void recv(Message&& message) override
    {
        auto const& header = message.header();

        json respHeader;

        auto it = header.find("From");
        if (it != header.end() && it->is_string()) {
            respHeader["To"] = *it;
        }

        it = header.find("To");
        if (it != header.end() && it->is_string()) {
            respHeader["From"] = *it;
        }

        it = header.find("Subject");
        if (it == header.end()) {
            respHeader["Subject"] = "command not run";
            send(Message(std::move(respHeader),
                 std::string{"Please send command in Subject.\n"}));
            return;
        }

        std::string const& cmd = it->get_ref<std::string const&>();
        auto result = run_command(cmd.c_str());

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

        respHeader["Subject"] =
            std::format("exit status {}", result.exit_status);

        send(Message(std::move(respHeader), std::move(parts)));
    }
};

int main(int argc, char** argv)
{
    ShellServer server;

    CLI::App app{"Postline shell server"};
    server.configure(app);
    CLI11_PARSE(app, argc, argv);

    return server.run();
}

