#include "rtsp/RtpPacketizer.hpp"

#include <algorithm>

#include "media/H264.hpp"

namespace mss {
namespace {
constexpr std::size_t kHeaderSize = 12;
constexpr std::uint8_t kFuA = 28;  // NAL type number used for FU-A fragments
}  // namespace

RtpPacketizer::RtpPacketizer(std::uint8_t payloadType, std::uint32_t ssrc, std::uint16_t firstSequence,
                             std::size_t maxPayload)
    : payloadType_(payloadType), ssrc_(ssrc), sequence_(firstSequence), maxPayload_(maxPayload) {
    packet_.reserve(kHeaderSize + maxPayload_ + 16);
}

void RtpPacketizer::writeHeader(bool marker, std::uint32_t timestamp) {
    // fetch_add returns the current number and moves the counter on by one;
    // it wraps around from 65535 to 0 automatically.
    const std::uint16_t sequence = sequence_.fetch_add(1);
    packet_.clear();
    packet_.push_back(0x80);  // version 2, no padding, no extension, no CSRCs
    packet_.push_back(static_cast<std::uint8_t>((marker ? 0x80 : 0x00) | (payloadType_ & 0x7F)));
    packet_.push_back(static_cast<std::uint8_t>(sequence >> 8));
    packet_.push_back(static_cast<std::uint8_t>(sequence & 0xFF));
    packet_.push_back(static_cast<std::uint8_t>(timestamp >> 24));
    packet_.push_back(static_cast<std::uint8_t>(timestamp >> 16));
    packet_.push_back(static_cast<std::uint8_t>(timestamp >> 8));
    packet_.push_back(static_cast<std::uint8_t>(timestamp));
    packet_.push_back(static_cast<std::uint8_t>(ssrc_ >> 24));
    packet_.push_back(static_cast<std::uint8_t>(ssrc_ >> 16));
    packet_.push_back(static_cast<std::uint8_t>(ssrc_ >> 8));
    packet_.push_back(static_cast<std::uint8_t>(ssrc_));
}

void RtpPacketizer::packetizeH264(const std::uint8_t* accessUnit, std::size_t size, std::uint32_t timestamp,
                                  const PacketSink& sink) {
    std::vector<h264::NalUnit> units = h264::splitAnnexB(accessUnit, size);

    // Access unit delimiters are not needed over RTP (the marker bit and the
    // timestamp already show where a picture ends).
    std::vector<h264::NalUnit> nals;
    for (const h264::NalUnit& unit : units) {
        if (unit.type() != h264::kNalAud) nals.push_back(unit);
    }

    for (std::size_t n = 0; n < nals.size(); ++n) {
        const std::uint8_t* nal = nals[n].data;
        const std::size_t nalSize = nals[n].size;
        const bool lastNal = (n + 1 == nals.size());

        if (nalSize <= maxPayload_) {
            // Fits: one packet containing the whole NAL unit.
            writeHeader(lastNal, timestamp);
            packet_.insert(packet_.end(), nal, nal + nalSize);
            sink(packet_.data(), packet_.size());
            continue;
        }

        // Too big: FU-A fragmentation. The original NAL header byte is not
        // copied; instead its parts are stored in the two FU bytes:
        //   FU indicator = F/NRI bits of the NAL header + type 28
        //   FU header    = start bit, end bit + the original NAL type
        const std::uint8_t indicator = static_cast<std::uint8_t>((nal[0] & 0xE0) | kFuA);
        const std::uint8_t nalType = nal[0] & 0x1F;
        const std::size_t chunkSize = maxPayload_ - 2;
        std::size_t offset = 1;  // skip the NAL header byte
        while (offset < nalSize) {
            const std::size_t length = std::min(chunkSize, nalSize - offset);
            const bool first = (offset == 1);
            const bool last = (offset + length == nalSize);
            writeHeader(last && lastNal, timestamp);
            packet_.push_back(indicator);
            packet_.push_back(static_cast<std::uint8_t>((first ? 0x80 : 0x00) | (last ? 0x40 : 0x00) | nalType));
            packet_.insert(packet_.end(), nal + offset, nal + offset + length);
            sink(packet_.data(), packet_.size());
            offset += length;
        }
    }
}

void RtpPacketizer::packetizeAac(const std::uint8_t* frame, std::size_t size, std::uint32_t timestamp,
                                 const PacketSink& sink) {
    if (size == 0 || size > 8191) return;  // the size field has only 13 bits
    writeHeader(true, timestamp);           // each packet holds a complete frame
    packet_.push_back(0x00);                // AU-headers-length = 16 bits ...
    packet_.push_back(0x10);
    packet_.push_back(static_cast<std::uint8_t>(size >> 5));           // AU-size (13 bits) ...
    packet_.push_back(static_cast<std::uint8_t>((size & 0x1F) << 3));  // ... + AU-index (3 bits) = 0
    packet_.insert(packet_.end(), frame, frame + size);
    sink(packet_.data(), packet_.size());
}

}  // namespace mss
