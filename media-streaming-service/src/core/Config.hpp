#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace mss {

// All settings of the server. Every field has a sensible default and can be
// changed with a command-line flag (run the server with --help to list them).
struct Config {
    // ---- Network ----
    std::string bindAddress = "0.0.0.0";  // 0.0.0.0 = accept connections on every network interface
    std::uint16_t httpPort = 8080;        // REST API, HLS and the web page
    std::uint16_t rtspPort = 8554;        // RTSP control connections
    std::uint16_t rtpPortMin = 50000;     // UDP port range used for RTP/RTCP over UDP
    std::uint16_t rtpPortMax = 50999;

    // ---- Folders (relative paths are resolved against the project folder) ----
    std::string rootDir;  // empty = detect automatically (folder that contains web/index.html)
    std::string videosDir = "videos";
    std::string audiosDir = "audios";
    std::string cacheDir = "cache";
    std::string webDir = "web";

    // ---- Threads and limits ----
    std::size_t httpThreads = 32;         // worker threads that handle HTTP requests
    std::size_t backgroundThreads = 4;    // threads for probing files and prefetching segments
    std::size_t cacheMegabytes = 256;     // memory budget of the segment cache
    std::size_t maxRtspSessions = 64;     // how many RTSP playback sessions may run at once
    int rtspSessionTimeoutSeconds = 60;   // UDP sessions without any sign of life are closed after this

    // ---- Transcoding / HLS ----
    std::string ffmpegPath = "ffmpeg";
    std::string ffprobePath = "ffprobe";
    std::string videoEncoder = "auto";    // auto, libx264, h264_videotoolbox, ...
    std::string x264Preset = "veryfast";  // speed/quality trade-off of libx264
    int segmentSeconds = 2;               // length of one HLS segment
    int maxRenditions = 4;                // how many video qualities to produce at most
    int maxConcurrentTranscodes = 2;      // how many ffmpeg processes may run at once

    std::string logLevel = "info";
    bool showHelp = false;

    // Reads command-line flags such as "--http-port 9000" or "--http-port=9000".
    // Returns false and fills `error` when a flag is unknown or a value is invalid.
    bool parseArgs(int argc, char** argv, std::string& error);

    // Turns relative folder paths into absolute ones.
    void resolvePaths(const char* argv0);

    static std::string usage();
};

}  // namespace mss
