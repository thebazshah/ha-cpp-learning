#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cache/SegmentCache.hpp"
#include "core/Json.hpp"
#include "core/ThreadPool.hpp"
#include "media/MediaLibrary.hpp"
#include "media/TranscodeManager.hpp"
#include "net/Socket.hpp"
#include "rtsp/RtspMessage.hpp"
#include "rtsp/RtspSession.hpp"

namespace mss {

struct RtspServerOptions {
    std::string bindAddress = "0.0.0.0";
    std::uint16_t port = 8554;
    std::uint16_t rtpPortMin = 50000;
    std::uint16_t rtpPortMax = 50999;
    std::size_t maxSessions = 64;
    int sessionTimeoutSeconds = 60;
};

class RtspServer;

// One RTSP client connection. A dedicated thread reads requests (and RTCP
// packets interleaved by TCP clients) and answers them.
class RtspConnection {
public:
    RtspConnection(Socket socket, std::string clientIp, RtspServer& server);
    ~RtspConnection();

    void start();
    void stop();  // makes the reader thread finish soon
    void join();
    bool finished() const { return finished_.load(); }

private:
    void run();
    bool processBuffer();  // returns false when the connection must be closed
    void handleRequest(const RtspRequest& request);

    RtspResponse handleDescribe(const RtspRequest& request);
    RtspResponse handleSetup(const RtspRequest& request);
    RtspResponse handlePlay(const RtspRequest& request, std::shared_ptr<RtspSession>& startAfterReply);
    RtspResponse handlePause(const RtspRequest& request);
    RtspResponse handleTeardown(const RtspRequest& request);
    RtspResponse handleKeepAlive(const RtspRequest& request);

    RtspServer& server_;
    std::shared_ptr<RtspChannelWriter> writer_;
    const std::string clientIp_;
    const std::string serverIp_;
    std::thread thread_;
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> finished_{false};
    std::string buffer_;
    std::vector<std::string> sessionIds_;  // sessions created on this connection
};

// The RTSP server: accepts connections on port 8554 and keeps track of all
// playback sessions. A "janitor" thread closes sessions whose client has
// disappeared and cleans up finished connections.
class RtspServer {
public:
    RtspServer(RtspServerOptions options, MediaLibrary& library, TranscodeManager& transcoder, SegmentCache& cache,
               ThreadPool& background);
    ~RtspServer();

    bool start(std::string& error);
    void stop();
    Json statsJson() const;

    // ---- used by RtspConnection ----
    MediaLibrary& library() { return library_; }
    TranscodeManager& transcoder() { return transcoder_; }
    SegmentCache& cache() { return cache_; }
    const RtspServerOptions& options() const { return options_; }

    // Creates and registers a session. Returns nullptr when the session limit is reached.
    std::shared_ptr<RtspSession> createSession(std::shared_ptr<TranscodeJob> job, const RenditionStreamInfo& info,
                                               std::size_t initialRendition, bool adaptive,
                                               std::shared_ptr<RtspChannelWriter> writer,
                                               const std::string& clientIp);
    std::shared_ptr<RtspSession> findSession(const std::string& id) const;
    void removeSession(const std::string& id);  // closes it

private:
    void acceptLoop();
    void janitorLoop();

    RtspServerOptions options_;
    MediaLibrary& library_;
    TranscodeManager& transcoder_;
    SegmentCache& cache_;
    ThreadPool& background_;
    UdpPortAllocator ports_;

    Socket listener_;
    std::atomic<bool> running_{false};
    std::thread acceptThread_;
    std::thread janitorThread_;

    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<RtspSession>> sessions_;
    std::vector<std::unique_ptr<RtspConnection>> connections_;
    std::uint64_t totalSessions_ = 0;
    std::uint64_t totalConnections_ = 0;
};

}  // namespace mss
