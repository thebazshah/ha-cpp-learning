#pragma once

#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>

#include "core/Json.hpp"

namespace mss {

// Server-side adaptive bitrate (ABR) logic for RTSP sessions.
//
// With HLS the *player* chooses the quality. With RTSP the *server* pushes
// the media, so the server must choose. We watch two signals:
//
//  1. Lateness: we schedule every frame for an exact moment. If a TCP client
//     cannot keep up, send() blocks and we fall behind schedule. Growing
//     lateness means "the network is slower than this bitrate".
//  2. Packet loss: RTCP receiver reports (sent by the player) tell us which
//     fraction of the UDP packets got lost.
//
// At every segment boundary (where all qualities have a key frame, so a
// switch is seamless) chooseNext() decides:
//  * congested  -> go one level down immediately;
//  * healthy for a while -> try one level up;
//  * after a downgrade, wait twice as long before trying to go up again
//    (exponential back-off), so we do not keep bouncing between two levels.
class AbrController {
public:
    AbrController(std::size_t levelCount, std::size_t initialLevel, bool enabled);

    void reportLateness(double secondsBehind);  // called for every frame sent
    void reportLoss(double fractionLost);       // called for every RTCP receiver report
    void reportStall();                         // called when sending stalled badly

    // Decides the level for the next segment and returns it.
    std::size_t chooseNext();

    std::size_t current() const;
    Json toJson() const;

private:
    mutable std::mutex mutex_;
    std::size_t levelCount_;
    std::size_t current_;
    bool enabled_;

    // Signals collected since the last decision.
    double windowMaxLateness_ = 0;
    double windowMaxLoss_ = 0;
    bool windowStalled_ = false;
    double previousLateness_ = 0;

    // Last values, for the statistics page.
    double lastLateness_ = 0;
    double lastLoss_ = 0;

    std::chrono::steady_clock::time_point lastSwitch_;
    std::chrono::steady_clock::time_point lastDowngrade_;
    double upgradeHoldSeconds_;
    int upgrades_ = 0;
    int downgrades_ = 0;
    std::string lastDecision_ = "start";
};

}  // namespace mss
