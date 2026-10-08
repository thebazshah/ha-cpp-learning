#include "engine/GameSession.hpp"

#include <algorithm>
#include <ctime>
#include <exception>
#include <random>

#include "core/Logger.hpp"
#include "core/StringUtils.hpp"

namespace cge {
namespace {

constexpr auto kStateInterval = std::chrono::milliseconds(500);  // how often "state" messages go out
constexpr auto kHeartbeat = std::chrono::seconds(1);            // send a frame at least this often
constexpr double kSmoothing = 0.1;                               // weight of new samples in averages

double milliseconds(std::chrono::steady_clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

// Exponentially weighted moving average: smooth numbers that react to change.
void smooth(double& average, double sample) { average = average == 0 ? sample : average + kSmoothing * (sample - average); }

std::string randomToken() {
    static std::mt19937_64 generator{std::random_device{}()};
    static std::mutex generatorMutex;
    std::lock_guard<std::mutex> lock(generatorMutex);
    char text[33];
    std::snprintf(text, sizeof(text), "%016llx%016llx", static_cast<unsigned long long>(generator()),
                  static_cast<unsigned long long>(generator()));
    return text;
}

// Keeps a player name short and free of control characters.
std::string cleanName(const std::string& raw, std::uint64_t viewerId) {
    std::string name;
    for (unsigned char c : raw) {
        if (c >= 32 && c != 127) name += static_cast<char>(c);
    }
    name = str::trim(name);
    if (name.size() > 24) {
        name.resize(24);
        // Do not cut a multi-byte UTF-8 character in half.
        while (!name.empty() && (static_cast<unsigned char>(name.back()) & 0xC0) == 0x80) name.pop_back();
        if (!name.empty() && (static_cast<unsigned char>(name.back()) & 0x80)) name.pop_back();
    }
    if (name.empty()) name = "Guest " + std::to_string(viewerId);
    return name;
}

}  // namespace

GameSession::GameSession(std::string id, std::string gameId, std::shared_ptr<GameLibrary> library,
                         SessionOptions options)
    : id_(std::move(id)),
      gameId_(std::move(gameId)),
      library_(std::move(library)),
      game_(nullptr, [lib = library_](Game* game) { lib->destroy(game); }),
      options_(options),
      createdAt_(Clock::now()),
      createdUnix_(static_cast<long long>(std::time(nullptr))),
      canvas_(library_->info().width, library_->info().height),
      encoder_(library_->info().width, library_->info().height),
      slots_(static_cast<std::size_t>(library_->info().maxPlayers)),
      lastViewerSeen_(Clock::now()) {}

GameSession::~GameSession() { stop(); }

double GameSession::serverTimeMs() const { return milliseconds(Clock::now() - createdAt_); }

// ------------------------------------------------------------------ lifecycle

void GameSession::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_ || stopped_) return;
    try {
        game_.reset(library_->create());
    } catch (const std::exception& error) {
        failed_ = true;
        failure_ = std::string("could not create the game: ") + error.what();
        return;
    }
    if (!game_) {
        failed_ = true;
        failure_ = "could not create the game";
        return;
    }
    running_ = true;
    loopThread_ = std::thread([this] { loop(); });
    LOG_INFO("session") << "Session " << id_ << " started (" << info().name << ")";
}

void GameSession::stop() {
    std::vector<std::shared_ptr<Viewer>> everyone;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_) return;
        stopped_ = true;
        running_ = false;
        everyone = viewers_;
        everyone.insert(everyone.end(), closedViewers_.begin(), closedViewers_.end());
        viewers_.clear();
        closedViewers_.clear();
    }
    wake_.notify_all();
    if (loopThread_.joinable()) loopThread_.join();
    for (auto& viewer : everyone) viewer->close();
    for (auto& viewer : everyone) viewer->join();
    game_.reset();  // destroy the game while its library is still loaded
    LOG_INFO("session") << "Session " << id_ << " stopped";
}

bool GameSession::failed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return failed_;
}

std::size_t GameSession::viewerCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return viewers_.size();
}

double GameSession::secondsWithoutViewers() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!viewers_.empty()) return 0.0;
    return std::chrono::duration<double>(Clock::now() - lastViewerSeen_).count();
}

