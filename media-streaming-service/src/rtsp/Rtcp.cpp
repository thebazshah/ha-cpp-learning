#include "rtsp/Rtcp.hpp"

#include <chrono>

namespace mss::rtcp {
namespace {

void put32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 24));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

std::uint32_t get32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}

constexpr std::uint8_t kSenderReport = 200;
constexpr std::uint8_t kReceiverReport = 201;
constexpr std::uint8_t kSourceDescription = 202;
constexpr std::uint8_t kBye = 203;

}  // namespace

std::uint64_t ntpNow() {
    using namespace std::chrono;
    const auto sinceEpoch = duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
    const std::uint64_t seconds = static_cast<std::uint64_t>(sinceEpoch / 1000000) + 2208988800ULL;  // 1900 -> 1970
    const std::uint64_t micros = static_cast<std::uint64_t>(sinceEpoch % 1000000);
    const std::uint64_t fraction = (micros << 32) / 1000000ULL;
    return (seconds << 32) | fraction;
}

std::vector<std::uint8_t> buildSenderReport(std::uint32_t ssrc, std::uint64_t ntpTime, std::uint32_t rtpTime,
                                            std::uint32_t packetCount, std::uint32_t octetCount,
                                            const std::string& cname) {
    std::vector<std::uint8_t> out;
    // --- Sender Report: 7 words = 28 bytes, so the length field is 6.
    out.push_back(0x80);  // version 2, no padding, 0 report blocks
    out.push_back(kSenderReport);
    out.push_back(0);
    out.push_back(6);
    put32(out, ssrc);
    put32(out, static_cast<std::uint32_t>(ntpTime >> 32));
    put32(out, static_cast<std::uint32_t>(ntpTime));
    put32(out, rtpTime);
    put32(out, packetCount);
    put32(out, octetCount);

    // --- SDES with one CNAME item, padded to a multiple of 4 bytes.
    const std::size_t nameLength = std::min<std::size_t>(cname.size(), 255);
    std::size_t sdesBytes = 4 + 4 + 2 + nameLength + 1;  // header + ssrc + item header + text + end marker
    while (sdesBytes % 4 != 0) ++sdesBytes;
    out.push_back(0x81);  // version 2, 1 chunk
    out.push_back(kSourceDescription);
    const std::uint16_t words = static_cast<std::uint16_t>(sdesBytes / 4 - 1);
    out.push_back(static_cast<std::uint8_t>(words >> 8));
    out.push_back(static_cast<std::uint8_t>(words));
    put32(out, ssrc);
    out.push_back(1);  // item type CNAME
    out.push_back(static_cast<std::uint8_t>(nameLength));
    out.insert(out.end(), cname.begin(), cname.begin() + static_cast<long>(nameLength));
    const std::size_t written = 4 + 4 + 2 + nameLength;
    for (std::size_t i = written; i < sdesBytes; ++i) out.push_back(0);  // end marker + padding
    return out;
}

std::vector<std::uint8_t> buildBye(std::uint32_t ssrc) {
    std::vector<std::uint8_t> out = {0x81, kBye, 0, 1};
    put32(out, ssrc);
    return out;
}

std::vector<ReportBlock> parseReportBlocks(const std::uint8_t* data, std::size_t size) {
    std::vector<ReportBlock> blocks;
    std::size_t offset = 0;
    // A compound packet is several RTCP packets back to back.
    while (offset + 4 <= size) {
        const std::uint8_t* packet = data + offset;
        if ((packet[0] >> 6) != 2) break;  // not RTCP version 2
        const int count = packet[0] & 0x1F;
        const std::uint8_t type = packet[1];
        const std::size_t length = (static_cast<std::size_t>((packet[2] << 8) | packet[3]) + 1) * 4;
        if (offset + length > size) break;

        std::size_t first = 0;
        if (type == kReceiverReport) first = 8;  // header + reporter SSRC
        if (type == kSenderReport) first = 28;   // header + SSRC + sender info
        if (first > 0) {
            for (int i = 0; i < count; ++i) {
                const std::size_t at = first + static_cast<std::size_t>(i) * 24;
                if (at + 24 > length) break;
                const std::uint8_t* block = packet + at;
                ReportBlock report;
                report.ssrc = get32(block);
                report.fractionLost = block[4] / 256.0;
                report.cumulativeLost = (static_cast<std::uint32_t>(block[5]) << 16) | (block[6] << 8) | block[7];
                report.highestSequence = get32(block + 8);
                report.jitter = get32(block + 12);
                blocks.push_back(report);
            }
        }
        offset += length;
    }
    return blocks;
}

}  // namespace mss::rtcp
