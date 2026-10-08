#include "rtsp/RtspServer.hpp"

#include <poll.h>
#include <sys/socket.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <random>

#include "core/Logger.hpp"
#include "core/StringUtils.hpp"
#include "rtsp/Sdp.hpp"

namespace mss {
namespace {

// DESCRIBE waits this long for the transcoder to produce the first segments.
constexpr auto kDescribeWait = std::chrono::seconds(30);

std::string newSessionId() {
    static std::mt19937_64 generator{std::random_device{}()};
    static std::mutex generatorMutex;
    std::lock_guard<std::mutex> lock(generatorMutex);
    char text[20];
    std::snprintf(text, sizeof(text), "%016llX", static_cast<unsigned long long>(generator()));
    return text;
}

RtspResponse errorResponse(int status) {
    RtspResponse response;
    response.status = status;
    return response;
}

// The quality level a new session starts with: the forced one if the URL
// asked for it (?rendition=N), otherwise the middle of the ladder, which
// starts quickly and lets the ABR controller move up or down from there.
std::size_t initialRendition(const TranscodeJob& job, const RtspTarget& target) {
    const std::size_t count = job.renditions().size();
    if (target.rendition >= 0 && static_cast<std::size_t>(target.rendition) < count) {
        return static_cast<std::size_t>(target.rendition);
    }
    return (count - 1) / 2;
}

}  // namespace

// =================================================================== RtspConnection

RtspConnection::RtspConnection(Socket socket, std::string clientIp, RtspServer& server)
    : server_(server),
      writer_(std::make_shared<RtspChannelWriter>(std::move(socket))),
      clientIp_(std::move(clientIp)),
      serverIp_(localAddress(writer_->fd())) {}

RtspConnection::~RtspConnection() {
    stop();
    join();
}

void RtspConnection::start() { thread_ = std::thread([this] { run(); }); }

void RtspConnection::stop() {
    stopRequested_ = true;
    writer_->shutdown();
}

void RtspConnection::join() {
    if (thread_.joinable()) thread_.join();
}

void RtspConnection::run() {
    LOG_DEBUG("rtsp") << "Connection from " << clientIp_;
    char chunk[16 * 1024];
    while (!stopRequested_) {
        pollfd waitFor{writer_->fd(), POLLIN, 0};
        const int ready = poll(&waitFor, 1, 500);
        if (ready == 0 || (ready < 0 && errno == EINTR)) continue;
        if (ready < 0) break;

        const ssize_t count = recv(writer_->fd(), chunk, sizeof(chunk), 0);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (count <= 0) break;  // the client closed the connection
        buffer_.append(chunk, static_cast<std::size_t>(count));
        if (!processBuffer()) break;
    }

    // The connection is gone: wake any sender blocked on it, close the
    // sessions that were created here, then close the socket.
    writer_->shutdown();
    for (const std::string& id : sessionIds_) server_.removeSession(id);
    sessionIds_.clear();
    writer_->close();
    LOG_DEBUG("rtsp") << "Connection from " << clientIp_ << " closed";
    finished_ = true;
}

bool RtspConnection::processBuffer() {
    while (!buffer_.empty()) {
        if (buffer_[0] == '$') {
            // Interleaved binary data ('$', channel, 16-bit length, packet):
            // RTCP receiver reports from a client that uses RTP over TCP.
            if (buffer_.size() < 4) return true;
            const std::size_t length = (static_cast<std::uint8_t>(buffer_[2]) << 8) | static_cast<std::uint8_t>(buffer_[3]);
            if (buffer_.size() < 4 + length) return true;
            const std::uint8_t channel = static_cast<std::uint8_t>(buffer_[1]);
            const auto* packet = reinterpret_cast<const std::uint8_t*>(buffer_.data() + 4);
            for (const std::string& id : sessionIds_) {
                if (auto session = server_.findSession(id)) session->onInterleavedPacket(channel, packet, length);
            }
            buffer_.erase(0, 4 + length);
            continue;
        }

        RtspRequest request;
        std::string error;
        const ParseResult result = parseRtspRequest(buffer_, request, error);
        if (result == ParseResult::NeedMore) return true;
        if (result == ParseResult::Error) {
            LOG_DEBUG("rtsp") << clientIp_ << " sent a malformed request: " << error;
            writer_->sendText(errorResponse(400).serialize(0));
            return false;
        }
        handleRequest(request);
    }
    return true;
}

void RtspConnection::handleRequest(const RtspRequest& request) {
    LOG_DEBUG("rtsp") << clientIp_ << " " << request.method << " " << request.url;

    std::shared_ptr<RtspSession> startAfterReply;
    RtspResponse response;
    const std::string& method = request.method;
    if (method == "OPTIONS") {
        response = handleKeepAlive(request);
        response.setHeader("Public", "OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN, GET_PARAMETER, SET_PARAMETER");
    } else if (method == "DESCRIBE") {
        response = handleDescribe(request);
    } else if (method == "SETUP") {
        response = handleSetup(request);
    } else if (method == "PLAY") {
        response = handlePlay(request, startAfterReply);
    } else if (method == "PAUSE") {
        response = handlePause(request);
    } else if (method == "TEARDOWN") {
        response = handleTeardown(request);
    } else if (method == "GET_PARAMETER" || method == "SET_PARAMETER") {
        response = handleKeepAlive(request);
    } else {
        response = errorResponse(405);
        response.setHeader("Allow", "OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN, GET_PARAMETER, SET_PARAMETER");
    }

    writer_->sendText(response.serialize(request.cseq()));
    // PLAY: media must only start after the client has received the reply.
    if (startAfterReply) startAfterReply->startStreaming();
}

RtspResponse RtspConnection::handleDescribe(const RtspRequest& request) {
    RtspTarget target;
    if (!parseRtspUrl(request.url, target)) return errorResponse(404);
    const std::optional<MediaItem> item = server_.library().find(target.kind, target.name);
    if (!item) return errorResponse(404);

    std::shared_ptr<TranscodeJob> job = server_.transcoder().ensureJob(*item);
    if (!job->waitUntilReady(kDescribeWait)) {
        LOG_WARN("rtsp") << "DESCRIBE " << item->id() << ": stream not ready ("
                         << (job->state() == JobState::Failed ? job->error() : "still transcoding") << ")";
        return errorResponse(job->state() == JobState::Failed ? 500 : 503);
    }

    const std::size_t rendition = initialRendition(*job, target);
    RenditionStreamInfo info;
    if (!job->streamInfo(rendition, server_.cache(), info)) return errorResponse(500);

    SdpDescription sdp;
    sdp.sessionName = item->name;
    sdp.serverIp = serverIp_;
    sdp.durationSeconds = item->info.durationSeconds;
    int nextTrackId = 0;
    if (info.hasVideo) {
        sdp.hasVideo = true;
        sdp.videoTrackId = nextTrackId++;
        sdp.profileLevelId = info.profileLevelId;
        sdp.sps = info.sps;
        sdp.pps = info.pps;
    }
    if (info.hasAudio) {
        sdp.hasAudio = true;
        sdp.audioTrackId = nextTrackId++;
        sdp.audioConfig = info.audioConfig;
    }

    RtspResponse response;
    response.setHeader("Content-Type", "application/sdp");
    // Relative track URLs ("trackID=0") are resolved against Content-Base.
    response.setHeader("Content-Base", str::endsWith(request.url, "/") ? request.url : request.url + "/");
    response.body = buildSdp(sdp);
    return response;
}

RtspResponse RtspConnection::handleSetup(const RtspRequest& request) {
    RtspTarget target;
    if (!parseRtspUrl(request.url, target)) return errorResponse(404);

    TransportSpec transport;
    if (!parseTransport(request.header("transport").value_or(""), transport)) return errorResponse(461);

    std::shared_ptr<RtspSession> session;
    const std::string sessionId = request.sessionId();
    if (!sessionId.empty()) {
        session = server_.findSession(sessionId);
        if (!session) return errorResponse(454);
    } else {
        // First SETUP: create the session.
        const std::optional<MediaItem> item = server_.library().find(target.kind, target.name);
        if (!item) return errorResponse(404);
        std::shared_ptr<TranscodeJob> job = server_.transcoder().ensureJob(*item);
        if (!job->waitUntilReady(kDescribeWait)) return errorResponse(503);
        const std::size_t rendition = initialRendition(*job, target);
        RenditionStreamInfo info;
        if (!job->streamInfo(rendition, server_.cache(), info)) return errorResponse(500);
        const bool adaptive = target.rendition < 0;
        session = server_.createSession(job, info, rendition, adaptive, writer_, clientIp_);
        if (!session) return errorResponse(453);  // too many sessions
        sessionIds_.push_back(session->id());
    }

    const int trackId = target.trackId >= 0 ? target.trackId : 0;
    std::string transportHeader;
    int errorStatus = 500;
    if (!session->setupTrack(trackId, transport, transportHeader, errorStatus)) return errorResponse(errorStatus);

    RtspResponse response;
    response.setHeader("Transport", transportHeader);
    response.setHeader("Session",
                       session->id() + ";timeout=" + std::to_string(server_.options().sessionTimeoutSeconds));
    return response;
}

RtspResponse RtspConnection::handlePlay(const RtspRequest& request, std::shared_ptr<RtspSession>& startAfterReply) {
    std::shared_ptr<RtspSession> session = server_.findSession(request.sessionId());
    if (!session) return errorResponse(454);

    // The aggregate URL (without "/trackID=N") is the base for RTP-Info.
    std::string baseUrl = request.url;
    const std::size_t track = baseUrl.find("/trackID=");
    if (track != std::string::npos) baseUrl.resize(track);

    std::string rtpInfo;
    std::string range;
    int errorStatus = 500;
    if (!session->preparePlay(parseNptStart(request.header("range").value_or("")), baseUrl, rtpInfo, range,
                              errorStatus)) {
        return errorResponse(errorStatus);
    }

    RtspResponse response;
    response.setHeader("Session", session->id());
    response.setHeader("Range", range);
    response.setHeader("RTP-Info", rtpInfo);
    startAfterReply = session;
    return response;
}

RtspResponse RtspConnection::handlePause(const RtspRequest& request) {
    std::shared_ptr<RtspSession> session = server_.findSession(request.sessionId());
    if (!session) return errorResponse(454);
    session->pause();
    RtspResponse response;
    response.setHeader("Session", session->id());
    return response;
}

RtspResponse RtspConnection::handleTeardown(const RtspRequest& request) {
    const std::string id = request.sessionId();
    if (!server_.findSession(id)) return errorResponse(454);
    server_.removeSession(id);
    for (auto it = sessionIds_.begin(); it != sessionIds_.end(); ++it) {
        if (*it == id) {
            sessionIds_.erase(it);
            break;
        }
    }
    return RtspResponse();
}

RtspResponse RtspConnection::handleKeepAlive(const RtspRequest& request) {
    // GET_PARAMETER / OPTIONS with a session id are used by players as "I am still here".
    if (auto session = server_.findSession(request.sessionId())) session->touch();
    return RtspResponse();
}

// =================================================================== RtspServer

RtspServer::RtspServer(RtspServerOptions options, MediaLibrary& library, TranscodeManager& transcoder,
                       SegmentCache& cache, ThreadPool& background)
    : options_(std::move(options)),
      library_(library),
      transcoder_(transcoder),
      cache_(cache),
      background_(background),
      ports_(options_.bindAddress, options_.rtpPortMin, options_.rtpPortMax) {}

RtspServer::~RtspServer() { stop(); }

bool RtspServer::start(std::string& error) {
    listener_ = createTcpListener(options_.bindAddress, options_.port, error);
    if (!listener_.valid()) return false;
    running_ = true;
    acceptThread_ = std::thread([this] { acceptLoop(); });
    janitorThread_ = std::thread([this] { janitorLoop(); });
    LOG_INFO("rtsp") << "RTSP server listening on " << options_.bindAddress << ":" << options_.port
                     << " (RTP/UDP ports " << options_.rtpPortMin << "-" << options_.rtpPortMax << ")";
    return true;
}

void RtspServer::stop() {
    if (!running_.exchange(false)) return;
    LOG_INFO("rtsp") << "Stopping RTSP server";
    if (acceptThread_.joinable()) acceptThread_.join();
    if (janitorThread_.joinable()) janitorThread_.join();
    listener_.close();

    std::vector<std::unique_ptr<RtspConnection>> connections;
    std::map<std::string, std::shared_ptr<RtspSession>> sessions;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        connections.swap(connections_);
    }
    for (auto& connection : connections) connection->stop();
    for (auto& connection : connections) connection->join();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions.swap(sessions_);
    }
    for (auto& entry : sessions) entry.second->close();
}

