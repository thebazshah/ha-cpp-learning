#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace mss {

// Splits media frames into RTP packets (RFC 3550).
//
// Every RTP packet starts with a 12-byte header:
//   version(2) padding(1) extension(1) csrc-count(4) | marker(1) payload-type(7)
//   sequence number (16 bits)   - +1 per packet, lets the receiver detect loss
//   timestamp (32 bits)         - media time of the frame (90 kHz for video)
//   SSRC (32 bits)              - random id of this stream
//
// Network packets should stay below ~1500 bytes (the usual MTU), so big
// video frames must be split into several RTP packets.
class RtpPacketizer {
public:
    // Called once for each finished RTP packet.
    using PacketSink = std::function<void(const std::uint8_t* packet, std::size_t size)>;

    RtpPacketizer(std::uint8_t payloadType, std::uint32_t ssrc, std::uint16_t firstSequence,
                  std::size_t maxPayload = 1400);

    // H.264 according to RFC 6184 (packetization-mode=1):
    //  * a NAL unit that fits into one packet is sent as it is ("single NAL unit packet");
    //  * a bigger NAL unit is cut into "FU-A" fragments, each with a 2-byte
    //    header that marks the first and last piece.
    // The marker bit is set on the last packet of the picture.
    void packetizeH264(const std::uint8_t* accessUnit, std::size_t size, std::uint32_t timestamp,
                       const PacketSink& sink);

    // AAC according to RFC 3640 (mode=AAC-hbr): one raw AAC frame per packet,
    // preceded by AU-headers-length (16 bits) and one AU-header
    // (13 bits frame size + 3 bits index).
    void packetizeAac(const std::uint8_t* frame, std::size_t size, std::uint32_t timestamp, const PacketSink& sink);

    // The sequence number the next packet will get. Safe to call from another thread.
    std::uint16_t nextSequence() const { return sequence_.load(); }
    std::uint32_t ssrc() const { return ssrc_; }
    std::uint8_t payloadType() const { return payloadType_; }

private:
    void writeHeader(bool marker, std::uint32_t timestamp);

    std::uint8_t payloadType_;
    std::uint32_t ssrc_;
    std::atomic<std::uint16_t> sequence_;
    std::size_t maxPayload_;
    std::vector<std::uint8_t> packet_;  // reused buffer for the packet being built
};

}  // namespace mss
