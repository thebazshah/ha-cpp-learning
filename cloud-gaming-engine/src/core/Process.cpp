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

extern char** environ;  // the current environment, passed on to child programs

namespace cge {
namespace {

void setCloseOnExec(int fd) {
    const int flags = fcntl(fd, F_GETFD);
    if (flags >= 0) fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

}  // namespace

ProcessResult runProcess(const std::vector<std::string>& args, int timeoutSeconds) {
    ProcessResult result;
    if (args.empty()) {
        result.error = "empty command";
        return result;
    }

    int pipeFds[2];
    if (pipe(pipeFds) != 0) {
        result.error = std::string("pipe() failed: ") + std::strerror(errno);
        return result;
    }
    setCloseOnExec(pipeFds[0]);
    setCloseOnExec(pipeFds[1]);

    // The child gets: stdin = /dev/null, stdout and stderr = our pipe.
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, pipeFds[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipeFds[1], STDERR_FILENO);

    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    short flags = 0;
    // main() blocks SIGINT/SIGTERM in all threads (it waits for them with
    // sigwait). Children inherit the mask, so give them an empty one.
    sigset_t emptyMask;
    sigemptyset(&emptyMask);
    posix_spawnattr_setsigmask(&attributes, &emptyMask);
    flags |= POSIX_SPAWN_SETSIGMASK;
    // The server ignores SIGPIPE; restore the default for the child.
    sigset_t defaultSignals;
    sigemptyset(&defaultSignals);
    sigaddset(&defaultSignals, SIGPIPE);
    posix_spawnattr_setsigdefault(&attributes, &defaultSignals);
    flags |= POSIX_SPAWN_SETSIGDEF;
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    // macOS: close every other descriptor (our sockets!) in the child.
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
    posix_spawnattr_setflags(&attributes, flags);

    std::vector<char*> argv;
    for (const std::string& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);

    pid_t pid = 0;
    const int spawnResult = posix_spawnp(&pid, args[0].c_str(), &actions, &attributes, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    close(pipeFds[1]);
    if (spawnResult != 0) {
        close(pipeFds[0]);
        result.error = "cannot start '" + args[0] + "': " + std::strerror(spawnResult);
        return result;
    }
    result.started = true;

    // Collect the output until the program closes its end of the pipe or the time is up.
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
        pollfd waitFor{pipeFds[0], POLLIN, 0};
        const int ready = poll(&waitFor, 1, static_cast<int>(remaining));
        if (ready <= 0) continue;  // timeout or EINTR: re-check the deadline
        const ssize_t count = read(pipeFds[0], buffer, sizeof(buffer));
        if (count > 0) {
            result.output.append(buffer, static_cast<std::size_t>(count));
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            break;  // end of output
        }
    }
    close(pipeFds[0]);

    if (result.timedOut) kill(pid, SIGKILL);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (WIFEXITED(status) && !result.timedOut) {
        result.exitCode = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status) && !result.timedOut) {
        result.crashed = true;
    }
    return result;
}

}  // namespace cge
