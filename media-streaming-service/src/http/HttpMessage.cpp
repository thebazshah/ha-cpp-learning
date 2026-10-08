#include "http/HttpMessage.hpp"

#include <ctime>

#include "core/StringUtils.hpp"

namespace mss {
namespace {

constexpr std::size_t kMaxHeaderBytes = 16 * 1024;  // bigger request heads are rejected (431)
constexpr long long kMaxBodyBytes = 1024 * 1024;    // bigger request bodies are rejected (413)

}  // namespace

// ---------------------------------------------------------------- request

std::optional<std::string> HttpRequest::header(const std::string& lowercaseName) const {
    for (const auto& entry : headers) {
        if (entry.first == lowercaseName) return entry.second;
    }
    return std::nullopt;
}

std::string HttpRequest::queryParam(const std::string& key) const {
    for (const std::string& pair : str::split(query, '&')) {
        const std::size_t equals = pair.find('=');
        std::string name;
        if (!str::urlDecode(pair.substr(0, equals), name, true) || name != key) continue;
        std::string value;
        if (equals != std::string::npos && str::urlDecode(pair.substr(equals + 1), value, true)) return value;
        return "";
    }
    return "";
}

bool HttpRequest::keepAlive() const {
    const std::string connection = str::toLower(header("connection").value_or(""));
    if (version == "HTTP/1.0") return connection.find("keep-alive") != std::string::npos;
    return connection.find("close") == std::string::npos;
}

// ---------------------------------------------------------------- response

void HttpResponse::setHeader(const std::string& name, const std::string& value) {
    for (auto& entry : headers) {
        if (str::iequals(entry.first, name)) {
            entry.second = value;
            return;
        }
    }
    headers.emplace_back(name, value);
}

bool HttpResponse::hasHeader(const std::string& name) const {
    for (const auto& entry : headers) {
        if (str::iequals(entry.first, name)) return true;
    }
    return false;
}

std::uint64_t HttpResponse::contentLength() const {
    if (file) return file->length;
    if (sharedBody) return sharedBody->size();
    return body.size();
}

HttpResponse HttpResponse::json(int status, const Json& value) {
    HttpResponse response;
    response.status = status;
    response.body = value.dump();
    response.setHeader("Content-Type", "application/json; charset=utf-8");
    response.setHeader("Cache-Control", "no-store");
    return response;
}

HttpResponse HttpResponse::text(int status, std::string content, const std::string& contentType) {
    HttpResponse response;
    response.status = status;
    response.body = std::move(content);
    response.setHeader("Content-Type", contentType);
    return response;
}

HttpResponse HttpResponse::error(int status, const std::string& message) {
    Json body = Json::object();
    body.set("error", message).set("status", status);
    return json(status, body);
}

const char* httpStatusText(int status) {
    switch (status) {
        case 200: return "OK";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 206: return "Partial Content";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 411: return "Length Required";
        case 413: return "Payload Too Large";
        case 416: return "Range Not Satisfiable";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        case 505: return "HTTP Version Not Supported";
        default: return "Unknown";
    }
}

// ---------------------------------------------------------------- parsing

ParseResult parseHttpRequest(std::string& buffer, HttpRequest& out, int& errorStatus, std::string& errorMessage) {
    // The head (request line + headers) ends with an empty line.
    const std::size_t headEnd = buffer.find("\r\n\r\n");
    if (headEnd == std::string::npos) {
        if (buffer.size() > kMaxHeaderBytes) {
            errorStatus = 431;
            errorMessage = "Request headers are too large";
            return ParseResult::Error;
        }
        return ParseResult::NeedMore;
    }
    if (headEnd > kMaxHeaderBytes) {
        errorStatus = 431;
        errorMessage = "Request headers are too large";
        return ParseResult::Error;
    }

    HttpRequest request;
    std::vector<std::string> lines;
    {
        std::size_t start = 0;
        while (start <= headEnd) {
            std::size_t end = buffer.find("\r\n", start);
            if (end == std::string::npos || end > headEnd) end = headEnd;
            lines.push_back(buffer.substr(start, end - start));
            if (end >= headEnd) break;
            start = end + 2;
        }
    }

    // Request line: METHOD SP TARGET SP VERSION
    const std::vector<std::string> requestLine = str::split(lines[0], ' ');
    if (requestLine.size() != 3 || requestLine[0].empty() || requestLine[1].empty()) {
        errorStatus = 400;
        errorMessage = "Malformed request line";
        return ParseResult::Error;
    }
    request.method = requestLine[0];
    request.target = requestLine[1];
    request.version = requestLine[2];
    if (!str::startsWith(request.version, "HTTP/1.")) {
        errorStatus = 505;
        errorMessage = "Only HTTP/1.x is supported";
        return ParseResult::Error;
    }
    if (request.target[0] != '/') {
        errorStatus = 400;
        errorMessage = "Request target must start with '/'";
        return ParseResult::Error;
    }

    // Split the target into path and query, and drop any "#fragment".
    std::string target = request.target;
    const std::size_t hash = target.find('#');
    if (hash != std::string::npos) target.resize(hash);
    const std::size_t question = target.find('?');
    request.path = target.substr(0, question);
    if (question != std::string::npos) request.query = target.substr(question + 1);

    // Header lines: "Name: value"
    for (std::size_t i = 1; i < lines.size(); ++i) {
        if (lines[i].empty()) continue;
        const std::size_t colon = lines[i].find(':');
        if (colon == std::string::npos || colon == 0) {
            errorStatus = 400;
            errorMessage = "Malformed header line";
            return ParseResult::Error;
        }
        request.headers.emplace_back(str::toLower(str::trim(lines[i].substr(0, colon))),
                                     str::trim(lines[i].substr(colon + 1)));
    }

    if (request.header("transfer-encoding")) {
        errorStatus = 501;
        errorMessage = "Chunked request bodies are not supported";
        return ParseResult::Error;
    }

    long long contentLength = 0;
    if (const auto lengthHeader = request.header("content-length")) {
        if (!str::parseInt64(*lengthHeader, contentLength) || contentLength < 0) {
            errorStatus = 400;
            errorMessage = "Invalid Content-Length";
            return ParseResult::Error;
        }
        if (contentLength > kMaxBodyBytes) {
            errorStatus = 413;
            errorMessage = "Request body is too large";
            return ParseResult::Error;
        }
    }

    const std::size_t total = headEnd + 4 + static_cast<std::size_t>(contentLength);
    if (buffer.size() < total) return ParseResult::NeedMore;  // body not fully received yet

    request.body = buffer.substr(headEnd + 4, static_cast<std::size_t>(contentLength));
    buffer.erase(0, total);
    out = std::move(request);
    return ParseResult::Complete;
}

std::string serializeResponseHead(const HttpResponse& response, bool keepAlive, std::uint64_t contentLength) {
    std::string head;
    head.reserve(256 + response.headers.size() * 48);
    head += "HTTP/1.1 ";
    head += std::to_string(response.status);
    head += ' ';
    head += httpStatusText(response.status);
    head += "\r\n";
    for (const auto& entry : response.headers) {
        head += entry.first;
        head += ": ";
        head += entry.second;
        head += "\r\n";
    }
    head += "Content-Length: " + std::to_string(contentLength) + "\r\n";
    head += keepAlive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
    head += "Date: " + str::httpDate(std::time(nullptr)) + "\r\n";
    head += "Server: MediaStreamingService/1.0\r\n\r\n";
    return head;
}

}  // namespace mss
