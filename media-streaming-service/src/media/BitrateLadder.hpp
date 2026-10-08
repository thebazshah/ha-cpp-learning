#pragma once

#include <string>
#include <vector>

#include "media/MediaProbe.hpp"

namespace mss {

// One quality level ("rendition") that we encode a file into.
//
// For adaptive bitrate streaming the same media is encoded several times at
// different sizes and bitrates. Together they form a "bitrate ladder":
//
//   r0   360p   800 kbps video +  96 kbps audio   <- slow networks
//   r1   480p  1400 kbps video + 128 kbps audio
//   r2   720p  2800 kbps video + 128 kbps audio
//   r3  1080p  5000 kbps video + 160 kbps audio   <- fast networks
//
// The player (or our RTSP server) picks the best level the network can carry.
struct Rendition {
    std::string name;          // folder name: "r0", "r1", ...
    int shortSide = 0;         // target size of the shorter picture side (360 = "360p"); 0 = audio only
    int videoKbps = 0;         // target video bitrate
    int audioKbps = 0;         // AAC bitrate (0 = the media has no audio)

    bool hasVideo() const { return shortSide > 0; }
    int maxRateKbps() const { return videoKbps * 12 / 10; }  // short peaks may reach +20%
    int bufferKbps() const { return videoKbps * 2; }         // rate-control buffer (2 seconds)

    // Peak bits per second of the whole MPEG-TS stream (for the HLS BANDWIDTH attribute).
    long long peakBandwidth() const;
    // Average bits per second of the whole stream.
    long long averageBandwidth() const;
    // Text for people: "720p" or "128 kbps".
    std::string label() const;
};

// Chooses the renditions for a file:
//  * video files get up to `maxRenditions` levels, never larger than the source
//    (we do not upscale), always ordered from lowest to highest quality;
//  * audio-only files get three AAC levels: 64, 128 and 192 kbps.
std::vector<Rendition> buildLadder(const MediaInfo& info, bool wantVideo, int maxRenditions);

}  // namespace mss
