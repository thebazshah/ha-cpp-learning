#include "http/HttpServer.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <exception>
#include <vector>

#include "core/Logger.hpp"

namespace cge {
namespace {

constexpr std::size_t kInlineBodyLimit = 64 * 1024;  // small bodies are sent in the same write as the headers

}  // namespace

HttpServer::HttpServer(HttpServerOptions options, const Router& router)
    : options_(std::move(options)), router_(router) {}

HttpServer::~HttpServer() { stop(); }

bool HttpServer::start(std::string& error) {
    listener_ = createTcpListener(options_.bindAddress, options_.port, error);
    if (!listener_.valid()) return false;

    int fds[2];
    if (pipe(fds) != 0) {
        error = "cannot create wake-up pipe";
        return false;
    }
    wakeRead_ = fds[0];
    wakeWrite_ = fds[1];
    setCloseOnExec(wakeRead_);
    setCloseOnExec(wakeWrite_);
    fcntl(wakeRead_, F_SETFL, O_NONBLOCK);
    fcntl(wakeWrite_, F_SETFL, O_NONBLOCK);

    workers_ = std::make_unique<ThreadPool>(options_.workerThreads, "http");
    running_ = true;
    startedAt_ = std::chrono::steady_clock::now();
    acceptThread_ = std::thread([this] { acceptLoop(); });
    pollThread_ = std::thread([this] { pollLoop(); });
    LOG_INFO("http") << "HTTP server listening on " << options_.bindAddress << ":" << options_.port << " with "
                     << options_.workerThreads << " worker threads";
    return true;
}

void HttpServer::stop() {
    if (!running_.exchange(false)) return;
    LOG_INFO("http") << "Stopping HTTP server";
    wakePoller();
    if (acceptThread_.joinable()) acceptThread_.join();
    if (pollThread_.joinable()) pollThread_.join();
    if (workers_) workers_->shutdown();  // lets requests that are already running finish
    listener_.close();
    {
        std::lock_guard<std::mutex> lock(handoverMutex_);
        handover_.clear();
    }
    if (wakeRead_ >= 0) close(wakeRead_);
    if (wakeWrite_ >= 0) close(wakeWrite_);
    wakeRead_ = wakeWrite_ = -1;
}

Json HttpServer::statsJson() const {
    Json stats = Json::object();
    stats.set("port", options_.port)
        .set("openConnections", openConnections_.load())
        .set("totalConnections", totalConnections_.load())
        .set("totalRequests", totalRequests_.load())
        .set("bytesSent", bytesSent_.load())
        .set("workerThreads", options_.workerThreads)
        .set("busyWorkers", workers_ ? workers_->busyThreads() : 0)
        .set("queuedRequests", workers_ ? workers_->queuedTasks() : 0);
    return stats;
}

// ------------------------------------------------------------------ threads

void HttpServer::acceptLoop() {
    while (running_) {
        // Wait at most 500 ms so we notice quickly when stop() is called.
        pollfd waitFor{listener_.fd(), POLLIN, 0};
        const int ready = poll(&waitFor, 1, 500);
        if (ready <= 0) continue;

        std::string ip;
        std::uint16_t port = 0;
        Socket socket = acceptClient(listener_, ip, port);
        if (!socket.valid()) continue;

        if (openConnections_.load() >= options_.maxConnections) {
            static const char kBusy[] =
                "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            sendAll(socket.fd(), kBusy, sizeof(kBusy) - 1);
            continue;  // the socket closes when it goes out of scope
        }

        socket.setNoDelay();
        socket.setTimeouts(options_.ioTimeoutSeconds * 1000, options_.ioTimeoutSeconds * 1000);

        auto connection = std::make_shared<Connection>(openConnections_);
        connection->socket = std::move(socket);
        connection->ip = ip;
        connection->port = port;
        connection->lastActive = std::chrono::steady_clock::now();
        ++totalConnections_;
        handOverToPoller(std::move(connection));
    }
}

void HttpServer::handOverToPoller(ConnectionPtr connection) {
    {
        std::lock_guard<std::mutex> lock(handoverMutex_);
        handover_.push_back(std::move(connection));
    }
    wakePoller();
}

void HttpServer::wakePoller() {
    if (wakeWrite_ >= 0) {
        const char byte = 1;
        (void)!write(wakeWrite_, &byte, 1);  // if the pipe is full the poller is awake anyway
    }
}

void HttpServer::pollLoop() {
    // Only this thread touches `idle`, so it needs no lock.
    std::vector<ConnectionPtr> idle;
    std::vector<pollfd> fds;

    while (running_) {
        {
            std::lock_guard<std::mutex> lock(handoverMutex_);
            for (ConnectionPtr& connection : handover_) idle.push_back(std::move(connection));
            handover_.clear();
        }

        fds.clear();
        fds.push_back(pollfd{wakeRead_, POLLIN, 0});
        for (const ConnectionPtr& connection : idle) fds.push_back(pollfd{connection->socket.fd(), POLLIN, 0});

        const int ready = poll(fds.data(), static_cast<nfds_t>(fds.size()), 1000);
        if (ready < 0 && errno != EINTR) {
            LOG_ERROR("http") << "poll() failed: " << errno;
            continue;
        }

        if (fds[0].revents & POLLIN) {
            char drain[256];
            while (read(wakeRead_, drain, sizeof(drain)) > 0) {
            }
        }

        const auto now = std::chrono::steady_clock::now();
        const auto idleLimit = std::chrono::seconds(options_.idleTimeoutSeconds);
        std::vector<ConnectionPtr> stillIdle;
        stillIdle.reserve(idle.size());

        for (std::size_t i = 0; i < idle.size(); ++i) {
            ConnectionPtr& connection = idle[i];
            const short events = fds[i + 1].revents;
            if (events & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) {
                // Data (or a close) arrived: let a worker handle it.
                ConnectionPtr job = std::move(connection);
                if (!workers_->submit([this, job] { serveConnection(job); })) {
                    // The pool is shutting down; dropping `job` closes the connection.
                }
            } else if (now - connection->lastActive > idleLimit) {
                // Silent for too long: drop it (closing the socket).
            } else {
                stillIdle.push_back(std::move(connection));
            }
        }
        idle.swap(stillIdle);
    }
    // Leaving the loop drops `idle`, which closes all remaining keep-alive connections.
}

// ------------------------------------------------------------------ requests

void HttpServer::serveConnection(const ConnectionPtr& connection) {
    char chunk[16 * 1024];
    while (running_) {
        HttpRequest request;
        int errorStatus = 400;
        std::string errorMessage;
        const ParseResult result = parseHttpRequest(connection->buffer, request, errorStatus, errorMessage);

        if (result == ParseResult::NeedMore) {
            const ssize_t count = recv(connection->socket.fd(), chunk, sizeof(chunk), 0);
            if (count > 0) {
                connection->buffer.append(chunk, static_cast<std::size_t>(count));
                continue;
            }
            if (count < 0 && errno == EINTR) continue;
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && !connection->buffer.empty()) {
                // The client started a request but stopped sending (timeout).
                HttpResponse timeout = HttpResponse::error(408, "Request timeout");
                sendResponse(*connection, "", timeout, false);
            }
            return;  // client closed the connection or an error happened
        }

        if (result == ParseResult::Error) {
            LOG_DEBUG("http") << connection->ip << " sent a bad request: " << errorMessage;
            HttpResponse response = HttpResponse::error(errorStatus, errorMessage);
            sendResponse(*connection, "", response, false);
            return;
        }

        // A complete request: run the handler and answer.
        request.remoteIp = connection->ip;
        const auto startedAt = std::chrono::steady_clock::now();
        bool keepAlive = request.keepAlive() && running_;
        HttpResponse response = handleRequest(request);
        ++totalRequests_;
        const bool sent = sendResponse(*connection, request.method, response, keepAlive);

        LOG_DEBUG("http") << connection->ip << " " << request.method << " " << request.target << " -> "
                          << response.status << " (" << response.contentLength() << " bytes, "
                          << std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - startedAt)
                                 .count()
                          << " ms)";

