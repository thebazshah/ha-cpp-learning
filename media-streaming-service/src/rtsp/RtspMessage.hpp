#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "http/HttpMessage.hpp"  // for ParseResult
#include "media/MediaLibrary.hpp"

// RTSP ("Real Time Streaming Protocol", RFC 2326) looks a lot like HTTP:
//
//   DESCRIBE rtsp://server:8554/videos/movie.mp4 RTSP/1.0
//   CSeq: 2
//   Accept: application/sdp
//
// The client uses it like a remote control: DESCRIBE (what is in the
// stream?), SETUP (how should each track be delivered?), PLAY, PAUSE and
// TEARDOWN. The media itself travels separately as RTP packets.
namespace mss {

struct RtspRequest {
    std::string method;   // OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN, GET_PARAMETER, ...
    std::string url;      // rtsp://host:port/path
    std::string version;  // RTSP/1.0
    std::vector<std::pair<std::string, std::string>> headers;  // names are lowercase
    std::string body;

    std::optional<std::string> header(const std::string& lowercaseName) const;
    int cseq() const;  // the sequence number every response must echo back
    // The session id from the "Session:" header (without ";timeout=...").
    std::string sessionId() const;
};

// Parses one request from the start of `buffer` (same rules as parseHttpRequest).
ParseResult parseRtspRequest(std::string& buffer, RtspRequest& out, std::string& error);

struct RtspResponse {
    int status = 200;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;

    void setHeader(const std::string& name, const std::string& value);
    std::string serialize(int cseq) const;
};

const char* rtspStatusText(int status);

// How the client wants a track delivered (from the "Transport:" header).
struct TransportSpec {
    bool tcp = false;         // true = RTP inside the RTSP TCP connection ("interleaved")
    int rtpChannel = 0;       // TCP: channel numbers for RTP and RTCP
    int rtcpChannel = 1;
    int clientRtpPort = 0;    // UDP: the client's ports for RTP and RTCP
    int clientRtcpPort = 0;
};

// Parses a Transport header such as
//   RTP/AVP;unicast;client_port=5000-5001
//   RTP/AVP/TCP;unicast;interleaved=0-1
// The header may list several comma-separated choices; the first one we
// support is used. Returns false if none is supported.
bool parseTransport(const std::string& header, TransportSpec& out);

// What an RTSP URL points to.
struct RtspTarget {
    MediaKind kind = MediaKind::Video;
    std::string name;    // decoded file name
    int trackId = -1;    // from ".../trackID=N" (-1 if absent)
    int rendition = -1;  // from "?rendition=N" to force one quality level (-1 = adaptive)
};

// Understands URLs like
//   rtsp://host:8554/videos/movie.mp4
//   rtsp://host:8554/videos/movie.mp4/trackID=1
//   rtsp://host:8554/videos/movie.mp4?rendition=2/trackID=0
bool parseRtspUrl(const std::string& url, RtspTarget& out);

// Reads the start time of "Range: npt=12.5-". Returns nullopt for "npt=now-" or no range.
std::optional<double> parseNptStart(const std::string& rangeHeader);

}  // namespace mss
