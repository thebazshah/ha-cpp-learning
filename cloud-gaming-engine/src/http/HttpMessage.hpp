#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/Json.hpp"

namespace cge {

// One HTTP request, as received from a browser or player.
struct HttpRequest {
    std::string method;   // "GET", "POST", ...
    std::string target;   // raw request target, e.g. "/hls/videos/a%20b.mp4/master.m3u8?x=1"
    std::string path;     // raw path without the query: "/hls/videos/a%20b.mp4/master.m3u8"
    std::string query;    // raw query string without '?': "x=1"
    std::string version;  // "HTTP/1.1"
    std::vector<std::pair<std::string, std::string>> headers;  // names are lowercase
    std::string body;

    std::map<std::string, std::string> params;  // path parameters filled by the Router ({name} parts)
    std::string remoteIp;

    // Returns the value of a header (pass the name in lowercase), if present.
    std::optional<std::string> header(const std::string& lowercaseName) const;

    // Returns a decoded query-string value, or "" if it is missing.
    std::string queryParam(const std::string& key) const;

    // HTTP/1.1 keeps the connection open unless "Connection: close" is sent.
    bool keepAlive() const;
};

class Socket;

// Called after a "101 Switching Protocols" response has been sent. It
// receives the TCP connection (now owned by the callee) and any bytes the
// client already sent after its request. Used to turn an HTTP connection
// into a WebSocket connection.
using UpgradeHandler = std::function<void(Socket socket, std::string pendingBytes)>;

// The answer we send back.
struct HttpResponse {
    int status = 200;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    UpgradeHandler upgrade;  // set only for protocol upgrades (WebSocket)

    // Adds or replaces a header.
    void setHeader(const std::string& name, const std::string& value);
    bool hasHeader(const std::string& name) const;

    std::uint64_t contentLength() const;

    static HttpResponse json(int status, const Json& value);
    static HttpResponse text(int status, std::string content, const std::string& contentType);
    // A JSON error: {"error": "...", "status": 404}
    static HttpResponse error(int status, const std::string& message);
};

// The standard reason phrase for a status code ("Not Found" for 404).
const char* httpStatusText(int status);

// Result of trying to read one request from the bytes received so far.
enum class ParseResult { NeedMore, Complete, Error };

// Tries to parse one complete request from the start of `buffer`.
//   Complete: `out` is filled and the request bytes are removed from `buffer`.
//   NeedMore: the request is not complete yet - read more bytes and call again.
//   Error:    the request is broken; `errorStatus` holds the HTTP status to answer with.
ParseResult parseHttpRequest(std::string& buffer, HttpRequest& out, int& errorStatus, std::string& errorMessage);

// Builds the status line and headers of a response, ending with an empty line.
std::string serializeResponseHead(const HttpResponse& response, bool keepAlive, std::uint64_t contentLength);

}  // namespace cge
