#include "core/Config.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <sstream>

#include "core/StringUtils.hpp"

namespace mss {
namespace fs = std::filesystem;

namespace {

// Helpers that convert a flag value and check its range.
bool readPort(const std::string& text, std::uint16_t& out) {
    long long value = 0;
    if (!str::parseInt64(text, value) || value < 1 || value > 65535) return false;
    out = static_cast<std::uint16_t>(value);
    return true;
}

bool readCount(const std::string& text, std::size_t& out, long long minimum, long long maximum) {
    long long value = 0;
    if (!str::parseInt64(text, value) || value < minimum || value > maximum) return false;
    out = static_cast<std::size_t>(value);
    return true;
}

bool readInt(const std::string& text, int& out, long long minimum, long long maximum) {
    long long value = 0;
    if (!str::parseInt64(text, value) || value < minimum || value > maximum) return false;
    out = static_cast<int>(value);
    return true;
}

}  // namespace

bool Config::parseArgs(int argc, char** argv, std::string& error) {
    using Setter = std::function<bool(const std::string&)>;
    auto text = [](std::string& field) -> Setter {
        return [&field](const std::string& value) {
            field = value;
            return !value.empty();
        };
    };

    // Every supported flag and the code that stores its value.
    const std::map<std::string, Setter> flags = {
        {"--bind", text(bindAddress)},
        {"--http-port", [this](const std::string& v) { return readPort(v, httpPort); }},
        {"--rtsp-port", [this](const std::string& v) { return readPort(v, rtspPort); }},
        {"--rtp-port-min", [this](const std::string& v) { return readPort(v, rtpPortMin); }},
        {"--rtp-port-max", [this](const std::string& v) { return readPort(v, rtpPortMax); }},
        {"--root", text(rootDir)},
        {"--videos-dir", text(videosDir)},
        {"--audios-dir", text(audiosDir)},
        {"--cache-dir", text(cacheDir)},
        {"--web-dir", text(webDir)},
        {"--http-threads", [this](const std::string& v) { return readCount(v, httpThreads, 2, 1024); }},
        {"--background-threads", [this](const std::string& v) { return readCount(v, backgroundThreads, 1, 256); }},
        {"--cache-mb", [this](const std::string& v) { return readCount(v, cacheMegabytes, 0, 65536); }},
        {"--max-rtsp-sessions", [this](const std::string& v) { return readCount(v, maxRtspSessions, 1, 10000); }},
        {"--rtsp-session-timeout", [this](const std::string& v) { return readInt(v, rtspSessionTimeoutSeconds, 5, 3600); }},
        {"--ffmpeg", text(ffmpegPath)},
        {"--ffprobe", text(ffprobePath)},
        {"--video-encoder", text(videoEncoder)},
        {"--x264-preset", text(x264Preset)},
        {"--segment-seconds", [this](const std::string& v) { return readInt(v, segmentSeconds, 1, 10); }},
        {"--max-renditions", [this](const std::string& v) { return readInt(v, maxRenditions, 1, 5); }},
        {"--max-transcodes", [this](const std::string& v) { return readInt(v, maxConcurrentTranscodes, 1, 16); }},
        {"--log-level", text(logLevel)},
    };

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            showHelp = true;
            continue;
        }

        // Accept both "--flag value" and "--flag=value".
        std::string value;
        bool hasValue = false;
        const std::size_t equals = arg.find('=');
        if (equals != std::string::npos) {
            value = arg.substr(equals + 1);
            arg = arg.substr(0, equals);
            hasValue = true;
        }

        const auto flag = flags.find(arg);
        if (flag == flags.end()) {
            error = "Unknown option: " + arg;
            return false;
        }
        if (!hasValue) {
            if (i + 1 >= argc) {
                error = "Missing value for " + arg;
                return false;
            }
            value = argv[++i];
        }
        if (!flag->second(value)) {
            error = "Invalid value for " + arg + ": '" + value + "'";
            return false;
        }
    }

    if (rtpPortMin % 2 != 0) ++rtpPortMin;  // RTP ports must be even (RTCP uses the next odd port)
    if (rtpPortMax < rtpPortMin + 1) {
        error = "--rtp-port-max must be at least --rtp-port-min + 1";
        return false;
    }
    return true;
}

