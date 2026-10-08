#include "core/Config.hpp"

#include <climits>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <sstream>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include "core/StringUtils.hpp"

namespace cge {
namespace fs = std::filesystem;

namespace {

bool readNumber(const std::string& text, long long minimum, long long maximum, long long& out) {
    long long value = 0;
    if (!str::parseInt64(text, value) || value < minimum || value > maximum) return false;
    out = value;
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
    auto number = [](auto& field, long long minimum, long long maximum) -> Setter {
        return [&field, minimum, maximum](const std::string& value) {
            long long parsed = 0;
            if (!readNumber(value, minimum, maximum, parsed)) return false;
            field = static_cast<std::remove_reference_t<decltype(field)>>(parsed);
            return true;
        };
    };

    const std::map<std::string, Setter> flags = {
        {"--bind", text(bindAddress)},
        {"--port", number(port, 1, 65535)},
        {"--root", text(rootDir)},
        {"--games-dir", text(gamesDir)},
        {"--build-dir", text(buildDir)},
        {"--web-dir", text(webDir)},
        {"--sdk-dir", text(sdkDir)},
        {"--cxx", text(compiler)},
        {"--scan-interval", number(scanIntervalSeconds, 1, 3600)},
        {"--http-threads", number(httpThreads, 2, 512)},
        {"--max-sessions", number(maxSessions, 1, 10000)},
        {"--max-viewers", number(maxViewersPerSession, 1, 1000)},
        {"--idle-timeout", number(idleTimeoutSeconds, 10, 86400)},
        {"--reconnect-grace", number(reconnectGraceSeconds, 0, 3600)},
        {"--log-level", text(logLevel)},
        {"--validate-game", text(validateGame)},
    };

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            showHelp = true;
            continue;
        }
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
    return true;
}

void Config::resolvePaths(const char* argv0) {
    std::error_code ignored;
    fs::path root;
    if (!rootDir.empty()) {
        root = fs::absolute(rootDir, ignored);
    } else {
        // The project folder is the one that contains web/index.html: either
        // the current folder or the parent of the executable's folder (build/).
        const fs::path current = fs::current_path(ignored);
        root = current;
        if (!fs::exists(current / "web" / "index.html", ignored)) {
            const fs::path exeDir = fs::path(executablePath(argv0)).parent_path();
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
        while (folder.size() > 1 && folder.back() == '/') folder.pop_back();
    };
    resolve(gamesDir);
    resolve(buildDir);
    resolve(webDir);
    resolve(sdkDir);
}

std::string executablePath(const char* argv0) {
#if defined(__APPLE__)
    char buffer[PATH_MAX];
    uint32_t size = sizeof(buffer);
    if (_NSGetExecutablePath(buffer, &size) == 0) {
        char resolved[PATH_MAX];
        if (realpath(buffer, resolved) != nullptr) return resolved;
        return buffer;
    }
#elif defined(__linux__)
    std::error_code error;
    const fs::path self = fs::read_symlink("/proc/self/exe", error);
    if (!error) return self.string();
#endif
    std::error_code error;
    return fs::absolute(argv0 ? argv0 : "", error).string();
}

std::string Config::usage() {
    std::ostringstream out;
    out << "Usage: cloud-gaming-engine [options]\n\n"
        << "  --bind ADDRESS          address to listen on (default 0.0.0.0)\n"
        << "  --port PORT             HTTP/WebSocket port (default 8090)\n"
        << "  --root DIR              project folder (default: auto-detect)\n"
        << "  --games-dir DIR         folder with game sources (default games)\n"
        << "  --build-dir DIR         compiled games (default cache/games)\n"
        << "  --web-dir DIR           web frontend (default web)\n"
        << "  --sdk-dir DIR           folder containing cge/GameApi.hpp (default sdk/include)\n"
        << "  --cxx PROGRAM           C++ compiler for games (default c++)\n"
        << "  --scan-interval S       seconds between checks of the games folder (default 3)\n"
        << "  --http-threads N        HTTP worker threads (default 16)\n"
        << "  --max-sessions N        sessions at the same time (default 100)\n"
        << "  --max-viewers N         players + spectators per session (default 32)\n"
        << "  --idle-timeout S        remove sessions nobody watched for S seconds (default 600)\n"
        << "  --reconnect-grace S     keep a disconnected player's slot for S seconds (default 60)\n"
        << "  --log-level LEVEL       debug, info, warn, error (default info)\n"
        << "  --help                  show this help\n";
    return out.str();
}

}  // namespace cge
