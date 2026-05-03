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
#include <postline/driver.h>
#include <postline/session.h>


namespace postline {

class Transport {
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

    explicit Transport(Config const& config)
    {
        if (config.port != 0 || !config.host.empty()) {
            open_socket(config);
        } else {
            open_stdio_or_files(config);
        }
    }

    ~Transport()
    {
        close();
    }

    Transport(Transport const&) = delete;
    Transport& operator=(Transport const&) = delete;

    Transport(Transport&&) = delete;
    Transport& operator=(Transport&&) = delete;

    int read_fd() const
    {
        return read_fd_;
    }

    int write_fd() const
    {
        return write_fd_;
    }

private:
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

    void close()
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

private:
    int read_fd_ = STDIN_FILENO;
    int write_fd_ = STDOUT_FILENO;

    bool owns_read_fd_ = false;
    bool owns_write_fd_ = false;
};

class ServerBase {
    SessionID session_id = NOT_A_SESSION;
    std::string last_from;
    AccessID last_access_id = NO_ACCESS_ID;

    void updateLastMessageInfo (Message const &msg) {
        SessionID last_session = msg.session_id();
        if (last_session != NOT_A_SESSION) {
            session_id = last_session;
        }
        last_from = msg.from();
        last_access_id = msg.message_id();  // access_id() is only available on runtime
        CHECK(last_access_id != NO_ACCESS_ID);
    }
protected:
    virtual DriverSpawnType spawn_type() const noexcept {
        return DriverSpawnType::ADDRESS;
    }
    virtual DriverHistoryMode history_mode() const noexcept {
        return DriverHistoryMode::NONE;
    }
public:
    virtual ~ServerBase() = default;

    virtual void configure(CLI::App& app)
    {
        transport_config_.configure(app);
    }

    int run()
    {
        Transport transport(transport_config_);

        read_fd_ = transport.read_fd();
        write_fd_ = transport.write_fd();

        send(protocol::handshake::Hello::make(int(spawn_type()), int(history_mode())));

        on_connect();

        for (;;) {
            Message msg = Message::read(read_fd_);

            std::string type = msg.type();

#if 0
            std::cerr << "RECEIVED " << type << std::endl;
        std::cerr << "======== " << msg.type() << std::endl;
        if (msg.header().contains("To")) {
            msg.formatEmail(std::cerr);
        }
        else {
            std::cerr << msg.header().dump(4) << std::endl;
            std::cerr << msg.body() << std::endl;
        }
        std::cerr << "========" << std::endl;
#endif


            if (type == protocol::handshake::BeginMemory::type) {
                for (;;) {

                    msg = Message::read(read_fd_);
                    if (msg.type() == protocol::handshake::EndMemory::type) {
                        break;
                    }
                    else {
                        updateMemory(std::move(msg));
                    }
                }
                continue;
            }

            if (type == protocol::handshake::Multi::type) {
                protocol::handshake::Multi multi(msg);
                for (size_t i = 0; i < multi.count; ++i) {
                    Message msg = Message::read(read_fd_);
                    updateLastMessageInfo(msg);
                    recv(std::move(msg));
                }
                continue;
            }

            if (type == protocol::handshake::Bye::type) {
                break;
            }

            updateLastMessageInfo(msg);
            recv(std::move(msg));
        }

        bye();
        return 0;
    }

protected:
    virtual void updateMemory(Message&& msg) {
    };

    virtual void recv(Message&& msg) = 0;

    virtual void on_connect () {}

    virtual void bye() {}

    void send(Message&& msg)
    {
#if 0
        if (msg.in_reply_to() == NO_ACCESS_ID 
                && msg.in_response_to() == NO_ACCESS_ID
                && !last_from.empty()) {
            CHECK(last_access_id != NO_ACCESS_ID);
            std::cerr << "You should set In-Reply-To or In-Response-To" << std::endl;
            std::cerr << "I'm doing it for you; please come back and fix it" << std::endl;
            char const *key = nullptr;
            if (msg.to() == last_from) {
                key = "In-Reply-To";
            }
            else {
                key = "In-Response-To";
            }
            msg.updateHeader([this, key](json &header) {
                    header[key] = std::format("{}", last_access_id);
                });
        }
#endif
        msg.updateHeader([this](json &header) {
                header["Session-ID"] = std::format("{}", session_id);
                });
        msg.write(write_fd_);
        last_from.clear();
        last_access_id = NO_ACCESS_ID;
    }

protected:
    Transport::Config transport_config_;

private:
    int read_fd_ = STDIN_FILENO;
    int write_fd_ = STDOUT_FILENO;
};

} // namespace postline
