#include "media/H264.hpp"

#include "core/StringUtils.hpp"

namespace mss::h264 {

std::vector<NalUnit> splitAnnexB(const std::uint8_t* data, std::size_t size) {
    std::vector<NalUnit> units;

    // Returns the position right after the next start code (00 00 01), or `size`.
    auto findNextStart = [&](std::size_t from) -> std::size_t {
        for (std::size_t i = from; i + 2 < size; ++i) {
            if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) return i + 3;
        }
        return size;
    };

    std::size_t start = findNextStart(0);
    while (start < size) {
        const std::size_t next = findNextStart(start);
        // The NAL unit ends where the next start code begins. The 4-byte form
        // (00 00 00 01) leaves an extra 00 behind, so trim trailing zeros
        // (a NAL unit never really ends with a zero byte).
        std::size_t end = (next == size) ? size : next - 3;
        while (end > start && data[end - 1] == 0) --end;
        if (end > start) units.push_back(NalUnit{data + start, end - start});
        start = next;
    }
    return units;
}

std::vector<std::uint8_t> removeEmulationPrevention(const std::uint8_t* data, std::size_t size) {
    std::vector<std::uint8_t> out;
    out.reserve(size);
    int zeros = 0;
    for (std::size_t i = 0; i < size; ++i) {
        // "00 00 03" -> drop the 03.
        if (zeros >= 2 && data[i] == 0x03) {
            zeros = 0;
            continue;
        }
        out.push_back(data[i]);
        zeros = (data[i] == 0) ? zeros + 1 : 0;
    }
    return out;
}

std::uint32_t BitReader::readBits(int count) {
    std::uint32_t value = 0;
    for (int i = 0; i < count; ++i) {
        if (bitPosition_ >= size_ * 8) {
            failed_ = true;
            return 0;
        }
        const std::uint8_t byte = data_[bitPosition_ / 8];
        const int bit = (byte >> (7 - (bitPosition_ % 8))) & 1;
        value = (value << 1) | static_cast<std::uint32_t>(bit);
        ++bitPosition_;
    }
    return value;
}

std::uint32_t BitReader::readUnsignedExpGolomb() {
    // Exp-Golomb: N zero bits, a one bit, then N more bits.
    // value = 2^N - 1 + (the N bits)
    int leadingZeros = 0;
    while (!failed_ && readBits(1) == 0) {
        if (++leadingZeros > 31) {
            failed_ = true;
            return 0;
        }
    }
    if (failed_) return 0;
    if (leadingZeros == 0) return 0;
    return ((1u << leadingZeros) - 1u) + readBits(leadingZeros);
}

std::int32_t BitReader::readSignedExpGolomb() {
    // Maps 0, 1, 2, 3, 4 ... to 0, 1, -1, 2, -2 ...
    const std::uint32_t code = readUnsignedExpGolomb();
    if (code % 2 == 1) return static_cast<std::int32_t>((code + 1) / 2);
    return -static_cast<std::int32_t>(code / 2);
}

namespace {

// Scaling lists only appear in High profiles. We do not need their values,
// but we must read past them to reach the picture size fields.
void skipScalingList(BitReader& reader, int listSize) {
    int lastScale = 8;
    int nextScale = 8;
    for (int j = 0; j < listSize; ++j) {
        if (nextScale != 0) {
            const int delta = reader.readSignedExpGolomb();
            nextScale = (lastScale + delta + 256) % 256;
        }
        lastScale = (nextScale == 0) ? lastScale : nextScale;
    }
}

}  // namespace

