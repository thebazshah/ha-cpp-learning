#include "engine/SessionManager.hpp"

#include <random>

#include "core/Logger.hpp"

namespace cge {

SessionManager::SessionManager(GameRegistry& registry, Options options)
    : registry_(registry), options_(options) {}

SessionManager::~SessionManager() { stop(); }

void SessionManager::start() {
    if (running_.exchange(true)) return;
    janitor_ = std::thread([this] { janitorLoop(); });
}

void SessionManager::stop() {
    if (running_.exchange(false)) {
        wake_.notify_all();
        if (janitor_.joinable()) janitor_.join();
    }
    std::map<std::string, std::shared_ptr<GameSession>> sessions;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions.swap(sessions_);
    }
    for (auto& entry : sessions) entry.second->stop();
}

std::string SessionManager::newSessionId() {
    // 8 characters from an alphabet without look-alikes (no 0/o, 1/l/i),
    // short enough to read out loud, long enough to be hard to guess.
    static const char kAlphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";
    static std::mt19937_64 generator{std::random_device{}()};
    std::string id;
    do {
        id.clear();
        for (int i = 0; i < 8; ++i) id += kAlphabet[generator() % (sizeof(kAlphabet) - 1)];
    } while (sessions_.count(id) > 0);
    return id;
}

std::shared_ptr<GameSession> SessionManager::create(const std::string& gameId, int& status, std::string& error) {
    const std::optional<GameEntry> entry = registry_.find(gameId);
    if (!entry) {
        status = 404;
        error = "Unknown game '" + gameId + "'";
        return nullptr;
    }
    std::shared_ptr<GameLibrary> library = registry_.library(gameId);
    if (!library) {
        status = 409;
        error = "The game '" + gameId + "' cannot be played right now (" + gameStateName(entry->state) + ")";
        return nullptr;
    }

    std::shared_ptr<GameSession> session;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (sessions_.size() >= options_.maxSessions) {
            status = 503;
            error = "Too many sessions are running. Try again later.";
            return nullptr;
        }
        session = std::make_shared<GameSession>(newSessionId(), gameId, library, options_.session);
        sessions_[session->id()] = session;
        ++totalCreated_;
    }
    session->start();
    if (session->failed()) {
        remove(session->id());
        status = 500;
        error = "The game could not be started";
        return nullptr;
    }
    status = 201;
    return session;
}

std::shared_ptr<GameSession> SessionManager::find(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = sessions_.find(id);
    return it == sessions_.end() ? nullptr : it->second;
}

bool SessionManager::remove(const std::string& id) {
    std::shared_ptr<GameSession> session;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = sessions_.find(id);
        if (it == sessions_.end()) return false;
        session = it->second;
        sessions_.erase(it);
    }
    session->stop();  // outside the lock: this waits for the session's threads
    return true;
}

std::vector<std::shared_ptr<GameSession>> SessionManager::list() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::shared_ptr<GameSession>> sessions;
    for (const auto& entry : sessions_) sessions.push_back(entry.second);
    return sessions;
}

std::size_t SessionManager::countForGame(const std::string& gameId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t count = 0;
    for (const auto& entry : sessions_) count += entry.second->gameId() == gameId ? 1 : 0;
    return count;
}

void SessionManager::janitorLoop() {
    while (running_) {
        {
            std::unique_lock<std::mutex> lock(wakeMutex_);
            wake_.wait_for(lock, std::chrono::seconds(2), [this] { return !running_.load(); });
        }
        if (!running_) break;

        std::vector<std::string> expired;
        for (const auto& session : list()) {
            const double idle = session->secondsWithoutViewers();
            if (idle > options_.idleTimeoutSeconds || (session->failed() && idle > 30)) {
                expired.push_back(session->id());
            }
        }
        for (const std::string& id : expired) {
            LOG_INFO("session") << "Removing idle session " << id;
            remove(id);
        }
    }
}

Json SessionManager::statsJson() const {
    std::vector<std::shared_ptr<GameSession>> sessions = list();
    std::size_t viewers = 0;
    for (const auto& session : sessions) viewers += session->viewerCount();
    std::lock_guard<std::mutex> lock(mutex_);
    Json stats = Json::object();
    stats.set("active", sessions.size())
        .set("max", options_.maxSessions)
        .set("totalCreated", totalCreated_)
        .set("connectedViewers", viewers)
        .set("idleTimeoutSeconds", options_.idleTimeoutSeconds);
    return stats;
}

}  // namespace cge
