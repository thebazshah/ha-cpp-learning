#pragma once

#include <chrono>
#include <string>

#include "cache/SegmentCache.hpp"
#include "core/Config.hpp"
#include "http/HttpServer.hpp"
#include "http/Router.hpp"
#include "media/MediaLibrary.hpp"
#include "media/TranscodeManager.hpp"
#include "rtsp/RtspServer.hpp"

namespace mss {

// The REST API and the web frontend files.
//
//   GET  /                                   the web page (web/index.html)
//   GET  /static/{file}                      app.js, style.css, ...
//   GET  /api/health                         is the server alive?
//   GET  /api/config                         ports and settings the web page needs
//   GET  /api/media                          all videos and audios
//   GET  /api/media/{type}/{name}            one file with details and stream status
//   POST /api/media/{type}/{name}/prepare    start transcoding (does nothing if already done)
//   GET  /api/stats                          live server metrics
class ApiController {
public:
    ApiController(const Config& config, MediaLibrary& library, TranscodeManager& transcoder, SegmentCache& cache,
                  const HttpServer& httpServer, const RtspServer& rtspServer);

    void registerRoutes(Router& router);

private:
    HttpResponse health(HttpRequest& request);
    HttpResponse configInfo(HttpRequest& request);
    HttpResponse listMedia(HttpRequest& request);
    HttpResponse mediaDetails(HttpRequest& request);
    HttpResponse prepareMedia(HttpRequest& request);
    HttpResponse stats(HttpRequest& request);
    HttpResponse staticFile(const std::string& fileName);

    Json mediaJson(const MediaItem& item, bool includeStream);

    const Config& config_;
    MediaLibrary& library_;
    TranscodeManager& transcoder_;
    SegmentCache& cache_;
    const HttpServer& httpServer_;
    const RtspServer& rtspServer_;
    const std::chrono::steady_clock::time_point startedAt_;
};

}  // namespace mss
