#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <random>
#include <string>

#include "http/HttpMessage.hpp"
#include "net/Socket.hpp"

// WebSocket (RFC 6455) on top of a plain TCP socket.
//
// Browsers cannot open raw TCP sockets, but they can open WebSockets: a
// WebSocket starts as an HTTP request ("please upgrade") and then keeps the
// same TCP connection open for two-way messages. On the wire every message
// is wrapped in a small frame header:
//
//   byte 0: FIN bit + opcode (1 = text, 2 = binary, 8 = close, 9 = ping, 10 = pong)
//   byte 1: MASK bit + payload length (0-125, or 126 = 16-bit length follows,
//           127 = 64-bit length follows)
//   [extended length] [4-byte masking key if MASK] [payload]
//
// Messages from browser to server are always "masked" (XOR-ed with the
// 4-byte key); messages from server to browser are not.
namespace cge::ws {

// The value for "Sec-WebSocket-Accept": base64(SHA-1(clientKey + magic GUID)).
std::string acceptKey(const std::string& clientKey);

// Checks that an HTTP request is a valid WebSocket upgrade request and
// returns the client's key. On failure fills `error`.
bool validateUpgradeRequest(const HttpRequest& request, std::string& clientKey, std::string& error);

// Builds the "101 Switching Protocols" response for a valid request.
HttpResponse upgradeResponse(const std::string& clientKey);

struct Message {
    bool binary = false;  // false = UTF-8 text
    std::string data;
};

// One open WebSocket connection. receive() is meant for one reader thread;
// the send functions may be called from any thread at the same time.
class Connection {
public:
    // isClient = true makes this the client side (masks outgoing frames).
    // `pending` holds bytes that were already read from the socket.
    Connection(Socket socket, bool isClient, std::string pending = "", std::size_t maxMessageBytes = 1 << 20);
    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    // Blocks until a complete text or binary message arrives. Pings are
    // answered automatically. Returns false when the connection is closed.
    bool receive(Message& out);

    bool sendText(const std::string& text);
    bool sendBinary(const void* data, std::size_t size);

    // Sends a close frame (best effort) and shuts the socket down, which also
    // wakes up a thread blocked in receive().
    void close(std::uint16_t code = 1000);
    bool isClosed() const { return closed_.load(); }

    int fd() const { return socket_.fd(); }
    std::uint64_t bytesSent() const { return bytesSent_.load(); }
    std::uint64_t bytesReceived() const { return bytesReceived_.load(); }

private:
    bool sendFrame(std::uint8_t opcode, const std::uint8_t* data, std::size_t size);
    bool readBytes(std::size_t count);  // makes sure buffer_ holds at least `count` bytes

    Socket socket_;
    const bool isClient_;
    const std::size_t maxMessageBytes_;
    std::string buffer_;  // received bytes not processed yet
    std::mutex writeMutex_;
    std::atomic<bool> closed_{false};
    std::atomic<bool> closeSent_{false};
    std::atomic<std::uint64_t> bytesSent_{0};
    std::atomic<std::uint64_t> bytesReceived_{0};
    std::mt19937 maskRandom_;
};

// Client side: connects to ws://host:port/path, performs the handshake and
// returns the open connection (nullptr + `error` on failure).
std::unique_ptr<Connection> connect(const std::string& host, std::uint16_t port, const std::string& path,
                                    std::string& error, std::size_t maxMessageBytes = 64 << 20);

}  // namespace cge::ws
