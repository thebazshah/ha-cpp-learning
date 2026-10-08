#include "rtsp/RtspSession.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <random>

#include "core/Logger.hpp"
#include "core/StringUtils.hpp"
#include "media/H264.hpp"
#include "rtsp/Rtcp.hpp"

namespace mss {
namespace {

using Clock = std::chrono::steady_clock;

// Frames are sent up to this much earlier than their real-time position.
// The client gets a small head start that absorbs network jitter, which
// helps startup and avoids stutter.
constexpr auto kSendAhead = std::chrono::milliseconds(500);
// If we fall this far behind schedule (for example the network stopped
// for a few seconds), we stop trying to catch up and restart the clock.
constexpr double kStallSeconds = 3.0;
constexpr auto kReportInterval = std::chrono::seconds(1);  // RTCP sender report period

long long nowMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

std::uint32_t randomNumber() {
    static thread_local std::mt19937 generator{std::random_device{}()};
    return generator();
}

const std::uint8_t kStartCode[4] = {0, 0, 0, 1};

// Makes sure a key frame carries SPS and PPS in front of it. After a quality
// switch the decoder needs the new parameter sets before the new key frame.
void ensureParameterSets(MediaFrame& frame, const std::vector<std::uint8_t>& sps,
                         const std::vector<std::uint8_t>& pps) {
    if (!frame.keyframe || sps.empty() || pps.empty()) return;
    for (const h264::NalUnit& nal : h264::splitAnnexB(frame.data.data(), frame.data.size())) {
        if (nal.type() == h264::kNalSps) return;  // already there
    }
    std::vector<std::uint8_t> data;
    data.reserve(frame.data.size() + sps.size() + pps.size() + 8);
    data.insert(data.end(), kStartCode, kStartCode + 4);
    data.insert(data.end(), sps.begin(), sps.end());
    data.insert(data.end(), kStartCode, kStartCode + 4);
    data.insert(data.end(), pps.begin(), pps.end());
    data.insert(data.end(), frame.data.begin(), frame.data.end());
    frame.data.swap(data);
}

}  // namespace

// =================================================================== RtspChannelWriter

RtspChannelWriter::RtspChannelWriter(Socket socket) : socket_(std::move(socket)), fd_(socket_.fd()) {}

bool RtspChannelWriter::sendText(const std::string& text) {
    std::lock_guard<std::mutex> lock(writeMutex_);
    if (closed_) return false;
    return sendAll(fd_, text.data(), text.size());
}

bool RtspChannelWriter::sendInterleaved(std::uint8_t channel, const std::uint8_t* data, std::size_t size) {
    if (size > 0xFFFF) return false;
    std::lock_guard<std::mutex> lock(writeMutex_);
    if (closed_) return false;
    frame_.clear();
    frame_.push_back('$');
    frame_.push_back(channel);
    frame_.push_back(static_cast<std::uint8_t>(size >> 8));
    frame_.push_back(static_cast<std::uint8_t>(size & 0xFF));
    frame_.insert(frame_.end(), data, data + size);
    return sendAll(fd_, frame_.data(), frame_.size());
}

void RtspChannelWriter::shutdown() {
    std::lock_guard<std::mutex> lock(fdMutex_);
    if (!closed_) ::shutdown(fd_, SHUT_RDWR);
}

void RtspChannelWriter::close() {
    std::lock_guard<std::mutex> writeLock(writeMutex_);
    std::lock_guard<std::mutex> fdLock(fdMutex_);
    if (closed_) return;
    closed_ = true;
    socket_.close();
}

// =================================================================== UdpPortAllocator

UdpPortAllocator::UdpPortAllocator(std::string bindAddress, std::uint16_t minPort, std::uint16_t maxPort)
    : bindAddress_(std::move(bindAddress)), minPort_(minPort), maxPort_(maxPort), next_(minPort) {}

bool UdpPortAllocator::allocate(Socket& rtp, Socket& rtcp, std::uint16_t& rtpPort) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int pairs = (maxPort_ - minPort_ + 1) / 2;
    for (int attempt = 0; attempt < pairs; ++attempt) {
        const std::uint16_t port = next_;
        next_ = static_cast<std::uint16_t>(next_ + 2);
        if (next_ + 1 > maxPort_ || next_ < minPort_) next_ = minPort_;

        // Both ports of the pair must be free; if one is busy, try the next pair.
        std::string error;
        Socket first = createUdpSocket(bindAddress_, port, error);
        if (!first.valid()) continue;
        Socket second = createUdpSocket(bindAddress_, static_cast<std::uint16_t>(port + 1), error);
        if (!second.valid()) continue;
        rtp = std::move(first);
        rtcp = std::move(second);
        rtpPort = port;
        return true;
    }
    return false;
}