void GameSession::fail(const std::string& message) {
    std::vector<std::shared_ptr<Viewer>> viewers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        failed_ = true;
        failure_ = message;
        running_ = false;
        viewers = viewers_;
    }
    LOG_ERROR("session") << "Session " << id_ << " stopped because the game failed: " << message;
    Json error = Json::object();
    error.set("type", "error").set("message", "The game stopped with an error: " + message);
    for (auto& viewer : viewers) viewer->queueText(error.dump());
}

bool GameSession::callGame(const std::function<void()>& action) {
    // A game that throws must not take the whole server down.
    try {
        action();
        return true;
    } catch (const std::exception& error) {
        fail(error.what());
    } catch (...) {
        fail("unknown exception");
    }
    return false;
}

// ------------------------------------------------------------------ viewers

void GameSession::attachViewer(Socket socket, std::string pendingBytes, const std::string& remoteIp) {
    socket.setNoDelay();              // send small packets immediately (lower latency)
    socket.setTimeouts(0, 5000);      // reads may wait forever; a write stuck for 5 s means the client is gone
    auto connection = std::make_unique<ws::Connection>(std::move(socket), false, std::move(pendingBytes), 64 * 1024);

    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_ || failed_ || viewers_.size() >= options_.maxViewers) {
        Json error = Json::object();
        error.set("type", "error").set("message", stopped_ || failed_ ? "This session has ended." : "This session is full.");
        connection->sendText(error.dump());
        connection->close(1013);
        return;
    }
    auto viewer = std::make_shared<Viewer>(nextViewerId_++, std::move(connection), remoteIp);
    viewers_.push_back(viewer);
    lastViewerSeen_ = Clock::now();
    viewer->start(shared_from_this(), viewer);
    LOG_INFO("session") << "Session " << id_ << ": viewer " << viewer->id() << " connected from " << remoteIp;
}

void GameSession::onViewerClosed(const std::shared_ptr<Viewer>& viewer) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = std::find(viewers_.begin(), viewers_.end(), viewer);
    if (it == viewers_.end()) return;  // already removed by stop()
    viewers_.erase(it);
    closedViewers_.push_back(viewer);  // the game loop joins its threads
    lastViewerSeen_ = Clock::now();

    if (viewer->player >= 0) {
        PlayerSlot& slot = slots_[static_cast<std::size_t>(viewer->player)];
        if (slot.viewerId == viewer->id() && slot.connected) {
            slot.connected = false;
            slot.leftAt = Clock::now();
            events_.push_back(PendingEvent{false, viewer->player, slot.name});
        }
    }
    playersChanged_ = true;
    LOG_INFO("session") << "Session " << id_ << ": " << (viewer->joined ? viewer->name : "viewer " + std::to_string(viewer->id()))
                        << " disconnected";
}

void GameSession::onViewerMessage(const std::shared_ptr<Viewer>& viewer, const ws::Message& message) {
    if (!message.binary || message.data.empty()) return;  // clients only send binary messages
    switch (static_cast<std::uint8_t>(message.data[0])) {
        case protocol::kJoin: {
            protocol::JoinRequest join;
            if (protocol::parseJoin(message.data, join)) handleJoin(viewer, join);
            break;
        }
        case protocol::kInput: {
            protocol::InputMessage input;
            if (protocol::parseInput(message.data, input)) handleInput(viewer, input);
            break;
        }
        case protocol::kPing: {
            // Answer right away (not on the next tick) so the measured round
            // trip time is pure network delay.
            protocol::PingMessage ping;
            if (protocol::parsePing(message.data, ping)) {
                viewer->roundTripMs = ping.lastRoundTripMs;
                viewer->queueBinary(protocol::buildPong(ping.clientTimeMs, serverTimeMs()));
            }
            break;
        }
        case protocol::kKeyframeRequest:
            viewer->needsKeyframe = true;
            break;
        default:
            break;  // unknown message: ignore
    }
}

