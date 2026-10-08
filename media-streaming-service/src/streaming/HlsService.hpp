#pragma once

#include <optional>

#include "cache/SegmentCache.hpp"
#include "core/ThreadPool.hpp"
#include "http/Router.hpp"
#include "media/MediaLibrary.hpp"
#include "media/TranscodeManager.hpp"

namespace mss {

// Serves media over plain HTTP:
//
//   GET /hls/{type}/{name}/master.m3u8          adaptive master playlist (starts transcoding)
//   GET /hls/{type}/{name}/{rendition}/index.m3u8   media playlist of one quality level
//   GET /hls/{type}/{name}/{rendition}/seg_N.ts     one media segment (served from the cache)
//   GET /media/{type}/{name}                    the original file, with HTTP Range support
//
// {type} is "videos" or "audios" and {name} is the URL-encoded file name.
class HlsService {
public:
    HlsService(MediaLibrary& library, TranscodeManager& transcoder, SegmentCache& cache, ThreadPool& background);

    void registerRoutes(Router& router);

private:
    HttpResponse masterPlaylist(HttpRequest& request);
    HttpResponse renditionFile(HttpRequest& request);
    HttpResponse directFile(HttpRequest& request);

    // Finds the media item named in the URL, or fills `error` with a 404 response.
    std::optional<MediaItem> findMedia(const HttpRequest& request, HttpResponse& error);

    MediaLibrary& library_;
    TranscodeManager& transcoder_;
    SegmentCache& cache_;
    ThreadPool& background_;
};

// MIME type for a media file name (used for direct playback).
std::string mediaContentType(const std::string& fileName);

// Parses a "Range: bytes=..." header for a file of `fileSize` bytes.
// Returns 0 = no usable range (send the whole file), 1 = valid range,
// -1 = range cannot be satisfied (answer 416).
int parseByteRange(const std::string& header, std::uint64_t fileSize, std::uint64_t maxOpenEndedLength,
                   std::uint64_t& start, std::uint64_t& end);

}  // namespace mss
