#include "rtsp/RtspMessage.hpp"

#include <ctime>

#include "core/StringUtils.hpp"

namespace mss {
namespace {
constexpr std::size_t kMaxHeadBytes = 16 * 1024;
constexpr long long kMaxBodyBytes = 64 * 1024;
}  // namespace

std::optional<std::string> RtspRequest::header(const std::string& lowercaseName) const {
    for (const auto& entry : headers) {
        if (entry.first == lowercaseName) return entry.second;
    }
    return std::nullopt;
}

int RtspRequest::cseq() const {
    long long value = 0;
    if (!str::parseInt64(header("cseq").value_or(""), value)) return 0;
    return static_cast<int>(value);
}

std::string RtspRequest::sessionId() const {
    std::string value = header("session").value_or("");
    const std::size_t semicolon = value.find(';');
    if (semicolon != std::string::npos) value.resize(semicolon);
    return str::trim(value);
}

ParseResult parseRtspRequest(std::string& buffer, RtspRequest& out, std::string& error) {
    const std::size_t headEnd = buffer.find("\r\n\r\n");
    if (headEnd == std::string::npos) {
        if (buffer.size() > kMaxHeadBytes) {
            error = "request head too large";
            return ParseResult::Error;
        }
        return ParseResult::NeedMore;
    }

    RtspRequest request;
    const std::vector<std::string> lines = str::split(buffer.substr(0, headEnd), '\n');
    const std::vector<std::string> first = str::split(str::trim(lines[0]), ' ');
    if (first.size() != 3 || !str::startsWith(first[2], "RTSP/")) {
        error = "malformed request line";
        return ParseResult::Error;
    }
    request.method = first[0];
    request.url = first[1];
    request.version = first[2];

    for (std::size_t i = 1; i < lines.size(); ++i) {
        const std::string line = str::trim(lines[i]);
        if (line.empty()) continue;
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            error = "malformed header";
            return ParseResult::Error;
        }
        request.headers.emplace_back(str::toLower(str::trim(line.substr(0, colon))), str::trim(line.substr(colon + 1)));
    }

    long long contentLength = 0;
    if (const auto length = request.header("content-length")) {
        if (!str::parseInt64(*length, contentLength) || contentLength < 0 || contentLength > kMaxBodyBytes) {
            error = "invalid Content-Length";
            return ParseResult::Error;
        }
    }
    const std::size_t total = headEnd + 4 + static_cast<std::size_t>(contentLength);
    if (buffer.size() < total) return ParseResult::NeedMore;

    request.body = buffer.substr(headEnd + 4, static_cast<std::size_t>(contentLength));
    buffer.erase(0, total);
    out = std::move(request);
    return ParseResult::Complete;
}

void RtspResponse::setHeader(const std::string& name, const std::string& value) {
    for (auto& entry : headers) {
        if (str::iequals(entry.first, name)) {
            entry.second = value;
            return;
        }
    }
    headers.emplace_back(name, value);
}

std::string RtspResponse::serialize(int cseq) const {
    std::string text = "RTSP/1.0 " + std::to_string(status) + " " + rtspStatusText(status) + "\r\n";
    text += "CSeq: " + std::to_string(cseq) + "\r\n";
    text += "Server: MediaStreamingService/1.0\r\n";
    text += "Date: " + str::httpDate(std::time(nullptr)) + "\r\n";
    for (const auto& entry : headers) text += entry.first + ": " + entry.second + "\r\n";
    if (!body.empty()) text += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    text += "\r\n";
    text += body;
    return text;
}

const char* rtspStatusText(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 453: return "Not Enough Bandwidth";
        case 454: return "Session Not Found";
        case 455: return "Method Not Valid in This State";
        case 457: return "Invalid Range";
        case 459: return "Aggregate Operation Not Allowed";
        case 461: return "Unsupported Transport";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        default: return "Unknown";
    }
}

