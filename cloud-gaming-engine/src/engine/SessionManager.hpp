#pragma once

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/Json.hpp"
#include "engine/GameRegistry.hpp"
#include "engine/GameSession.hpp"

namespace cge {

// Creates, finds and cleans up game sessions.
//
// A session lives as long as somebody uses it. A janitor thread removes
// sessions that nobody has watched for `idleTimeoutSeconds`, and sessions
// whose game failed.
class SessionManager {
public:
    struct Options {
        std::size_t maxSessions = 100;
        int idleTimeoutSeconds = 600;
        SessionOptions session;
    };

    SessionManager(GameRegistry& registry, Options options);
    ~SessionManager();

    void start();
    void stop();  // stops every session

    // Starts a new session of a game. On failure returns nullptr and fills an
    // HTTP status (404 unknown game, 409 game not playable, 503 too many sessions).
    std::shared_ptr<GameSession> create(const std::string& gameId, int& status, std::string& error);
    std::shared_ptr<GameSession> find(const std::string& id) const;
    bool remove(const std::string& id);
    std::vector<std::shared_ptr<GameSession>> list() const;
    std::size_t countForGame(const std::string& gameId) const;

    Json statsJson() const;

private:
    void janitorLoop();
    std::string newSessionId();

    GameRegistry& registry_;
    Options options_;
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<GameSession>> sessions_;
    std::uint64_t totalCreated_ = 0;
    std::thread janitor_;
    std::atomic<bool> running_{false};
    std::mutex wakeMutex_;
    std::condition_variable wake_;
};

}  // namespace cge
