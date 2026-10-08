#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// RTCP ("RTP Control Protocol", RFC 3550) runs next to RTP and carries
// statistics instead of media:
//  * Sender Reports (SR) from us tell the player which RTP timestamp belongs
//    to which wall-clock time. Players use this to keep audio and video in sync.
//  * Receiver Reports (RR) from the player tell us how many packets were
//    lost. Our adaptive bitrate logic uses that to choose a lower quality.
namespace mss::rtcp {

// The current time as a 64-bit NTP timestamp (seconds since 1900 + fraction).
std::uint64_t ntpNow();

// A Sender Report followed by an SDES packet with our CNAME (a compound
// packet, as RFC 3550 requires).
std::vector<std::uint8_t> buildSenderReport(std::uint32_t ssrc, std::uint64_t ntpTime, std::uint32_t rtpTime,
                                            std::uint32_t packetCount, std::uint32_t octetCount,
                                            const std::string& cname);

// A BYE packet: "this stream has ended". RFC 3550 says RTCP must travel as
// compound packets that start with a report, and some players (FFmpeg)
// ignore a BYE sent on its own - so send it right after a sender report,
// in the same datagram (see RtspSession::goodbyePacket).
std::vector<std::uint8_t> buildBye(std::uint32_t ssrc);

struct ReportBlock {
    std::uint32_t ssrc = 0;          // which of our streams the report is about
    double fractionLost = 0;         // 0.0 ... 1.0 since the previous report
    std::uint32_t cumulativeLost = 0;
    std::uint32_t highestSequence = 0;
    std::uint32_t jitter = 0;        // in RTP timestamp units
};

// Extracts all report blocks from a (compound) RTCP packet.
std::vector<ReportBlock> parseReportBlocks(const std::uint8_t* data, std::size_t size);

}  // namespace mss::rtcp
