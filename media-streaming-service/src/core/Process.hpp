#pragma once

#include <sys/types.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mss {

// Result of running a program to completion with Process::run().
struct ProcessResult {
    bool started = false;   // false if the program could not be launched at all
    bool timedOut = false;  // true if we had to kill it because it took too long
    int exitCode = -1;      // the program's exit code (-1 if it was killed by a signal)
    std::string output;     // everything the program printed to standard output
    std::string error;      // why the launch failed (when started == false)
};

// Starts and controls an external program. We use it to run ffmpeg (which
// encodes H.264/AAC) and ffprobe (which reads media file details).
//
// The child's standard output is connected to a pipe we can read line by
// line. Its standard error goes to a log file, and its standard input is
// /dev/null so it can never wait for keyboard input.
class Process {
public:
    // Starts the program args[0] (looked up in PATH) with the given arguments.
    // Returns nullptr and fills `error` when the program cannot be started.
    static std::unique_ptr<Process> start(const std::vector<std::string>& args, const std::string& stderrPath,
                                          std::string& error);

    // Runs a program, collects its standard output and waits for it to exit.
    // The program is killed if it runs longer than timeoutSeconds.
    static ProcessResult run(const std::vector<std::string>& args, int timeoutSeconds);

    ~Process();
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    // Reads one line of the program's standard output (without the newline).
    // Blocks until a full line is available. Returns false at end of output.
    // Only one thread should call this.
    bool readLine(std::string& line);

    // Waits until the program has exited and returns its exit code
    // (-1 if a signal killed it). Only one thread should call this.
    int wait();

    // Sends a signal (for example SIGTERM) if the program is still running.
    // Safe to call from any thread, even while another thread is in wait().
    void sendSignal(int signalNumber);

    pid_t pid() const { return pid_; }

private:
    Process(pid_t pid, int stdoutFd) : pid_(pid), stdoutFd_(stdoutFd) {}

    // Checks whether the child has exited (without blocking). Must hold reapMutex_.
    bool reapIfExitedLocked();

    pid_t pid_;
    int stdoutFd_;
    std::string pending_;  // bytes read from the pipe that do not yet form a full line

    // Protects reaped_/exitCode_. After a child has been reaped its process
    // id may be reused by the system, so sendSignal() must never signal it
    // again - this mutex makes that check safe.
    std::mutex reapMutex_;
    bool reaped_ = false;
    int exitCode_ = -1;
};

}  // namespace mss