        if (sent && response.status == 101 && response.upgrade) {
            // Protocol switch (WebSocket): the connection leaves the HTTP
            // server. The upgrade handler now owns the socket.
            Socket socket(connection->socket.release());
            std::string pending;
            pending.swap(connection->buffer);
            response.upgrade(std::move(socket), std::move(pending));
            return;
        }
        if (!sent || !keepAlive) return;
        connection->lastActive = std::chrono::steady_clock::now();
        if (connection->buffer.empty()) {
            // Nothing else is waiting: give the connection back to the poll thread.
            handOverToPoller(connection);
            return;
        }
        // The client already sent the next request (pipelining): handle it now.
    }
}

HttpResponse HttpServer::handleRequest(HttpRequest& request) {
    HttpResponse response;
    if (request.method == "OPTIONS") {
        // CORS "preflight" request from a browser: say which methods are allowed.
        response.status = 204;
        response.setHeader("Access-Control-Allow-Methods", "GET, HEAD, POST, DELETE, OPTIONS");
        response.setHeader("Access-Control-Allow-Headers", "Content-Type, Range");
        response.setHeader("Access-Control-Max-Age", "86400");
    } else {
        // HEAD is answered exactly like GET, but without sending the body.
        const std::string originalMethod = request.method;
        if (request.method == "HEAD") request.method = "GET";
        try {
            if (!router_.dispatch(request, response)) response = HttpResponse::error(404, "Not found");
        } catch (const std::exception& error) {
            LOG_ERROR("http") << "Handler for " << request.path << " failed: " << error.what();
            response = HttpResponse::error(500, "Internal server error");
        }
        request.method = originalMethod;
    }
    // Allow web pages from other origins (for example a separate player page) to use the API.
    response.setHeader("Access-Control-Allow-Origin", "*");
    response.setHeader("Access-Control-Expose-Headers", "Content-Length, Content-Range");
    return response;
}

bool HttpServer::sendResponse(Connection& connection, const std::string& method, HttpResponse& response,
                              bool keepAlive) {
    const int fd = connection.socket.fd();
    const std::uint64_t length = response.contentLength();
    std::string head = serializeResponseHead(response, keepAlive, length);

    bool ok = true;
    std::uint64_t written = head.size();
    if (method == "HEAD" || length == 0) {
        ok = sendAll(fd, head.data(), head.size());
    } else {
        const std::string& body = response.body;
        if (body.size() <= kInlineBodyLimit) {
            head += body;  // one system call for small responses
            ok = sendAll(fd, head.data(), head.size());
        } else {
            ok = sendAll(fd, head.data(), head.size()) && sendAll(fd, body.data(), body.size());
        }
        written += body.size();
    }
    if (ok) bytesSent_ += written;
    return ok;
}

}  // namespace cge
