#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "media/Aac.hpp"

namespace mss {

// Everything needed to describe a stream in SDP ("Session Description
// Protocol", RFC 4566). The RTSP DESCRIBE response contains this text and
// tells the player which tracks exist and how to decode them.
struct SdpDescription {
    std::string sessionName;
    std::string serverIp;
    double durationSeconds = 0;

    bool hasVideo = false;
    int videoTrackId = 0;
    int videoPayloadType = 96;  // 96-127 are "dynamic" RTP payload types
    std::string profileLevelId; // H.264 profile/level, e.g. "4D401F"
    std::vector<std::uint8_t> sps;
    std::vector<std::uint8_t> pps;

    bool hasAudio = false;
    int audioTrackId = 1;
    int audioPayloadType = 97;
    aac::AdtsHeader audioConfig;
};

// Builds the SDP text. Example for a video with audio:
//   m=video 0 RTP/AVP 96
//   a=rtpmap:96 H264/90000
//   a=fmtp:96 packetization-mode=1;profile-level-id=4D401E;sprop-parameter-sets=Z01AHp...,aO48gA==
//   a=control:trackID=0
//   m=audio 0 RTP/AVP 97
//   a=rtpmap:97 MPEG4-GENERIC/48000/2
//   a=fmtp:97 streamtype=5;profile-level-id=1;mode=AAC-hbr;sizelength=13;indexlength=3;indexdeltalength=3;config=1190
//   a=control:trackID=1
std::string buildSdp(const SdpDescription& description);

}  // namespace mss
