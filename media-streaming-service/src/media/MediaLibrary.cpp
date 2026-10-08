#include "media/MediaLibrary.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <filesystem>
#include <future>
#include <set>

#include "core/Logger.hpp"
#include "core/StringUtils.hpp"

namespace mss {
namespace fs = std::filesystem;

namespace {

const std::set<std::string>& videoExtensions() {
    static const std::set<std::string> extensions = {"mp4", "m4v", "mov", "mkv", "webm", "avi", "flv",
                                                     "wmv", "ts",  "mts", "m2ts", "mpg", "mpeg", "3gp", "ogv"};
    return extensions;
}

const std::set<std::string>& audioExtensions() {
    static const std::set<std::string> extensions = {"mp3",  "aac", "m4a", "wav", "flac", "ogg",
                                                     "opus", "wma", "aif", "aiff", "alac", "oga"};
    return extensions;
}

// Reads size and modification time with stat(). Returns false if the path
// is not a regular file.
bool fileFacts(const std::string& path, std::uint64_t& size, long long& modified) {
    struct stat facts {};
    if (stat(path.c_str(), &facts) != 0 || !S_ISREG(facts.st_mode)) return false;
    size = static_cast<std::uint64_t>(facts.st_size);
    modified = static_cast<long long>(facts.st_mtime);
    return true;
}

bool isSafeName(const std::string& name) {
    return !name.empty() && name[0] != '.' && name.find('/') == std::string::npos &&
           name.find('\0') == std::string::npos;
}

}  // namespace

const char* mediaKindFolder(MediaKind kind) { return kind == MediaKind::Video ? "videos" : "audios"; }

bool parseMediaKind(const std::string& text, MediaKind& out) {
    if (text == "videos" || text == "video") {
        out = MediaKind::Video;
        return true;
    }
    if (text == "audios" || text == "audio") {
        out = MediaKind::Audio;
        return true;
    }
    return false;
}

MediaLibrary::MediaLibrary(std::string videosDir, std::string audiosDir, std::string ffprobePath,
                           ThreadPool& probePool)
    : videosDir_(std::move(videosDir)),
      audiosDir_(std::move(audiosDir)),
      ffprobePath_(std::move(ffprobePath)),
      probePool_(probePool) {}

const std::string& MediaLibrary::folderPath(MediaKind kind) const {
    return kind == MediaKind::Video ? videosDir_ : audiosDir_;
}

bool MediaLibrary::isSupportedFile(MediaKind kind, const std::string& fileName) {
    const std::string extension = str::fileExtension(fileName);
    const auto& allowed = kind == MediaKind::Video ? videoExtensions() : audioExtensions();
    return allowed.count(extension) > 0;
}

bool MediaLibrary::cachedInfo(const std::string& path, std::uint64_t size, long long modified, MediaInfo& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = probeCache_.find(path);
    if (it == probeCache_.end() || it->second.size != size || it->second.modified != modified) return false;
    out = it->second.info;
    return true;
}

void MediaLibrary::storeInfo(const std::string& path, std::uint64_t size, long long modified, const MediaInfo& info) {
    std::lock_guard<std::mutex> lock(mutex_);
    probeCache_[path] = ProbeEntry{size, modified, info};
}

std::vector<MediaItem> MediaLibrary::list(MediaKind kind) {
    std::vector<MediaItem> items;
    std::error_code error;
    const std::string& folder = folderPath(kind);

    for (fs::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
        const std::string name = it->path().filename().string();
        if (!isSafeName(name) || !isSupportedFile(kind, name)) continue;  // skips .gitkeep, .DS_Store, ...
        MediaItem item;
        item.kind = kind;
        item.name = name;
        item.path = it->path().string();
        if (!fileFacts(item.path, item.sizeBytes, item.modifiedUnix)) continue;
        items.push_back(std::move(item));
    }
    if (error) LOG_WARN("media") << "Cannot read folder " << folder << ": " << error.message();

    // Probe the files we have not seen before - in parallel on the background pool.
    std::vector<std::pair<std::size_t, std::future<MediaInfo>>> pending;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (cachedInfo(items[i].path, items[i].sizeBytes, items[i].modifiedUnix, items[i].info)) continue;
        const std::string path = items[i].path;
        const std::string ffprobe = ffprobePath_;
        pending.emplace_back(i, probePool_.submitWithResult([ffprobe, path] { return probeMediaFile(ffprobe, path); }));
    }
    for (auto& entry : pending) {
        MediaItem& item = items[entry.first];
        item.info = entry.second.get();
        storeInfo(item.path, item.sizeBytes, item.modifiedUnix, item.info);
        if (!item.info.ok) LOG_WARN("media") << "Cannot read " << item.id() << ": " << item.info.error;
    }

    std::sort(items.begin(), items.end(), [](const MediaItem& a, const MediaItem& b) { return a.name < b.name; });
    return items;
}

std::optional<MediaItem> MediaLibrary::find(MediaKind kind, const std::string& name) {
    if (!isSafeName(name) || !isSupportedFile(kind, name)) return std::nullopt;

    MediaItem item;
    item.kind = kind;
    item.name = name;
    item.path = folderPath(kind) + "/" + name;
    if (!fileFacts(item.path, item.sizeBytes, item.modifiedUnix)) return std::nullopt;

    if (!cachedInfo(item.path, item.sizeBytes, item.modifiedUnix, item.info)) {
        item.info = probeMediaFile(ffprobePath_, item.path);
        storeInfo(item.path, item.sizeBytes, item.modifiedUnix, item.info);
    }
    return item;
}

}  // namespace mss