void RtspServer::acceptLoop() {
    while (running_) {
        pollfd waitFor{listener_.fd(), POLLIN, 0};
        if (poll(&waitFor, 1, 500) <= 0) continue;
        std::string ip;
        std::uint16_t port = 0;
        Socket socket = acceptClient(listener_, ip, port);
        if (!socket.valid()) continue;
        socket.setNoDelay();
        // Writes give up after 5 s; a client that stops reading for that long is treated as gone.
        socket.setTimeouts(0, 5000);

        auto connection = std::make_unique<RtspConnection>(std::move(socket), ip, *this);
        connection->start();
        std::lock_guard<std::mutex> lock(mutex_);
        connections_.push_back(std::move(connection));
        ++totalConnections_;
    }
}

void RtspServer::janitorLoop() {
    const std::chrono::seconds timeout(options_.sessionTimeoutSeconds);
    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        std::vector<std::shared_ptr<RtspSession>> expired;
        std::vector<std::unique_ptr<RtspConnection>> finished;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto now = std::chrono::steady_clock::now();
            for (auto it = sessions_.begin(); it != sessions_.end();) {
                if (it->second->expired(now, timeout)) {
                    expired.push_back(it->second);
                    it = sessions_.erase(it);
                } else {
                    ++it;
                }
            }
            for (auto it = connections_.begin(); it != connections_.end();) {
                if ((*it)->finished()) {
                    finished.push_back(std::move(*it));
                    it = connections_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        // Close and join outside the lock (these can take a moment).
        for (auto& session : expired) {
            if (!session->isClosed()) LOG_INFO("rtsp") << "Session " << session->id() << " timed out";
            session->close();
        }
        for (auto& connection : finished) connection->join();
    }
}

std::shared_ptr<RtspSession> RtspServer::createSession(std::shared_ptr<TranscodeJob> job,
                                                       const RenditionStreamInfo& info, std::size_t initialRendition,
                                                       bool adaptive, std::shared_ptr<RtspChannelWriter> writer,
                                                       const std::string& clientIp) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sessions_.size() >= options_.maxSessions) {
        LOG_WARN("rtsp") << "Session limit (" << options_.maxSessions << ") reached";
        return nullptr;
    }
    auto session = std::make_shared<RtspSession>(newSessionId(), std::move(job), info, initialRendition, adaptive,
                                                 SessionServices{cache_, background_, ports_}, std::move(writer),
                                                 clientIp);
    sessions_[session->id()] = session;
    ++totalSessions_;
    return session;
}

std::shared_ptr<RtspSession> RtspServer::findSession(const std::string& id) const {
    if (id.empty()) return nullptr;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = sessions_.find(id);
    return it == sessions_.end() ? nullptr : it->second;
}

void RtspServer::removeSession(const std::string& id) {
    std::shared_ptr<RtspSession> session;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = sessions_.find(id);
        if (it == sessions_.end()) return;
        session = it->second;
        sessions_.erase(it);
    }
    session->close();
    LOG_INFO("rtsp") << "Session " << id << " closed";
}

Json RtspServer::statsJson() const {
    std::vector<std::shared_ptr<RtspSession>> sessions;
    Json stats = Json::object();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : sessions_) sessions.push_back(entry.second);
        stats.set("port", options_.port)
            .set("activeSessions", sessions_.size())
            .set("maxSessions", options_.maxSessions)
            .set("openConnections", connections_.size())
            .set("totalSessions", totalSessions_)
            .set("totalConnections", totalConnections_);
    }
    Json list = Json::array();
    for (const auto& session : sessions) list.push(session->toJson());
    stats.set("sessions", list);
    return stats;
}

}  // namespace mss