// =================================================================== RtspSession

RtspSession::RtspSession(std::string id, std::shared_ptr<TranscodeJob> job, const RenditionStreamInfo& info,
                         std::size_t initialRendition, bool adaptive, SessionServices services,
                         std::shared_ptr<RtspChannelWriter> writer, std::string clientIp)
    : id_(std::move(id)),
      job_(std::move(job)),
      services_(services),
      writer_(std::move(writer)),
      clientIp_(std::move(clientIp)),
      createdAt_(Clock::now()),
      ptsOrigin_(info.firstPts),
      abr_(job_->renditions().size(), initialRendition, adaptive),
      currentRendition_(initialRendition),
      lastActivity_(nowMillis()) {
    // Track ids follow the SDP: video is track 0 (if present), audio comes next.
    int nextTrackId = 0;
    auto addTrack = [&](StreamKind kind, std::uint8_t payloadType, std::uint32_t clockRate) {
        auto track = std::make_unique<Track>();
        track->trackId = nextTrackId++;
        track->kind = kind;
        track->clockRate = clockRate;
        track->ssrc = randomNumber();
        track->rtpBase = randomNumber();  // RFC 3550 recommends a random start
        track->packetizer = std::make_unique<RtpPacketizer>(payloadType, track->ssrc,
                                                            static_cast<std::uint16_t>(randomNumber()));
        tracks_.push_back(std::move(track));
    };
    if (info.hasVideo) addTrack(StreamKind::Video, 96, 90000);
    if (info.hasAudio) {
        addTrack(StreamKind::Audio, 97,
                 static_cast<std::uint32_t>(info.audioConfig.sampleRate > 0 ? info.audioConfig.sampleRate : 48000));
    }
}

RtspSession::~RtspSession() { close(); }

RtspSession::Track* RtspSession::trackFor(StreamKind kind) {
    for (auto& track : tracks_) {
        if (track->kind == kind) return track.get();
    }
    return nullptr;
}

RtspSession::Track* RtspSession::trackById(int trackId) {
    for (auto& track : tracks_) {
        if (track->trackId == trackId) return track.get();
    }
    return nullptr;
}

std::uint32_t RtspSession::rtpTimestamp(const Track& track, std::int64_t pts) const {
    // Convert from the 90 kHz media clock to the track's RTP clock. Unsigned
    // overflow is fine here: RTP timestamps are meant to wrap around.
    const std::int64_t ticks = ((pts - ptsOrigin_) * static_cast<std::int64_t>(track.clockRate)) / 90000;
    return track.rtpBase + static_cast<std::uint32_t>(ticks);
}

void RtspSession::touch() { lastActivity_ = nowMillis(); }

bool RtspSession::expired(Clock::time_point now, std::chrono::seconds timeout) const {
    if (closed_) return true;
    if (usesTcp()) return writer_->isClosed();  // TCP sessions live as long as their connection
    const long long nowMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    return nowMs - lastActivity_.load() > std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count();
}

bool RtspSession::usesTcp() const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& track : tracks_) {
        if (track->configured && track->tcp) return true;
    }
    return false;
}

