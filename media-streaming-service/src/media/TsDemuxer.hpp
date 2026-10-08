#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "media/Aac.hpp"

// Reads MPEG-TS (MPEG Transport Stream) files - the format of our HLS segments.
//
// Quick background:
//  * A TS file is a sequence of 188-byte packets. Each packet starts with the
//    sync byte 0x47 and carries a 13-bit PID (packet identifier).
//  * PID 0 holds the PAT (Program Association Table), which points to the
//    PMT (Program Map Table). The PMT lists which PID carries video (H.264)
//    and which carries audio (AAC).
//  * Audio/video data is split into PES packets ("Packetized Elementary
//    Stream"). A PES packet has a header with timestamps (PTS/DTS) and spans
//    many TS packets.
//  * Timestamps use a 90 kHz clock: 90000 ticks = 1 second.
//
// The demuxer turns a segment into a list of timed frames that the RTSP
// server can send over RTP.
namespace mss {

enum class StreamKind { Video, Audio };

struct MediaFrame {
    StreamKind kind = StreamKind::Video;
    std::int64_t pts = 0;  // presentation time stamp (90 kHz)
    std::int64_t dts = 0;  // decode time stamp (90 kHz); frames must be sent in DTS order
    bool keyframe = false;
    // Video: one complete access unit (picture) in Annex B format.
    // Audio: one raw AAC frame (the ADTS header is removed).
    std::vector<std::uint8_t> data;
};

struct DemuxResult {
    std::vector<MediaFrame> frames;  // sorted by DTS (audio and video interleaved)
    bool hasVideo = false;
    bool hasAudio = false;
    std::vector<std::uint8_t> sps;  // first H.264 SPS NAL unit found (no start code)
    std::vector<std::uint8_t> pps;  // first H.264 PPS NAL unit found
    bool audioConfigKnown = false;
    aac::AdtsHeader audioConfig;    // configuration of the AAC audio
};

// Demuxes a complete MPEG-TS buffer. Returns false (and fills `error`) when
// the data is not a usable transport stream.
bool demuxTransportStream(const std::uint8_t* data, std::size_t size, DemuxResult& out, std::string& error);

}  // namespace mss
