#include "streaming/HlsService.hpp"

#include <cctype>
#include <cstdio>
#include <filesystem>

#include "core/Logger.hpp"
#include "core/StringUtils.hpp"

namespace mss {
namespace fs = std::filesystem;

namespace {

// How long a master playlist request may wait for the first segments.
// Players give up after ~10 s, so we answer before that with 503 + Retry-After.
constexpr auto kMasterWait = std::chrono::seconds(8);

// When a player asks for "bytes=N-" (everything from N), we send at most this
// much. Players then simply ask for the next part. This stops a slow client
// from occupying a worker thread for a whole multi-gigabyte download.
constexpr std::uint64_t kMaxOpenEndedRange = 4 * 1024 * 1024;

// Segment files are named seg_00000.ts, seg_00001.ts, ...
bool isSegmentName(const std::string& name, long long& number) {
    if (!str::startsWith(name, "seg_") || !str::endsWith(name, ".ts") || name.size() <= 7) return false;
    const std::string digits = name.substr(4, name.size() - 7);
    for (char c : digits) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    return str::parseInt64(digits, number);
}

bool parseRenditionName(const std::string& name, std::size_t count, std::size_t& index) {
    long long value = 0;
    if (name.size() < 2 || name[0] != 'r' || !str::parseInt64(name.substr(1), value)) return false;
    if (value < 0 || static_cast<std::size_t>(value) >= count) return false;
    index = static_cast<std::size_t>(value);
    return true;
}

}  // namespace

std::string mediaContentType(const std::string& fileName) {
    const std::string extension = str::fileExtension(fileName);
    if (extension == "mp4" || extension == "m4v") return "video/mp4";
    if (extension == "mov") return "video/quicktime";
    if (extension == "webm") return "video/webm";
    if (extension == "mkv") return "video/x-matroska";
    if (extension == "ogv") return "video/ogg";
    if (extension == "ts" || extension == "mts" || extension == "m2ts") return "video/mp2t";
    if (extension == "avi") return "video/x-msvideo";
    if (extension == "mp3") return "audio/mpeg";
    if (extension == "m4a" || extension == "alac") return "audio/mp4";
    if (extension == "aac") return "audio/aac";
    if (extension == "wav") return "audio/wav";
    if (extension == "flac") return "audio/flac";
    if (extension == "ogg" || extension == "oga" || extension == "opus") return "audio/ogg";
    return "application/octet-stream";
}

int parseByteRange(const std::string& header, std::uint64_t fileSize, std::uint64_t maxOpenEndedLength,
                   std::uint64_t& start, std::uint64_t& end) {
    const std::string value = str::trim(header);
    if (!str::startsWith(value, "bytes=")) return 0;
    const std::string spec = value.substr(6);
    if (spec.find(',') != std::string::npos) return 0;  // several ranges: we just send the whole file
    const std::size_t dash = spec.find('-');
    if (dash == std::string::npos) return 0;
    const std::string first = str::trim(spec.substr(0, dash));
    const std::string last = str::trim(spec.substr(dash + 1));
    long long a = 0;
    long long b = 0;

    if (first.empty()) {
        // "bytes=-500": the last 500 bytes.
        if (!str::parseInt64(last, b) || b <= 0) return 0;
        if (fileSize == 0) return -1;
        const std::uint64_t suffix = std::min<std::uint64_t>(static_cast<std::uint64_t>(b), fileSize);
        start = fileSize - suffix;
        end = fileSize - 1;
        return 1;
    }
    if (!str::parseInt64(first, a) || a < 0) return 0;
    if (static_cast<std::uint64_t>(a) >= fileSize) return -1;
    start = static_cast<std::uint64_t>(a);
    if (last.empty()) {
        // "bytes=1000-": from 1000 to the end (limited, see kMaxOpenEndedRange).
        end = std::min<std::uint64_t>(fileSize - 1, start + maxOpenEndedLength - 1);
        return 1;
    }
    if (!str::parseInt64(last, b) || b < a) return 0;
    end = std::min<std::uint64_t>(static_cast<std::uint64_t>(b), fileSize - 1);
    return 1;
}

HlsService::HlsService(MediaLibrary& library, TranscodeManager& transcoder, SegmentCache& cache,
                       ThreadPool& background)
    : library_(library), transcoder_(transcoder), cache_(cache), background_(background) {}

void HlsService::registerRoutes(Router& router) {
    router.add("GET", "/hls/{type}/{name}/master.m3u8", [this](HttpRequest& r) { return masterPlaylist(r); });
    router.add("GET", "/hls/{type}/{name}/{rendition}/{file}", [this](HttpRequest& r) { return renditionFile(r); });
    router.add("GET", "/media/{type}/{name}", [this](HttpRequest& r) { return directFile(r); });
}

std::optional<MediaItem> HlsService::findMedia(const HttpRequest& request, HttpResponse& error) {
    MediaKind kind;
    if (!parseMediaKind(request.params.at("type"), kind)) {
        error = HttpResponse::error(404, "Unknown media type (use 'videos' or 'audios')");
        return std::nullopt;
    }
    std::optional<MediaItem> item = library_.find(kind, request.params.at("name"));
    if (!item) error = HttpResponse::error(404, "Media file not found");
    return item;
}

HttpResponse HlsService::masterPlaylist(HttpRequest& request) {
    HttpResponse error;
    const std::optional<MediaItem> item = findMedia(request, error);
    if (!item) return error;

    // Make sure the HLS ladder exists or is being produced, then wait a
    // little for the first segments so the player can start right away.
    std::shared_ptr<TranscodeJob> job = transcoder_.ensureJob(*item);
    if (job->state() == JobState::Failed) return HttpResponse::error(500, job->error());
    if (!job->waitUntilReady(kMasterWait)) {
        if (job->state() == JobState::Failed) return HttpResponse::error(500, job->error());
        HttpResponse busy = HttpResponse::error(503, "The stream is still being prepared, try again shortly");
        busy.setHeader("Retry-After", "2");
        return busy;
    }

    std::vector<HlsVariant> variants;
    for (std::size_t i = 0; i < job->renditions().size(); ++i) {
        const Rendition& rendition = job->renditions()[i];
        HlsVariant variant;
        variant.uri = rendition.name + "/index.m3u8";
        variant.bandwidth = rendition.peakBandwidth();
        variant.averageBandwidth = rendition.averageBandwidth();
        RenditionStreamInfo info;
        if (job->streamInfo(i, cache_, info)) {
            variant.codecs = info.codecs;
            variant.width = info.width;
            variant.height = info.height;
        }
        variants.push_back(variant);
    }

    HttpResponse response = HttpResponse::text(200, buildMasterPlaylist(variants), "application/vnd.apple.mpegurl");
    response.setHeader("Cache-Control", "no-cache");
    return response;
}

HttpResponse HlsService::renditionFile(HttpRequest& request) {
    HttpResponse error;
    const std::optional<MediaItem> item = findMedia(request, error);
    if (!item) return error;

    std::shared_ptr<TranscodeJob> job = transcoder_.findJob(*item);
    if (!job) return HttpResponse::error(404, "This stream has not been prepared yet - load master.m3u8 first");

    std::size_t index = 0;
    if (!parseRenditionName(request.params.at("rendition"), job->renditions().size(), index)) {
        return HttpResponse::error(404, "Unknown rendition");
    }
    const std::string& file = request.params.at("file");
    const std::string dir = job->renditionDir(index);

    if (file == "index.m3u8") {
        // Media playlists change while ffmpeg is still working, so they are
        // always read fresh from disk and never cached by the browser.
        std::string text;
        if (!readWholeFile(dir + "/index.m3u8", text)) return HttpResponse::error(404, "Playlist not available yet");
        HttpResponse response = HttpResponse::text(200, std::move(text), "application/vnd.apple.mpegurl");
        response.setHeader("Cache-Control", "no-cache");
        return response;
    }

    long long number = 0;
    if (!isSegmentName(file, number)) return HttpResponse::error(404, "Unknown file");

    const SegmentCache::Data data = cache_.get(dir + "/" + file);
    if (!data) return HttpResponse::error(404, "Segment not available");

    // Players read segments in order, so load the next one in the background
    // now. When the player asks for it, it is already in memory.
    char nextName[32];
    std::snprintf(nextName, sizeof(nextName), "seg_%05lld.ts", number + 1);
    const std::string nextPath = dir + "/" + nextName;
    std::error_code ignored;
    if (fs::exists(nextPath, ignored)) cache_.prefetch(background_, nextPath);

    HttpResponse response;
    response.sharedBody = data;
    response.setHeader("Content-Type", "video/mp2t");
    // A finished segment never changes, so browsers and proxies may keep it.
    response.setHeader("Cache-Control", "public, max-age=86400");
    return response;
}

HttpResponse HlsService::directFile(HttpRequest& request) {
    HttpResponse error;
    const std::optional<MediaItem> item = findMedia(request, error);
    if (!item) return error;

    const std::uint64_t size = item->sizeBytes;
    HttpResponse response;
    response.setHeader("Content-Type", mediaContentType(item->name));
    response.setHeader("Accept-Ranges", "bytes");
    response.setHeader("Cache-Control", "no-cache");

    std::uint64_t start = 0;
    std::uint64_t end = size == 0 ? 0 : size - 1;
    const int range = parseByteRange(request.header("range").value_or(""), size, kMaxOpenEndedRange, start, end);
    if (range < 0) {
        HttpResponse invalid = HttpResponse::error(416, "Requested range is outside the file");
        invalid.setHeader("Content-Range", "bytes */" + std::to_string(size));
        return invalid;
    }
    if (range == 1) {
        response.status = 206;
        response.setHeader("Content-Range",
                           "bytes " + std::to_string(start) + "-" + std::to_string(end) + "/" + std::to_string(size));
        response.file = FileBody{item->path, start, end - start + 1};
    } else {
        response.file = FileBody{item->path, 0, size};
    }
    return response;
}

}  // namespace mss
