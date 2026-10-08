#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <cge/GameApi.hpp>

#include "codec/FrameCodec.hpp"
#include "core/Json.hpp"
#include "engine/GameLibrary.hpp"
#include "engine/Viewer.hpp"
#include "net/Socket.hpp"
#include "protocol/Protocol.hpp"
#include "ws/WebSocket.hpp"

namespace cge {

struct SessionOptions {
    int reconnectGraceSeconds = 60;  // a disconnected player keeps their slot this long
    std::size_t maxViewers = 32;     // players + spectators
};

// One running game with its players and spectators.
//
// The game loop runs on its own thread at the game's tick rate:
//
//   +---------+   +--------+   +--------+   +-----------------+   +--------+   +-----------+
//   | inputs  |-->| update |-->| render |-->| capture+encode  |-->| frames |-->| viewers'  |
//   | (queue) |   | (game) |   | (game) |   | (tile delta)    |   | (once) |   | queues    |
//   +---------+   +--------+   +--------+   +-----------------+   +--------+   +-----------+
//
// Each frame is encoded ONCE and the same bytes go to every viewer.
// Only the game loop thread ever calls into the game, so games do not need locks.
class GameSession : public std::enable_shared_from_this<GameSession>, public GameHost {
public:
    using Clock = std::chrono::steady_clock;

    GameSession(std::string id, std::string gameId, std::shared_ptr<GameLibrary> library, SessionOptions options);
    ~GameSession() override;

    void start();  // starts the game loop thread
    void stop();   // stops the loop, disconnects everybody, waits for all threads

    // Takes over a TCP connection that was just upgraded to WebSocket.
    void attachViewer(Socket socket, std::string pendingBytes, const std::string& remoteIp);

    // ---- called by viewer threads ----
    void onViewerMessage(const std::shared_ptr<Viewer>& viewer, const ws::Message& message);
    void onViewerClosed(const std::shared_ptr<Viewer>& viewer);

    // ---- information ----
    const std::string& id() const { return id_; }
    const std::string& gameId() const { return gameId_; }
    const GameMetadata& info() const { return library_->info(); }
    std::size_t viewerCount() const;
    double secondsWithoutViewers() const;  // 0 while somebody is connected
    bool failed() const;
    Json summaryJson() const;  // short version for lists
    Json detailsJson() const;  // everything, including metrics

    // ---- GameHost: called by the game (on the game loop thread) ----
    std::string playerName(int player) const override;
    bool playerConnected(int player) const override;
    void log(const std::string& message) override;

private:
    struct PlayerSlot {
        bool taken = false;       // somebody owns this slot (maybe disconnected right now)
        bool connected = false;
        std::string name;
        std::string token;        // secret that lets the owner reclaim the slot after reconnecting
        std::uint64_t viewerId = 0;
        Clock::time_point leftAt;
        std::uint32_t lastInputSequence = 0;  // newest input already applied by the game
    };
    struct PendingInput {
        InputEvent event;
        std::uint32_t sequence;
        Clock::time_point arrived;
    };
    struct PendingEvent {
        bool joined;  // true = playerJoined, false = playerLeft
        int player;
        std::string name;
    };
    // Engine measurements, updated by the game loop (protected by mutex_).
    struct EngineMetrics {
        double actualTicksPerSecond = 0;
        double updateMs = 0;      // average time spent in game input+update
        double renderMs = 0;      // average time spent in game render (= frame capture)
        double encodeMs = 0;      // average time spent encoding
        double loadPercent = 0;   // share of the tick budget that was used
        double changedTilesPercent = 0;
        double averageDeltaBytes = 0;
        std::size_t lastKeyframeBytes = 0;
        double outputKbps = 0;    // video data queued to all viewers
        double inputDelayMs = 0;  // how long inputs waited for the next tick
        std::uint64_t ticks = 0;
        std::uint64_t lateTicks = 0;  // ticks that started much too late (overload)
        std::uint64_t framesEncoded = 0;
        std::uint64_t keyframes = 0;
        std::uint64_t deltaFrames = 0;
        std::uint64_t idleTicks = 0;  // nothing changed, nothing sent
        std::uint64_t inputsProcessed = 0;
        std::uint64_t bytesQueued = 0;
        codec::EncodeStats lastEncode;
    };

    void loop();
    bool callGame(const std::function<void()>& action);
    void fail(const std::string& message);
    void handleJoin(const std::shared_ptr<Viewer>& viewer, const protocol::JoinRequest& request);
    void handleInput(const std::shared_ptr<Viewer>& viewer, const protocol::InputMessage& input);
    void releaseExpiredSlotsLocked(Clock::time_point now);
    std::string buildStateMessage();
    Json playersJsonLocked() const;
    Json engineJsonLocked() const;
    Json gameMetricsJsonLocked() const;
    Json viewersJsonLocked() const;
    double serverTimeMs() const;

    const std::string id_;
    const std::string gameId_;
    std::shared_ptr<GameLibrary> library_;  // declared before game_: the game is destroyed first
    std::unique_ptr<Game, std::function<void(Game*)>> game_;
    const SessionOptions options_;
    const Clock::time_point createdAt_;
    const long long createdUnix_;

    Canvas canvas_;
    codec::FrameEncoder encoder_;
    std::uint32_t frameNumber_ = 0;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool running_ = false;
    bool stopped_ = false;
    bool failed_ = false;
    std::string failure_;
    std::thread loopThread_;
    std::vector<PlayerSlot> slots_;
    std::vector<std::shared_ptr<Viewer>> viewers_;
    std::vector<std::shared_ptr<Viewer>> closedViewers_;  // disconnected; threads still to be joined
    std::vector<PendingInput> inputs_;
    std::vector<PendingEvent> events_;
    bool playersChanged_ = true;
    Clock::time_point lastViewerSeen_;
    std::uint64_t nextViewerId_ = 1;
    std::string gameStatus_;                                       // cached game->status()
    std::vector<std::pair<std::string, std::string>> gameMetrics_;  // cached game->metrics()
    EngineMetrics metrics_;
};

}  // namespace cge