void GameSession::handleJoin(const std::shared_ptr<Viewer>& viewer, const protocol::JoinRequest& request) {
    Json welcome = Json::object();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (viewer->joined) return;  // joining twice is not allowed
        const std::string name = cleanName(request.name, viewer->id());
        int slotIndex = -1;

        // 1. A returning player (same token) gets their old slot back.
        if (!request.token.empty()) {
            for (std::size_t i = 0; i < slots_.size(); ++i) {
                if (slots_[i].taken && slots_[i].token == request.token) {
                    slotIndex = static_cast<int>(i);
                    break;
                }
            }
            if (slotIndex >= 0) {
                PlayerSlot& slot = slots_[static_cast<std::size_t>(slotIndex)];
                // The same player opened a second tab: the older tab becomes a spectator.
                for (auto& other : viewers_) {
                    if (other != viewer && other->player == slotIndex) other->player = -1;
                }
                if (slot.connected) events_.push_back(PendingEvent{false, slotIndex, slot.name});
            }
        }
        // 2. Otherwise take the first free slot (unless the user only wants to watch).
        if (slotIndex < 0 && !request.spectate) {
            for (std::size_t i = 0; i < slots_.size(); ++i) {
                if (!slots_[i].taken) {
                    slotIndex = static_cast<int>(i);
                    slots_[i] = PlayerSlot();
                    slots_[i].taken = true;
                    slots_[i].token = randomToken();
                    break;
                }
            }
        }

        viewer->joined = true;
        viewer->name = name;
        viewer->player = slotIndex;
        viewer->needsKeyframe = true;
        std::string token;
        if (slotIndex >= 0) {
            PlayerSlot& slot = slots_[static_cast<std::size_t>(slotIndex)];
            slot.connected = true;
            slot.name = name;
            slot.viewerId = viewer->id();
            slot.lastInputSequence = 0;  // the new connection counts its inputs from 1 again
            token = slot.token;
            events_.push_back(PendingEvent{true, slotIndex, name});
        }
        playersChanged_ = true;

        Json game = Json::object();
        game.set("id", gameId_)
            .set("name", info().name)
            .set("description", info().description)
            .set("width", info().width)
            .set("height", info().height)
            .set("minPlayers", info().minPlayers)
            .set("maxPlayers", info().maxPlayers)
            .set("ticksPerSecond", info().ticksPerSecond);
        welcome.set("type", "welcome")
            .set("sessionId", id_)
            .set("viewerId", viewer->id())
            .set("name", name)
            .set("role", slotIndex >= 0 ? "player" : "spectator")
            .set("player", slotIndex)
            .set("token", token)
            .set("game", game)
            .set("codec", Json::object().set("name", "CGV1").set("tileSize", codec::kTileSize));
        LOG_INFO("session") << "Session " << id_ << ": " << name << " joined as "
                            << (slotIndex >= 0 ? "player " + std::to_string(slotIndex + 1) : std::string("spectator"));
    }
    viewer->queueText(welcome.dump());
    wake_.notify_all();
}

void GameSession::handleInput(const std::shared_ptr<Viewer>& viewer, const protocol::InputMessage& input) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!viewer->joined || viewer->player < 0) return;  // spectators cannot play
    if (inputs_.size() >= 2000) return;                 // flood protection
    PendingInput pending;
    pending.event.kind = input.kind;
    pending.event.player = viewer->player;
    pending.event.x = std::max(-1, std::min(info().width, input.x));
    pending.event.y = std::max(-1, std::min(info().height, input.y));
    pending.event.button = input.button;
    pending.event.key = input.key.substr(0, 32);
    pending.sequence = input.sequence;
    pending.arrived = Clock::now();
    inputs_.push_back(std::move(pending));
    ++viewer->inputs;
}

void GameSession::releaseExpiredSlotsLocked(Clock::time_point now) {
    for (PlayerSlot& slot : slots_) {
        if (slot.taken && !slot.connected &&
            now - slot.leftAt > std::chrono::seconds(options_.reconnectGraceSeconds)) {
            LOG_INFO("session") << "Session " << id_ << ": slot of " << slot.name << " is free again";
            slot = PlayerSlot();
            playersChanged_ = true;
        }
    }
}

// ------------------------------------------------------------------ GameHost

std::string GameSession::playerName(int player) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (player < 0 || player >= static_cast<int>(slots_.size()) || !slots_[static_cast<std::size_t>(player)].taken) {
        return "";
    }
    return slots_[static_cast<std::size_t>(player)].name;
}

bool GameSession::playerConnected(int player) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (player < 0 || player >= static_cast<int>(slots_.size())) return false;
    return slots_[static_cast<std::size_t>(player)].connected;
}

void GameSession::log(const std::string& message) { LOG_INFO("game") << "[" << id_ << "] " << message; }

// ------------------------------------------------------------------ the game loop

