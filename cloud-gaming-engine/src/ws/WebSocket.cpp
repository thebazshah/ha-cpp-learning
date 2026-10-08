#include "ws/WebSocket.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "core/Sha1.hpp"
#include "core/StringUtils.hpp"

namespace cge::ws {
namespace {

// Fixed string from the WebSocket standard, mixed into the handshake hash.
constexpr const char* kMagicGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

constexpr std::uint8_t kOpContinuation = 0x0;
constexpr std::uint8_t kOpText = 0x1;
constexpr std::uint8_t kOpBinary = 0x2;
constexpr std::uint8_t kOpClose = 0x8;
constexpr std::uint8_t kOpPing = 0x9;
constexpr std::uint8_t kOpPong = 0xA;

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

bool headerContainsToken(const std::string& value, const std::string& token) {
    for (const std::string& part : str::split(value, ',')) {
        if (str::iequals(str::trim(part), token)) return true;
    }
    return false;
}

}  // namespace

std::string acceptKey(const std::string& clientKey) {
    const auto digest = sha1(clientKey + kMagicGuid);
    return str::base64Encode(digest.data(), digest.size());
}

bool validateUpgradeRequest(const HttpRequest& request, std::string& clientKey, std::string& error) {
    if (request.method != "GET") {
        error = "WebSocket upgrades must use GET";
        return false;
    }
    if (!str::iequals(request.header("upgrade").value_or(""), "websocket")) {
        error = "missing 'Upgrade: websocket' header";
        return false;
    }
    if (!headerContainsToken(request.header("connection").value_or(""), "upgrade")) {
        error = "missing 'Connection: Upgrade' header";
        return false;
    }
    if (request.header("sec-websocket-version").value_or("") != "13") {
        error = "only WebSocket version 13 is supported";
        return false;
    }
    clientKey = request.header("sec-websocket-key").value_or("");
    if (clientKey.empty()) {
        error = "missing Sec-WebSocket-Key header";
        return false;
    }
    return true;
}

HttpResponse upgradeResponse(const std::string& clientKey) {
    HttpResponse response;
    response.status = 101;
    response.setHeader("Upgrade", "websocket");
    response.setHeader("Connection", "Upgrade");
    response.setHeader("Sec-WebSocket-Accept", acceptKey(clientKey));
    return response;
}

// =================================================================== Connection

Connection::Connection(Socket socket, bool isClient, std::string pending, std::size_t maxMessageBytes)
    : socket_(std::move(socket)),
      isClient_(isClient),
      maxMessageBytes_(maxMessageBytes),
      buffer_(std::move(pending)),
      maskRandom_(std::random_device{}()) {}

Connection::~Connection() { close(1001); }

bool Connection::readBytes(std::size_t count) {
    char chunk[16 * 1024];
    while (buffer_.size() < count) {
        const ssize_t got = ::recv(socket_.fd(), chunk, sizeof(chunk), 0);
        if (got > 0) {
            buffer_.append(chunk, static_cast<std::size_t>(got));
            bytesReceived_ += static_cast<std::uint64_t>(got);
            continue;
        }
        if (got < 0 && errno == EINTR) continue;
        return false;  // closed, error, or socket shut down by close()
    }
    return true;
}

bool Connection::receive(Message& out) {
    std::string assembled;    // payload of a message split into several frames
    std::uint8_t messageType = 0;

    while (!closed_) {
        // ---- frame header
        if (!readBytes(2)) break;
        const std::uint8_t b0 = static_cast<std::uint8_t>(buffer_[0]);
        const std::uint8_t b1 = static_cast<std::uint8_t>(buffer_[1]);
        const bool fin = (b0 & 0x80) != 0;
        const std::uint8_t opcode = b0 & 0x0F;
        const bool masked = (b1 & 0x80) != 0;
        std::uint64_t length = b1 & 0x7F;
        std::size_t headerSize = 2;
        if (length == 126) {
            if (!readBytes(4)) break;
            length = (static_cast<std::uint8_t>(buffer_[2]) << 8) | static_cast<std::uint8_t>(buffer_[3]);
            headerSize = 4;
        } else if (length == 127) {
            if (!readBytes(10)) break;
            length = 0;
            for (int i = 0; i < 8; ++i) length = (length << 8) | static_cast<std::uint8_t>(buffer_[2 + i]);
            headerSize = 10;
        }
        // The client must mask; the server must not. Anything else is a protocol error.
        if (masked == isClient_ || length > maxMessageBytes_ || assembled.size() + length > maxMessageBytes_) {
            close(1002);
            break;
        }
        std::uint8_t mask[4] = {0, 0, 0, 0};
        if (masked) {
            if (!readBytes(headerSize + 4)) break;
            for (int i = 0; i < 4; ++i) mask[i] = static_cast<std::uint8_t>(buffer_[headerSize + i]);
            headerSize += 4;
        }

        // ---- payload
        const std::size_t total = headerSize + static_cast<std::size_t>(length);
        if (!readBytes(total)) break;
        std::string payload = buffer_.substr(headerSize, static_cast<std::size_t>(length));
        buffer_.erase(0, total);
        if (masked) {
            for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>(payload[i] ^ mask[i % 4]);
        }

        // ---- control frames (may arrive between the parts of a split message)
        if (opcode == kOpClose) {
            if (!closeSent_) {
                closeSent_ = true;
                sendFrame(kOpClose, reinterpret_cast<const std::uint8_t*>(payload.data()), std::min<std::size_t>(payload.size(), 2));
            }
            close();
            break;
        }
        if (opcode == kOpPing) {
            sendFrame(kOpPong, reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size());
            continue;
        }
        if (opcode == kOpPong) continue;

        // ---- data frames
        if (opcode == kOpText || opcode == kOpBinary) {
            messageType = opcode;
            assembled = std::move(payload);
        } else if (opcode == kOpContinuation && messageType != 0) {
            assembled += payload;
        } else {
            close(1002);  // unknown opcode or a continuation without a start
            break;
        }
        if (fin) {
            out.binary = (messageType == kOpBinary);
            out.data = std::move(assembled);
            return true;
        }
    }
    closed_ = true;
    return false;
}

bool Connection::sendFrame(std::uint8_t opcode, const std::uint8_t* data, std::size_t size) {
    std::lock_guard<std::mutex> lock(writeMutex_);
    if (!socket_.valid()) return false;

    // Build the header (2 to 14 bytes).
    std::uint8_t header[14];
    std::size_t headerSize = 2;
    header[0] = static_cast<std::uint8_t>(0x80 | opcode);  // FIN + opcode: we never split messages
    const std::uint8_t maskBit = isClient_ ? 0x80 : 0x00;
    if (size < 126) {
        header[1] = static_cast<std::uint8_t>(maskBit | size);
    } else if (size <= 0xFFFF) {
        header[1] = static_cast<std::uint8_t>(maskBit | 126);
        header[2] = static_cast<std::uint8_t>(size >> 8);
        header[3] = static_cast<std::uint8_t>(size);
        headerSize = 4;
    } else {
        header[1] = static_cast<std::uint8_t>(maskBit | 127);
        for (int i = 0; i < 8; ++i) header[2 + i] = static_cast<std::uint8_t>(static_cast<std::uint64_t>(size) >> (56 - i * 8));
        headerSize = 10;
    }

    bool ok;
    if (isClient_) {
        // Clients must mask every frame with a fresh random key.
        std::uint8_t mask[4];
        const std::uint32_t random = maskRandom_();
        std::memcpy(mask, &random, 4);
        std::memcpy(header + headerSize, mask, 4);
        headerSize += 4;
        std::string frame(reinterpret_cast<const char*>(header), headerSize);
        frame.reserve(headerSize + size);
        for (std::size_t i = 0; i < size; ++i) frame += static_cast<char>(data[i] ^ mask[i % 4]);
        ok = sendAll(socket_.fd(), frame.data(), frame.size());
    } else if (size <= 64 * 1024) {
        // Small frame: one write (fewer system calls, one TCP packet).
        std::string frame(reinterpret_cast<const char*>(header), headerSize);
        frame.append(reinterpret_cast<const char*>(data), size);
        ok = sendAll(socket_.fd(), frame.data(), frame.size());
    } else {
        ok = sendAll(socket_.fd(), header, headerSize) && sendAll(socket_.fd(), data, size);
    }
    if (ok) bytesSent_ += headerSize + size;
    return ok;
}

bool Connection::sendText(const std::string& text) {
    if (closed_) return false;
    return sendFrame(kOpText, reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

bool Connection::sendBinary(const void* data, std::size_t size) {
    if (closed_) return false;
    return sendFrame(kOpBinary, static_cast<const std::uint8_t*>(data), size);
}

void Connection::close(std::uint16_t code) {
    if (!closeSent_.exchange(true) && socket_.valid()) {
        // Best effort: tell the other side why we are closing.
        const std::uint8_t payload[2] = {static_cast<std::uint8_t>(code >> 8), static_cast<std::uint8_t>(code)};
        // Do not wait long for a client that stopped reading.
        sendFrame(kOpClose, payload, 2);
    }
    closed_ = true;
    if (socket_.valid()) ::shutdown(socket_.fd(), SHUT_RDWR);  // wakes up a blocked receive()
}

// =================================================================== client

std::unique_ptr<Connection> connect(const std::string& host, std::uint16_t port, const std::string& path,
                                    std::string& error, std::size_t maxMessageBytes) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found) != 0 || found == nullptr) {
        error = "cannot resolve host " + host;
        return nullptr;
    }
    Socket socket(::socket(found->ai_family, found->ai_socktype, found->ai_protocol));
    const bool connected = socket.valid() && ::connect(socket.fd(), found->ai_addr, found->ai_addrlen) == 0;
    freeaddrinfo(found);
    if (!connected) {
        error = "cannot connect to " + host + ":" + std::to_string(port) + ": " + std::strerror(errno);
        return nullptr;
    }
    setCloseOnExec(socket.fd());
    socket.setNoDelay();
    socket.setTimeouts(10000, 10000);  // only for the handshake

