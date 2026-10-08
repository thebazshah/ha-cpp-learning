#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/ThreadPool.hpp"
#include "media/MediaProbe.hpp"

namespace mss {

// Which library folder a file comes from.
enum class MediaKind { Video, Audio };

// "videos" or "audios" - used both as folder name and in URLs.
const char* mediaKindFolder(MediaKind kind);
// Accepts "videos"/"video" and "audios"/"audio".
bool parseMediaKind(const std::string& text, MediaKind& out);

// One media file found in the videos/ or audios/ folder.
struct MediaItem {
    MediaKind kind = MediaKind::Video;
    std::string name;  // file name only, e.g. "holiday.mp4"
    std::string path;  // absolute path on disk
    std::uint64_t sizeBytes = 0;
    long long modifiedUnix = 0;  // last modification time (seconds since 1970)
    MediaInfo info;              // details from ffprobe

    // Stable id used in URLs: "videos/holiday.mp4".
    std::string id() const { return std::string(mediaKindFolder(kind)) + "/" + name; }
};

// Keeps track of the media files in the two library folders.
//
// The folders are re-scanned on every request, so files you copy in show up
// immediately - no restart needed. Running ffprobe is slow-ish (~50 ms per
// file), so probe results are cached and only refreshed when a file's size
// or modification time changes. New files are probed in parallel on the
// background thread pool.
class MediaLibrary {
public:
    MediaLibrary(std::string videosDir, std::string audiosDir, std::string ffprobePath, ThreadPool& probePool);

    // All supported files of one kind, sorted by name.
    std::vector<MediaItem> list(MediaKind kind);

    // Looks up one file by its plain name. Names with '/' or starting with '.'
    // are rejected, so requests can never escape the library folders.
    std::optional<MediaItem> find(MediaKind kind, const std::string& name);

    const std::string& folderPath(MediaKind kind) const;

    // True if the file extension belongs to the given library.
    static bool isSupportedFile(MediaKind kind, const std::string& fileName);

private:
    struct ProbeEntry {
        std::uint64_t size = 0;
        long long modified = 0;
        MediaInfo info;
    };

    bool cachedInfo(const std::string& path, std::uint64_t size, long long modified, MediaInfo& out);
    void storeInfo(const std::string& path, std::uint64_t size, long long modified, const MediaInfo& info);

    std::string videosDir_;
    std::string audiosDir_;
    std::string ffprobePath_;
    ThreadPool& probePool_;
    std::mutex mutex_;
    std::unordered_map<std::string, ProbeEntry> probeCache_;
};

}  // namespace mss
