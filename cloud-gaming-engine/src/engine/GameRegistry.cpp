#include "engine/GameRegistry.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <sstream>

#include "core/Logger.hpp"
#include "core/Process.hpp"
#include "core/StringUtils.hpp"

namespace cge {
namespace fs = std::filesystem;

namespace {

constexpr int kCompileTimeoutSeconds = 180;
constexpr int kValidationTimeoutSeconds = 30;

bool isValidId(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    for (char c : id) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') return false;
    }
    return true;
}

// Size + modification time (in nanoseconds when available) of a file, as text.
std::string fileStamp(const std::string& path) {
    struct stat facts {};
    if (stat(path.c_str(), &facts) != 0) return "missing";
#if defined(__APPLE__)
    const long long nanos = static_cast<long long>(facts.st_mtimespec.tv_sec) * 1000000000LL + facts.st_mtimespec.tv_nsec;
#else
    const long long nanos = static_cast<long long>(facts.st_mtim.tv_sec) * 1000000000LL + facts.st_mtim.tv_nsec;
#endif
    return std::to_string(facts.st_size) + "@" + std::to_string(nanos);
}

// Keeps the end of a long text (compiler errors can be very long).
std::string tail(const std::string& text, std::size_t maxLines, std::size_t maxChars) {
    std::vector<std::string> lines = str::split(text, '\n');
    while (!lines.empty() && str::trim(lines.back()).empty()) lines.pop_back();
    std::size_t start = lines.size() > maxLines ? lines.size() - maxLines : 0;
    std::string out;
    if (start > 0) out = "... (" + std::to_string(start) + " earlier lines omitted)\n";
    for (std::size_t i = start; i < lines.size(); ++i) out += lines[i] + "\n";
    if (out.size() > maxChars) out = "..." + out.substr(out.size() - maxChars);
    return out;
}

long long nowUnix() { return static_cast<long long>(std::time(nullptr)); }

}  // namespace

const char* gameStateName(GameState state) {
    switch (state) {
        case GameState::Building: return "building";
        case GameState::Ready: return "ready";
        case GameState::BuildFailed: return "build_failed";
        case GameState::Invalid: return "invalid";
    }
    return "unknown";
}

GameRegistry::GameRegistry(Options options) : options_(std::move(options)) {
    std::error_code ignored;
    fs::create_directories(options_.buildDir, ignored);
    fs::create_directories(options_.gamesDir, ignored);
}

GameRegistry::~GameRegistry() { stop(); }

std::string GameRegistry::compilerFlagsText() const {
    return options_.compiler + " -std=c++17 -O2 -fPIC -shared -fvisibility=hidden";
}

void GameRegistry::startWatching() {
    if (running_.exchange(true)) return;
    watcher_ = std::thread([this] {
        // Scan right away, then every few seconds.
        while (running_) {
            scanOnce();
            std::unique_lock<std::mutex> lock(wakeMutex_);
            wake_.wait_for(lock, std::chrono::seconds(options_.scanIntervalSeconds), [this] { return !running_; });
        }
    });
}

void GameRegistry::stop() {
    if (!running_.exchange(false)) return;
    wake_.notify_all();
    if (watcher_.joinable()) watcher_.join();
}

void GameRegistry::scanNow() { scanOnce(); }

