#pragma once

#include <netinet/in.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace cge {

// Owns one socket file descriptor and closes it automatically (RAII).
//
// A Socket can be moved but not copied, so exactly one object is ever
// responsible for closing a given descriptor.
class Socket {
public:
    Socket() = default;
    explicit Socket(int fd) : fd_(fd) {}
    ~Socket() { close(); }

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept : fd_(other.release()) {}
    Socket& operator=(Socket&& other) noexcept;

    int fd() const { return fd_; }
    bool valid() const { return fd_ >= 0; }

    void close();
    int release();  // gives up ownership without closing

    // Read/write timeouts. A blocked recv()/send() gives up after this long.
    bool setTimeouts(int receiveMillis, int sendMillis) const;
    bool setNoDelay() const;  // disables Nagle's algorithm: small packets are sent immediately
    bool setNonBlocking(bool enabled) const;

private:
    int fd_ = -1;
};

// Marks a descriptor so it is closed automatically in child processes (compiler, validator).
void setCloseOnExec(int fd);

// Creates a TCP socket that listens for connections on address:port.
Socket createTcpListener(const std::string& address, std::uint16_t port, std::string& error);

// Waits for the next connection on a listening socket. Returns an invalid
// Socket on error. Fills the client's IP address and port.
Socket acceptClient(const Socket& listener, std::string& clientIp, std::uint16_t& clientPort);

// Creates a UDP socket bound to address:port (port 0 = any free port).
Socket createUdpSocket(const std::string& address, std::uint16_t port, std::string& error);

// Sends every byte, retrying after partial writes. Returns false if the
// connection failed or the send timeout expired.
bool sendAll(int fd, const void* data, std::size_t size);

// Fills a sockaddr_in from "1.2.3.4" and a port. Returns false for bad addresses.
bool makeAddress(const std::string& ip, std::uint16_t port, sockaddr_in& out);

// The local IP address of a connected socket (the address the client used to reach us).
std::string localAddress(int fd);

}  // namespace cge
