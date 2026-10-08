#pragma once

#include <chrono>
#include <string>

#include "core/Config.hpp"
#include "engine/GameRegistry.hpp"
#include "engine/SessionManager.hpp"
#include "http/HttpServer.hpp"
#include "http/Router.hpp"

namespace cge {

// The REST API, the WebSocket entry point and the web page.
//
//   GET    /                                  lobby (web/index.html)
//   GET    /play/{sessionId}                  game page (the same index.html)
//   GET    /static/{file}                     app.js, style.css
//   GET    /api/health                        is the server alive?
//   GET    /api/games                         all games: ready ones and ones with errors
//   POST   /api/games/{gameId}/sessions       start a new session -> {session, url}
//   GET    /api/sessions                      running sessions
//   GET    /api/sessions/{sessionId}          one session with all metrics
//   DELETE /api/sessions/{sessionId}          end a session
//   GET    /api/stats                         server-wide numbers
//   GET    /ws/{sessionId}                    WebSocket: join and play (see protocol/Protocol.hpp)
class ApiController {
public:
    ApiController(const Config& config, GameRegistry& registry, SessionManager& sessions, const HttpServer& http);
    void registerRoutes(Router& router);

private:
    HttpResponse listGames(HttpRequest& request);
    HttpResponse createSession(HttpRequest& request);
    HttpResponse listSessions(HttpRequest& request);
    HttpResponse sessionDetails(HttpRequest& request);
    HttpResponse deleteSession(HttpRequest& request);
    HttpResponse stats(HttpRequest& request);
    HttpResponse webSocket(HttpRequest& request);
    HttpResponse staticFile(const std::string& fileName);

    const Config& config_;
    GameRegistry& registry_;
    SessionManager& sessions_;
    const HttpServer& http_;
    const std::chrono::steady_clock::time_point startedAt_;
};

}  // namespace cge