void GameRegistry::scanOnce() {
    std::lock_guard<std::mutex> scanLock(scanMutex_);
    std::error_code error;

    // 1. Find the game folders and what is inside them.
    struct Candidate {
        std::string id;
        std::string folder;
        std::vector<std::string> sources;  // .cpp/.cc/.cxx
        std::string prebuilt;              // game.so / game.dylib, if any
        std::string fingerprint;
    };
    std::vector<Candidate> candidates;
    const std::string sdkHeader = options_.sdkIncludeDir + "/cge/GameApi.hpp";

    for (fs::directory_iterator it(options_.gamesDir, error), end; !error && it != end; it.increment(error)) {
        if (!it->is_directory(error)) continue;
        Candidate candidate;
        candidate.id = it->path().filename().string();
        candidate.folder = it->path().string();
        if (candidate.id.empty() || candidate.id[0] == '.') continue;

        std::vector<std::string> stamped;  // every file that affects the build
        for (fs::recursive_directory_iterator file(candidate.folder, error), fileEnd; !error && file != fileEnd;
             file.increment(error)) {
            if (!file->is_regular_file(error)) continue;
            const std::string path = file->path().string();
            const std::string extension = str::fileExtension(path);
            const std::string name = file->path().filename().string();
            if (extension == "cpp" || extension == "cc" || extension == "cxx") {
                candidate.sources.push_back(path);
            } else if (name == "game.so" || name == "game.dylib") {
                candidate.prebuilt = path;
            } else if (extension != "h" && extension != "hpp" && extension != "hh" && extension != "inl") {
                continue;  // README files, images, ... do not affect the build
            }
            stamped.push_back(path.substr(candidate.folder.size()) + "=" + fileStamp(path));
        }
        error.clear();
        std::sort(candidate.sources.begin(), candidate.sources.end());
        std::sort(stamped.begin(), stamped.end());

        // The fingerprint changes whenever a source file, the SDK header,
        // the compiler flags or the SDK version changes.
        std::string material = compilerFlagsText() + "|api" + std::to_string(kApiVersion) + "|" + fileStamp(sdkHeader);
        for (const std::string& stamp : stamped) material += "|" + stamp;
        char hash[17];
        std::snprintf(hash, sizeof(hash), "%016llx", static_cast<unsigned long long>(str::fnv1a64(material)));
        candidate.fingerprint = hash;
        candidates.push_back(std::move(candidate));
    }

    // 2. Forget games whose folder was deleted.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = games_.begin(); it != games_.end();) {
            const bool stillThere = std::any_of(candidates.begin(), candidates.end(),
                                                [&](const Candidate& c) { return c.id == it->first; });
            if (!stillThere) {
                LOG_INFO("games") << "Game '" << it->first << "' was removed";
                // Delete its compiled libraries too (sessions that are still
                // running keep working: the loaded copy stays in memory).
                std::error_code ignored;
                for (fs::directory_iterator file(options_.buildDir, ignored), end; !ignored && file != end;
                     file.increment(ignored)) {
                    const std::string name = file->path().filename().string();
                    if (str::startsWith(name, it->first + "-") && name.size() == it->first.size() + 1 + 16 + 3) {
                        fs::remove(file->path(), ignored);
                    }
                }
                it = games_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // 3. (Re)build every game that is new or changed.
    for (const Candidate& candidate : candidates) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto existing = games_.find(candidate.id);
            if (existing != games_.end() && existing->second.fingerprint == candidate.fingerprint) continue;
        }

        GameEntry entry;
        entry.id = candidate.id;
        entry.folder = candidate.folder;
        entry.fingerprint = candidate.fingerprint;
        entry.updatedUnix = nowUnix();

        if (!isValidId(candidate.id)) {
            entry.state = GameState::Invalid;
            entry.error = "The folder name must only use letters, digits, '-' and '_' (it becomes part of URLs).";
        } else if (candidate.sources.empty() && candidate.prebuilt.empty()) {
            entry.state = GameState::Invalid;
            entry.error = "No C++ source files (*.cpp) and no prebuilt game.so found in this folder.";
        } else {
            // Show "building" in the lobby while we work (keep the old library
            // available until the new one is ready).
            {
                std::lock_guard<std::mutex> lock(mutex_);
                GameEntry& shown = games_[candidate.id];
                const bool firstBuild = shown.id.empty();
                shown.id = candidate.id;
                shown.folder = candidate.folder;
                if (firstBuild || shown.state != GameState::Ready) shown.state = GameState::Building;
            }
            buildGame(entry, candidate.sources, candidate.prebuilt);
        }

        std::lock_guard<std::mutex> lock(mutex_);
        GameEntry& stored = games_[candidate.id];
        if (entry.state != GameState::Ready && stored.state == GameState::Ready && stored.library) {
            // Keep serving the last working version, but show the new problem.
            LOG_WARN("games") << "New version of '" << candidate.id << "' failed; keeping the previous version";
            stored.fingerprint = entry.fingerprint;
            stored.error = "The latest change could not be used (the previous version is still playable):\n" + entry.error;
            stored.updatedUnix = entry.updatedUnix;
        } else {
            stored = std::move(entry);
        }
    }
}

