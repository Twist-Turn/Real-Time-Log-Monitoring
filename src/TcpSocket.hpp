#pragma once
/**
 * @file TcpSocket.hpp
 * @brief RAII wrapper around POSIX TCP sockets.
 *
 * Manages socket lifecycle (create, bind, listen, accept, close) with
 * automatic cleanup in the destructor. Move-only semantics prevent
 * accidental double-close.
 */

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace logmonitor {

class TcpSocket {
public:
    /// Create an uninitialized (invalid) socket
    TcpSocket() noexcept : fd_(-1) {}

    /// Take ownership of an existing file descriptor
    explicit TcpSocket(int fd) noexcept : fd_(fd) {}

    /// Create a new TCP socket
    static TcpSocket create() {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            throw std::runtime_error(
                std::string("socket() failed: ") + std::strerror(errno));
        }
        return TcpSocket(fd);
    }

    ~TcpSocket() {
        close();
    }

    // Move-only
    TcpSocket(TcpSocket&& other) noexcept : fd_(other.fd_) {
        other.fd_ = -1;
    }

    TcpSocket& operator=(TcpSocket&& other) noexcept {
        if (this != &other) {
            close();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;

    /// Connect to a remote host
    void connect(const std::string& host, uint16_t port) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
            throw std::runtime_error("Invalid address: " + host);
        }
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            throw std::runtime_error(
                std::string("connect() failed: ") + std::strerror(errno));
        }
    }

    /// Enable SO_REUSEADDR to allow quick server restarts
    void set_reuse_addr() {
        int opt = 1;
        if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            throw std::runtime_error(
                std::string("setsockopt(REUSEADDR) failed: ") + std::strerror(errno));
        }
    }

    /// Set socket to non-blocking mode
    void set_nonblocking() {
        int flags = ::fcntl(fd_, F_GETFL, 0);
        if (flags < 0 || ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
            throw std::runtime_error(
                std::string("fcntl(O_NONBLOCK) failed: ") + std::strerror(errno));
        }
    }

    /// Bind to a port on all interfaces
    void bind(uint16_t port) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);

        if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            throw std::runtime_error(
                std::string("bind() failed on port ") + std::to_string(port)
                + ": " + std::strerror(errno));
        }
    }

    /// Start listening for connections
    void listen(int backlog = 128) {
        if (::listen(fd_, backlog) < 0) {
            throw std::runtime_error(
                std::string("listen() failed: ") + std::strerror(errno));
        }
    }

    /// Accept a connection. Returns a new TcpSocket for the client.
    /// Returns an invalid socket (fd == -1) if non-blocking and no connection pending.
    TcpSocket accept() {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = ::accept(fd_,
                                  reinterpret_cast<sockaddr*>(&client_addr),
                                  &client_len);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return TcpSocket();  // No pending connection
            }
            throw std::runtime_error(
                std::string("accept() failed: ") + std::strerror(errno));
        }
        return TcpSocket(client_fd);
    }

    /// Send all bytes, retrying on partial writes
    bool send_all(const std::string& data) {
        std::size_t total_sent = 0;
        while (total_sent < data.size()) {
            ssize_t sent = ::send(fd_, data.data() + total_sent,
                                   data.size() - total_sent, 0);
            if (sent < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            total_sent += static_cast<std::size_t>(sent);
        }
        return true;
    }

    /// Receive up to max_bytes into a string. Returns empty on EOF/error.
    std::string recv(std::size_t max_bytes = 4096) {
        std::string buffer(max_bytes, '\0');
        ssize_t n = ::recv(fd_, buffer.data(), max_bytes, 0);
        if (n <= 0) return {};
        buffer.resize(static_cast<std::size_t>(n));
        return buffer;
    }

    /// Read a single line (up to delimiter '\n'). Blocks until newline or EOF.
    std::string recv_line(std::size_t max_len = 8192) {
        std::string line;
        line.reserve(256);
        char ch;
        while (line.size() < max_len) {
            ssize_t n = ::recv(fd_, &ch, 1, 0);
            if (n <= 0) break;
            if (ch == '\n') break;
            line += ch;
        }
        // Strip trailing \r if present
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        return line;
    }

    [[nodiscard]] int fd() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    explicit operator bool() const noexcept { return valid(); }

    /// Release ownership of the fd without closing
    int release() noexcept {
        int fd = fd_;
        fd_ = -1;
        return fd;
    }

    void close() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_;
};

} // namespace logmonitor
