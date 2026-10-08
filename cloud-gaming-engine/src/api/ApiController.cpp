#include "api/ApiController.hpp"

#include <fstream>
#include <iterator>

#include "core/Logger.hpp"
#include "core/StringUtils.hpp"
#include "ws/WebSocket.hpp"

namespace cge {
namespace {

bool readWholeFile(const std::string& path, std::string& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}

std::string contentTypeFor(const std::string& name) {
    const std::string extension = str::fileExtension(name);
    if (extension == "html") return "text/html; charset=utf-8";
    if (extension == "js") return "text/javascript; charset=utf-8";
    if (extension == "css") return "text/css; charset=utf-8";
    if (extension == "svg") return "image/svg+xml";
    if (extension == "png") return "image/png";
    return "application/octet-stream";
}

Json gameJson(const GameEntry& entry, std::size_t activeSessions) {
    Json game = Json::object();
    game.set("id", entry.id)
        .set("status", gameStateName(entry.state))
        .set("ready", entry.state == GameState::Ready)
        .set("error", entry.error)
        .set("prebuilt", entry.prebuilt)
        .set("buildSeconds", entry.buildSeconds)
        .set("updatedUnix", entry.updatedUnix)
        .set("activeSessions", activeSessions);
    if (entry.state == GameState::Ready) {
        game.set("name", entry.info.name)
            .set("description", entry.info.description)
            .set("width", entry.info.width)
            .set("height", entry.info.height)
            .set("minPlayers", entry.info.minPlayers)
            .set("maxPlayers", entry.info.maxPlayers)
            .set("ticksPerSecond", entry.info.ticksPerSecond);
    } else {
        game.set("name", entry.id);  // the real name is only known once the game is loaded
    }
    return game;
}

}  // namespace

ApiController::ApiController(const Config& config, GameRegistry& registry, SessionManager& sessions,
                             const HttpServer& http)
    : config_(config), registry_(registry), sessions_(sessions), http_(http), startedAt_(std::chrono::steady_clock::now()) {}

void ApiController::registerRoutes(Router& router) {
    router.add("GET", "/", [this](HttpRequest&) { return staticFile("index.html"); });
    router.add("GET", "/play/{sessionId}", [this](HttpRequest&) { return staticFile("index.html"); });
    router.add("GET", "/static/{file}", [this](HttpRequest& r) { return staticFile(r.params.at("file")); });
    router.add("GET", "/favicon.ico", [](HttpRequest&) {
        HttpResponse response;
        response.status = 204;
        return response;
    });
    router.add("GET", "/api/health", [this](HttpRequest&) {
        Json body = Json::object();
        body.set("status", "ok")
            .set("uptimeSeconds", std::chrono::duration<double>(std::chrono::steady_clock::now() - startedAt_).count());
        return HttpResponse::json(200, body);
    });
    router.add("GET", "/api/games", [this](HttpRequest& r) { return listGames(r); });
    router.add("POST", "/api/games/{gameId}/sessions", [this](HttpRequest& r) { return createSession(r); });
    router.add("GET", "/api/sessions", [this](HttpRequest& r) { return listSessions(r); });
    router.add("GET", "/api/sessions/{sessionId}", [this](HttpRequest& r) { return sessionDetails(r); });
    router.add("DELETE", "/api/sessions/{sessionId}", [this](HttpRequest& r) { return deleteSession(r); });
    router.add("GET", "/api/stats", [this](HttpRequest& r) { return stats(r); });
    router.add("GET", "/ws/{sessionId}", [this](HttpRequest& r) { return webSocket(r); });
}

HttpResponse ApiController::staticFile(const std::string& fileName) {
    if (fileName.empty() || fileName[0] == '.' || fileName.find('/') != std::string::npos) {
        return HttpResponse::error(404, "Not found");
    }
    std::string content;
    if (!readWholeFile(config_.webDir + "/" + fileName, content)) return HttpResponse::error(404, "Not found");
    HttpResponse response = HttpResponse::text(200, std::move(content), contentTypeFor(fileName));
    response.setHeader("Cache-Control", "no-cache");
    return response;
}

HttpResponse ApiController::listGames(HttpRequest& request) {
    // ?status=ready returns only the games that can be played right now.
    const bool onlyReady = request.queryParam("status") == "ready";
    Json games = Json::array();
    int ready = 0;
    for (const GameEntry& entry : registry_.list()) {
        if (entry.state == GameState::Ready) ++ready;
        if (onlyReady && entry.state != GameState::Ready) continue;
        games.push(gameJson(entry, sessions_.countForGame(entry.id)));
    }
    Json body = Json::object();
    body.set("games", games).set("readyCount", ready).set("gamesFolder", config_.gamesDir);
    return HttpResponse::json(200, body);
}

HttpResponse ApiController::createSession(HttpRequest& request) {
    int status = 500;
    std::string error;
    std::shared_ptr<GameSession> session = sessions_.create(request.params.at("gameId"), status, error);
    if (!session) return HttpResponse::error(status, error);
    Json body = Json::object();
    body.set("session", session->summaryJson())
        .set("url", "/play/" + session->id())
        .set("webSocket", "/ws/" + session->id());
    HttpResponse response = HttpResponse::json(201, body);
    response.setHeader("Location", "/api/sessions/" + session->id());
    return response;
}

HttpResponse ApiController::listSessions(HttpRequest&) {
    Json list = Json::array();
    for (const auto& session : sessions_.list()) list.push(session->summaryJson());
    return HttpResponse::json(200, Json::object().set("sessions", list));
}

HttpResponse ApiController::sessionDetails(HttpRequest& request) {
    std::shared_ptr<GameSession> session = sessions_.find(request.params.at("sessionId"));
    if (!session) return HttpResponse::error(404, "Session not found (it may have ended)");
    return HttpResponse::json(200, session->detailsJson());
}

HttpResponse ApiController::deleteSession(HttpRequest& request) {
    if (!sessions_.remove(request.params.at("sessionId"))) return HttpResponse::error(404, "Session not found");
    return HttpResponse::json(200, Json::object().set("deleted", true));
}

HttpResponse ApiController::stats(HttpRequest&) {
    Json body = Json::object();
    body.set("uptimeSeconds", std::chrono::duration<double>(std::chrono::steady_clock::now() - startedAt_).count())
        .set("http", http_.statsJson())
        .set("sessions", sessions_.statsJson())
        .set("games", registry_.statsJson());
    return HttpResponse::json(200, body);
}

HttpResponse ApiController::webSocket(HttpRequest& request) {
    std::shared_ptr<GameSession> session = sessions_.find(request.params.at("sessionId"));
    if (!session) return HttpResponse::error(404, "Session not found (it may have ended)");

    std::string key;
    std::string error;
    if (!ws::validateUpgradeRequest(request, key, error)) return HttpResponse::error(426, error);

    // Answer "101 Switching Protocols"; after that the HTTP server hands the
    // TCP connection to the session, which turns it into a viewer.
    HttpResponse response = ws::upgradeResponse(key);
    const std::string remoteIp = request.remoteIp;
    std::weak_ptr<GameSession> weakSession = session;
    response.upgrade = [weakSession, remoteIp](Socket socket, std::string pending) {
        if (std::shared_ptr<GameSession> target = weakSession.lock()) {
            target->attachViewer(std::move(socket), std::move(pending), remoteIp);
        }
    };
    return response;
}

}  // namespace cge
