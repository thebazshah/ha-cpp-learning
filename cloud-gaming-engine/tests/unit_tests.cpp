// Unit tests for the Cloud Gaming Engine.
//
//   cmake --build build && ./build/unit_tests
//
// Besides small parsers, these tests build real games with the C++ compiler
// (a good one, one with a syntax error, one that crashes, one that throws)
// and play a complete Tic-Tac-Toe round through a real GameSession over
// WebSocket connections.

#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "codec/FrameCodec.hpp"
#include "core/Config.hpp"
#include "core/Sha1.hpp"
#include "core/StringUtils.hpp"
#include "engine/GameLibrary.hpp"
#include "engine/GameRegistry.hpp"
#include "engine/GameSession.hpp"
#include "protocol/Protocol.hpp"
#include "ws/WebSocket.hpp"

using namespace cge;
namespace fs = std::filesystem;

// ------------------------------------------------------------ tiny test framework
namespace {
int gFailures = 0;
int gChecks = 0;
std::string gSelfPath;
std::vector<std::pair<std::string, std::function<void()>>>& registry() {
    static std::vector<std::pair<std::string, std::function<void()>>> tests;
    return tests;
}
struct Register {
    Register(const char* name, std::function<void()> test) { registry().emplace_back(name, std::move(test)); }
};
}  // namespace

#define TEST(name)                                \
    static void name();                           \
    static Register register_##name(#name, name); \
    static void name()

#define CHECK(condition)                                                                              \
    do {                                                                                              \
        ++gChecks;                                                                                    \
        if (!(condition)) {                                                                           \
            ++gFailures;                                                                              \
            std::cerr << "  FAILED: " << #condition << "  (" << __FILE__ << ":" << __LINE__ << ")\n"; \
        }                                                                                             \
    } while (0)

