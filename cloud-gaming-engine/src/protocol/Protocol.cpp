#include "protocol/Protocol.hpp"

#include <algorithm>
#include <cstring>

namespace cge::protocol {
namespace {

// Appends numbers in little-endian byte order, independent of the CPU.
class Writer {
public:
    void u8(std::uint8_t value) { bytes.push_back(value); }
    void u16(std::uint16_t value) { put(value, 2); }
    void u32(std::uint32_t value) { put(value, 4); }
    void i16(std::int16_t value) { put(static_cast<std::uint16_t>(value), 2); }
    void f32(float value) {
        std::uint32_t bits;
        std::memcpy(&bits, &value, 4);
        put(bits, 4);
    }
    void f64(double value) {
        std::uint64_t bits;
        std::memcpy(&bits, &value, 8);
        put(bits, 8);
    }
    void shortString(const std::string& text) {  // u8 length + bytes (max 255)
        const std::size_t length = std::min<std::size_t>(text.size(), 255);
        u8(static_cast<std::uint8_t>(length));
        bytes.insert(bytes.end(), text.begin(), text.begin() + static_cast<long>(length));
    }
    std::vector<std::uint8_t> bytes;

private:
    void put(std::uint64_t value, int count) {
        for (int i = 0; i < count; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }
};

// Reads little-endian numbers; any read past the end makes ok() false.
class Reader {
public:
    Reader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}
    std::uint8_t u8() { return static_cast<std::uint8_t>(get(1)); }
    std::uint16_t u16() { return static_cast<std::uint16_t>(get(2)); }
    std::uint32_t u32() { return static_cast<std::uint32_t>(get(4)); }
    std::int16_t i16() { return static_cast<std::int16_t>(static_cast<std::uint16_t>(get(2))); }
    float f32() {
        const std::uint32_t bits = static_cast<std::uint32_t>(get(4));
        float value;
        std::memcpy(&value, &bits, 4);
        return value;
    }
    double f64() {
        const std::uint64_t bits = get(8);
        double value;
        std::memcpy(&value, &bits, 8);
        return value;
    }
    std::string shortString() {
        const std::size_t length = u8();
        if (!ok_ || position_ + length > size_) {
            ok_ = false;
            return "";
        }
        std::string text(reinterpret_cast<const char*>(data_ + position_), length);
        position_ += length;
        return text;
    }
    bool ok() const { return ok_; }
    std::size_t position() const { return position_; }

private:
    std::uint64_t get(int count) {
        if (position_ + static_cast<std::size_t>(count) > size_) {
            ok_ = false;
            return 0;
        }
        std::uint64_t value = 0;
        for (int i = 0; i < count; ++i) value |= static_cast<std::uint64_t>(data_[position_ + i]) << (8 * i);
        position_ += static_cast<std::size_t>(count);
        return value;
    }

    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t position_ = 0;
    bool ok_ = true;
};

Reader readerFor(const std::string& data) {
    return Reader(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
}

std::string toString(const std::vector<std::uint8_t>& bytes) { return std::string(bytes.begin(), bytes.end()); }

}  // namespace

// ------------------------------------------------------------------ parsing

bool parseJoin(const std::string& data, JoinRequest& out) {
    Reader reader = readerFor(data);
    if (reader.u8() != kJoin) return false;
    JoinRequest join;
    join.name = reader.shortString();
    join.token = reader.shortString();
    join.spectate = reader.u8() != 0;
    if (!reader.ok()) return false;
    out = join;
    return true;
}

bool parseInput(const std::string& data, InputMessage& out) {
    Reader reader = readerFor(data);
    if (reader.u8() != kInput) return false;
    InputMessage input;
    const std::uint8_t kind = reader.u8();
    input.sequence = reader.u32();
    input.x = reader.i16();
    input.y = reader.i16();
    input.button = reader.u8();
    input.key = reader.shortString();
    if (!reader.ok() || kind < 1 || kind > 5) return false;
    input.kind = static_cast<InputKind>(kind);
    out = input;
    return true;
}

bool parsePing(const std::string& data, PingMessage& out) {
    Reader reader = readerFor(data);
    if (reader.u8() != kPing) return false;
    PingMessage ping;
    ping.clientTimeMs = reader.f64();
    ping.lastRoundTripMs = reader.f32();
    if (!reader.ok()) return false;
    out = ping;
    return true;
}

bool parseFrame(const std::uint8_t* data, std::size_t size, FrameHeader& header, std::size_t& tileDataOffset) {
    Reader reader(data, size);
    if (reader.u8() != kFrame) return false;
    FrameHeader parsed;
    parsed.keyframe = (reader.u8() & 0x01) != 0;
    parsed.frameNumber = reader.u32();
    parsed.serverTimeMs = reader.f64();
    parsed.width = reader.u16();
    parsed.height = reader.u16();
    parsed.encodeMs = reader.f32();
    const int ackCount = reader.u8();
    for (int i = 0; i < ackCount; ++i) {
        InputAck ack;
        ack.player = reader.u8();
        ack.sequence = reader.u32();
        parsed.acks.push_back(ack);
    }
    if (!reader.ok()) return false;
    header = parsed;
    tileDataOffset = reader.position();
    return true;
}

bool parsePong(const std::string& data, double& clientTimeMs, double& serverTimeMs) {
    Reader reader = readerFor(data);
    if (reader.u8() != kPong) return false;
    clientTimeMs = reader.f64();
    serverTimeMs = reader.f64();
    return reader.ok();
}

// ------------------------------------------------------------------ building

std::vector<std::uint8_t> buildFrame(const FrameHeader& header, const std::vector<std::uint8_t>& tileData) {
    Writer writer;
    writer.bytes.reserve(32 + header.acks.size() * 5 + tileData.size());
    writer.u8(kFrame);
    writer.u8(header.keyframe ? 1 : 0);
    writer.u32(header.frameNumber);
    writer.f64(header.serverTimeMs);
    writer.u16(header.width);
    writer.u16(header.height);
    writer.f32(header.encodeMs);
    writer.u8(static_cast<std::uint8_t>(std::min<std::size_t>(header.acks.size(), 255)));
    for (std::size_t i = 0; i < header.acks.size() && i < 255; ++i) {
        writer.u8(header.acks[i].player);
        writer.u32(header.acks[i].sequence);
    }
    writer.bytes.insert(writer.bytes.end(), tileData.begin(), tileData.end());
    return std::move(writer.bytes);
}

std::vector<std::uint8_t> buildPong(double clientTimeMs, double serverTimeMs) {
    Writer writer;
    writer.u8(kPong);
    writer.f64(clientTimeMs);
    writer.f64(serverTimeMs);
    return std::move(writer.bytes);
}

std::string buildJoin(const JoinRequest& join) {
    Writer writer;
    writer.u8(kJoin);
    writer.shortString(join.name);
    writer.shortString(join.token);
    writer.u8(join.spectate ? 1 : 0);
    return toString(writer.bytes);
}

std::string buildInput(const InputMessage& input) {
    Writer writer;
    writer.u8(kInput);
    writer.u8(static_cast<std::uint8_t>(input.kind));
    writer.u32(input.sequence);
    writer.i16(static_cast<std::int16_t>(input.x));
    writer.i16(static_cast<std::int16_t>(input.y));
    writer.u8(static_cast<std::uint8_t>(input.button));
    writer.shortString(input.key);
    return toString(writer.bytes);
}

std::string buildPing(const PingMessage& ping) {
    Writer writer;
    writer.u8(kPing);
    writer.f64(ping.clientTimeMs);
    writer.f32(ping.lastRoundTripMs);
    return toString(writer.bytes);
}

}  // namespace cge::protocol
