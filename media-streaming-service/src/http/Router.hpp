#pragma once

#include <functional>
#include <string>
#include <vector>

#include "http/HttpMessage.hpp"

namespace mss {

// Maps "METHOD + path pattern" to a handler function.
//
// Patterns are split into segments by '/'. A segment written as {name}
// matches any single path segment and its decoded value is stored in
// request.params["name"]:
//
//   router.add("GET", "/api/media/{type}/{name}", handler);
//   // GET /api/media/videos/my%20movie.mp4
//   //   -> params["type"] = "videos", params["name"] = "my movie.mp4"
class Router {
public:
    using Handler = std::function<HttpResponse(HttpRequest&)>;

    void add(const std::string& method, const std::string& pattern, Handler handler);

    // Finds the matching route and runs its handler. Returns false if no
    // pattern matches the path. If the path matches but the method does not,
    // `response` becomes 405 Method Not Allowed and true is returned.
    bool dispatch(HttpRequest& request, HttpResponse& response) const;

private:
    struct Route {
        std::string method;
        std::vector<std::string> segments;
        Handler handler;
    };
    std::vector<Route> routes_;
};

// Splits a path into non-empty segments: "/a//b/" -> {"a", "b"}.
std::vector<std::string> splitPath(const std::string& path);

}  // namespace mss