bool RtspSession::setupTrack(int trackId, const TransportSpec& request, std::string& transportHeader,
                             int& errorStatus) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == State::Closed) {
        errorStatus = 454;
        return false;
    }
    Track* track = trackById(trackId);
    if (track == nullptr) {
        errorStatus = 404;
        return false;
    }
    if (sender_.joinable()) {
        // Transports are fixed once streaming has started (the sender thread uses them).
        errorStatus = 455;
        return false;
    }

    char ssrcText[16];
    std::snprintf(ssrcText, sizeof(ssrcText), "%08X", track->ssrc);

    if (request.tcp) {
        track->tcp = true;
        track->rtpChannel = static_cast<std::uint8_t>(request.rtpChannel);
        track->rtcpChannel = static_cast<std::uint8_t>(request.rtcpChannel);
        transportHeader = "RTP/AVP/TCP;unicast;interleaved=" + std::to_string(request.rtpChannel) + "-" +
                          std::to_string(request.rtcpChannel) + ";ssrc=" + ssrcText;
    } else {
        Socket rtp;
        Socket rtcp;
        std::uint16_t port = 0;
        if (!services_.ports.allocate(rtp, rtcp, port)) {
            errorStatus = 453;  // no free UDP ports
            return false;
        }
        rtcp.setNonBlocking(true);  // we only poll it for receiver reports
        track->tcp = false;
        track->rtpSocket = std::move(rtp);
        track->rtcpSocket = std::move(rtcp);
        track->serverRtpPort = port;
        makeAddress(clientIp_, static_cast<std::uint16_t>(request.clientRtpPort), track->clientRtp);
        makeAddress(clientIp_, static_cast<std::uint16_t>(request.clientRtcpPort), track->clientRtcp);
        transportHeader = "RTP/AVP;unicast;client_port=" + std::to_string(request.clientRtpPort) + "-" +
                          std::to_string(request.clientRtcpPort) + ";server_port=" + std::to_string(port) + "-" +
                          std::to_string(port + 1) + ";ssrc=" + ssrcText;
    }
    track->configured = true;
    touch();
    return true;
}

bool RtspSession::preparePlay(std::optional<double> startNpt, const std::string& baseUrl, std::string& rtpInfo,
                              std::string& range, int& errorStatus) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == State::Closed) {
        errorStatus = 454;
        return false;
    }
    bool anyTrack = false;
    for (const auto& track : tracks_) anyTrack = anyTrack || track->configured;
    if (!anyTrack) {
        errorStatus = 455;  // PLAY before SETUP
        return false;
    }

    HlsMediaPlaylist playlist;
    job_->readPlaylist(currentRendition_, playlist);

    // A new position is needed for the first PLAY, after the end, or when the
    // client asks for a position. Otherwise PLAY simply resumes after PAUSE.
    const bool reposition = startNpt.has_value() || state_ == State::Ready || state_ == State::Finished;
    double startSeconds = 0.0;
    std::int64_t positionPts = ptsOrigin_;
    if (reposition) {
        const double requested = startNpt.value_or(0.0);
        const std::size_t index = playlist.segmentIndexAt(requested);
        // We always start at the beginning of a segment, because segments
        // start with a key frame (a decoder cannot start anywhere else).
        startSeconds = playlist.segments.empty() ? 0.0 : playlist.segments[index].startTime;
        positionPts = ptsOrigin_ + static_cast<std::int64_t>(startSeconds * 90000.0);
        queue_.clear();
        nextSegment_ = index;
        firstSegmentOfPlay_ = index;
        ++generation_;
    } else {
        positionPts = queue_.empty() ? lastSentPts_ : queue_.front().pts;
        startSeconds = std::max(0.0, static_cast<double>(positionPts - ptsOrigin_) / 90000.0);
    }
    needAnchor_ = true;

    std::string base = baseUrl;
    if (!base.empty() && base.back() != '/') base += '/';
    rtpInfo.clear();
    for (const auto& track : tracks_) {
        if (!track->configured) continue;
        if (!rtpInfo.empty()) rtpInfo += ",";
        rtpInfo += "url=" + base + "trackID=" + std::to_string(track->trackId) +
                   ";seq=" + std::to_string(track->packetizer->nextSequence()) +
                   ";rtptime=" + std::to_string(rtpTimestamp(*track, positionPts));
    }
    const double duration = job_->media().info.durationSeconds;
    range = "npt=" + str::formatDouble(startSeconds, 3) + "-" + (duration > 0 ? str::formatDouble(duration, 3) : "");
    touch();
    return true;
}

void RtspSession::startStreaming() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == State::Closed) return;
        state_ = State::Playing;
        if (!sender_.joinable()) sender_ = std::thread([this] { senderLoop(); });
    }
    wake_.notify_all();
    LOG_INFO("rtsp") << "Session " << id_ << " playing " << job_->media().id() << " to " << clientIp_
                     << (usesTcp() ? " (RTP over TCP)" : " (RTP over UDP)");
}

void RtspSession::pause() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == State::Playing) state_ = State::Paused;
    }
    wake_.notify_all();
    touch();
}

