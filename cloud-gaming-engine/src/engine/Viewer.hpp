#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ws/WebSocket.hpp"

namespace cge {

class GameSession;

// One person (browser tab) or bot connected to a session over a WebSocket.
//
// Every viewer has two threads:
//   * reader: waits for messages from the client (join, input, ping) and
//     passes them to the session;
//   * writer: sends queued messages (video frames, state, pong) to the client.
//
// Latency handling: the frame queue is tiny (kMaxQueuedFrames). If the
// client's network cannot keep up, new frames are DROPPED instead of piling
// up (old frames are worthless in a live game - "latest frame wins"). The
// viewer is then marked "needs keyframe", and as soon as the queue has room
// again it receives one complete picture and is back in sync.
class Viewer {
public:
    static constexpr std::size_t kMaxQueuedFrames = 2;

    Viewer(std::uint64_t id, std::unique_ptr<ws::Connection> connection, std::string remoteIp);
    ~Viewer();

    Viewer(const Viewer&) = delete;
    Viewer& operator=(const Viewer&) = delete;

    // Starts the reader and writer threads.
    void start(const std::shared_ptr<GameSession>& session, const std::shared_ptr<Viewer>& self);
    // Closes the connection; both threads finish soon after.
    void close();
    // Waits for both threads. Never call this from the viewer's own threads.
    void join();

    // ---- outgoing (called by the session) ----
    // Returns false if the queue was full and the frame was dropped.
    bool queueFrame(std::shared_ptr<const std::vector<std::uint8_t>> frame);
    void queueText(std::string text);
    void queueBinary(std::vector<std::uint8_t> data);

    std::uint64_t id() const { return id_; }
    const std::string& remoteIp() const { return remoteIp_; }
    std::uint64_t bytesSent() const { return connection_->bytesSent(); }
    std::size_t queuedFrames() const;

    // ---- set by the session (protected by the session's mutex) ----
    bool joined = false;
    std::string name;
    int player = -1;  // player slot, or -1 for a spectator

    // ---- statistics / flags (atomic: read and written by several threads) ----
    std::atomic<bool> needsKeyframe{true};
    std::atomic<std::uint64_t> framesSent{0};
    std::atomic<std::uint64_t> keyframesSent{0};
    std::atomic<std::uint64_t> framesDropped{0};
    std::atomic<std::uint64_t> inputs{0};
    std::atomic<float> roundTripMs{0};  // reported by the client in its pings

private:
    struct Outgoing {
        enum Kind { Frame, Text, Binary } kind = Text;
        std::shared_ptr<const std::vector<std::uint8_t>> frame;
        std::string text;
        std::vector<std::uint8_t> binary;
    };

    void readerLoop(std::weak_ptr<GameSession> session, std::weak_ptr<Viewer> self);
    void writerLoop();
    void push(Outgoing item);

    const std::uint64_t id_;
    std::unique_ptr<ws::Connection> connection_;
    const std::string remoteIp_;

    mutable std::mutex queueMutex_;
    std::condition_variable queueChanged_;
    std::deque<Outgoing> queue_;
    std::size_t queuedFrames_ = 0;
    bool closing_ = false;

    std::thread reader_;
    std::thread writer_;
};

}  // namespace cge
