#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Helpers for AAC audio in ADTS format.
//
// Quick background:
//  * AAC audio is stored as a sequence of frames. Each frame holds 1024
//    audio samples per channel (about 21 ms at 48 kHz).
//  * In MPEG-TS files every AAC frame starts with a 7-byte (or 9-byte) ADTS
//    header that says how long the frame is and how the audio is configured
//    (sample rate, channels, AAC profile).
//  * RTP/RTSP does not send ADTS headers. Instead the audio configuration is
//    described once, in the SDP, as an "AudioSpecificConfig" (2 bytes).
namespace mss::aac {

struct AdtsHeader {
    int headerLength = 0;     // 7, or 9 when a CRC is present
    int frameLength = 0;      // header + payload, in bytes
    int audioObjectType = 0;  // 2 = AAC-LC (the common "Low Complexity" profile)
    int samplingIndex = 0;    // index into the standard sample-rate table
    int sampleRate = 0;       // in Hz, e.g. 48000
    int channelConfig = 0;    // number of channels for normal layouts (1 = mono, 2 = stereo)
};

// Reads an ADTS header at the start of `data`. Returns false if it is not a valid header.
bool parseAdtsHeader(const std::uint8_t* data, std::size_t size, AdtsHeader& out);

// Builds the 2-byte AudioSpecificConfig used in SDP ("config=1190" for 48 kHz stereo AAC-LC).
std::vector<std::uint8_t> audioSpecificConfig(const AdtsHeader& header);

// Sample rate for a sampling index (0 = 96000 ... 12 = 7350). Returns 0 for invalid indexes.
int sampleRateForIndex(int index);

}  // namespace mss::aac
