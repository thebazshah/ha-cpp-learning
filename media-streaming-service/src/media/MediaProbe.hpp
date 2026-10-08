#pragma once

#include <string>

namespace mss {

// What we know about a media file after asking ffprobe.
struct MediaInfo {
    bool ok = false;
    std::string error;

    std::string formatName;      // e.g. "mov,mp4,m4a,3gp,3g2,mj2"
    double durationSeconds = 0;  // 0 when unknown
    long long bitRate = 0;       // overall bits per second, 0 when unknown

    bool hasVideo = false;
    std::string videoCodec;  // e.g. "h264", "hevc", "vp9"
    int width = 0;
    int height = 0;
    double frameRate = 0;

    bool hasAudio = false;
    std::string audioCodec;  // e.g. "aac", "mp3", "opus"
    int sampleRate = 0;
    int channels = 0;
};

// Runs ffprobe on a file and reads the first video and first audio stream.
// Cover art pictures inside music files are not counted as video.
MediaInfo probeMediaFile(const std::string& ffprobePath, const std::string& filePath);

// Parses the text output of
//   ffprobe -show_entries ... -of default=noprint_wrappers=0
// (separated out so it can be unit-tested without running ffprobe).
MediaInfo parseFfprobeOutput(const std::string& text);

}  // namespace mss
