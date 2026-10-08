#include "media/HlsPlaylist.hpp"

#include <sstream>

#include "core/StringUtils.hpp"

namespace mss {

double HlsMediaPlaylist::totalDuration() const {
    if (segments.empty()) return 0.0;
    return segments.back().startTime + segments.back().duration;
}

std::size_t HlsMediaPlaylist::segmentIndexAt(double seconds) const {
    if (segments.empty()) return 0;
    for (std::size_t i = 0; i < segments.size(); ++i) {
        if (seconds < segments[i].startTime + segments[i].duration) return i;
    }
    return segments.size() - 1;
}

bool parseMediaPlaylist(const std::string& text, HlsMediaPlaylist& out) {
    HlsMediaPlaylist playlist;
    std::istringstream input(text);
    std::string line;
    bool first = true;
    double pendingDuration = 0.0;
    double clock = 0.0;

    while (std::getline(input, line)) {
        line = str::trim(line);
        if (first) {
            // Skip a possible UTF-8 byte order mark before #EXTM3U.
            if (str::startsWith(line, "\xEF\xBB\xBF")) line = line.substr(3);
            if (line != "#EXTM3U") return false;
            first = false;
            continue;
        }
        if (line.empty()) continue;

        if (str::startsWith(line, "#EXTINF:")) {
            // "#EXTINF:2.000000," -> 2.0 (anything after the comma is a title)
            std::string value = line.substr(8);
            const std::size_t comma = value.find(',');
            if (comma != std::string::npos) value.resize(comma);
            if (!str::parseDouble(value, pendingDuration)) pendingDuration = 0.0;
        } else if (str::startsWith(line, "#EXT-X-TARGETDURATION:")) {
            long long value = 0;
            if (str::parseInt64(line.substr(22), value)) playlist.targetDuration = static_cast<int>(value);
        } else if (str::startsWith(line, "#EXT-X-MEDIA-SEQUENCE:")) {
            str::parseInt64(line.substr(22), playlist.mediaSequence);
        } else if (str::startsWith(line, "#EXT-X-PLAYLIST-TYPE:")) {
            playlist.playlistType = line.substr(21);
        } else if (line == "#EXT-X-ENDLIST") {
            playlist.endList = true;
        } else if (line[0] != '#') {
            // A line that is not a tag is the URI of a segment.
            playlist.segments.push_back(HlsSegment{line, pendingDuration, clock});
            clock += pendingDuration;
            pendingDuration = 0.0;
        }
    }
    if (first) return false;  // empty text
    out = std::move(playlist);
    return true;
}

std::string buildMasterPlaylist(const std::vector<HlsVariant>& variants) {
    std::ostringstream out;
    out << "#EXTM3U\n";
    out << "#EXT-X-VERSION:3\n";
    out << "#EXT-X-INDEPENDENT-SEGMENTS\n";
    for (const HlsVariant& variant : variants) {
        out << "#EXT-X-STREAM-INF:BANDWIDTH=" << variant.bandwidth;
        if (variant.averageBandwidth > 0) out << ",AVERAGE-BANDWIDTH=" << variant.averageBandwidth;
        if (variant.width > 0 && variant.height > 0) out << ",RESOLUTION=" << variant.width << "x" << variant.height;
        if (!variant.codecs.empty()) out << ",CODECS=\"" << variant.codecs << "\"";
        out << "\n" << variant.uri << "\n";
    }
    return out.str();
}

}  // namespace mss
