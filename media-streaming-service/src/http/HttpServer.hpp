#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/Json.hpp"
#include "core/ThreadPool.hpp"
#include "http/HttpMessage.hpp"
#include "http/Router.hpp"
#include "net/Socket.hpp"

namespace mss {

struct HttpServerOptions {
    std::string bindAddress = "0.0.0.0";
    std::uint16_t port = 8080;
    std::size_t workerThreads = 32;
    int idleTimeoutSeconds = 30;  // close keep-alive connections that stay silent this long
    int ioTimeoutSeconds = 30;    // give up on a read or write that makes no progress this long
    std::size_t maxConnections = 2048;
};

// A multithreaded HTTP/1.1 server built directly on POSIX sockets.
//
// How it works (a small "reactor" design):
//
//   accept thread  --new connection-->  poll thread  --"has data"-->  worker pool
//        ^                                   ^                              |
//        |                                   +---- keep-alive connection ---+
//
//  1. The accept thread takes new TCP connections and hands them to the poll thread.
//  2. The poll thread watches all idle connections with poll(). When one has
//     data to read, it gives the connection to a worker thread.
//  3. A worker reads the request, runs the matching handler, writes the
//     response, and then gives the connection back to the poll thread so the
//     browser can reuse it (keep-alive).
//
// Because idle connections are only watched by poll() (and do not each block
// a thread), a few dozen workers can serve many open browser connections.
class HttpServer {
public:
    HttpServer(HttpServerOptions options, const Router& router);
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    bool start(std::string& error);
    void stop();

    Json statsJson() const;

private:
    struct Connection {
        explicit Connection(std::atomic<std::size_t>& counter) : openCounter(counter) { ++openCounter; }
        ~Connection() { --openCounter; }  // the socket closes itself (Socket is RAII)

        Socket socket;
        std::string ip;
        std::uint16_t port = 0;
        std::string buffer;  // bytes received but not yet parsed
        std::chrono::steady_clock::time_point lastActive;
        std::atomic<std::size_t>& openCounter;
    };
    using ConnectionPtr = std::shared_ptr<Connection>;

    void acceptLoop();
    void pollLoop();
    void serveConnection(const ConnectionPtr& connection);
    void handOverToPoller(ConnectionPtr connection);
    void wakePoller();
    HttpResponse handleRequest(HttpRequest& request);
    bool sendResponse(Connection& connection, const std::string& method, HttpResponse& response, bool keepAlive);
    bool sendFileRegion(int socketFd, const FileBody& file);

    HttpServerOptions options_;
    const Router& router_;
    std::unique_ptr<ThreadPool> workers_;
    Socket listener_;
    int wakeRead_ = -1;   // writing a byte to wakeWrite_ makes poll() return immediately
    int wakeWrite_ = -1;
    std::thread acceptThread_;
    std::thread pollThread_;
    std::atomic<bool> running_{false};

    std::mutex handoverMutex_;
    std::vector<ConnectionPtr> handover_;  // connections waiting to be watched by the poll thread

    std::atomic<std::size_t> openConnections_{0};
    std::atomic<std::uint64_t> totalConnections_{0};
    std::atomic<std::uint64_t> totalRequests_{0};
    std::atomic<std::uint64_t> bytesSent_{0};
    std::chrono::steady_clock::time_point startedAt_;
};

}  // namespace mss
