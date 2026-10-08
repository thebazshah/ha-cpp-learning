#include "api/ApiController.hpp"

#include <sys/stat.h>

#include "core/StringUtils.hpp"

namespace mss {
namespace {

std::string contentTypeForWebFile(const std::string& name) {
    const std::string extension = str::fileExtension(name);
    if (extension == "html") return "text/html; charset=utf-8";
    if (extension == "js") return "text/javascript; charset=utf-8";
    if (extension == "css") return "text/css; charset=utf-8";
    if (extension == "svg") return "image/svg+xml";
    if (extension == "png") return "image/png";
    if (extension == "ico") return "image/x-icon";
    if (extension == "json") return "application/json";
    return "application/octet-stream";
}

}  // namespace

ApiController::ApiController(const Config& config, MediaLibrary& library, TranscodeManager& transcoder,
                             SegmentCache& cache, const HttpServer& httpServer, const RtspServer& rtspServer)
    : config_(config),
      library_(library),
      transcoder_(transcoder),
      cache_(cache),
      httpServer_(httpServer),
      rtspServer_(rtspServer),
      startedAt_(std::chrono::steady_clock::now()) {}

void ApiController::registerRoutes(Router& router) {
    router.add("GET", "/", [this](HttpRequest&) { return staticFile("index.html"); });
    router.add("GET", "/static/{file}", [this](HttpRequest& r) { return staticFile(r.params.at("file")); });
    router.add("GET", "/favicon.ico", [](HttpRequest&) {
        HttpResponse response;
        response.status = 204;  // no icon - this just stops the browser's 404 noise
        return response;
    });
    router.add("GET", "/api/health", [this](HttpRequest& r) { return health(r); });
    router.add("GET", "/api/config", [this](HttpRequest& r) { return configInfo(r); });
    router.add("GET", "/api/media", [this](HttpRequest& r) { return listMedia(r); });
    router.add("GET", "/api/media/{type}/{name}", [this](HttpRequest& r) { return mediaDetails(r); });
    router.add("POST", "/api/media/{type}/{name}/prepare", [this](HttpRequest& r) { return prepareMedia(r); });
    router.add("GET", "/api/stats", [this](HttpRequest& r) { return stats(r); });
}

HttpResponse ApiController::staticFile(const std::string& fileName) {
    // Only plain file names inside the web folder are allowed (no "../").
    if (fileName.empty() || fileName[0] == '.' || fileName.find('/') != std::string::npos) {
        return HttpResponse::error(404, "Not found");
    }
    const std::string path = config_.webDir + "/" + fileName;
    std::string content;
    if (!readWholeFile(path, content)) return HttpResponse::error(404, "Not found");
    HttpResponse response = HttpResponse::text(200, std::move(content), contentTypeForWebFile(fileName));
    response.setHeader("Cache-Control", "no-cache");  // always pick up edits to the frontend
    return response;
}

HttpResponse ApiController::health(HttpRequest&) {
    Json body = Json::object();
    body.set("status", "ok")
        .set("uptimeSeconds",
             std::chrono::duration<double>(std::chrono::steady_clock::now() - startedAt_).count());
    return HttpResponse::json(200, body);
}

HttpResponse ApiController::configInfo(HttpRequest&) {
    Json body = Json::object();
    body.set("httpPort", config_.httpPort)
        .set("rtspPort", config_.rtspPort)
        .set("segmentSeconds", config_.segmentSeconds)
        .set("maxRenditions", config_.maxRenditions)
        .set("videoEncoder", transcoder_.settings().videoEncoder)
        .set("videosFolder", config_.videosDir)
        .set("audiosFolder", config_.audiosDir);
    return HttpResponse::json(200, body);
}

Json ApiController::mediaJson(const MediaItem& item, bool includeStream) {
    const std::string folder = mediaKindFolder(item.kind);
    const std::string encoded = str::urlEncode(item.name);
    const MediaInfo& info = item.info;

    Json video = nullptr;
    if (info.hasVideo) {
        video = Json::object();
        video.set("codec", info.videoCodec)
            .set("width", info.width)
            .set("height", info.height)
            .set("frameRate", info.frameRate);
    }
    Json audio = nullptr;
    if (info.hasAudio) {
        audio = Json::object();
        audio.set("codec", info.audioCodec).set("sampleRate", info.sampleRate).set("channels", info.channels);
    }

    Json urls = Json::object();
    urls.set("hls", "/hls/" + folder + "/" + encoded + "/master.m3u8")
        .set("direct", "/media/" + folder + "/" + encoded)
        .set("rtspPath", "/" + folder + "/" + encoded)
        .set("details", "/api/media/" + folder + "/" + encoded)
        .set("prepare", "/api/media/" + folder + "/" + encoded + "/prepare");

    Json json = Json::object();
    json.set("id", item.id())
        .set("type", item.kind == MediaKind::Video ? "video" : "audio")
        .set("folder", folder)
        .set("name", item.name)
        .set("sizeBytes", item.sizeBytes)
        .set("modified", item.modifiedUnix)
        .set("playable", info.ok)
        .set("probeError", info.error)
        .set("durationSeconds", info.durationSeconds)
        .set("bitRate", info.bitRate)
        .set("container", info.formatName)
        .set("video", video)
        .set("audio", audio)
        .set("urls", urls);

    // Show the transcoding status if a job exists already (does not start one).
    Json stream = nullptr;
    if (includeStream) {
        if (std::shared_ptr<TranscodeJob> job = transcoder_.findJob(item)) stream = job->toJson();
    }
    json.set("stream", stream);
    return json;
}

HttpResponse ApiController::listMedia(HttpRequest&) {
    Json videos = Json::array();
    for (const MediaItem& item : library_.list(MediaKind::Video)) videos.push(mediaJson(item, true));
    Json audios = Json::array();
    for (const MediaItem& item : library_.list(MediaKind::Audio)) audios.push(mediaJson(item, true));

    Json body = Json::object();
    body.set("videos", videos).set("audios", audios);
    return HttpResponse::json(200, body);
}

HttpResponse ApiController::mediaDetails(HttpRequest& request) {
    MediaKind kind;
    if (!parseMediaKind(request.params.at("type"), kind)) return HttpResponse::error(404, "Unknown media type");
    const std::optional<MediaItem> item = library_.find(kind, request.params.at("name"));
    if (!item) return HttpResponse::error(404, "Media file not found");
    return HttpResponse::json(200, mediaJson(*item, true));
}

HttpResponse ApiController::prepareMedia(HttpRequest& request) {
    MediaKind kind;
    if (!parseMediaKind(request.params.at("type"), kind)) return HttpResponse::error(404, "Unknown media type");
    const std::optional<MediaItem> item = library_.find(kind, request.params.at("name"));
    if (!item) return HttpResponse::error(404, "Media file not found");

    std::shared_ptr<TranscodeJob> job = transcoder_.ensureJob(*item);
    // 200 = ready to play now, 202 = accepted, still preparing.
    return HttpResponse::json(job->ready() ? 200 : 202, job->toJson());
}

HttpResponse ApiController::stats(HttpRequest&) {
    Json body = Json::object();
    body.set("uptimeSeconds", std::chrono::duration<double>(std::chrono::steady_clock::now() - startedAt_).count())
        .set("http", httpServer_.statsJson())
        .set("rtsp", rtspServer_.statsJson())
        .set("cache", cache_.statsJson())
        .set("transcoder", transcoder_.statsJson());
    return HttpResponse::json(200, body);
}

}  // namespace mss
