#include "core/Sha1.hpp"

#include <vector>

namespace cge {
namespace {

std::uint32_t rotateLeft(std::uint32_t value, int bits) { return (value << bits) | (value >> (32 - bits)); }

}  // namespace

std::array<std::uint8_t, 20> sha1(const std::string& data) {
    std::uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};

    // Padding: append 0x80, then zeros until the length is 56 mod 64, then
    // the original length in bits as a 64-bit big-endian number.
    std::vector<std::uint8_t> message(data.begin(), data.end());
    const std::uint64_t bitLength = static_cast<std::uint64_t>(data.size()) * 8;
    message.push_back(0x80);
    while (message.size() % 64 != 56) message.push_back(0);
    for (int i = 7; i >= 0; --i) message.push_back(static_cast<std::uint8_t>(bitLength >> (i * 8)));

    // Process every 64-byte block.
    for (std::size_t block = 0; block < message.size(); block += 64) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(message[block + i * 4]) << 24) |
                   (static_cast<std::uint32_t>(message[block + i * 4 + 1]) << 16) |
                   (static_cast<std::uint32_t>(message[block + i * 4 + 2]) << 8) |
                   static_cast<std::uint32_t>(message[block + i * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i) w[i] = rotateLeft(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f;
            std::uint32_t k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            const std::uint32_t temp = rotateLeft(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotateLeft(b, 30);
            b = a;
            a = temp;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }

    std::array<std::uint8_t, 20> digest{};
    for (int i = 0; i < 5; ++i) {
        digest[i * 4] = static_cast<std::uint8_t>(h[i] >> 24);
        digest[i * 4 + 1] = static_cast<std::uint8_t>(h[i] >> 16);
        digest[i * 4 + 2] = static_cast<std::uint8_t>(h[i] >> 8);
        digest[i * 4 + 3] = static_cast<std::uint8_t>(h[i]);
    }
    return digest;
}

}  // namespace cge