    // A random 16-byte key, base64 encoded, as the standard requires.
    std::random_device random;
    std::uint8_t keyBytes[16];
    for (std::uint8_t& byte : keyBytes) byte = static_cast<std::uint8_t>(random());
    const std::string key = str::base64Encode(keyBytes, sizeof(keyBytes));
    const std::string request = "GET " + path + " HTTP/1.1\r\nHost: " + host + ":" + std::to_string(port) +
                                "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
                                "\r\nSec-WebSocket-Version: 13\r\n\r\n";
    if (!sendAll(socket.fd(), request.data(), request.size())) {
        error = "cannot send the handshake";
        return nullptr;
    }

    std::string response;
    char chunk[4096];
    std::size_t headEnd = std::string::npos;
    while ((headEnd = response.find("\r\n\r\n")) == std::string::npos) {
        const ssize_t got = ::recv(socket.fd(), chunk, sizeof(chunk), 0);
        if (got <= 0 || response.size() > 16384) {
            error = "no handshake response";
            return nullptr;
        }
        response.append(chunk, static_cast<std::size_t>(got));
    }
    const std::string head = response.substr(0, headEnd);
    if (!str::startsWith(head, "HTTP/1.1 101")) {
        error = "server refused the WebSocket: " + head.substr(0, head.find("\r\n"));
        return nullptr;
    }
    if (head.find(acceptKey(key)) == std::string::npos) {
        error = "wrong Sec-WebSocket-Accept value";
        return nullptr;
    }
    socket.setTimeouts(0, 10000);  // receive() blocks without a time limit from now on
    return std::make_unique<Connection>(std::move(socket), true, response.substr(headEnd + 4), maxMessageBytes);
}

}  // namespace cge::ws
