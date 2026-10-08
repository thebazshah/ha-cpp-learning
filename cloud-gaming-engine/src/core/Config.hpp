#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace cge {

// All server settings. Every field has a default and a command-line flag
// (run with --help to list them).
struct Config {
    std::string bindAddress = "0.0.0.0";  // 0.0.0.0 = reachable from other computers too
    std::uint16_t port = 8090;            // REST API, web page and WebSockets

    // Folders (relative paths are resolved against the project folder).
    std::string rootDir;  // empty = auto-detect (the folder that contains web/index.html)
    std::string gamesDir = "games";
    std::string buildDir = "cache/games";
    std::string webDir = "web";
    std::string sdkDir = "sdk/include";

    std::string compiler = "c++";     // used to build the games
    int scanIntervalSeconds = 3;       // how often games/ is checked for changes

    std::size_t httpThreads = 16;
    std::size_t maxSessions = 100;
    std::size_t maxViewersPerSession = 32;
    int idleTimeoutSeconds = 600;      // sessions without viewers are removed after this
    int reconnectGraceSeconds = 60;    // a disconnected player keeps their slot this long

    std::string logLevel = "info";
    bool showHelp = false;

    // Set when started as "--validate-game <library>" (internal use).
    std::string validateGame;

    bool parseArgs(int argc, char** argv, std::string& error);
    void resolvePaths(const char* argv0);
    static std::string usage();
};

// Absolute path of the running executable (needed to start the game validator).
std::string executablePath(const char* argv0);

}  // namespace cge