void GameSession::loop() {
    const auto interval = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(1.0 / info().ticksPerSecond));
    auto nextTick = Clock::now();
    auto lastTick = nextTick;
    auto lastState = Clock::time_point();
    auto lastFrameSent = Clock::time_point();
    auto windowStart = nextTick;
    std::uint64_t ticksInWindow = 0;
    std::uint64_t bytesInWindow = 0;
    std::vector<std::uint8_t> delta;
    std::vector<std::uint8_t> key;

    if (!callGame([this] { game_->start(*this); })) return;

    for (;;) {
        std::vector<PendingEvent> events;
        std::vector<PendingInput> inputs;
        std::vector<std::shared_ptr<Viewer>> joined;
        std::vector<std::shared_ptr<Viewer>> finished;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            // Sleep until the next tick is due (stop() wakes us up early).
            wake_.wait_until(lock, nextTick, [this] { return !running_; });
            if (!running_) break;
            events.swap(events_);
            inputs.swap(inputs_);
            finished.swap(closedViewers_);
            for (const auto& viewer : viewers_) {
                if (viewer->joined) joined.push_back(viewer);
            }
            releaseExpiredSlotsLocked(Clock::now());
        }
        for (auto& viewer : finished) viewer->join();  // threads of viewers that left
        finished.clear();

        const auto tickStart = Clock::now();
        const double seconds = std::min(0.25, std::chrono::duration<double>(tickStart - lastTick).count());
        lastTick = tickStart;

        // ---- 1. Game logic: deliver joins/leaves and inputs, then advance time.
        if (!callGame([&] {
                for (const PendingEvent& event : events) {
                    if (event.joined) game_->playerJoined(event.player, event.name);
                    else game_->playerLeft(event.player);
                }
                for (const PendingInput& input : inputs) game_->input(input.event);
                game_->update(seconds);
            })) {
            break;
        }
        const auto afterUpdate = Clock::now();

        // ---- 2. Render = "frame capture": the game draws into our canvas.
        if (!callGame([&] { game_->render(canvas_); })) break;
        const auto afterRender = Clock::now();

        // ---- 3. Acknowledge applied inputs (the client measures latency with these).
        std::vector<protocol::InputAck> acks;
        double inputDelaySum = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const PendingInput& input : inputs) {
                inputDelaySum += milliseconds(tickStart - input.arrived);
                PlayerSlot& slot = slots_[static_cast<std::size_t>(input.event.player)];
                slot.lastInputSequence = std::max(slot.lastInputSequence, input.sequence);
            }
            for (std::size_t i = 0; i < slots_.size(); ++i) {
                if (slots_[i].connected) {
                    acks.push_back(protocol::InputAck{static_cast<std::uint8_t>(i), slots_[i].lastInputSequence});
                }
            }
        }

        // ---- 4. Encode: a delta for viewers that are in sync, a keyframe for
        //         viewers that just joined or missed frames.
        bool wantDelta = false;
        bool wantKey = false;
        for (const auto& viewer : joined) {
            if (viewer->needsKeyframe) wantKey = true;
            else wantDelta = true;
        }
        std::shared_ptr<std::vector<std::uint8_t>> deltaMessage;
        std::shared_ptr<std::vector<std::uint8_t>> keyMessage;
        codec::EncodeStats encodeStats;
        double encodeMs = 0;
        if (wantDelta || wantKey) {
            const auto encodeStart = Clock::now();
            encoder_.encode(canvas_.data(), wantDelta ? &delta : nullptr, wantKey ? &key : nullptr, encodeStats);
            encodeMs = milliseconds(Clock::now() - encodeStart) + milliseconds(afterRender - afterUpdate);

            protocol::FrameHeader header;
            header.serverTimeMs = serverTimeMs();
            header.width = static_cast<std::uint16_t>(info().width);
            header.height = static_cast<std::uint16_t>(info().height);
            header.encodeMs = static_cast<float>(encodeMs);
            header.acks = acks;
            // Save bandwidth: send a delta only if something changed, inputs
            // need acknowledging, or as a heartbeat once per second.
            const bool worthSending =
                encodeStats.changedTiles > 0 || !inputs.empty() || tickStart - lastFrameSent >= kHeartbeat;
            if (wantDelta && worthSending) {
                header.keyframe = false;
                header.frameNumber = ++frameNumber_;
                deltaMessage = std::make_shared<std::vector<std::uint8_t>>(protocol::buildFrame(header, delta));
            }
            if (wantKey) {
                header.keyframe = true;
                header.frameNumber = ++frameNumber_;
                keyMessage = std::make_shared<std::vector<std::uint8_t>>(protocol::buildFrame(header, key));
            }
        }

        // ---- 5. Hand the frame to every viewer's send queue.
        std::uint64_t bytesThisTick = 0;
        for (const auto& viewer : joined) {
            if (viewer->needsKeyframe) {
                if (keyMessage && viewer->queueFrame(keyMessage)) {
                    viewer->needsKeyframe = false;
                    bytesThisTick += keyMessage->size();
                }
            } else if (deltaMessage) {
                if (viewer->queueFrame(deltaMessage)) {
                    bytesThisTick += deltaMessage->size();
                } else {
                    // The client's network is too slow: skip ahead with a keyframe later.
                    viewer->needsKeyframe = true;
                }
            }
        }
        if (deltaMessage || keyMessage) lastFrameSent = tickStart;

        // ---- 6. Statistics.
        const auto tickEnd = Clock::now();
        ++ticksInWindow;
        bytesInWindow += bytesThisTick;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            EngineMetrics& m = metrics_;
            ++m.ticks;
            smooth(m.updateMs, milliseconds(afterUpdate - tickStart));
            smooth(m.renderMs, milliseconds(afterRender - afterUpdate));
            if (wantDelta || wantKey) {
                smooth(m.encodeMs, encodeMs - milliseconds(afterRender - afterUpdate));
                smooth(m.changedTilesPercent, 100.0 * encodeStats.changedTiles / std::max(1, encodeStats.totalTiles));
                m.lastEncode = encodeStats;
            }
            smooth(m.loadPercent, 100.0 * milliseconds(tickEnd - tickStart) / milliseconds(interval));
            if (deltaMessage) {
                ++m.deltaFrames;
                smooth(m.averageDeltaBytes, static_cast<double>(deltaMessage->size()));
            }
            if (keyMessage) {
                ++m.keyframes;
                m.lastKeyframeBytes = keyMessage->size();
            }
            if (deltaMessage || keyMessage) ++m.framesEncoded;
            else ++m.idleTicks;
            if (!inputs.empty()) smooth(m.inputDelayMs, inputDelaySum / inputs.size());
            m.inputsProcessed += inputs.size();
            m.bytesQueued += bytesThisTick;
            const double windowSeconds = std::chrono::duration<double>(tickEnd - windowStart).count();
            if (windowSeconds >= 1.0) {
                m.actualTicksPerSecond = ticksInWindow / windowSeconds;
                m.outputKbps = bytesInWindow * 8.0 / 1000.0 / windowSeconds;
                ticksInWindow = 0;
                bytesInWindow = 0;
                windowStart = tickEnd;
            }
        }

        // ---- 7. Every 500 ms (or when players change) tell everybody the state.
        bool sendState;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            sendState = playersChanged_ || tickEnd - lastState >= kStateInterval;
            playersChanged_ = false;
        }
        if (sendState) {
            lastState = tickEnd;
            std::string status;
            std::vector<std::pair<std::string, std::string>> gameMetrics;
            if (!callGame([&] {
                    status = game_->status();
                    gameMetrics = game_->metrics();
                })) {
                break;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                gameStatus_ = status;
                gameMetrics_ = gameMetrics;
            }
            const std::string state = buildStateMessage();
            for (const auto& viewer : joined) viewer->queueText(state);
        }

        // ---- 8. Schedule the next tick. If we fell far behind (the machine
        //         was overloaded), skip the missed ticks instead of racing.
        nextTick += interval;
        const auto now = Clock::now();
        if (nextTick < now - 2 * interval) {
            nextTick = now;
            std::lock_guard<std::mutex> lock(mutex_);
            ++metrics_.lateTicks;
        }
    }
}

