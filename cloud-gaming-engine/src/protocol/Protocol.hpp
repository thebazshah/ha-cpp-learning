#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <cge/GameApi.hpp>

// The messages exchanged over the WebSocket between browser and server.
//
// Binary messages use little-endian numbers (the byte order of x86 and ARM,
// and what JavaScript's DataView reads with `true` as the last argument).
// Text messages are JSON (server -> browser only: welcome, state, error).
//
// ---------------------------------------------------------------- client -> server
//
//   JOIN   u8 0x01, u8 nameLength, name (UTF-8), u8 tokenLength, token, u8 spectate (0/1)
//   INPUT  u8 0x02, u8 kind (1-5 = cge::InputKind), u32 sequence, i16 x, i16 y,
//          u8 button, u8 keyLength, key (UTF-8)
//   PING   u8 0x03, f64 clientTimeMs, f32 lastRoundTripMs
//   KEY    u8 0x04                                  "please send me a keyframe"
//
// ---------------------------------------------------------------- server -> client
//
//   FRAME  u8 0x10, u8 flags (bit 0 = keyframe), u32 frameNumber,
//          f64 serverTimeMs, u16 width, u16 height, f32 encodeMs,
//          u8 ackCount, ackCount x (u8 player, u32 lastInputSequence),
//          tile data (see codec/FrameCodec.hpp)
//   PONG   u8 0x11, f64 clientTimeMs (echoed back), f64 serverTimeMs
//
// The "acks" in every frame tell each player which of their inputs is
// already included in this picture. The browser uses that to measure the
// real input-to-screen latency.
namespace cge::protocol {

enum MessageType : std::uint8_t {
    kJoin = 0x01,
    kInput = 0x02,
    kPing = 0x03,
    kKeyframeRequest = 0x04,
    kFrame = 0x10,
    kPong = 0x11,
};

struct JoinRequest {
    std::string name;
    std::string token;      // secret from an earlier "welcome" - lets a player reclaim their slot
    bool spectate = false;  // true = only watch
};

struct InputMessage {
    std::uint32_t sequence = 0;  // increases with every input the client sends
    InputKind kind = InputKind::PointerDown;
    int x = 0;
    int y = 0;
    int button = 0;
    std::string key;
};

struct PingMessage {
    double clientTimeMs = 0;
    float lastRoundTripMs = 0;  // the client's latest measurement, shown on the server's stats
};

struct InputAck {
    std::uint8_t player = 0;
    std::uint32_t sequence = 0;
};

struct FrameHeader {
    bool keyframe = false;
    std::uint32_t frameNumber = 0;
    double serverTimeMs = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    float encodeMs = 0;
    std::vector<InputAck> acks;
};

// ---- parsing (the first byte decides the message type)
bool parseJoin(const std::string& data, JoinRequest& out);
bool parseInput(const std::string& data, InputMessage& out);
bool parsePing(const std::string& data, PingMessage& out);
bool parseFrame(const std::uint8_t* data, std::size_t size, FrameHeader& header, std::size_t& tileDataOffset);
bool parsePong(const std::string& data, double& clientTimeMs, double& serverTimeMs);

// ---- building
std::vector<std::uint8_t> buildFrame(const FrameHeader& header, const std::vector<std::uint8_t>& tileData);
std::vector<std::uint8_t> buildPong(double clientTimeMs, double serverTimeMs);
std::string buildJoin(const JoinRequest& join);
std::string buildInput(const InputMessage& input);
std::string buildPing(const PingMessage& ping);

}  // namespace cge::protocol
