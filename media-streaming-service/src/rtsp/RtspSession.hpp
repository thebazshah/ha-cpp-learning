#pragma once

#include <netinet/in.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "cache/SegmentCache.hpp"
#include "core/Json.hpp"
#include "core/ThreadPool.hpp"
#include "media/TranscodeManager.hpp"
#include "media/TsDemuxer.hpp"
#include "net/Socket.hpp"
#include "rtsp/AbrController.hpp"
#include "rtsp/RtpPacketizer.hpp"
#include "rtsp/RtspMessage.hpp"

namespace mss {

// The TCP connection of one RTSP client, shared by the connection (which
// sends RTSP responses) and its sessions (which send RTP packets
// "interleaved" into the same connection when the client asked for TCP).
// A mutex makes sure two writers never mix their bytes.
class RtspChannelWriter {
public:
    explicit RtspChannelWriter(Socket socket);

    int fd() const { return fd_; }
    bool sendText(const std::string& text);
    // Sends one RTP/RTCP packet framed as: '$', channel, 16-bit length, packet.
    bool sendInterleaved(std::uint8_t channel, const std::uint8_t* data, std::size_t size);

    void shutdown();  // wakes up anything blocked on the socket (it stays open)
    void close();     // closes the socket for good
    bool isClosed() const { return closed_.load(); }

private:
    std::mutex writeMutex_;  // held while writing
    std::mutex fdMutex_;     // held while shutting down / closing the descriptor
    Socket socket_;
    int fd_;
    std::atomic<bool> closed_{false};
    std::vector<std::uint8_t> frame_;  // reused buffer for interleaved packets
};

// Hands out pairs of UDP ports (even port for RTP, next odd port for RTCP).
class UdpPortAllocator {
public:
    UdpPortAllocator(std::string bindAddress, std::uint16_t minPort, std::uint16_t maxPort);
    bool allocate(Socket& rtp, Socket& rtcp, std::uint16_t& rtpPort);

private:
    std::mutex mutex_;
    std::string bindAddress_;
    std::uint16_t minPort_;
    std::uint16_t maxPort_;
    std::uint16_t next_;
};

// Things every session needs from the server.
struct SessionServices {
    SegmentCache& cache;
    ThreadPool& background;
    UdpPortAllocator& ports;
};

// One RTSP playback session: streams one media file to one client over RTP.
//
// A dedicated sender thread runs the streaming loop:
//   1. load the next HLS segment (through the cache) and demux it into frames;
//   2. wait until each frame's scheduled send time (real-time pacing);
//   3. cut the frame into RTP packets and send them (UDP or TCP);
//   4. send RTCP sender reports and read RTCP receiver reports;
//   5. at each segment boundary, let the ABR controller pick the quality.
//
// The RTSP connection thread calls setupTrack/preparePlay/startStreaming/
// pause/close; the sender thread does the streaming.
class RtspSession {
public:
    RtspSession(std::string id, std::shared_ptr<TranscodeJob> job, const RenditionStreamInfo& info,
                std::size_t initialRendition, bool adaptive, SessionServices services,
                std::shared_ptr<RtspChannelWriter> writer, std::string clientIp);
    ~RtspSession();

    RtspSession(const RtspSession&) = delete;
    RtspSession& operator=(const RtspSession&) = delete;

    const std::string& id() const { return id_; }

    // SETUP: prepares delivery of one track. On success fills the Transport
    // header for the response; on failure fills an RTSP error status.
    bool setupTrack(int trackId, const TransportSpec& request, std::string& transportHeader, int& errorStatus);

    // PLAY, part 1: chooses the start position. Fills the RTP-Info and Range
    // response headers. `startNpt` is the requested start in seconds (if any).
    bool preparePlay(std::optional<double> startNpt, const std::string& baseUrl, std::string& rtpInfo,
                     std::string& range, int& errorStatus);
    // PLAY, part 2: starts (or resumes) sending - called after the PLAY response was sent.
    void startStreaming();

