#include "http/Router.hpp"

#include "core/StringUtils.hpp"

namespace mss {

std::vector<std::string> splitPath(const std::string& path) {
    std::vector<std::string> segments;
    for (std::string& part : str::split(path, '/')) {
        if (!part.empty()) segments.push_back(std::move(part));
    }
    return segments;
}

void Router::add(const std::string& method, const std::string& pattern, Handler handler) {
    routes_.push_back(Route{method, splitPath(pattern), std::move(handler)});
}

bool Router::dispatch(HttpRequest& request, HttpResponse& response) const {
    // Decode every segment separately. This way an encoded slash ("%2F")
    // inside a file name stays part of that one segment.
    std::vector<std::string> segments;
    for (const std::string& raw : splitPath(request.path)) {
        std::string decoded;
        if (!str::urlDecode(raw, decoded, false)) {
            response = HttpResponse::error(400, "Malformed percent-encoding in path");
            return true;
        }
        segments.push_back(std::move(decoded));
    }

    bool pathMatchedOtherMethod = false;
    for (const Route& route : routes_) {
        if (route.segments.size() != segments.size()) continue;

        std::map<std::string, std::string> params;
        bool matches = true;
        for (std::size_t i = 0; i < segments.size() && matches; ++i) {
            const std::string& expected = route.segments[i];
            if (expected.size() > 2 && expected.front() == '{' && expected.back() == '}') {
                params[expected.substr(1, expected.size() - 2)] = segments[i];
            } else if (expected != segments[i]) {
                matches = false;
            }
        }
        if (!matches) continue;

        if (route.method != request.method) {
            pathMatchedOtherMethod = true;
            continue;
        }
        request.params = std::move(params);
        response = route.handler(request);
        return true;
    }

    if (pathMatchedOtherMethod) {
        response = HttpResponse::error(405, "Method not allowed");
        return true;
    }
    return false;
}

}  // namespace mss
