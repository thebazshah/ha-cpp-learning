#include "core/Process.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>

extern char** environ;  // the current environment, passed on to child programs

namespace mss {
namespace {

void setCloseOnExec(int fd) {
    const int flags = fcntl(fd, F_GETFD);
    if (flags >= 0) fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

// Launches a child process with posix_spawn. On success returns true and
// gives back the child's process id and the read end of its stdout pipe.
bool spawnChild(const std::vector<std::string>& args, const std::string& stderrPath, pid_t& pidOut, int& stdoutFd,
                std::string& error) {
    if (args.empty()) {
        error = "empty command";
        return false;
    }

    int pipeFds[2];
    if (pipe(pipeFds) != 0) {
        error = std::string("pipe() failed: ") + std::strerror(errno);
        return false;
    }
    // Our own copies of the pipe must not leak into other child programs.
    setCloseOnExec(pipeFds[0]);
    setCloseOnExec(pipeFds[1]);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, pipeFds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, stderrPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC,
                                     0644);

    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    short flags = 0;

    // The server blocks SIGINT/SIGTERM in its threads (main() waits for them
    // with sigwait). A child inherits that mask, so clear it - otherwise
    // ffmpeg would ignore our request to stop.
    sigset_t emptyMask;
    sigemptyset(&emptyMask);
    posix_spawnattr_setsigmask(&attributes, &emptyMask);
    flags |= POSIX_SPAWN_SETSIGMASK;

    // The server ignores SIGPIPE. Ignored signals are inherited too, so give
    // the child the normal default behaviour back.
    sigset_t defaultSignals;
    sigemptyset(&defaultSignals);
    sigaddset(&defaultSignals, SIGPIPE);
    posix_spawnattr_setsigdefault(&attributes, &defaultSignals);
    flags |= POSIX_SPAWN_SETSIGDEF;

#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    // macOS extra: close every file descriptor in the child except the three
    // set up above. This stops ffmpeg from accidentally keeping our network
    // sockets open.
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
    posix_spawnattr_setflags(&attributes, flags);

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const std::string& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);

    pid_t pid = 0;
    const int result = posix_spawnp(&pid, args[0].c_str(), &actions, &attributes, argv.data(), environ);

    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    close(pipeFds[1]);  // only the child writes to the pipe

    if (result != 0) {
        close(pipeFds[0]);
        error = "cannot start '" + args[0] + "': " + std::strerror(result);
        return false;
    }
    pidOut = pid;
    stdoutFd = pipeFds[0];
    return true;
}

int decodeExitStatus(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;  // killed by a signal
}

}  // namespace

std::unique_ptr<Process> Process::start(const std::vector<std::string>& args, const std::string& stderrPath,
                                        std::string& error) {
    pid_t pid = 0;
    int stdoutFd = -1;
    if (!spawnChild(args, stderrPath, pid, stdoutFd, error)) return nullptr;
    return std::unique_ptr<Process>(new Process(pid, stdoutFd));
}

ProcessResult Process::run(const std::vector<std::string>& args, int timeoutSeconds) {
    ProcessResult result;
    pid_t pid = 0;
    int stdoutFd = -1;
    if (!spawnChild(args, "/dev/null", pid, stdoutFd, result.error)) return result;
    result.started = true;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
    char buffer[8192];
    for (;;) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   deadline - std::chrono::steady_clock::now())
                                   .count();
        if (remaining <= 0) {
            result.timedOut = true;
            break;
        }
        pollfd waitFor{stdoutFd, POLLIN, 0};
        const int ready = poll(&waitFor, 1, static_cast<int>(remaining));
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) continue;  // timeout is checked at the top of the loop
        const ssize_t count = read(stdoutFd, buffer, sizeof(buffer));
        if (count > 0) {
            result.output.append(buffer, static_cast<std::size_t>(count));
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            break;  // end of output: the program closed stdout (usually because it exited)
        }
    }
    close(stdoutFd);

    if (result.timedOut) kill(pid, SIGKILL);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    result.exitCode = result.timedOut ? -1 : decodeExitStatus(status);
    return result;
}

Process::~Process() {
    // Make sure we never leave a zombie or an orphaned ffmpeg behind.
    bool alreadyReaped;
    {
        std::lock_guard<std::mutex> lock(reapMutex_);
        alreadyReaped = reaped_ || reapIfExitedLocked();
    }
    if (!alreadyReaped) {
        sendSignal(SIGKILL);
        wait();
    }
    if (stdoutFd_ >= 0) close(stdoutFd_);
}

bool Process::readLine(std::string& line) {
    for (;;) {
        const std::size_t newline = pending_.find('\n');
        if (newline != std::string::npos) {
            line = pending_.substr(0, newline);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            pending_.erase(0, newline + 1);
            return true;
        }
        if (stdoutFd_ < 0) return false;

        char buffer[4096];
        const ssize_t count = read(stdoutFd_, buffer, sizeof(buffer));
        if (count > 0) {
            pending_.append(buffer, static_cast<std::size_t>(count));
            continue;
        }
        if (count < 0 && errno == EINTR) continue;

        // End of output. Return whatever is left as a final line.
        close(stdoutFd_);
        stdoutFd_ = -1;
        if (!pending_.empty()) {
            line.swap(pending_);
            pending_.clear();
            return true;
        }
        return false;
    }
}

bool Process::reapIfExitedLocked() {
    if (reaped_) return true;
    int status = 0;
    const pid_t result = waitpid(pid_, &status, WNOHANG);
    if (result == pid_) {
        reaped_ = true;
        exitCode_ = decodeExitStatus(status);
        return true;
    }
    if (result < 0 && errno == ECHILD) {  // somebody else already reaped it
        reaped_ = true;
        return true;
    }
    return false;
}

int Process::wait() {
    // We poll with WNOHANG instead of blocking in waitpid() so that
    // sendSignal() can safely check reaped_ at any time.
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(reapMutex_);
            if (reapIfExitedLocked()) return exitCode_;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

void Process::sendSignal(int signalNumber) {
    std::lock_guard<std::mutex> lock(reapMutex_);
    if (!reaped_) kill(pid_, signalNumber);
}

}  // namespace mss