void GameRegistry::buildGame(GameEntry& entry, const std::vector<std::string>& sources,
                             const std::string& prebuiltLibrary) {
    const auto started = std::chrono::steady_clock::now();
    std::string libraryPath;

    if (!sources.empty()) {
        // ---- compile the sources into a shared library
        libraryPath = options_.buildDir + "/" + entry.id + "-" + entry.fingerprint + ".so";
        std::error_code ignored;
        if (!fs::exists(libraryPath, ignored)) {
            LOG_INFO("games") << "Compiling game '" << entry.id << "' (" << sources.size() << " source file"
                              << (sources.size() == 1 ? "" : "s") << ")";
            const std::string temporary = libraryPath + ".tmp";
            std::vector<std::string> command = {options_.compiler, "-std=c++17", "-O2", "-fPIC", "-shared",
                                                "-fvisibility=hidden", "-Wall", "-I", options_.sdkIncludeDir};
            command.insert(command.end(), sources.begin(), sources.end());
            command.insert(command.end(), {"-o", temporary});
            ++builds_;
            const ProcessResult result = runProcess(command, kCompileTimeoutSeconds);
            if (!result.started) {
                entry.state = GameState::BuildFailed;
                entry.error = "Cannot run the C++ compiler '" + options_.compiler + "': " + result.error;
                return;
            }
            if (result.timedOut || result.exitCode != 0) {
                fs::remove(temporary, ignored);
                entry.state = GameState::BuildFailed;
                entry.error = result.timedOut ? "The compiler took too long." : tail(result.output, 40, 6000);
                LOG_WARN("games") << "Game '" << entry.id << "' does not compile";
                return;
            }
            fs::rename(temporary, libraryPath, ignored);
            // Delete older builds of this game (running sessions keep their copy in memory).
            for (fs::directory_iterator it(options_.buildDir, ignored), end; !ignored && it != end; it.increment(ignored)) {
                const std::string name = it->path().filename().string();
                if (str::startsWith(name, entry.id + "-") && it->path().string() != libraryPath &&
                    name.size() == entry.id.size() + 1 + 16 + 3) {
                    fs::remove(it->path(), ignored);
                }
            }
        }
    } else {
        entry.prebuilt = true;
        libraryPath = prebuiltLibrary;
    }

    // ---- test-play it in a separate process
    const ProcessResult test =
        runProcess({options_.validatorProgram, "--validate-game", libraryPath}, kValidationTimeoutSeconds);
    if (!test.started || test.timedOut || test.crashed || test.exitCode != 0) {
        entry.state = GameState::Invalid;
        if (!test.started) {
            entry.error = "Cannot start the game validator: " + test.error;
        } else if (test.timedOut) {
            entry.error = "The test run did not finish within " + std::to_string(kValidationTimeoutSeconds) +
                          " seconds (endless loop?).";
        } else if (test.crashed) {
            entry.error = "The game crashed during the test run (for example a null pointer or an out-of-range "
                          "access).\n" + tail(test.output, 10, 2000);
        } else {
            entry.error = "The game failed the test run: " + tail(test.output, 10, 2000);
        }
        LOG_WARN("games") << "Game '" << entry.id << "' failed validation";
        return;
    }

    // ---- load it into the server
    std::string error;
    std::shared_ptr<GameLibrary> library = GameLibrary::load(libraryPath, error);
    if (!library) {
        entry.state = GameState::Invalid;
        entry.error = error;
        return;
    }
    entry.state = GameState::Ready;
    entry.library = library;
    entry.info = library->info();
    entry.buildSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    LOG_INFO("games") << "Game '" << entry.id << "' (" << entry.info.name << ") is ready ("
                      << str::formatDouble(entry.buildSeconds, 2) << " s)";
}

std::vector<GameEntry> GameRegistry::list() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<GameEntry> entries;
    for (const auto& item : games_) entries.push_back(item.second);
    return entries;
}

std::optional<GameEntry> GameRegistry::find(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = games_.find(id);
    if (it == games_.end()) return std::nullopt;
    return it->second;
}

std::shared_ptr<GameLibrary> GameRegistry::library(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = games_.find(id);
    if (it == games_.end() || it->second.state != GameState::Ready) return nullptr;
    return it->second.library;
}

Json GameRegistry::statsJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    int ready = 0, building = 0, failed = 0;
    for (const auto& item : games_) {
        if (item.second.state == GameState::Ready) ++ready;
        else if (item.second.state == GameState::Building) ++building;
        else ++failed;
    }
    Json stats = Json::object();
    stats.set("total", games_.size())
        .set("ready", ready)
        .set("building", building)
        .set("failed", failed)
        .set("compilerRuns", builds_.load());
    return stats;
}

}  // namespace cge
