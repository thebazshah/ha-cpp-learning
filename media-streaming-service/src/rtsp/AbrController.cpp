#include "rtsp/AbrController.hpp"

#include <algorithm>

namespace mss {
namespace {

constexpr double kBaseUpgradeHold = 6.0;   // seconds of good network before trying a higher level
constexpr double kMaxUpgradeHold = 60.0;
constexpr double kLossTooHigh = 0.08;      // more than 8% lost packets = congested
constexpr double kLossHealthy = 0.02;
constexpr double kLatenessHealthy = 0.15;  // seconds behind schedule that count as "on time"
constexpr double kLatenessGrowing = 0.25;  // falling this much further behind within one segment = congested
constexpr double kLatenessCritical = 2.0;  // this far behind = congested no matter what

}  // namespace

AbrController::AbrController(std::size_t levelCount, std::size_t initialLevel, bool enabled)
    : levelCount_(std::max<std::size_t>(1, levelCount)),
      current_(std::min(initialLevel, std::max<std::size_t>(1, levelCount) - 1)),
      enabled_(enabled && levelCount > 1),
      lastSwitch_(std::chrono::steady_clock::now()),
      lastDowngrade_(std::chrono::steady_clock::now()),
      upgradeHoldSeconds_(kBaseUpgradeHold) {}

void AbrController::reportLateness(double secondsBehind) {
    std::lock_guard<std::mutex> lock(mutex_);
    windowMaxLateness_ = std::max(windowMaxLateness_, secondsBehind);
    lastLateness_ = secondsBehind;
}

void AbrController::reportLoss(double fractionLost) {
    std::lock_guard<std::mutex> lock(mutex_);
    windowMaxLoss_ = std::max(windowMaxLoss_, fractionLost);
    lastLoss_ = fractionLost;
}

void AbrController::reportStall() {
    std::lock_guard<std::mutex> lock(mutex_);
    windowStalled_ = true;
}

std::size_t AbrController::chooseNext() {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    const double lateness = windowMaxLateness_;
    const double growth = lateness - previousLateness_;
    const double secondsSinceSwitch = std::chrono::duration<double>(now - lastSwitch_).count();
    const double secondsSinceDowngrade = std::chrono::duration<double>(now - lastDowngrade_).count();

    const bool congested = windowStalled_ || windowMaxLoss_ > kLossTooHigh ||
                           (lateness > 0.5 && growth > kLatenessGrowing) || lateness > kLatenessCritical;
    const bool healthy = windowMaxLoss_ < kLossHealthy && lateness < kLatenessHealthy;

    if (enabled_) {
        if (congested && current_ > 0) {
            --current_;
            ++downgrades_;
            lastSwitch_ = now;
            lastDowngrade_ = now;
            upgradeHoldSeconds_ = std::min(upgradeHoldSeconds_ * 2.0, kMaxUpgradeHold);
            lastDecision_ = windowMaxLoss_ > kLossTooHigh ? "down: packet loss" : "down: falling behind schedule";
        } else if (healthy && current_ + 1 < levelCount_ && secondsSinceSwitch >= upgradeHoldSeconds_) {
            ++current_;
            ++upgrades_;
            lastSwitch_ = now;
            lastDecision_ = "up: network is keeping up";
        } else if (healthy && secondsSinceDowngrade > kMaxUpgradeHold) {
            upgradeHoldSeconds_ = kBaseUpgradeHold;  // things have been good for a long time: be optimistic again
        }
    }

    previousLateness_ = lateness;
    windowMaxLateness_ = 0;
    windowMaxLoss_ = 0;
    windowStalled_ = false;
    return current_;
}

std::size_t AbrController::current() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_;
}

Json AbrController::toJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Json json = Json::object();
    json.set("enabled", enabled_)
        .set("level", current_)
        .set("levels", levelCount_)
        .set("upgrades", upgrades_)
        .set("downgrades", downgrades_)
        .set("lastDecision", lastDecision_)
        .set("latenessSeconds", lastLateness_)
        .set("packetLoss", lastLoss_)
        .set("upgradeHoldSeconds", upgradeHoldSeconds_);
    return json;
}

}  // namespace mss
