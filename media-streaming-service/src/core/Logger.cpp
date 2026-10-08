#include "core/Logger.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace mss {
namespace {

std::atomic<int> gLevel{static_cast<int>(LogLevel::Info)};

// Only one thread may print at a time, otherwise lines from different
// threads could get mixed together.
std::mutex gWriteMutex;

const char* levelName(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO ";
        case LogLevel::Warn: return "WARN ";
        case LogLevel::Error: return "ERROR";
    }
    return "?????";
}

}  // namespace

void Logger::setLevel(LogLevel level) { gLevel.store(static_cast<int>(level)); }

LogLevel Logger::level() { return static_cast<LogLevel>(gLevel.load()); }

bool Logger::isEnabled(LogLevel level) { return static_cast<int>(level) >= gLevel.load(); }

bool Logger::parseLevel(const std::string& text, LogLevel& out) {
    if (text == "debug") {
        out = LogLevel::Debug;
    } else if (text == "info") {
        out = LogLevel::Info;
    } else if (text == "warn" || text == "warning") {
        out = LogLevel::Warn;
    } else if (text == "error") {
        out = LogLevel::Error;
    } else {
        return false;
    }
    return true;
}

void Logger::write(LogLevel level, const std::string& component, const std::string& message) {
    using namespace std::chrono;

    // Build a "YYYY-MM-DD HH:MM:SS.mmm" timestamp in local time.
    const auto now = system_clock::now();
    const std::time_t seconds = system_clock::to_time_t(now);
    const long long millis = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm local{};
    localtime_r(&seconds, &local);
    char timeText[32];
    std::strftime(timeText, sizeof(timeText), "%Y-%m-%d %H:%M:%S", &local);

    std::lock_guard<std::mutex> lock(gWriteMutex);
    std::fprintf(stderr, "%s.%03lld %s [%s] %s\n", timeText, millis, levelName(level), component.c_str(),
                 message.c_str());
    std::fflush(stderr);
}

}  // namespace mss
