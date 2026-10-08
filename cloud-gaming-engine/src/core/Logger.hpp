#pragma once

#include <sstream>
#include <string>

namespace cge {

// How important a log message is. Messages below the configured level are skipped.
enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };

// A tiny thread-safe logger that writes one line per message to standard error.
//
// Example output:
//   2026-10-08 12:00:01.123 INFO  [http] Listening on 0.0.0.0:8080
class Logger {
public:
    static void setLevel(LogLevel level);
    static LogLevel level();
    static bool isEnabled(LogLevel level);

    // Understands "debug", "info", "warn" and "error". Returns false for anything else.
    static bool parseLevel(const std::string& text, LogLevel& out);

    // Writes one complete line. Safe to call from many threads at the same time.
    static void write(LogLevel level, const std::string& component, const std::string& message);
};

// Collects one log message with the << operator and writes it when the
// statement ends (that is when the temporary LogLine object is destroyed).
class LogLine {
public:
    LogLine(LogLevel level, const char* component)
        : level_(level), component_(component), enabled_(Logger::isEnabled(level)) {}

    ~LogLine() {
        if (enabled_) Logger::write(level_, component_, stream_.str());
    }

    LogLine(const LogLine&) = delete;
    LogLine& operator=(const LogLine&) = delete;

    template <typename T>
    LogLine& operator<<(const T& value) {
        if (enabled_) stream_ << value;
        return *this;
    }

private:
    LogLevel level_;
    const char* component_;
    bool enabled_;
    std::ostringstream stream_;
};

}  // namespace cge

// Usage:  LOG_INFO("http") << "Listening on port " << port;
#define LOG_DEBUG(component) ::cge::LogLine(::cge::LogLevel::Debug, component)
#define LOG_INFO(component) ::cge::LogLine(::cge::LogLevel::Info, component)
#define LOG_WARN(component) ::cge::LogLine(::cge::LogLevel::Warn, component)
#define LOG_ERROR(component) ::cge::LogLine(::cge::LogLevel::Error, component)
