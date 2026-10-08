#include "media/Aac.hpp"

namespace mss::aac {

int sampleRateForIndex(int index) {
    static const int kRates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                 22050, 16000, 12000, 11025, 8000,  7350};
    if (index < 0 || index >= static_cast<int>(sizeof(kRates) / sizeof(kRates[0]))) return 0;
    return kRates[index];
}

bool parseAdtsHeader(const std::uint8_t* data, std::size_t size, AdtsHeader& out) {
    if (size < 7) return false;
    // Every ADTS header starts with the 12-bit sync word 0xFFF.
    if (data[0] != 0xFF || (data[1] & 0xF0) != 0xF0) return false;

    // Bit layout of the first 7 bytes (ISO/IEC 13818-7):
    //   syncword(12) id(1) layer(2) protection_absent(1)
    //   profile(2) sampling_index(4) private(1) channel_config(3)
    //   original(1) home(1) copyright_bits(2) frame_length(13) fullness(11) raw_blocks(2)
    const bool protectionAbsent = (data[1] & 0x01) != 0;
    AdtsHeader header;
    header.headerLength = protectionAbsent ? 7 : 9;
    header.audioObjectType = ((data[2] >> 6) & 0x03) + 1;
    header.samplingIndex = (data[2] >> 2) & 0x0F;
    header.channelConfig = ((data[2] & 0x01) << 2) | ((data[3] >> 6) & 0x03);
    header.frameLength = ((data[3] & 0x03) << 11) | (data[4] << 3) | ((data[5] >> 5) & 0x07);
    header.sampleRate = sampleRateForIndex(header.samplingIndex);

    if (header.sampleRate == 0) return false;
    if (header.frameLength < header.headerLength) return false;
    out = header;
    return true;
}

std::vector<std::uint8_t> audioSpecificConfig(const AdtsHeader& header) {
    // AudioSpecificConfig: object_type(5) sampling_index(4) channel_config(4) + 3 zero bits.
    const std::uint8_t first = static_cast<std::uint8_t>((header.audioObjectType << 3) | (header.samplingIndex >> 1));
    const std::uint8_t second = static_cast<std::uint8_t>(((header.samplingIndex & 0x01) << 7) |
                                                          ((header.channelConfig & 0x0F) << 3));
    return {first, second};
}

}  // namespace mss::aac