namespace {

std::string hex(const std::array<std::uint8_t, 20>& digest) { return str::toHex(digest.data(), digest.size(), false); }

std::pair<Socket, Socket> socketPair() {
    int fds[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    return {Socket(fds[0]), Socket(fds[1])};
}

// Draws something game-like onto a canvas.
void drawScene(Canvas& canvas, int shift) {
    canvas.clear(0x202030);
    canvas.fillRect(10 + shift, 10, 30, 20, 0xFF0000);
    canvas.fillCircle(60, 40, 15, 0x00FF00);
    canvas.drawText(5, 55, "HELLO", 1, 0xFFFFFF);
}

}  // namespace

// ------------------------------------------------------------ core

TEST(sha1_and_websocket_accept_key) {
    CHECK(hex(sha1("abc")) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(hex(sha1("")) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(hex(sha1(std::string(1000, 'a'))) == "291e9a6c66994949b57ba5e650361e98fc36b1ba");
    // The example from RFC 6455, section 1.3.
    CHECK(ws::acceptKey("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

// ------------------------------------------------------------ codec

TEST(codec_keyframe_and_delta_round_trip) {
    // 100x70 is not a multiple of 16, so edge tiles are smaller.
    Canvas canvas(100, 70);
    codec::FrameEncoder encoder(100, 70);
    codec::FrameDecoder decoder(100, 70);
    CHECK(encoder.tileCount() == 7 * 5);

    drawScene(canvas, 0);
    std::vector<std::uint8_t> delta, key;
    codec::EncodeStats stats;
    encoder.encode(canvas.data(), &delta, &key, stats);
    CHECK(stats.changedTiles == 35);  // the first frame: everything is new
    CHECK(decoder.apply(key.data(), key.size()));
    CHECK(std::equal(decoder.rgba().begin(), decoder.rgba().end(), canvas.data()));

    // Move the red box a little: only a few tiles change.
    drawScene(canvas, 6);
    encoder.encode(canvas.data(), &delta, nullptr, stats);
    CHECK(stats.changedTiles > 0 && stats.changedTiles < 6);
    CHECK(decoder.apply(delta.data(), delta.size()));
    CHECK(std::equal(decoder.rgba().begin(), decoder.rgba().end(), canvas.data()));
    CHECK(delta.size() < key.size() / 3);

    // Nothing changed: an empty delta (just the tile count 0).
    encoder.encode(canvas.data(), &delta, nullptr, stats);
    CHECK(stats.changedTiles == 0);
    CHECK(delta.size() == 2);

    // A noisy tile is stored RAW, a flat one SOLID.
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) canvas.setPixel(x, y, static_cast<Color>((x * 7919 + y * 104729) & 0xFFFFFF));
    encoder.encode(canvas.data(), &delta, &key, stats);
    CHECK(stats.rawTiles >= 1);
    CHECK(stats.solidTiles >= 1);
    codec::FrameDecoder fresh(100, 70);
    CHECK(fresh.apply(key.data(), key.size()));
    CHECK(std::equal(fresh.rgba().begin(), fresh.rgba().end(), canvas.data()));

    // Broken data is rejected.
    std::vector<std::uint8_t> broken(key.begin(), key.begin() + static_cast<long>(key.size() / 2));
    codec::FrameDecoder victim(100, 70);
    CHECK(!victim.apply(broken.data(), broken.size()));
}

// ------------------------------------------------------------ protocol

TEST(protocol_messages_round_trip) {
    protocol::JoinRequest join;
    join.name = "Ana";
    join.token = "secret";
    join.spectate = true;
    protocol::JoinRequest joinBack;
    CHECK(protocol::parseJoin(protocol::buildJoin(join), joinBack));
    CHECK(joinBack.name == "Ana" && joinBack.token == "secret" && joinBack.spectate);

    protocol::InputMessage input;
    input.kind = InputKind::KeyDown;
    input.sequence = 4000000000u;
    input.x = -5;
    input.y = 700;
    input.key = "ArrowUp";
    protocol::InputMessage inputBack;
    CHECK(protocol::parseInput(protocol::buildInput(input), inputBack));
    CHECK(inputBack.kind == InputKind::KeyDown && inputBack.sequence == 4000000000u && inputBack.x == -5 &&
          inputBack.y == 700 && inputBack.key == "ArrowUp");
    CHECK(!protocol::parseInput(std::string("\x02\x09", 2), inputBack));  // too short / bad kind

    protocol::FrameHeader header;
    header.keyframe = true;
    header.frameNumber = 77;
    header.serverTimeMs = 1234.5;
    header.width = 480;
    header.height = 640;
    header.encodeMs = 0.25f;
    header.acks = {{0, 10}, {1, 20}};
    const std::vector<std::uint8_t> tiles = {0, 0};
    const std::vector<std::uint8_t> frame = protocol::buildFrame(header, tiles);
    protocol::FrameHeader back;
    std::size_t offset = 0;
    CHECK(protocol::parseFrame(frame.data(), frame.size(), back, offset));
    CHECK(back.keyframe && back.frameNumber == 77 && back.serverTimeMs == 1234.5 && back.width == 480 &&
          back.height == 640 && back.acks.size() == 2 && back.acks[1].sequence == 20);
    CHECK(offset == frame.size() - 2);
}

// ------------------------------------------------------------ WebSocket

TEST(websocket_frames_over_a_socket_pair) {
    auto sockets = socketPair();
    ws::Connection server(std::move(sockets.first), false);
    ws::Connection client(std::move(sockets.second), true);

    // A big message needs the 64-bit length form; send it from another thread
    // because the socket buffer is smaller than the message.
    const std::string big(70000, 'x');
    std::thread sender([&] {
        CHECK(client.sendText("hello"));
        CHECK(client.sendBinary(big.data(), big.size()));
    });
    ws::Message message;
    CHECK(server.receive(message) && !message.binary && message.data == "hello");
    CHECK(server.receive(message) && message.binary && message.data == big);
    sender.join();

    CHECK(server.sendBinary("\x01\x02", 2));
    CHECK(client.receive(message) && message.binary && message.data == std::string("\x01\x02", 2));

    client.close();
    CHECK(!server.receive(message));  // the close frame ends the connection
}

// ------------------------------------------------------------ building and loading games

namespace {

const char* kBrokenGame = R"(#include <cge/GameApi.hpp>
class Broken : public cge::Game {
    void input(const cge::InputEvent&) override {}
    void update(double) override { this line is not C++ }
    void render(cge::Canvas&) override {}
};
CGE_EXPORT_GAME(Broken, {"Broken", "", 64, 64, 1, 1, 30})
)";

const char* kCrashingGame = R"(#include <cge/GameApi.hpp>
class Crash : public cge::Game {
    int ticks = 0;
    void input(const cge::InputEvent&) override {}
    void update(double) override { ++ticks; }
    void render(cge::Canvas& canvas) override {
        if (ticks > 20) { volatile int* nothing = nullptr; *nothing = 1; }  // a classic bug
        canvas.clear(0);
    }
};
CGE_EXPORT_GAME(Crash, {"Crash", "", 64, 64, 1, 1, 30})
)";

const char* kThrowingGame = R"(#include <cge/GameApi.hpp>
#include <stdexcept>
class Thrower : public cge::Game {
    void input(const cge::InputEvent& e) override { if (e.key == "Escape") throw std::runtime_error("bad key"); }
    void update(double) override {}
    void render(cge::Canvas& canvas) override { canvas.clear(0); }
};
CGE_EXPORT_GAME(Thrower, {"Thrower", "", 64, 64, 1, 1, 30})
)";

std::string gBuiltTicTacToe;  // filled by the registry test, used by the session test

}  // namespace

TEST(registry_builds_tests_and_rejects_bad_games) {
    const fs::path root = fs::temp_directory_path() / ("cge-test-" + std::to_string(getpid()));
    const fs::path games = root / "games";
    fs::create_directories(games / "tictactoe");
    fs::create_directories(games / "broken");
    fs::create_directories(games / "crash");
    fs::create_directories(games / "thrower");
    fs::create_directories(games / "empty");
    fs::copy_file("games/tictactoe/tictactoe.cpp", games / "tictactoe" / "tictactoe.cpp");
    std::ofstream(games / "broken" / "game.cpp") << kBrokenGame;
    std::ofstream(games / "crash" / "game.cpp") << kCrashingGame;
    std::ofstream(games / "thrower" / "game.cpp") << kThrowingGame;

    GameRegistry::Options options;
    options.gamesDir = games.string();
    options.buildDir = (root / "build").string();
    options.sdkIncludeDir = fs::absolute("sdk/include").string();
    options.validatorProgram = gSelfPath;
    GameRegistry registry(options);
    registry.scanNow();

    auto state = [&](const std::string& id) {
        auto entry = registry.find(id);
        return entry ? entry->state : GameState::Building;
    };
    CHECK(state("tictactoe") == GameState::Ready);
    CHECK(state("broken") == GameState::BuildFailed);
    CHECK(state("crash") == GameState::Invalid);
    CHECK(state("thrower") == GameState::Invalid);
    CHECK(state("empty") == GameState::Invalid);
    CHECK(registry.find("broken")->error.find("error") != std::string::npos);  // the compiler message is kept
    CHECK(registry.find("crash")->error.find("crashed") != std::string::npos);
    CHECK(registry.find("thrower")->error.find("bad key") != std::string::npos);

    auto ticTacToe = registry.library("tictactoe");
    CHECK(ticTacToe && ticTacToe->info().name == "Tic-Tac-Toe" && ticTacToe->info().maxPlayers == 2);
    CHECK(!registry.library("broken"));
    if (ticTacToe) {
        // Keep a copy of the built library for the session test.
        gBuiltTicTacToe = (fs::temp_directory_path() / ("cge-ttt-" + std::to_string(getpid()) + ".so")).string();
        fs::copy_file(ticTacToe->path(), gBuiltTicTacToe, fs::copy_options::overwrite_existing);
    }

    // A second scan without changes must not rebuild anything.
    const std::string fingerprint = registry.find("tictactoe")->fingerprint;
    registry.scanNow();
    CHECK(registry.find("tictactoe")->fingerprint == fingerprint);

    // Deleting a folder removes the game.
    fs::remove_all(games / "empty");
    registry.scanNow();
    CHECK(!registry.find("empty"));
    fs::remove_all(root);
}

// ------------------------------------------------------------ a full session

namespace {

// A test client on one end of a socket pair: reads messages and decodes frames.
struct TestClient {
    std::unique_ptr<ws::Connection> connection;
    std::unique_ptr<codec::FrameDecoder> decoder;
    std::string lastStatus;
    std::string welcome;
    std::uint32_t lastFrame = 0;
    std::uint32_t ackedSequence = 0;
    int player = -1;
    bool decodeOk = true;
    int keyframes = 0;

    // Reads messages until `done()` returns true (false after a timeout).
    bool readUntil(const std::function<bool()>& done) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        ws::Message message;
        while (!done()) {
            if (std::chrono::steady_clock::now() > deadline || !connection->receive(message)) return false;
            if (!message.binary) {
                if (message.data.find("\"type\":\"welcome\"") != std::string::npos) {
                    welcome = message.data;
                    const std::size_t at = message.data.find("\"player\":");
                    player = std::atoi(message.data.c_str() + at + 9);
                }
                const std::size_t at = message.data.find("\"status\":\"");
                if (message.data.find("\"type\":\"state\"") != std::string::npos && at != std::string::npos) {
                    lastStatus = message.data.substr(at + 10, message.data.find('"', at + 10) - at - 10);
                }
                continue;
            }
            const auto* data = reinterpret_cast<const std::uint8_t*>(message.data.data());
            if (data[0] != protocol::kFrame) continue;
            protocol::FrameHeader header;
            std::size_t offset = 0;
            if (!protocol::parseFrame(data, message.data.size(), header, offset)) continue;
            if (!decoder) decoder = std::make_unique<codec::FrameDecoder>(header.width, header.height);
            decodeOk = decodeOk && decoder->apply(data + offset, message.data.size() - offset);
            lastFrame = header.frameNumber;
            if (header.keyframe) ++keyframes;
            for (const auto& ack : header.acks) {
                if (ack.player == player) ackedSequence = ack.sequence;
            }
        }
        return true;
    }

    void send(const std::string& message) { connection->sendBinary(message.data(), message.size()); }
};

}  // namespace

TEST(session_plays_a_full_round_of_tic_tac_toe) {
    if (gBuiltTicTacToe.empty()) {
        std::cout << "  (skipped: the registry test did not build Tic-Tac-Toe)\n";
        return;
    }
    std::string error;
    auto library = GameLibrary::load(gBuiltTicTacToe, error);
    CHECK(library != nullptr);
    if (!library) return;

    auto session = std::make_shared<GameSession>("testsess", "tictactoe", library, SessionOptions{});
    session->start();

    TestClient alice, bob;
    for (TestClient* client : {&alice, &bob}) {
        auto sockets = socketPair();
        sockets.second.setTimeouts(5000, 5000);
        session->attachViewer(std::move(sockets.first), "", "127.0.0.1");
        client->connection = std::make_unique<ws::Connection>(std::move(sockets.second), true, "", 16 << 20);
    }
    protocol::JoinRequest join;
    join.name = "Alice";
    alice.send(protocol::buildJoin(join));
    CHECK(alice.readUntil([&] { return alice.player == 0 && alice.decoder != nullptr; }));
    join.name = "Bob";
    bob.send(protocol::buildJoin(join));
    CHECK(bob.readUntil([&] { return bob.player == 1 && bob.decoder != nullptr; }));
    CHECK(alice.welcome.find("\"role\":\"player\"") != std::string::npos);

    // X (Alice) takes the top row: squares 1, 2, 3. O (Bob) plays 4 and 5.
    std::uint32_t sequence[2] = {0, 0};
    auto press = [&](TestClient& who, const std::string& key) {
        protocol::InputMessage input;
        input.kind = InputKind::KeyDown;
        input.key = key;
        input.sequence = ++sequence[who.player];
        who.send(protocol::buildInput(input));
        const std::uint32_t wanted = input.sequence;
        CHECK(who.readUntil([&] { return who.ackedSequence >= wanted; }));  // wait until the game applied it
    };
    press(alice, "1");
    press(bob, "4");
    press(alice, "2");
    press(bob, "5");
    press(alice, "3");

    CHECK(alice.readUntil([&] { return alice.lastStatus.find("wins") != std::string::npos; }));
    CHECK(alice.lastStatus.find("Alice") != std::string::npos);
    const Json details = session->detailsJson();
    CHECK(details.dump().find("\"name\":\"X wins\",\"value\":\"1\"") != std::string::npos);

    // Both players must see exactly the same picture. Once the winning-line
    // animation is over the picture stays still; then both ask for a fresh
    // keyframe, which must decode to identical pixels.
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    for (TestClient* client : {&alice, &bob}) {
        const int before = client->keyframes;
        client->send(std::string(1, static_cast<char>(protocol::kKeyframeRequest)));
        CHECK(client->readUntil([&] { return client->keyframes > before; }));
    }
    CHECK(alice.decodeOk && bob.decodeOk);
    CHECK(alice.decoder->rgba() == bob.decoder->rgba());

    alice.connection->close();
    bob.connection->close();
    session->stop();
    std::remove(gBuiltTicTacToe.c_str());
}

// ------------------------------------------------------------ main

int main(int argc, char** argv) {
    // The registry test starts this program again as the game validator.
    if (argc == 3 && std::string(argv[1]) == "--validate-game") return runGameValidation(argv[2]);
    gSelfPath = executablePath(argv[0]);

    for (const auto& test : registry()) {
        const int before = gFailures;
        std::cout << "[ RUN  ] " << test.first << "\n";
        test.second();
        std::cout << (gFailures == before ? "[  OK  ] " : "[ FAIL ] ") << test.first << "\n";
    }
    std::cout << "\n" << registry().size() << " tests, " << gChecks << " checks, " << gFailures << " failures\n";
    return gFailures == 0 ? 0 : 1;
}
