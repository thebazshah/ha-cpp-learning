#include "media/MediaProbe.hpp"

#include <map>
#include <sstream>
#include <vector>

#include "core/Process.hpp"
#include "core/StringUtils.hpp"

namespace mss {
namespace {

using Section = std::map<std::string, std::string>;

long long readInt(const Section& section, const std::string& key) {
    const auto it = section.find(key);
    long long value = 0;
    if (it == section.end() || !str::parseInt64(it->second, value)) return 0;
    return value;
}

double readDouble(const Section& section, const std::string& key) {
    const auto it = section.find(key);
    double value = 0;
    if (it == section.end() || !str::parseDouble(it->second, value)) return 0;
    return value;
}

std::string readText(const Section& section, const std::string& key) {
    const auto it = section.find(key);
    return it == section.end() ? "" : it->second;
}

// Frame rates are written as fractions: "30000/1001" -> 29.97.
double readFraction(const Section& section, const std::string& key) {
    const std::string text = readText(section, key);
    const std::size_t slash = text.find('/');
    if (slash == std::string::npos) return 0;
    double numerator = 0;
    double denominator = 0;
    if (!str::parseDouble(text.substr(0, slash), numerator) || !str::parseDouble(text.substr(slash + 1), denominator) ||
        denominator <= 0) {
        return 0;
    }
    return numerator / denominator;
}

}  // namespace

MediaInfo parseFfprobeOutput(const std::string& text) {
    // The output looks like:
    //   [STREAM]
    //   codec_type=video
    //   width=1280
    //   ...
    //   [/STREAM]
    //   [FORMAT]
    //   duration=6.000000
    //   [/FORMAT]
    std::vector<Section> streams;
    Section format;
    Section* current = nullptr;

    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line)) {
        line = str::trim(line);
        if (line == "[STREAM]") {
            streams.emplace_back();
            current = &streams.back();
        } else if (line == "[FORMAT]") {
            current = &format;
        } else if (!line.empty() && line[0] == '[') {
            current = nullptr;  // end of a section
        } else if (current != nullptr) {
            const std::size_t equals = line.find('=');
            if (equals != std::string::npos) (*current)[line.substr(0, equals)] = line.substr(equals + 1);
        }
    }

    MediaInfo info;
    info.formatName = readText(format, "format_name");
    info.durationSeconds = readDouble(format, "duration");
    info.bitRate = readInt(format, "bit_rate");

    for (const Section& stream : streams) {
        const std::string type = readText(stream, "codec_type");
        if (type == "video" && !info.hasVideo) {
            // Album art in an MP3/M4A shows up as a "video" stream with the
            // attached_pic flag. It is a still picture, not a video.
            if (readText(stream, "DISPOSITION:attached_pic") == "1") continue;
            info.hasVideo = true;
            info.videoCodec = readText(stream, "codec_name");
            info.width = static_cast<int>(readInt(stream, "width"));
            info.height = static_cast<int>(readInt(stream, "height"));
            info.frameRate = readFraction(stream, "avg_frame_rate");
            if (info.frameRate <= 0) info.frameRate = readFraction(stream, "r_frame_rate");
        } else if (type == "audio" && !info.hasAudio) {
            info.hasAudio = true;
            info.audioCodec = readText(stream, "codec_name");
            info.sampleRate = static_cast<int>(readInt(stream, "sample_rate"));
            info.channels = static_cast<int>(readInt(stream, "channels"));
        }
    }

    info.ok = info.hasVideo || info.hasAudio;
    if (!info.ok) info.error = "no audio or video stream found";
    return info;
}

MediaInfo probeMediaFile(const std::string& ffprobePath, const std::string& filePath) {
    // The "file:" prefix makes ffprobe treat the path as a plain file even if
    // its name contains a ':' (which could otherwise look like a protocol).
    const std::vector<std::string> args = {
        ffprobePath,
        "-v", "error",
        "-show_entries",
        "format=duration,bit_rate,format_name:stream=index,codec_type,codec_name,width,height,"
        "avg_frame_rate,r_frame_rate,sample_rate,channels:stream_disposition=attached_pic",
        "-of", "default=noprint_wrappers=0",
        "file:" + filePath,
    };
    const ProcessResult result = Process::run(args, 30);
    if (!result.started) {
        MediaInfo info;
        info.error = result.error;
        return info;
    }
    if (result.timedOut) {
        MediaInfo info;
        info.error = "ffprobe timed out";
        return info;
    }
    MediaInfo info = parseFfprobeOutput(result.output);
    if (!info.ok && result.exitCode != 0) info.error = "ffprobe could not read this file (not a media file?)";
    return info;
}

}  // namespace mss
