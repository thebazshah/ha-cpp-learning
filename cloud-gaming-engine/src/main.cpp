// Cloud Gaming Engine - entry point.
//
//   games/<name>/*.cpp --> GameRegistry (compile, test, load) --> SessionManager --> GameSession (game loop)
//                                                                                        |
//   Browser <==== WebSocket over TCP (frames out, input in) ====> HttpServer + ApiController
//
// The same executable has a second, internal mode:
//   cloud-gaming-engine --validate-game <library.so>
// It test-plays one game and exits. The server runs it as a separate
// process, so a crashing game cannot take the server down.

#include <pthread.h>
#include <signal.h>

#include <csignal>
#include <filesystem>
#include <iostream>

#include "api/ApiController.hpp"
#include "core/Config.hpp"
#include "core/Logger.hpp"
#include "engine/GameLibrary.hpp"
#include "engine/GameRegistry.hpp"
#include "engine/SessionManager.hpp"
#include "http/HttpServer.hpp"
#include "http/Router.hpp"

using namespace cge;

int main(int argc, char** argv) {
    Config config;
    std::string error;
    if (!config.parseArgs(argc, argv, error)) {
        std::cerr << error << "\n\n" << Config::usage();
        return 2;
    }
    if (config.showHelp) {
        std::cout << Config::usage();
        return 0;
    }
    if (!config.validateGame.empty()) return runGameValidation(config.validateGame);

    LogLevel level = LogLevel::Info;
    if (!Logger::parseLevel(config.logLevel, level)) {
        std::cerr << "Unknown log level '" << config.logLevel << "'\n";
        return 2;
    }
    Logger::setLevel(level);

    // Ctrl+C / SIGTERM are received by sigwait() in main (no code runs in a
    // signal handler); a vanished client must not kill us with SIGPIPE.
    sigset_t stopSignals;
    sigemptyset(&stopSignals);
    sigaddset(&stopSignals, SIGINT);
    sigaddset(&stopSignals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &stopSignals, nullptr);
    std::signal(SIGPIPE, SIG_IGN);

    config.resolvePaths(argv[0]);
    std::error_code ignored;
    std::filesystem::create_directories(config.gamesDir, ignored);
    std::filesystem::create_directories(config.buildDir, ignored);
    if (!std::filesystem::exists(config.sdkDir + "/cge/GameApi.hpp", ignored)) {
        LOG_ERROR("main") << "SDK header not found at " << config.sdkDir << "/cge/GameApi.hpp (use --sdk-dir)";
        return 1;
    }

    LOG_INFO("main") << "Project folder: " << config.rootDir;
    LOG_INFO("main") << "Games folder:   " << config.gamesDir;

    // ---- Build and load the games in the background. The web page already
    //      works meanwhile and shows them as "building".
    GameRegistry::Options registryOptions;
    registryOptions.gamesDir = config.gamesDir;
    registryOptions.buildDir = config.buildDir;
    registryOptions.sdkIncludeDir = config.sdkDir;
    registryOptions.compiler = config.compiler;
    registryOptions.validatorProgram = executablePath(argv[0]);
    registryOptions.scanIntervalSeconds = config.scanIntervalSeconds;
    GameRegistry registry(registryOptions);
    registry.startWatching();

    SessionManager::Options sessionOptions;
    sessionOptions.maxSessions = config.maxSessions;
    sessionOptions.idleTimeoutSeconds = config.idleTimeoutSeconds;
    sessionOptions.session.reconnectGraceSeconds = config.reconnectGraceSeconds;
    sessionOptions.session.maxViewers = config.maxViewersPerSession;
    SessionManager sessions(registry, sessionOptions);
    sessions.start();

    Router router;
    HttpServerOptions httpOptions;
    httpOptions.bindAddress = config.bindAddress;
    httpOptions.port = config.port;
    httpOptions.workerThreads = config.httpThreads;
    HttpServer http(httpOptions, router);
    ApiController api(config, registry, sessions, http);
    api.registerRoutes(router);

    if (!http.start(error)) {
        LOG_ERROR("main") << error;
        return 1;
    }
    LOG_INFO("main") << "Ready. Open http://localhost:" << config.port << "/ in your browser. Press Ctrl+C to stop.";

    int received = 0;
    sigwait(&stopSignals, &received);
    LOG_INFO("main") << "Shutting down...";
    http.stop();       // no new requests or connections
    sessions.stop();   // ends every game and disconnects all players
    registry.stop();   // stops watching the games folder
    LOG_INFO("main") << "Goodbye.";
    return 0;
}
