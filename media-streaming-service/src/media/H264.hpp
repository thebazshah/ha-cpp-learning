#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Helpers for reading H.264 (also called AVC) video bitstreams.
//
// Quick background:
//  * An H.264 stream is a sequence of NAL units ("Network Abstraction Layer"
//    units). Each NAL unit starts with a 1-byte header whose lowest 5 bits
//    are its type: 1 = normal picture slice, 5 = IDR (key frame) slice,
//    6 = SEI (extra info), 7 = SPS, 8 = PPS, 9 = access unit delimiter.
//  * In "Annex B" format (used inside MPEG-TS files) every NAL unit is
//    preceded by a start code: 00 00 01 or 00 00 00 01.
//  * The SPS ("Sequence Parameter Set") describes the whole video: profile,
//    level, picture width and height, ... A decoder needs SPS and PPS before
//    it can decode anything.
namespace mss::h264 {

enum NalType : std::uint8_t {
    kNalSlice = 1,
    kNalIdr = 5,
    kNalSei = 6,
    kNalSps = 7,
    kNalPps = 8,
    kNalAud = 9,
};

// A view of one NAL unit inside a bigger buffer (no copy). `data` points at
// the NAL header byte; the start code is not included.
struct NalUnit {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    std::uint8_t type() const { return size > 0 ? (data[0] & 0x1F) : 0; }
};

// Finds all NAL units in an Annex B buffer.
std::vector<NalUnit> splitAnnexB(const std::uint8_t* data, std::size_t size);

// Inside NAL units the byte sequence 00 00 03 is used to avoid fake start
// codes ("emulation prevention"). This removes those extra 03 bytes so the
// real bits can be read.
std::vector<std::uint8_t> removeEmulationPrevention(const std::uint8_t* data, std::size_t size);

// The fields of an SPS that this server needs.
struct SpsInfo {
    int profileIdc = 0;       // 66 = Baseline, 77 = Main, 100 = High
    int constraintFlags = 0;  // the byte after profile_idc
    int levelIdc = 0;         // 30 = level 3.0, 31 = level 3.1, 40 = level 4.0
    int width = 0;            // visible picture size in pixels (after cropping)
    int height = 0;
};

// Parses an SPS NAL unit (including its 1-byte header). Returns false if broken.
bool parseSps(const std::uint8_t* nal, std::size_t size, SpsInfo& out);

// The codec string used in HLS playlists and browsers, e.g. "avc1.4d401f".
std::string codecString(const SpsInfo& sps);

// The "profile-level-id" used in RTSP/SDP, e.g. "4D401F".
std::string profileLevelId(const SpsInfo& sps);

// Reads a bitstream bit by bit, including Exp-Golomb codes (the variable-length
// numbers used everywhere in H.264 headers). Reading past the end sets failed().
class BitReader {
public:
    BitReader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    std::uint32_t readBits(int count);
    bool readFlag() { return readBits(1) != 0; }
    std::uint32_t readUnsignedExpGolomb();
    std::int32_t readSignedExpGolomb();
    bool failed() const { return failed_; }

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t bitPosition_ = 0;
    bool failed_ = false;
};

}  // namespace mss::h264
