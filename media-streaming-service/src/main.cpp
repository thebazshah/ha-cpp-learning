// Media Streaming Service - entry point.
//
// Wires all parts together:
//
//   SegmentCache ----+---------------------------+
//                    |                           |
//   MediaLibrary --> TranscodeManager (ffmpeg) --+--> HlsService  --+--> HttpServer (REST API, HLS, web page)
//                    |                           |    ApiController-+
//                    +---------------------------+--> RtspServer (RTSP + RTP/RTCP)
//
// and then waits for Ctrl+C (SIGINT) or SIGTERM to shut down cleanly.

#include <pthread.h>
#include <signal.h>

#include <csignal>
#include <filesystem>
#include <iostream>
#include <string>

#include "api/ApiController.hpp"
#include "cache/SegmentCache.hpp"
#include "core/Config.hpp"
#include "core/Logger.hpp"
#include "core/Process.hpp"
#include "core/ThreadPool.hpp"
#include "http/HttpServer.hpp"
#include "http/Router.hpp"
#include "media/MediaLibrary.hpp"
#include "media/TranscodeManager.hpp"
#include "rtsp/RtspServer.hpp"
#include "streaming/HlsService.hpp"

using namespace mss;

namespace {

// Checks that a program (ffmpeg/ffprobe) can be started.
bool programWorks(const std::string& program) {
    const ProcessResult result = Process::run({program, "-hide_banner", "-version"}, 15);
    return result.started && result.exitCode == 0;
}

}  // namespace

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
    LogLevel level = LogLevel::Info;
    if (!Logger::parseLevel(config.logLevel, level)) {
        std::cerr << "Unknown log level '" << config.logLevel << "'\n";
        return 2;
    }
    Logger::setLevel(level);

    // Block SIGINT/SIGTERM in every thread; the main thread waits for them
    // with sigwait() below. This is the safe way to handle Ctrl+C in a
    // multithreaded program (no code runs inside a signal handler).
    sigset_t stopSignals;
    sigemptyset(&stopSignals);
    sigaddset(&stopSignals, SIGINT);
    sigaddset(&stopSignals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &stopSignals, nullptr);
    // Writing to a socket whose client has gone away must return an error,
    // not kill the whole process.
    std::signal(SIGPIPE, SIG_IGN);

    config.resolvePaths(argv[0]);
    std::error_code ignored;
    std::filesystem::create_directories(config.videosDir, ignored);
    std::filesystem::create_directories(config.audiosDir, ignored);
    std::filesystem::create_directories(config.cacheDir, ignored);

    // ---- FFmpeg is required for H.264/AAC encoding and for reading media details.
    if (!programWorks(config.ffmpegPath) || !programWorks(config.ffprobePath)) {
        LOG_ERROR("main") << "ffmpeg/ffprobe not found. Install FFmpeg (macOS: 'brew install ffmpeg', "
                          << "Ubuntu: 'sudo apt install ffmpeg') or pass --ffmpeg/--ffprobe paths.";
        return 1;
    }
    const std::string encoder = TranscodeManager::detectVideoEncoder(config.ffmpegPath, config.videoEncoder);
    if (encoder.empty()) {
        LOG_ERROR("main") << "ffmpeg has no usable H.264 encoder (wanted '" << config.videoEncoder
                          << "'). Install an FFmpeg build with libx264.";
        return 1;
    }

    LOG_INFO("main") << "Project folder: " << config.rootDir;
    LOG_INFO("main") << "Videos: " << config.videosDir << "  Audios: " << config.audiosDir;
    LOG_INFO("main") << "Cache: " << config.cacheDir << "  Web: " << config.webDir;
    LOG_INFO("main") << "H.264 encoder: " << encoder << ", segment length " << config.segmentSeconds << " s";

    // ---- Build the components (the order matters for shutdown: see below).
    SegmentCache cache(config.cacheMegabytes * 1024 * 1024);
    ThreadPool background(config.backgroundThreads, "background");
    MediaLibrary library(config.videosDir, config.audiosDir, config.ffprobePath, background);

    TranscodeSettings transcodeSettings;
    transcodeSettings.ffmpegPath = config.ffmpegPath;
    transcodeSettings.cacheDir = config.cacheDir;
    transcodeSettings.videoEncoder = encoder;
    transcodeSettings.x264Preset = config.x264Preset;
    transcodeSettings.segmentSeconds = config.segmentSeconds;
    transcodeSettings.maxRenditions = config.maxRenditions;
    transcodeSettings.maxConcurrentJobs = config.maxConcurrentTranscodes;
    TranscodeManager transcoder(transcodeSettings, cache);

    RtspServerOptions rtspOptions;
    rtspOptions.bindAddress = config.bindAddress;
    rtspOptions.port = config.rtspPort;
    rtspOptions.rtpPortMin = config.rtpPortMin;
    rtspOptions.rtpPortMax = config.rtpPortMax;
    rtspOptions.maxSessions = config.maxRtspSessions;
    rtspOptions.sessionTimeoutSeconds = config.rtspSessionTimeoutSeconds;
    RtspServer rtspServer(rtspOptions, library, transcoder, cache, background);

    Router router;
    HttpServerOptions httpOptions;
    httpOptions.bindAddress = config.bindAddress;
    httpOptions.port = config.httpPort;
    httpOptions.workerThreads = config.httpThreads;
    HttpServer httpServer(httpOptions, router);

    HlsService hls(library, transcoder, cache, background);
    ApiController api(config, library, transcoder, cache, httpServer, rtspServer);
    hls.registerRoutes(router);
    api.registerRoutes(router);

    // ---- Start listening.
    if (!httpServer.start(error)) {
        LOG_ERROR("main") << "HTTP server: " << error;
        return 1;
    }
    if (!rtspServer.start(error)) {
        LOG_ERROR("main") << "RTSP server: " << error;
        httpServer.stop();
        return 1;
    }

    LOG_INFO("main") << "Ready. Open http://localhost:" << config.httpPort << "/ in your browser.";
    LOG_INFO("main") << "RTSP example: rtsp://localhost:" << config.rtspPort << "/videos/<file name>";
    LOG_INFO("main") << "Press Ctrl+C to stop.";

    int received = 0;
    sigwait(&stopSignals, &received);
    LOG_INFO("main") << "Received " << (received == SIGINT ? "SIGINT" : "SIGTERM") << ", shutting down...";

    // ---- Shut down in reverse order of dependency:
    // stop ffmpeg first (wakes anyone waiting for a stream), then the
    // servers, then the background pool.
    transcoder.shutdown();
    rtspServer.stop();
    httpServer.stop();
    background.shutdown();
    LOG_INFO("main") << "Goodbye.";
    return 0;
}
