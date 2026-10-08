#include "net/Socket.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace cge {
namespace {

// MSG_NOSIGNAL (Linux) stops send() from raising SIGPIPE when the peer has
// gone away. macOS does not have it; there we use SO_NOSIGPIPE instead and
// the server also ignores SIGPIPE globally.
#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

void preventSigpipe(int fd) {
#ifdef SO_NOSIGPIPE
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#else
    (void)fd;
#endif
}

}  // namespace

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = other.release();
    }
    return *this;
}

void Socket::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

int Socket::release() {
    const int fd = fd_;
    fd_ = -1;
    return fd;
}

bool Socket::setTimeouts(int receiveMillis, int sendMillis) const {
    timeval receive{};
    receive.tv_sec = receiveMillis / 1000;
    receive.tv_usec = (receiveMillis % 1000) * 1000;
    timeval send{};
    send.tv_sec = sendMillis / 1000;
    send.tv_usec = (sendMillis % 1000) * 1000;
    return setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &receive, sizeof(receive)) == 0 &&
           setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &send, sizeof(send)) == 0;
}

bool Socket::setNoDelay() const {
    int on = 1;
    return setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on)) == 0;
}

bool Socket::setNonBlocking(bool enabled) const {
    const int flags = fcntl(fd_, F_GETFL);
    if (flags < 0) return false;
    const int newFlags = enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return fcntl(fd_, F_SETFL, newFlags) == 0;
}

void setCloseOnExec(int fd) {
    const int flags = fcntl(fd, F_GETFD);
    if (flags >= 0) fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

bool makeAddress(const std::string& ip, std::uint16_t port, sockaddr_in& out) {
    std::memset(&out, 0, sizeof(out));
    out.sin_family = AF_INET;
    out.sin_port = htons(port);
    return inet_pton(AF_INET, ip.c_str(), &out.sin_addr) == 1;
}

Socket createTcpListener(const std::string& address, std::uint16_t port, std::string& error) {
    sockaddr_in bindAddress{};
    if (!makeAddress(address, port, bindAddress)) {
        error = "invalid bind address '" + address + "'";
        return Socket();
    }
    Socket socket(::socket(AF_INET, SOCK_STREAM, 0));
    if (!socket.valid()) {
        error = std::string("socket() failed: ") + std::strerror(errno);
        return Socket();
    }
    setCloseOnExec(socket.fd());

    // SO_REUSEADDR lets us restart the server right away without waiting
    // for old connections of the previous run to time out.
    int on = 1;
    setsockopt(socket.fd(), SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    if (bind(socket.fd(), reinterpret_cast<sockaddr*>(&bindAddress), sizeof(bindAddress)) != 0) {
        error = "cannot listen on " + address + ":" + std::to_string(port) + ": " + std::strerror(errno);
        return Socket();
    }
    if (listen(socket.fd(), 512) != 0) {
        error = std::string("listen() failed: ") + std::strerror(errno);
        return Socket();
    }
    return socket;
}

Socket acceptClient(const Socket& listener, std::string& clientIp, std::uint16_t& clientPort) {
    sockaddr_in peer{};
    socklen_t length = sizeof(peer);
    const int fd = ::accept(listener.fd(), reinterpret_cast<sockaddr*>(&peer), &length);
    if (fd < 0) return Socket();
    setCloseOnExec(fd);
    preventSigpipe(fd);

    char text[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &peer.sin_addr, text, sizeof(text));
    clientIp = text;
    clientPort = ntohs(peer.sin_port);
    return Socket(fd);
}

Socket createUdpSocket(const std::string& address, std::uint16_t port, std::string& error) {
    sockaddr_in bindAddress{};
    if (!makeAddress(address, port, bindAddress)) {
        error = "invalid bind address '" + address + "'";
        return Socket();
    }
    Socket socket(::socket(AF_INET, SOCK_DGRAM, 0));
    if (!socket.valid()) {
        error = std::string("socket() failed: ") + std::strerror(errno);
        return Socket();
    }
    setCloseOnExec(socket.fd());
    if (bind(socket.fd(), reinterpret_cast<sockaddr*>(&bindAddress), sizeof(bindAddress)) != 0) {
        error = std::string("bind() failed: ") + std::strerror(errno);
        return Socket();
    }
    // A bigger send buffer absorbs short bursts (for example a large video keyframe).
    int bufferSize = 1 << 20;
    setsockopt(socket.fd(), SOL_SOCKET, SO_SNDBUF, &bufferSize, sizeof(bufferSize));
    return socket;
}

bool sendAll(int fd, const void* data, std::size_t size) {
    const char* cursor = static_cast<const char*>(data);
    while (size > 0) {
        const ssize_t sent = ::send(fd, cursor, size, kSendFlags);
        if (sent > 0) {
            cursor += sent;
            size -= static_cast<std::size_t>(sent);
            continue;
        }
        if (sent < 0 && errno == EINTR) continue;  // interrupted by a signal: just try again
        return false;  // error, closed connection or send timeout
    }
    return true;
}

std::string localAddress(int fd) {
    sockaddr_in local{};
    socklen_t length = sizeof(local);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&local), &length) != 0) return "0.0.0.0";
    char text[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &local.sin_addr, text, sizeof(text));
    return text;
}

}  // namespace cge
