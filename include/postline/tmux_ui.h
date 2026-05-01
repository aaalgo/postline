#pragma once

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

class TMuxUI {
public:
    TMuxUI() {
        init_paths();
        create_files();
        redirect_stdout();
        start_tmux();
        attach_tmux_async();
    }

    ~TMuxUI() {
        stop();
    }

    TMuxUI(TMuxUI const&) = delete;
    TMuxUI& operator=(TMuxUI const&) = delete;

    void start_helper(std::filesystem::path path_to_helper, bool fake = false) {
        if (helper_started_) {
            throw std::runtime_error("tmux input helper already started");
        }

        if (!std::filesystem::exists(path_to_helper)) {
            throw std::runtime_error(
                "tmux input helper does not exist: " + path_to_helper.string()
            );
        }

        std::string bottom_cmd =
            shell_quote(path_to_helper.string()) +
            " --stdin " + shell_quote(input_fifo_.string()) + 
            " --stdout " + shell_quote(output_fifo_.string());

        if (fake) {
            bottom_cmd = "echo run from terminal the following commnad; echo " + bottom_cmd + "; sleep infinity";
        }
        else {
            bottom_cmd = bottom_cmd + " 2> user.log";
        }


        run_cmd({
            "tmux", "split-window",
            "-v",
            "-l", "25%",
            "-t", session_ + ":0",
            "bash", "-lc", bottom_cmd
        });

        run_cmd({
            "tmux", "select-pane",
            "-t", session_ + ":0.1"
        });

        helper_started_ = true;
    }    

    std::filesystem::path cli_input_path() const {
        return input_fifo_;
    }

    std::filesystem::path cli_output_path() const {
        return output_fifo_;
    }

    std::filesystem::path log_path() const {
        return log_file_;
    }

    std::string session_name() const {
        return session_;
    }

    void stop() {
        if (stopped_) {
            return;
        }

        stopped_ = true;

        if (!session_.empty()) {
            run_quiet({"tmux", "kill-session", "-t", session_});
        }

        if (attach_pid_ > 0) {
            int status = 0;
            ::waitpid(attach_pid_, &status, 0);
            attach_pid_ = -1;
        }

        restore_stdout();
if (!log_file_.empty() && std::filesystem::exists(log_file_)) {
    ::execlp(
        "tail",
        "tail",
        "-n", "200",   // last 200 lines
        log_file_.c_str(),
        static_cast<char*>(nullptr)
    );
    // if execlp fails, just fall through
}        
        cleanup();
    }

private:
    std::filesystem::path dir_;
    std::filesystem::path log_file_;
    std::filesystem::path input_fifo_;
    std::filesystem::path output_fifo_;

    std::string session_;

    int saved_stdout_ = -1;
    pid_t attach_pid_ = -1;
    bool helper_started_ = false;
    bool stopped_ = false;

private:
    static std::string shell_quote(std::string const& s) {
        std::string out = "'";

        for (char c : s) {
            if (c == '\'') {
                out += "'\\''";
            } else {
                out += c;
            }
        }

        out += "'";
        return out;
    }

    static void run_cmd(std::vector<std::string> const& argv) {
        pid_t pid = ::fork();

        if (pid < 0) {
            throw std::runtime_error("fork failed");
        }

        if (pid == 0) {
            std::vector<char*> args;
            args.reserve(argv.size() + 1);

            for (auto const& s : argv) {
                args.push_back(const_cast<char*>(s.c_str()));
            }

            args.push_back(nullptr);

            ::execvp(args[0], args.data());
            ::_exit(127);
        }

        int status = 0;

        if (::waitpid(pid, &status, 0) < 0) {
            throw std::runtime_error("waitpid failed");
        }

        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            throw std::runtime_error("command failed: " + argv[0]);
        }
    }

    static bool run_quiet(std::vector<std::string> const& argv) {
        pid_t pid = ::fork();

        if (pid < 0) {
            return false;
        }

        if (pid == 0) {
            int devnull = ::open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                ::dup2(devnull, STDOUT_FILENO);
                ::dup2(devnull, STDERR_FILENO);
                ::close(devnull);
            }

            std::vector<char*> args;
            args.reserve(argv.size() + 1);

            for (auto const& s : argv) {
                args.push_back(const_cast<char*>(s.c_str()));
            }

            args.push_back(nullptr);

            ::execvp(args[0], args.data());
            ::_exit(127);
        }

        int status = 0;

        if (::waitpid(pid, &status, 0) < 0) {
            return false;
        }

        return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }

    void init_paths() {
        pid_t pid = ::getpid();

        session_ = "postline-" + std::to_string(pid);

        dir_ = std::filesystem::temp_directory_path()
             / ("postline-tmux-ui-" + std::to_string(pid));

        log_file_ = dir_ / "stdout.log";
        input_fifo_ = dir_ / "cli_input.fifo";
        output_fifo_ = dir_ / "cli_output.fifo";
    }

    void create_files() {
        std::filesystem::create_directories(dir_);

        {
            std::ofstream out(log_file_);
            out << "=== postline tmux ui ===\n";
        }

        if (::mkfifo(input_fifo_.c_str(), 0600) < 0) {
            if (errno != EEXIST) {
                throw std::runtime_error("mkfifo input fifo failed");
            }
        }

        if (::mkfifo(output_fifo_.c_str(), 0600) < 0) {
            if (errno != EEXIST) {
                throw std::runtime_error("mkfifo output fifo failed");
            }
        }
    }

    void redirect_stdout() {
        std::fflush(stdout);
        std::cout.flush();

        saved_stdout_ = ::dup(STDOUT_FILENO);

        if (saved_stdout_ < 0) {
            throw std::runtime_error("dup stdout failed");
        }

        int fd = ::open(
            log_file_.c_str(),
            O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
            0644
        );

        if (fd < 0) {
            throw std::runtime_error("open log file failed");
        }

        if (::dup2(fd, STDOUT_FILENO) < 0) {
            ::close(fd);
            throw std::runtime_error("dup2 stdout failed");
        }

        ::close(fd);
        std::cout.clear();
        std::ios::sync_with_stdio(true);
        std::setvbuf(stdout, nullptr, _IOLBF, 0);
    }

    void start_tmux() {
        std::string top_cmd =
            "tail -n +1 -F " + shell_quote(log_file_.string());

        run_cmd({
            "tmux", "new-session",
            "-d",
            "-s", session_,
            "bash", "-lc", top_cmd
        });
        run_cmd({
            "tmux", "set-option",
            "-t", session_,
            "status", "off"
        });        
        run_cmd({
            "tmux", "set-option",
            "-t", session_,
            "pane-border-status", "off"
        });        
        /*
        run_cmd({
            "tmux", "set-option",
            "-t", session_,
            "mouse", "on"
        });        
        run_cmd({
            "tmux", "unbind", "-n",
            "MouseDrag1Pane"
        });        
        */
    }

    void attach_tmux_async() {
        attach_pid_ = ::fork();

        if (attach_pid_ < 0) {
            throw std::runtime_error("fork tmux attach failed");
        }

        if (attach_pid_ == 0) {
            ::execlp(
                "tmux",
                "tmux",
                "attach-session",
                "-t",
                session_.c_str(),
                static_cast<char*>(nullptr)
            );

            ::_exit(127);
        }
    }

    void restore_stdout() {
        if (saved_stdout_ >= 0) {
            std::fflush(stdout);
            std::cout.flush();

            ::dup2(saved_stdout_, STDOUT_FILENO);
            ::close(saved_stdout_);

            saved_stdout_ = -1;
        }
    }

    void cleanup() {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
};