void Config::resolvePaths(const char* argv0) {
    std::error_code ignored;
    fs::path root;

    if (!rootDir.empty()) {
        root = fs::absolute(rootDir, ignored);
    } else {
        // Find the project folder: the one that contains web/index.html.
        // First look at the current folder, then near the executable
        // (for example when it is started as ./build/media-streaming-service).
        const fs::path currentDir = fs::current_path(ignored);
        root = currentDir;
        if (!fs::exists(currentDir / "web" / "index.html", ignored) && argv0 != nullptr &&
            std::string(argv0).find('/') != std::string::npos) {
            const fs::path exeDir = fs::absolute(argv0, ignored).parent_path();
            for (const fs::path& candidate : {exeDir.parent_path(), exeDir}) {
                if (fs::exists(candidate / "web" / "index.html", ignored)) {
                    root = candidate;
                    break;
                }
            }
        }
    }
    root = root.lexically_normal();
    rootDir = root.string();

    auto resolve = [&root](std::string& folder) {
        fs::path path(folder);
        if (path.is_relative()) path = root / path;
        folder = path.lexically_normal().string();
        // Remove a trailing slash so later "folder + '/' + name" joins look clean.
        while (folder.size() > 1 && folder.back() == '/') folder.pop_back();
    };
    resolve(videosDir);
    resolve(audiosDir);
    resolve(cacheDir);
    resolve(webDir);
}

std::string Config::usage() {
    std::ostringstream out;
    out << "Usage: media-streaming-service [options]\n\n"
        << "Network:\n"
        << "  --bind ADDRESS             address to listen on (default 0.0.0.0)\n"
        << "  --http-port PORT           HTTP port for REST API, HLS and web UI (default 8080)\n"
        << "  --rtsp-port PORT           RTSP port (default 8554)\n"
        << "  --rtp-port-min PORT        first UDP port for RTP over UDP (default 50000)\n"
        << "  --rtp-port-max PORT        last UDP port for RTP over UDP (default 50999)\n\n"
        << "Folders (relative to the project folder):\n"
        << "  --root DIR                 project folder (default: auto-detect)\n"
        << "  --videos-dir DIR           video files (default videos)\n"
        << "  --audios-dir DIR           audio files (default audios)\n"
        << "  --cache-dir DIR            transcoded HLS output (default cache)\n"
        << "  --web-dir DIR              web frontend files (default web)\n\n"
        << "Performance:\n"
        << "  --http-threads N           HTTP worker threads (default 32)\n"
        << "  --background-threads N     probe/prefetch threads (default 4)\n"
        << "  --cache-mb N               segment cache size in MB (default 256)\n"
        << "  --max-rtsp-sessions N      concurrent RTSP sessions (default 64)\n"
        << "  --rtsp-session-timeout S   idle UDP session timeout in seconds (default 60)\n\n"
        << "Transcoding:\n"
        << "  --ffmpeg PATH              ffmpeg program (default ffmpeg)\n"
        << "  --ffprobe PATH             ffprobe program (default ffprobe)\n"
        << "  --video-encoder NAME       auto, libx264, h264_videotoolbox (default auto)\n"
        << "  --x264-preset NAME         libx264 preset (default veryfast)\n"
        << "  --segment-seconds N        HLS segment length 1-10 (default 2)\n"
        << "  --max-renditions N         video qualities to produce 1-5 (default 4)\n"
        << "  --max-transcodes N         ffmpeg processes at once (default 2)\n\n"
        << "Other:\n"
        << "  --log-level LEVEL          debug, info, warn, error (default info)\n"
        << "  --help                     show this help\n";
    return out.str();
}

}  // namespace mss