void RtspSession::close() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return;
        closed_ = true;
        stopRequested_ = true;
        state_ = State::Closed;
    }
    wake_.notify_all();
    if (sender_.joinable() && sender_.get_id() != std::this_thread::get_id()) sender_.join();

    // Tell UDP clients the stream is over, then free the ports.
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& track : tracks_) {
        if (track->configured && !track->tcp && track->rtcpSocket.valid()) {
            const std::vector<std::uint8_t> bye = goodbyePacket(*track, lastSentPts_);
            sendto(track->rtcpSocket.fd(), bye.data(), bye.size(), 0,
                   reinterpret_cast<const sockaddr*>(&track->clientRtcp), sizeof(track->clientRtcp));
        }
        track->rtpSocket.close();
        track->rtcpSocket.close();
    }
}

void RtspSession::onInterleavedPacket(std::uint8_t channel, const std::uint8_t* data, std::size_t size) {
    bool isRtcp = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& track : tracks_) {
            if (track->configured && track->tcp && track->rtcpChannel == channel) isRtcp = true;
        }
    }
    if (isRtcp) handleRtcp(data, size);
}

void RtspSession::handleRtcp(const std::uint8_t* data, std::size_t size) {
    touch();  // any RTCP from the client proves it is still alive
    for (const rtcp::ReportBlock& block : rtcp::parseReportBlocks(data, size)) {
        abr_.reportLoss(block.fractionLost);
    }
}

// ------------------------------------------------------------------ sending

bool RtspSession::transmit(Track& track, bool rtcpPacket, const std::uint8_t* data, std::size_t size) {
    bool ok;
    if (track.tcp) {
        ok = writer_->sendInterleaved(rtcpPacket ? track.rtcpChannel : track.rtpChannel, data, size);
    } else {
        const Socket& socket = rtcpPacket ? track.rtcpSocket : track.rtpSocket;
        const sockaddr_in& target = rtcpPacket ? track.clientRtcp : track.clientRtp;
        const ssize_t sent =
            sendto(socket.fd(), data, size, 0, reinterpret_cast<const sockaddr*>(&target), sizeof(target));
        // UDP has no delivery guarantee anyway: a failed send (e.g. full
        // buffer) is counted, but does not end the session.
        if (sent < 0) ++sendErrors_;
        ok = true;
    }
    if (ok) bytesSent_ += size;
    return ok;
}

bool RtspSession::sendFrame(const MediaFrame& frame) {
    Track* track = trackFor(frame.kind);
    if (track == nullptr || !track->configured) return true;  // the client did not SETUP this track

    const std::uint32_t timestamp = rtpTimestamp(*track, frame.pts);
    bool ok = true;
    const RtpPacketizer::PacketSink sink = [&](const std::uint8_t* packet, std::size_t size) {
        if (!ok) return;
        ok = transmit(*track, false, packet, size);
        if (ok) {
            ++track->packets;
            track->octets += size - 12;  // RTCP counts payload bytes only (without the RTP header)
        }
    };
    if (frame.kind == StreamKind::Video) {
        track->packetizer->packetizeH264(frame.data.data(), frame.data.size(), timestamp, sink);
    } else {
        track->packetizer->packetizeAac(frame.data.data(), frame.data.size(), timestamp, sink);
    }
    return ok;
}

void RtspSession::serviceRtcp(std::optional<std::int64_t> mediaPtsNow) {
    // 1. Read receiver reports that arrived on our UDP RTCP ports.
    for (auto& track : tracks_) {
        if (!track->configured || track->tcp || !track->rtcpSocket.valid()) continue;
        std::uint8_t buffer[2048];
        for (;;) {
            const ssize_t got = recv(track->rtcpSocket.fd(), buffer, sizeof(buffer), 0);
            if (got <= 0) break;  // nothing more (the socket is non-blocking)
            handleRtcp(buffer, static_cast<std::size_t>(got));
        }
    }

    // 2. Send a sender report about once per second per track.
    if (!mediaPtsNow) return;
    const auto now = Clock::now();
    for (auto& track : tracks_) {
        if (!track->configured || track->packets.load() == 0) continue;
        if (now - track->lastReport < kReportInterval) continue;
        track->lastReport = now;
        const std::vector<std::uint8_t> report = rtcp::buildSenderReport(
            track->ssrc, rtcp::ntpNow(), rtpTimestamp(*track, *mediaPtsNow),
            static_cast<std::uint32_t>(track->packets.load()), static_cast<std::uint32_t>(track->octets.load()),
            "mss@" + clientIp_);
        transmit(*track, true, report.data(), report.size());
    }
}

