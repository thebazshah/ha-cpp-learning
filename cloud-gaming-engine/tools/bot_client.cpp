// cge-bot: a tiny command-line client for the Cloud Gaming Engine.
//
// It does everything the browser does, without a screen:
//   1. opens a WebSocket to /ws/<session> (an HTTP upgrade on a TCP socket),
//   2. sends JOIN with a name,
//   3. decodes every video frame with the same codec as the browser,
//   4. sends random clicks (input forwarding) and pings (latency measurement),
//   5. prints statistics at the end and can save the last frame as an image.
//
// Examples:
//   ./build/cge-bot --session abcd2345 --name Robo --seconds 20
//   ./build/cge-bot --session abcd2345 --spectate --save last-frame.ppm
//
// Several bots can play against each other, which makes a nice load test.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>

#include "codec/FrameCodec.hpp"
#include "protocol/Protocol.hpp"
#include "ws/WebSocket.hpp"

using namespace cge;
using Clock = std::chrono::steady_clock;

namespace {

struct Options {
    std::string host = "127.0.0.1";
    int port = 8090;
    std::string session;
    std::string name = "Bot";
    int seconds = 15;
    int clickMs = 600;
    bool spectate = false;
    bool quiet = false;
    std::string savePath;
};

double nowMs() {
    return std::chrono::duration<double, std::milli>(Clock::now().time_since_epoch()).count();
}

// Reads a whole number that follows "key": in a JSON text (enough for our needs).
int jsonInt(const std::string& text, const std::string& key, int fallback) {
    const std::size_t at = text.find("\"" + key + "\":");
    if (at == std::string::npos) return fallback;
    return std::atoi(text.c_str() + at + key.size() + 3);
}

std::string jsonString(const std::string& text, const std::string& key) {
    const std::size_t at = text.find("\"" + key + "\":\"");
    if (at == std::string::npos) return "";
    const std::size_t start = at + key.size() + 4;
    return text.substr(start, text.find('"', start) - start);
}

bool parseOptions(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (arg == "--host") options.host = next();
        else if (arg == "--port") options.port = std::atoi(next().c_str());
        else if (arg == "--session") options.session = next();
        else if (arg == "--name") options.name = next();
        else if (arg == "--seconds") options.seconds = std::atoi(next().c_str());
        else if (arg == "--click-ms") options.clickMs = std::atoi(next().c_str());
        else if (arg == "--spectate") options.spectate = true;
        else if (arg == "--quiet") options.quiet = true;
        else if (arg == "--save") options.savePath = next();
        else return false;
    }
    return !options.session.empty() && options.port > 0 && options.seconds > 0 && options.clickMs > 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parseOptions(argc, argv, options)) {
        std::cerr << "Usage: cge-bot --session ID [--host 127.0.0.1] [--port 8090] [--name Bot] [--seconds 15]\n"
                     "               [--click-ms 600] [--spectate] [--save frame.ppm] [--quiet]\n";
        return 2;
    }

    std::string error;
    std::unique_ptr<ws::Connection> connection =
        ws::connect(options.host, static_cast<std::uint16_t>(options.port), "/ws/" + options.session, error);
    if (!connection) {
        std::cerr << "Cannot connect: " << error << "\n";
        return 1;
    }

    // ---- shared between the reader thread and the main thread
    std::mutex mutex;
    std::unique_ptr<codec::FrameDecoder> decoder;
    std::map<std::uint32_t, double> inputSentAt;  // sequence -> time sent
    int myPlayer = -1;
    std::string role = "?";
    std::string lastStatus;
    std::atomic<int> width{0}, height{0};
    std::uint64_t frames = 0, keyframes = 0, deltas = 0, bytes = 0, decodeErrors = 0, serverErrors = 0;
    double rttSum = 0, rttLast = 0, latencySum = 0, latencyMax = 0;
    int rttCount = 0, latencyCount = 0;

    // ---- reader thread: everything the server sends
    std::thread reader([&] {
        ws::Message message;
        while (connection->receive(message)) {
            std::lock_guard<std::mutex> lock(mutex);
            if (!message.binary) {
                const std::string type = jsonString(message.data, "type");
                if (type == "welcome") {
                    myPlayer = jsonInt(message.data, "player", -1);
                    role = jsonString(message.data, "role");
                    if (!options.quiet) std::cout << "Joined as " << role << (myPlayer >= 0 ? " (player " + std::to_string(myPlayer + 1) + ")" : "") << "\n";
                } else if (type == "state") {
                    const std::string status = jsonString(message.data, "status");
                    if (status != lastStatus && !options.quiet) std::cout << "Game: " << status << "\n";
                    lastStatus = status;
                } else if (type == "error") {
                    ++serverErrors;
                    std::cout << "Server error: " << jsonString(message.data, "message") << "\n";
                }
                continue;
            }
            const auto* data = reinterpret_cast<const std::uint8_t*>(message.data.data());
            if (message.data.empty()) continue;
            if (data[0] == protocol::kPong) {
                double sentAt = 0, serverTime = 0;
                if (protocol::parsePong(message.data, sentAt, serverTime)) {
                    rttLast = nowMs() - sentAt;
                    rttSum += rttLast;
                    ++rttCount;
                }
                continue;
            }
            protocol::FrameHeader header;
            std::size_t offset = 0;
            if (!protocol::parseFrame(data, message.data.size(), header, offset)) continue;
            if (!decoder) {
                decoder = std::make_unique<codec::FrameDecoder>(header.width, header.height);
                width = header.width;
                height = header.height;
            }
            if (!decoder->apply(data + offset, message.data.size() - offset)) ++decodeErrors;
            ++frames;
            bytes += message.data.size();
            if (header.keyframe) ++keyframes;
            else ++deltas;
            // Input-to-frame latency: the frame says which of our inputs it already includes.
            for (const protocol::InputAck& ack : header.acks) {
                if (static_cast<int>(ack.player) != myPlayer) continue;
                for (auto it = inputSentAt.begin(); it != inputSentAt.end() && it->first <= ack.sequence;) {
                    const double latency = nowMs() - it->second;
                    latencySum += latency;
                    latencyMax = std::max(latencyMax, latency);
                    ++latencyCount;
                    it = inputSentAt.erase(it);
                }
            }
        }
    });

    // ---- main thread: join, then click and ping until the time is up
    protocol::JoinRequest join;
    join.name = options.name;
    join.spectate = options.spectate;
    const std::string joinMessage = protocol::buildJoin(join);
    connection->sendBinary(joinMessage.data(), joinMessage.size());

    std::mt19937 random(std::random_device{}());
    std::uint32_t sequence = 0;
    const auto end = Clock::now() + std::chrono::seconds(options.seconds);
    auto nextClick = Clock::now() + std::chrono::milliseconds(options.clickMs);
    auto nextPing = Clock::now();
    while (Clock::now() < end && !connection->isClosed()) {
        const auto now = Clock::now();
        if (now >= nextPing) {
            protocol::PingMessage ping;
            ping.clientTimeMs = nowMs();
            {
                std::lock_guard<std::mutex> lock(mutex);
                ping.lastRoundTripMs = static_cast<float>(rttLast);
            }
            const std::string message = protocol::buildPing(ping);
            connection->sendBinary(message.data(), message.size());
            nextPing = now + std::chrono::seconds(1);
        }
        if (!options.spectate && now >= nextClick && width > 0) {
            // A random click somewhere on the canvas (games ignore useless clicks).
            protocol::InputMessage input;
            input.kind = InputKind::PointerDown;
            input.x = static_cast<int>(random() % static_cast<unsigned>(width.load()));
            input.y = static_cast<int>(random() % static_cast<unsigned>(height.load()));
            input.sequence = ++sequence;
            {
                std::lock_guard<std::mutex> lock(mutex);
                inputSentAt[input.sequence] = nowMs();
            }
            const std::string message = protocol::buildInput(input);
            connection->sendBinary(message.data(), message.size());
            nextClick = now + std::chrono::milliseconds(options.clickMs);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    connection->close();
    reader.join();

    // ---- summary
    std::lock_guard<std::mutex> lock(mutex);
    std::printf("\n=== %s summary (%d s) ===\n", options.name.c_str(), options.seconds);
    std::printf("role               : %s\n", role.c_str());
    std::printf("frames received    : %llu (%llu keyframes, %llu deltas)\n", static_cast<unsigned long long>(frames),
                static_cast<unsigned long long>(keyframes), static_cast<unsigned long long>(deltas));
    std::printf("data received      : %.1f KB (%.1f kbps)\n", bytes / 1024.0, bytes * 8.0 / 1000.0 / options.seconds);
    std::printf("decode errors      : %llu\n", static_cast<unsigned long long>(decodeErrors));
    std::printf("round trip time    : %.2f ms average over %d pings\n", rttCount ? rttSum / rttCount : 0.0, rttCount);
    std::printf("input -> frame     : %.2f ms average, %.2f ms worst (%d inputs)\n",
                latencyCount ? latencySum / latencyCount : 0.0, latencyMax, latencyCount);
    std::printf("last game status   : %s\n", lastStatus.c_str());

    if (!options.savePath.empty() && decoder) {
        // PPM is the simplest image format: a short text header + raw RGB.
        if (FILE* file = std::fopen(options.savePath.c_str(), "wb")) {
            std::fprintf(file, "P6 %d %d 255\n", decoder->width(), decoder->height());
            const auto& pixels = decoder->rgba();
            for (std::size_t i = 0; i < pixels.size(); i += 4) std::fwrite(&pixels[i], 1, 3, file);
            std::fclose(file);
            std::printf("last frame saved   : %s\n", options.savePath.c_str());
        }
    }
    return (decodeErrors == 0 && serverErrors == 0 && frames > 0) ? 0 : 1;
}
