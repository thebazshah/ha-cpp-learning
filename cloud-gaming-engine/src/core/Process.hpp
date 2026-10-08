#pragma once

#include <string>
#include <vector>

namespace cge {

// Result of running an external program with runProcess().
struct ProcessResult {
    bool started = false;   // false if the program could not be launched at all
    bool timedOut = false;  // true if we had to kill it because it took too long
    bool crashed = false;   // true if a signal (for example a segmentation fault) ended it
    int exitCode = -1;      // the program's exit code (-1 if it did not exit normally)
    std::string output;     // everything it printed (standard output and standard error together)
    std::string error;      // why the launch failed (when started == false)
};

// Runs a program and waits for it to finish. Used for two jobs:
//  * compiling a game with the C++ compiler (the output holds compiler errors);
//  * testing a freshly built game in a separate "validator" process, so a
//    game that crashes cannot take the server down with it.
// The program is killed if it runs longer than `timeoutSeconds`.
ProcessResult runProcess(const std::vector<std::string>& args, int timeoutSeconds);

}  // namespace cge