std::vector<std::uint8_t> RtspSession::goodbyePacket(const Track& track, std::int64_t pts) const {
    // Compound RTCP packet: sender report (+ SDES) followed by BYE.
    std::vector<std::uint8_t> packet = rtcp::buildSenderReport(
        track.ssrc, rtcp::ntpNow(), rtpTimestamp(track, pts), static_cast<std::uint32_t>(track.packets.load()),
        static_cast<std::uint32_t>(track.octets.load()), "mss@" + clientIp_);
    const std::vector<std::uint8_t> bye = rtcp::buildBye(track.ssrc);
    packet.insert(packet.end(), bye.begin(), bye.end());
    return packet;
}

void RtspSession::sendBye(std::int64_t pts) {
    for (auto& track : tracks_) {
        if (!track->configured) continue;
        const std::vector<std::uint8_t> packet = goodbyePacket(*track, pts);
        transmit(*track, true, packet.data(), packet.size());
    }
}

RtspSession::SegmentLoad RtspSession::loadSegment(std::size_t index, std::size_t rendition) {
    SegmentLoad result;
    HlsMediaPlaylist playlist;
    if (!job_->readPlaylist(rendition, playlist)) {
        result.status = job_->state() == JobState::Failed ? LoadStatus::End : LoadStatus::NotYet;
        return result;
    }
    if (index >= playlist.segments.size()) {
        // Either the media is finished, or ffmpeg has not produced this segment yet.
        if (playlist.endList || job_->state() == JobState::Failed) {
            result.status = LoadStatus::End;
        } else {
            result.status = LoadStatus::NotYet;
        }
        return result;
    }

    const std::string dir = job_->renditionDir(rendition);
    const SegmentCache::Data data = services_.cache.get(dir + "/" + playlist.segments[index].uri);
    if (!data) {
        result.status = LoadStatus::Error;
        return result;
    }
    // Warm the cache with the following segment while this one is playing.
    if (index + 1 < playlist.segments.size()) {
        services_.cache.prefetch(services_.background, dir + "/" + playlist.segments[index + 1].uri);
    }

    DemuxResult demuxed;
    std::string error;
    if (!demuxTransportStream(reinterpret_cast<const std::uint8_t*>(data->data()), data->size(), demuxed, error)) {
        LOG_WARN("rtsp") << "Session " << id_ << ": cannot demux segment " << index << ": " << error;
        result.status = LoadStatus::Error;
        return result;
    }
    for (MediaFrame& frame : demuxed.frames) {
        if (frame.kind == StreamKind::Video) ensureParameterSets(frame, demuxed.sps, demuxed.pps);
    }
    result.frames = std::move(demuxed.frames);
    result.status = LoadStatus::Loaded;
    return result;
}