    void pause();
    void close();  // stops the sender thread and closes the UDP sockets (safe to call twice)

    // RTCP packets that arrived inside the TCP connection.
    void onInterleavedPacket(std::uint8_t channel, const std::uint8_t* data, std::size_t size);

    void touch();  // records "the client is still alive"
    bool expired(std::chrono::steady_clock::time_point now, std::chrono::seconds timeout) const;
    bool usesTcp() const;
    bool isClosed() const { return closed_.load(); }

    Json toJson() const;

private:
    enum class State { Ready, Playing, Paused, Finished, Closed };

    struct Track {
        int trackId = -1;
        StreamKind kind = StreamKind::Video;
        std::uint32_t clockRate = 90000;  // RTP timestamp ticks per second
        bool configured = false;          // SETUP done
        bool tcp = false;
        std::uint8_t rtpChannel = 0;
        std::uint8_t rtcpChannel = 1;
        Socket rtpSocket;                 // UDP only
        Socket rtcpSocket;
        sockaddr_in clientRtp{};
        sockaddr_in clientRtcp{};
        std::uint16_t serverRtpPort = 0;
        std::uint32_t ssrc = 0;
        std::uint32_t rtpBase = 0;        // random RTP timestamp at media time 0
        std::unique_ptr<RtpPacketizer> packetizer;
        std::atomic<std::uint64_t> packets{0};
        std::atomic<std::uint64_t> octets{0};
        std::chrono::steady_clock::time_point lastReport;
    };

    enum class LoadStatus { Loaded, NotYet, End, Error };
    struct SegmentLoad {
        LoadStatus status = LoadStatus::Error;
        std::vector<MediaFrame> frames;
    };

    void senderLoop();
    SegmentLoad loadSegment(std::size_t index, std::size_t rendition);
    bool sendFrame(const MediaFrame& frame);
    void serviceRtcp(std::optional<std::int64_t> mediaPtsNow);
    void handleRtcp(const std::uint8_t* data, std::size_t size);
    bool transmit(Track& track, bool rtcp, const std::uint8_t* data, std::size_t size);
    std::vector<std::uint8_t> goodbyePacket(const Track& track, std::int64_t pts) const;
    void sendBye(std::int64_t pts);
    Track* trackFor(StreamKind kind);
    Track* trackById(int trackId);
    std::uint32_t rtpTimestamp(const Track& track, std::int64_t pts) const;

    const std::string id_;
    std::shared_ptr<TranscodeJob> job_;
    SessionServices services_;
    std::shared_ptr<RtspChannelWriter> writer_;
    const std::string clientIp_;
    const std::chrono::steady_clock::time_point createdAt_;
    const std::int64_t ptsOrigin_;  // 90 kHz timestamp that corresponds to media time 0
    std::vector<std::unique_ptr<Track>> tracks_;
    AbrController abr_;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::thread sender_;
    State state_ = State::Ready;
    bool stopRequested_ = false;
    std::atomic<bool> closed_{false};
    std::deque<MediaFrame> queue_;     // demuxed frames waiting to be sent
    std::size_t nextSegment_ = 0;      // index of the next segment to load
    std::size_t firstSegmentOfPlay_ = 0;
    std::size_t currentRendition_;
    std::uint64_t generation_ = 0;     // +1 on every seek; stale segment loads are dropped
    bool needAnchor_ = true;           // the next frame defines the pacing clock
    std::int64_t anchorPts_ = 0;       // (anchorPts_, anchorWall_) = "this media time is sent at this moment"
    std::chrono::steady_clock::time_point anchorWall_;
    std::int64_t lastSentPts_ = 0;
    double lastLateness_ = 0;

    std::atomic<std::uint64_t> stalls_{0};
    std::atomic<std::uint64_t> renditionSwitches_{0};
    std::atomic<std::uint64_t> bytesSent_{0};
    std::atomic<std::uint64_t> sendErrors_{0};
    std::atomic<long long> lastActivity_;  // steady clock in milliseconds
};

}  // namespace mss