// ------------------------------------------------------------------ JSON views

Json GameSession::playersJsonLocked() const {
    Json players = Json::array();
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        Json slot = Json::object();
        slot.set("slot", i).set("taken", slots_[i].taken).set("connected", slots_[i].connected).set("name", slots_[i].name);
        players.push(slot);
    }
    return players;
}

Json GameSession::engineJsonLocked() const {
    const EngineMetrics& m = metrics_;
    Json tiles = Json::object();
    tiles.set("total", m.lastEncode.totalTiles)
        .set("changed", m.lastEncode.changedTiles)
        .set("solid", m.lastEncode.solidTiles)
        .set("rle", m.lastEncode.rleTiles)
        .set("raw", m.lastEncode.rawTiles);
    Json engine = Json::object();
    engine.set("targetTicksPerSecond", info().ticksPerSecond)
        .set("actualTicksPerSecond", m.actualTicksPerSecond)
        .set("updateMs", m.updateMs)
        .set("renderMs", m.renderMs)
        .set("encodeMs", m.encodeMs)
        .set("loadPercent", m.loadPercent)
        .set("changedTilesPercent", m.changedTilesPercent)
        .set("averageDeltaBytes", m.averageDeltaBytes)
        .set("lastKeyframeBytes", m.lastKeyframeBytes)
        .set("outputKbps", m.outputKbps)
        .set("inputDelayMs", m.inputDelayMs)
        .set("ticks", m.ticks)
        .set("lateTicks", m.lateTicks)
        .set("framesEncoded", m.framesEncoded)
        .set("keyframes", m.keyframes)
        .set("deltaFrames", m.deltaFrames)
        .set("idleTicks", m.idleTicks)
        .set("inputsProcessed", m.inputsProcessed)
        .set("bytesQueued", m.bytesQueued)
        .set("canvas", std::to_string(info().width) + "x" + std::to_string(info().height))
        .set("lastTiles", tiles)
        .set("uptimeSeconds", std::chrono::duration<double>(Clock::now() - createdAt_).count());
    return engine;
}