void RtspSession::senderLoop() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopRequested_) {
        if (state_ != State::Playing) {
            wake_.wait(lock);
            continue;
        }

        // ---- 1. Need more frames? Load the next segment.
        if (queue_.empty()) {
            const std::uint64_t generation = generation_;
            const std::size_t index = nextSegment_;
            // Quality decisions happen at segment boundaries, but not for the
            // very first segment after PLAY (we keep the current level there).
            const std::size_t rendition = index > firstSegmentOfPlay_ ? abr_.chooseNext() : currentRendition_;

            lock.unlock();
            SegmentLoad load = loadSegment(index, rendition);
            lock.lock();
            if (stopRequested_ || generation != generation_) continue;  // a seek happened meanwhile

            switch (load.status) {
                case LoadStatus::Loaded:
                    if (rendition != currentRendition_) {
                        ++renditionSwitches_;
                        LOG_INFO("rtsp") << "Session " << id_ << ": switching quality "
                                         << job_->renditions()[currentRendition_].label() << " -> "
                                         << job_->renditions()[rendition].label();
                        currentRendition_ = rendition;
                    }
                    for (MediaFrame& frame : load.frames) queue_.push_back(std::move(frame));
                    ++nextSegment_;
                    break;
                case LoadStatus::NotYet:
                    // The transcoder has not written this segment yet: wait a bit.
                    wake_.wait_for(lock, std::chrono::milliseconds(250));
                    break;
                case LoadStatus::Error:
                    ++nextSegment_;  // skip a broken segment instead of stopping
                    break;
                case LoadStatus::End: {
                    LOG_INFO("rtsp") << "Session " << id_ << ": end of " << job_->media().id();
                    state_ = State::Finished;
                    const std::int64_t lastPts = lastSentPts_;
                    lock.unlock();
                    sendBye(lastPts);  // tells the player that the stream is complete
                    lock.lock();
                    break;
                }
            }
            continue;
        }

        // ---- 2. Wait until the next frame is due.
        const auto now = Clock::now();
        const MediaFrame& next = queue_.front();
        if (needAnchor_) {
            anchorPts_ = next.dts;
            anchorWall_ = now;
            needAnchor_ = false;
        }
        const auto offset = std::chrono::microseconds((next.dts - anchorPts_) * 1000000 / 90000);
        const auto due = anchorWall_ + offset - kSendAhead;
        const std::int64_t mediaNow =
            anchorPts_ + std::chrono::duration_cast<std::chrono::microseconds>(now - anchorWall_).count() * 9 / 100;

        if (now < due) {
            lock.unlock();
            serviceRtcp(mediaNow);
            lock.lock();
            // Sleep until the frame is due, but wake at least every 50 ms
            // to read RTCP. PAUSE/TEARDOWN/seek wake us up immediately.
            wake_.wait_until(lock, std::min(due, now + std::chrono::milliseconds(50)));
            continue;
        }

        // ---- 3. Send it.
        double behind = std::chrono::duration<double>(now - due).count();
        if (behind > kStallSeconds) {
            // We are far behind (the network stalled). Do not burst everything
            // out at once - restart the pacing clock from this frame instead.
            ++stalls_;
            abr_.reportStall();
            LOG_WARN("rtsp") << "Session " << id_ << ": network stalled (" << str::formatDouble(behind, 1)
                             << " s behind), re-buffering";
            anchorPts_ = next.dts;
            anchorWall_ = now;
            behind = 0;
        }
        MediaFrame frame = std::move(queue_.front());
        queue_.pop_front();
        lastLateness_ = behind;
        lock.unlock();

        abr_.reportLateness(behind);
        const bool sent = sendFrame(frame);
        serviceRtcp(mediaNow);

        lock.lock();
        lastSentPts_ = frame.pts;
        if (!sent) {
            LOG_INFO("rtsp") << "Session " << id_ << ": client connection lost";
            state_ = State::Closed;
            stopRequested_ = true;
        }
    }
}

Json RtspSession::toJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const char* stateName = "ready";
    switch (state_) {
        case State::Ready: stateName = "ready"; break;
        case State::Playing: stateName = "playing"; break;
        case State::Paused: stateName = "paused"; break;
        case State::Finished: stateName = "finished"; break;
        case State::Closed: stateName = "closed"; break;
    }
    bool tcp = false;
    std::uint64_t packets = 0;
    for (const auto& track : tracks_) {
        if (track->configured && track->tcp) tcp = true;
        packets += track->packets.load();
    }
    const double age = std::chrono::duration<double>(Clock::now() - createdAt_).count();
    const double position = std::max(0.0, static_cast<double>(lastSentPts_ - ptsOrigin_) / 90000.0);
    const Rendition& rendition = job_->renditions()[currentRendition_];

    Json json = Json::object();
    json.set("id", id_)
        .set("media", job_->media().id())
        .set("client", clientIp_)
        .set("state", stateName)
        .set("transport", tcp ? "RTP/TCP" : "RTP/UDP")
        .set("rendition", rendition.label())
        .set("renditionIndex", currentRendition_)
        .set("targetKbps", rendition.videoKbps + rendition.audioKbps)
        .set("positionSeconds", position)
        .set("ageSeconds", age)
        .set("bytesSent", bytesSent_.load())
        .set("averageKbps", age > 0 ? static_cast<double>(bytesSent_.load()) * 8.0 / 1000.0 / age : 0.0)
        .set("packetsSent", packets)
        .set("latenessSeconds", lastLateness_)
        .set("stalls", stalls_.load())
        .set("switches", renditionSwitches_.load())
        .set("sendErrors", sendErrors_.load())
        .set("abr", abr_.toJson());
    return json;
}

}  // namespace mss
