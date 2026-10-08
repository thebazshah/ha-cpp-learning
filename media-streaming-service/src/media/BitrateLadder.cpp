#include "media/BitrateLadder.hpp"

#include <algorithm>

namespace mss {
namespace {

struct Rung {
    int shortSide;
    int videoKbps;
    int audioKbps;
};

// The full ladder, from lowest to highest quality.
const Rung kVideoRungs[] = {
    {240, 400, 64},
    {360, 800, 96},
    {480, 1400, 128},
    {720, 2800, 128},
    {1080, 5000, 160},
};

// MPEG-TS adds roughly 10% overhead (packet headers, PES headers, tables).
constexpr double kTsOverhead = 1.10;

}  // namespace

long long Rendition::peakBandwidth() const {
    const double bits = (static_cast<double>(maxRateKbps()) + audioKbps) * 1000.0 * kTsOverhead;
    return static_cast<long long>(bits);
}

long long Rendition::averageBandwidth() const {
    const double bits = (static_cast<double>(videoKbps) + audioKbps) * 1000.0 * kTsOverhead;
    return static_cast<long long>(bits);
}

std::string Rendition::label() const {
    if (hasVideo()) return std::to_string(shortSide) + "p";
    return std::to_string(audioKbps) + " kbps";
}

std::vector<Rendition> buildLadder(const MediaInfo& info, bool wantVideo, int maxRenditions) {
    std::vector<Rendition> ladder;
    const int audioOnlyRates[] = {64, 128, 192};

    if (!wantVideo || !info.hasVideo) {
        if (!info.hasAudio) return ladder;  // nothing we can stream
        for (int kbps : audioOnlyRates) {
            Rendition rendition;
            rendition.audioKbps = kbps;
            ladder.push_back(rendition);
        }
    } else {
        // The short side decides the "p" number for both landscape and
        // portrait videos (a 1080x1920 phone video is "1080p" too).
        const int sourceShortSide = std::min(info.width, info.height);
        std::vector<Rung> chosen;
        for (const Rung& rung : kVideoRungs) {
            if (rung.shortSide <= sourceShortSide) chosen.push_back(rung);
        }
        if (chosen.empty()) {
            // A tiny source (smaller than 240p): keep its own size.
            int side = std::max(2, sourceShortSide - (sourceShortSide % 2));
            chosen.push_back(Rung{side, 300, 64});
        }
        // Keep the best `maxRenditions` levels.
        const std::size_t keep = static_cast<std::size_t>(std::max(1, maxRenditions));
        if (chosen.size() > keep) chosen.erase(chosen.begin(), chosen.end() - static_cast<long>(keep));

        for (const Rung& rung : chosen) {
            Rendition rendition;
            rendition.shortSide = rung.shortSide;
            rendition.videoKbps = rung.videoKbps;
            rendition.audioKbps = info.hasAudio ? rung.audioKbps : 0;
            ladder.push_back(rendition);
        }
    }

    for (std::size_t i = 0; i < ladder.size(); ++i) ladder[i].name = "r" + std::to_string(i);
    return ladder;
}

}  // namespace mss
