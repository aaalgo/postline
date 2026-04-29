#include <iostream>
#include <string>
#include <vector>
#include <array>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <postline/common.h>

using namespace postline;

static std::string read_all_from_fd(int fd)
{
    std::string out;
    std::array<char, 4096> buf{};

    while (true) {
        ssize_t n = ::read(fd, buf.data(), buf.size());
        if (n > 0) {
            out.append(buf.data(), n);
        } else if (n == 0) {
            break;
        } else {
            if (errno == EINTR) continue;
            throw std::runtime_error("read failed");
        }
    }

    return out;
}

struct CommandResult {
    int exit_status = -1;
    std::string stdout_;
    std::string stderr_;
};

static CommandResult run_command(char const *cmd)
{
    int out_pipe[2];
    int err_pipe[2];

    if (::pipe(out_pipe) < 0) throw std::runtime_error("pipe stdout failed");
    if (::pipe(err_pipe) < 0) throw std::runtime_error("pipe stderr failed");

    pid_t pid = ::fork();
    if (pid < 0) {
        throw std::runtime_error("fork failed");
    }

    if (pid == 0) {
        // child
        ::close(out_pipe[0]);
        ::close(err_pipe[0]);

        ::dup2(out_pipe[1], STDOUT_FILENO);
        ::dup2(err_pipe[1], STDERR_FILENO);

        ::close(out_pipe[1]);
        ::close(err_pipe[1]);

        execlp(cmd, cmd, nullptr);

        _exit(127);
    }

    // parent
    ::close(out_pipe[1]);
    ::close(err_pipe[1]);

    CommandResult result;
    result.stdout_ = read_all_from_fd(out_pipe[0]);
    result.stderr_ = read_all_from_fd(err_pipe[0]);

    ::close(out_pipe[0]);
    ::close(err_pipe[0]);

    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) {
        throw std::runtime_error("waitpid failed");
    }

    if (WIFEXITED(status)) {
        result.exit_status = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_status = 128 + WTERMSIG(status);
    }

    return result;
}


int main()
{
    protocol::agent::Hello::make(0, 0).write(STDOUT_FILENO);

    for (;;) {
        Message message = Message::read(STDIN_FILENO);
        auto const& header = message.header();

        if (header.value("type", std::string()) == "driver:bye") {
            break;
        }

        json respHeader;

        auto it = header.find("From");
        if (it != header.end()) {
            respHeader["To"] = *it;
        }

        it = header.find("To");
        if (it != header.end()) {
            respHeader["From"] = *it;
        }

        it = header.find("Subject");
        if (it == header.end()) {
            respHeader["Subject"] = "command not run";
            std::string body = "Please send command in Subject.\n";
            Message resp(std::move(respHeader), std::move(body));
            resp.write(STDOUT_FILENO);
        }
        else {
            std::string const &cmd = it->get_ref<std::string const &>();
            auto result = run_command(cmd.c_str());
            std::vector<Message> parts;
            //parts.emplace_back(json{}, "Stdout and stderr are attached.\n");
            if (result.stdout_.size()) {
                parts.emplace_back(
                        json{{"Content-Disposition", "attachment; filename=\"stdout\""}},
                        std::move(result.stdout_));
            }
            if (result.stderr_.size()) {
                parts.emplace_back(
                        json{{"Content-Disposition", "attachment; filename=\"stderr\""}},
                        std::move(result.stderr_));
            }
            Message resp(json{{"Subject", std::format("exit status {}", result.exit_status)}},
                            parts);
            resp.write(STDOUT_FILENO);
        }
    }
    return 0;
}
