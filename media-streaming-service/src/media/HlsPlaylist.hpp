#pragma once

#include <cstddef>
#include <string>
#include <vector>

// Reading and writing HLS playlists (.m3u8 text files).
//
// HLS ("HTTP Live Streaming") cuts media into short segment files. A
// *media playlist* lists the segments of one quality level:
//
//   #EXTM3U
//   #EXT-X-TARGETDURATION:2
//   #EXTINF:2.000000,
//   seg_00000.ts
//   #EXTINF:2.000000,
//   seg_00001.ts
//   #EXT-X-ENDLIST            <- present once the playlist is complete
//
// A *master playlist* lists the available quality levels (renditions) so the
// player can switch between them depending on the network speed. That is
// adaptive bitrate streaming (ABR).
namespace mss {

struct HlsSegment {
    std::string uri;         // file name relative to the playlist, e.g. "seg_00003.ts"
    double duration = 0.0;   // seconds
    double startTime = 0.0;  // seconds from the start of the media
};

struct HlsMediaPlaylist {
    int targetDuration = 0;
    long long mediaSequence = 0;
    bool endList = false;     // true when no more segments will be added
    std::string playlistType; // "EVENT" while growing, "VOD" for fixed playlists
    std::vector<HlsSegment> segments;

    double totalDuration() const;
    // Index of the segment that contains `seconds` (clamped to the valid range).
    std::size_t segmentIndexAt(double seconds) const;
};

// Parses a media playlist. Returns false if the text is not a playlist.
bool parseMediaPlaylist(const std::string& text, HlsMediaPlaylist& out);

// One entry of a master playlist.
struct HlsVariant {
    std::string uri;            // e.g. "r1/index.m3u8"
    long long bandwidth = 0;    // peak bits per second (required by HLS)
    long long averageBandwidth = 0;
    std::string codecs;         // e.g. "avc1.4d401f,mp4a.40.2"
    int width = 0;              // 0 for audio-only variants
    int height = 0;
};

std::string buildMasterPlaylist(const std::vector<HlsVariant>& variants);

}  // namespace mss