bool parseTransport(const std::string& header, TransportSpec& out) {
    for (const std::string& choice : str::split(header, ',')) {
        const std::vector<std::string> parts = str::split(str::trim(choice), ';');
        if (parts.empty()) continue;
        const std::string protocol = str::toLower(str::trim(parts[0]));
        TransportSpec spec;
        if (protocol == "rtp/avp" || protocol == "rtp/avp/udp") {
            spec.tcp = false;
        } else if (protocol == "rtp/avp/tcp") {
            spec.tcp = true;
        } else {
            continue;  // e.g. RTP/SAVP (encrypted) - not supported
        }

        bool multicast = false;
        bool haveClientPorts = false;
        for (std::size_t i = 1; i < parts.size(); ++i) {
            const std::string part = str::trim(parts[i]);
            const std::size_t equals = part.find('=');
            const std::string key = str::toLower(part.substr(0, equals));
            const std::string value = equals == std::string::npos ? "" : part.substr(equals + 1);
            if (key == "multicast") multicast = true;

            // Values look like "5000-5001" (a pair) or "5000" (single).
            long long low = 0;
            long long high = 0;
            const std::size_t dash = value.find('-');
            const bool parsed = str::parseInt64(value.substr(0, dash), low) &&
                                (dash == std::string::npos ? (high = low + 1, true)
                                                           : str::parseInt64(value.substr(dash + 1), high));
            if (key == "client_port" && parsed && low > 0 && low < 65536 && high > 0 && high < 65536) {
                spec.clientRtpPort = static_cast<int>(low);
                spec.clientRtcpPort = static_cast<int>(high);
                haveClientPorts = true;
            } else if (key == "interleaved" && parsed && low >= 0 && low < 256 && high >= 0 && high < 256) {
                spec.rtpChannel = static_cast<int>(low);
                spec.rtcpChannel = static_cast<int>(high);
            }
        }
        if (multicast) continue;                    // we only do unicast
        if (!spec.tcp && !haveClientPorts) continue;  // UDP needs to know where to send
        out = spec;
        return true;
    }
    return false;
}

bool parseRtspUrl(const std::string& url, RtspTarget& out) {
    // Strip "rtsp://host:port" so only the path (and query) is left.
    std::string rest = url;
    const std::size_t scheme = rest.find("://");
    if (scheme != std::string::npos) {
        const std::size_t slash = rest.find('/', scheme + 3);
        rest = slash == std::string::npos ? "/" : rest.substr(slash);
    }

    std::string path = rest;
    std::string query;
    const std::size_t question = rest.find('?');
    if (question != std::string::npos) {
        path = rest.substr(0, question);
        query = rest.substr(question + 1);
    }

    RtspTarget target;
    std::vector<std::string> segments;
    for (const std::string& segment : str::split(path, '/')) {
        if (!segment.empty()) segments.push_back(segment);
    }

    auto readKeyValue = [&target](const std::string& item) {
        const std::size_t equals = item.find('=');
        if (equals == std::string::npos) return false;
        const std::string key = str::toLower(item.substr(0, equals));
        long long value = 0;
        if (!str::parseInt64(item.substr(equals + 1), value) || value < 0 || value > 1000) return false;
        if (key == "trackid") target.trackId = static_cast<int>(value);
        else if (key == "rendition") target.rendition = static_cast<int>(value);
        else return false;
        return true;
    };

    // A trailing "/trackID=N" path segment selects a track.
    if (!segments.empty() && str::startsWith(str::toLower(segments.back()), "trackid=")) {
        if (!readKeyValue(segments.back())) return false;
        segments.pop_back();
    }
    // The query may hold "rendition=N" and (after a '/') "trackID=N".
    for (const std::string& piece : str::split(query, '&')) {
        for (const std::string& item : str::split(piece, '/')) {
            if (!item.empty()) readKeyValue(item);
        }
    }

    if (segments.size() != 2) return false;
    if (!parseMediaKind(segments[0], target.kind)) return false;
    if (!str::urlDecode(segments[1], target.name, false) || target.name.empty()) return false;
    out = target;
    return true;
}

std::optional<double> parseNptStart(const std::string& rangeHeader) {
    const std::string value = str::trim(rangeHeader);
    if (!str::startsWith(value, "npt=")) return std::nullopt;
    const std::string spec = value.substr(4);
    const std::size_t dash = spec.find('-');
    const std::string start = str::trim(spec.substr(0, dash));
    if (start.empty() || start == "now") return std::nullopt;
    double seconds = 0;
    if (!str::parseDouble(start, seconds) || seconds < 0) return std::nullopt;
    return seconds;
}

}  // namespace mss