Json GameSession::gameMetricsJsonLocked() const {
    Json list = Json::array();
    for (const auto& metric : gameMetrics_) list.push(Json::object().set("name", metric.first).set("value", metric.second));
    return list;
}

Json GameSession::viewersJsonLocked() const {
    Json list = Json::array();
    for (const auto& viewer : viewers_) {
        Json item = Json::object();
        item.set("id", viewer->id())
            .set("name", viewer->joined ? viewer->name : "(connecting)")
            .set("role", !viewer->joined ? "connecting" : viewer->player >= 0 ? "player" : "spectator")
            .set("player", viewer->player)
            .set("roundTripMs", static_cast<double>(viewer->roundTripMs.load()))
            .set("framesSent", viewer->framesSent.load())
            .set("keyframesSent", viewer->keyframesSent.load())
            .set("framesDropped", viewer->framesDropped.load())
            .set("queuedFrames", viewer->queuedFrames())
            .set("bytesSent", viewer->bytesSent())
            .set("inputs", viewer->inputs.load());
        list.push(item);
    }
    return list;
}

std::string GameSession::buildStateMessage() {
    std::lock_guard<std::mutex> lock(mutex_);
    Json state = Json::object();
    state.set("type", "state")
        .set("serverTimeMs", serverTimeMs())
        .set("status", gameStatus_)
        .set("players", playersJsonLocked())
        .set("gameMetrics", gameMetricsJsonLocked())
        .set("engine", engineJsonLocked())
        .set("viewers", viewersJsonLocked());
    return state.dump();
}

Json GameSession::summaryJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    int playersTaken = 0;
    int playersConnected = 0;
    for (const PlayerSlot& slot : slots_) {
        playersTaken += slot.taken ? 1 : 0;
        playersConnected += slot.connected ? 1 : 0;
    }
    Json summary = Json::object();
    summary.set("id", id_)
        .set("url", "/play/" + id_)
        .set("gameId", gameId_)
        .set("gameName", info().name)
        .set("createdUnix", createdUnix_)
        .set("uptimeSeconds", std::chrono::duration<double>(Clock::now() - createdAt_).count())
        .set("players", playersJsonLocked())
        .set("playersTaken", playersTaken)
        .set("playersConnected", playersConnected)
        .set("maxPlayers", info().maxPlayers)
        .set("viewers", viewers_.size())
        .set("status", gameStatus_)
        .set("failed", failed_)
        .set("error", failure_);
    return summary;
}

Json GameSession::detailsJson() const {
    Json details = summaryJson();
    std::lock_guard<std::mutex> lock(mutex_);
    details.set("engine", engineJsonLocked())
        .set("gameMetrics", gameMetricsJsonLocked())
        .set("viewerList", viewersJsonLocked())
        .set("game", Json::object()
                         .set("name", info().name)
                         .set("description", info().description)
                         .set("width", info().width)
                         .set("height", info().height)
                         .set("minPlayers", info().minPlayers)
                         .set("maxPlayers", info().maxPlayers)
                         .set("ticksPerSecond", info().ticksPerSecond));
    return details;
}

}  // namespace cge