bool parseSps(const std::uint8_t* nal, std::size_t size, SpsInfo& out) {
    if (size < 4 || (nal[0] & 0x1F) != kNalSps) return false;

    const std::vector<std::uint8_t> rbsp = removeEmulationPrevention(nal + 1, size - 1);
    BitReader reader(rbsp.data(), rbsp.size());

    SpsInfo sps;
    sps.profileIdc = static_cast<int>(reader.readBits(8));
    sps.constraintFlags = static_cast<int>(reader.readBits(8));
    sps.levelIdc = static_cast<int>(reader.readBits(8));
    reader.readUnsignedExpGolomb();  // seq_parameter_set_id

    std::uint32_t chromaFormat = 1;  // 4:2:0 unless the SPS says otherwise
    bool separateColourPlanes = false;
    const int p = sps.profileIdc;
    if (p == 100 || p == 110 || p == 122 || p == 244 || p == 44 || p == 83 || p == 86 || p == 118 || p == 128 ||
        p == 138 || p == 139 || p == 134 || p == 135) {
        chromaFormat = reader.readUnsignedExpGolomb();
        if (chromaFormat == 3) separateColourPlanes = reader.readFlag();
        reader.readUnsignedExpGolomb();  // bit_depth_luma_minus8
        reader.readUnsignedExpGolomb();  // bit_depth_chroma_minus8
        reader.readFlag();               // qpprime_y_zero_transform_bypass_flag
        if (reader.readFlag()) {         // seq_scaling_matrix_present_flag
            const int lists = (chromaFormat == 3) ? 12 : 8;
            for (int i = 0; i < lists; ++i) {
                if (reader.readFlag()) skipScalingList(reader, i < 6 ? 16 : 64);
            }
        }
    }

    reader.readUnsignedExpGolomb();  // log2_max_frame_num_minus4
    const std::uint32_t pocType = reader.readUnsignedExpGolomb();
    if (pocType == 0) {
        reader.readUnsignedExpGolomb();  // log2_max_pic_order_cnt_lsb_minus4
    } else if (pocType == 1) {
        reader.readFlag();               // delta_pic_order_always_zero_flag
        reader.readSignedExpGolomb();    // offset_for_non_ref_pic
        reader.readSignedExpGolomb();    // offset_for_top_to_bottom_field
        const std::uint32_t cycle = reader.readUnsignedExpGolomb();
        if (cycle > 255) return false;
        for (std::uint32_t i = 0; i < cycle; ++i) reader.readSignedExpGolomb();
    }
    reader.readUnsignedExpGolomb();  // max_num_ref_frames
    reader.readFlag();               // gaps_in_frame_num_value_allowed_flag

    const std::uint32_t widthInMacroblocks = reader.readUnsignedExpGolomb() + 1;
    const std::uint32_t heightInMapUnits = reader.readUnsignedExpGolomb() + 1;
    const bool frameMbsOnly = reader.readFlag();
    if (!frameMbsOnly) reader.readFlag();  // mb_adaptive_frame_field_flag
    reader.readFlag();                     // direct_8x8_inference_flag

    std::uint32_t cropLeft = 0, cropRight = 0, cropTop = 0, cropBottom = 0;
    if (reader.readFlag()) {  // frame_cropping_flag
        cropLeft = reader.readUnsignedExpGolomb();
        cropRight = reader.readUnsignedExpGolomb();
        cropTop = reader.readUnsignedExpGolomb();
        cropBottom = reader.readUnsignedExpGolomb();
    }
    if (reader.failed()) return false;

    // Pictures are made of 16x16 macroblocks; cropping removes the padding.
    int width = static_cast<int>(widthInMacroblocks * 16);
    int height = static_cast<int>((2 - (frameMbsOnly ? 1 : 0)) * heightInMapUnits * 16);
    int cropUnitX = 1;
    int cropUnitY = 2 - (frameMbsOnly ? 1 : 0);
    if (chromaFormat != 0 && !separateColourPlanes) {
        const int subWidth = (chromaFormat == 3) ? 1 : 2;
        const int subHeight = (chromaFormat == 1) ? 2 : 1;
        cropUnitX = subWidth;
        cropUnitY = subHeight * (2 - (frameMbsOnly ? 1 : 0));
    }
    width -= static_cast<int>(cropLeft + cropRight) * cropUnitX;
    height -= static_cast<int>(cropTop + cropBottom) * cropUnitY;
    if (width <= 0 || height <= 0) return false;

    sps.width = width;
    sps.height = height;
    out = sps;
    return true;
}

std::string codecString(const SpsInfo& sps) {
    const std::uint8_t bytes[3] = {static_cast<std::uint8_t>(sps.profileIdc),
                                   static_cast<std::uint8_t>(sps.constraintFlags),
                                   static_cast<std::uint8_t>(sps.levelIdc)};
    return "avc1." + str::toHex(bytes, 3, false);
}

std::string profileLevelId(const SpsInfo& sps) {
    const std::uint8_t bytes[3] = {static_cast<std::uint8_t>(sps.profileIdc),
                                   static_cast<std::uint8_t>(sps.constraintFlags),
                                   static_cast<std::uint8_t>(sps.levelIdc)};
    return str::toHex(bytes, 3, true);
}

}  // namespace mss::h264
