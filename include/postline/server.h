#pragma once

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>
#include <postline/common.h>
#include <postline/protocol.h>
#include <postline/service.h>

namespace postline {

class Server: immobile {
public:
    struct Config {
        std::string stdin_path;
        std::string stdout_path;
        std::string host;
        int port = 0;

        void configure(CLI::App& app)
        {
            app.add_option("--stdin", stdin_path,
                           "Read protocol input from file instead of stdin");

            app.add_option("--stdout", stdout_path,
                           "Write protocol output to file instead of stdout");

            app.add_option("--host", host,
                           "Listen host, e.g. 0.0.0.0");

            app.add_option("--port", port,
                           "Listen port")
                ->check(CLI::Range(1, 65535));
        }
    };

protected:
    int read_fd_ = STDIN_FILENO;
    int write_fd_ = STDOUT_FILENO;

    bool owns_read_fd_ = false;
    bool owns_write_fd_ = false;

    void open_stdio_or_files(Config const& config)
    {
        if (!config.stdin_path.empty()) {
            read_fd_ = ::open(config.stdin_path.c_str(),
                              O_RDONLY | O_CLOEXEC);
            CHECK_FD(read_fd_);
            owns_read_fd_ = true;
        } else {
            read_fd_ = STDIN_FILENO;
            owns_read_fd_ = false;
        }

        if (!config.stdout_path.empty()) {
            write_fd_ = ::open(config.stdout_path.c_str(),
                               O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                               0644);
            CHECK_FD(write_fd_);
            owns_write_fd_ = true;
        } else {
            write_fd_ = STDOUT_FILENO;
            owns_write_fd_ = false;
        }
    }

    void open_socket(Config const& config)
    {
        CHECK(config.port != 0);

        std::string host = config.host.empty()
            ? std::string{"127.0.0.1"}
            : config.host;

        int listen_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        CHECK_FD(listen_fd);

        int yes = 1;
        CHECK(::setsockopt(listen_fd,
                           SOL_SOCKET,
                           SO_REUSEADDR,
                           &yes,
                           sizeof(yes)) == 0);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(config.port));

        CHECK(::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) == 1);

        CHECK(::bind(listen_fd,
                     reinterpret_cast<sockaddr*>(&addr),
                     sizeof(addr)) == 0);

        CHECK(::listen(listen_fd, 1) == 0);

        int conn_fd = ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
        CHECK_FD(conn_fd);

        ::close(listen_fd);

        read_fd_ = conn_fd;
        write_fd_ = conn_fd;

        owns_read_fd_ = true;
        owns_write_fd_ = false;
    }
public:
    explicit Server(Config const& config)
    {
        if (config.port != 0 || !config.host.empty()) {
            open_socket(config);
        } else {
            open_stdio_or_files(config);
        }
    }

    ~Server()
    {
        if (read_fd_ == write_fd_) {
            if ((owns_read_fd_ || owns_write_fd_) && read_fd_ >= 0) {
                ::close(read_fd_);
            }
        } else {
            if (owns_read_fd_ && read_fd_ >= 0) {
                ::close(read_fd_);
            }

            if (owns_write_fd_ && write_fd_ >= 0) {
                ::close(write_fd_);
            }
        }

        read_fd_ = -1;
        write_fd_ = -1;
        owns_read_fd_ = false;
        owns_write_fd_ = false;
    }

    int run(Service *service)
    {
        protocol::handshake::Hello::make().write(write_fd_);

        {
            std::vector<Message> resp = service->on_connect();
            for (auto &msg: resp) {
                msg.write(write_fd_);
            }
        }

        for (;;) {
            Message msg = Message::read(read_fd_);

            std::string type = msg.type();

            if (type == protocol::handshake::Bye::type) {
                break;
            }

            if (type == protocol::handshake::BeginMemory::type) {
                for (;;) {
                    msg = Message::read(read_fd_);
                    if (msg.type() == protocol::handshake::EndMemory::type) {
                        break;
                    }
                    else {
                        service->on_memory(std::move(msg));
                    }
                }
                continue;
            }

            if (type == protocol::handshake::Multi::type) {
                CHECK(0);
#if 0
                protocol::handshake::Multi multi(msg);
                for (size_t i = 0; i < multi.count; ++i) {
                    Message msg = Message::read(read_fd_);
                    //recv(std::move(msg));
                }
                continue;
#endif
            }

            std::vector<Message> resp = service->on_message(std::move(msg));
            for (auto &msg: resp) {
                msg.write(write_fd_);
            }
        }
        service->on_exit();
        return 0;
    }
};

} // namespace postline
