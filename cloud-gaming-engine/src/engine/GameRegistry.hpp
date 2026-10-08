#pragma once

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "core/Json.hpp"
#include "engine/GameLibrary.hpp"

namespace cge {

enum class GameState {
    Building,     // being compiled / tested right now
    Ready,        // compiled, tested and loaded - can be played
    BuildFailed,  // the C++ compiler reported errors
    Invalid,      // compiled, but crashed, hung or misbehaved in the test run
};
const char* gameStateName(GameState state);

// Everything the engine knows about one folder in games/.
struct GameEntry {
    std::string id;           // the folder name, used in URLs
    std::string folder;       // absolute path
    GameState state = GameState::Building;
    std::string error;        // compiler output or test failure (for BuildFailed / Invalid)
    std::string fingerprint;  // hash of the sources; a new hash triggers a rebuild
    bool prebuilt = false;    // true if the folder ships a ready game.so instead of sources
    double buildSeconds = 0;
    long long updatedUnix = 0;
    GameMetadata info;        // valid when state == Ready
    std::shared_ptr<GameLibrary> library;
};

// Watches the games/ folder, builds and tests every game in it, and keeps
// the loaded libraries.
//
//  * Each sub-folder of games/ is one game. It contains C++ files (*.cpp)
//    that include <cge/GameApi.hpp> - or a prebuilt "game.so".
//  * The registry compiles the sources into a shared library with the
//    system C++ compiler. Compiler errors are kept and shown in the lobby.
//  * The new library is test-played in a separate process (see
//    runGameValidation). Only games that pass are offered for play.
//  * A background thread rescans every few seconds: new folders appear,
//    edited games are rebuilt (hot reload), deleted folders disappear.
class GameRegistry {
public:
    struct Options {
        std::string gamesDir;
        std::string buildDir;        // where compiled libraries are kept
        std::string sdkIncludeDir;   // folder that contains cge/GameApi.hpp
        std::string compiler = "c++";
        std::string validatorProgram;  // our own executable (run with --validate-game)
        int scanIntervalSeconds = 3;
    };

    explicit GameRegistry(Options options);
    ~GameRegistry();

    // Scans and builds once, synchronously (used at start-up).
    void scanNow();
    // Starts the background watcher thread (it scans immediately, then every
    // scanIntervalSeconds) / stops it.
    void startWatching();
    void stop();

    std::vector<GameEntry> list() const;
    std::optional<GameEntry> find(const std::string& id) const;
    // The loaded library of a playable game, or nullptr.
    std::shared_ptr<GameLibrary> library(const std::string& id) const;

    Json statsJson() const;

private:
    void scanOnce();
    void buildGame(GameEntry& entry, const std::vector<std::string>& sources, const std::string& prebuiltLibrary);
    std::string compilerFlagsText() const;

    Options options_;
    mutable std::mutex mutex_;
    std::map<std::string, GameEntry> games_;
    std::mutex scanMutex_;  // only one scan at a time
    std::thread watcher_;
    std::atomic<bool> running_{false};
    std::mutex wakeMutex_;
    std::condition_variable wake_;
    std::atomic<std::uint64_t> builds_{0};
};

}  // namespace cge
