#include "codec/FrameCodec.hpp"

#include <algorithm>
#include <cstring>

namespace cge::codec {
namespace {

void put16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
}

void write16At(std::vector<std::uint8_t>& out, std::size_t position, std::uint16_t value) {
    out[position] = static_cast<std::uint8_t>(value & 0xFF);
    out[position + 1] = static_cast<std::uint8_t>(value >> 8);
}

}  // namespace

// =================================================================== encoder

FrameEncoder::FrameEncoder(int width, int height)
    : width_(width),
      height_(height),
      tilesX_((width + kTileSize - 1) / kTileSize),
      tilesY_((height + kTileSize - 1) / kTileSize),
      previous_(static_cast<std::size_t>(width) * height * 4, 0) {}

bool FrameEncoder::tileChanged(const std::uint8_t* rgba, int tileX, int tileY) const {
    if (!hasPrevious_) return true;
    const int x0 = tileX * kTileSize;
    const int y0 = tileY * kTileSize;
    const int tileWidth = std::min(kTileSize, width_ - x0);
    const int tileHeight = std::min(kTileSize, height_ - y0);
    const std::size_t rowBytes = static_cast<std::size_t>(tileWidth) * 4;
    for (int row = 0; row < tileHeight; ++row) {
        const std::size_t offset = (static_cast<std::size_t>(y0 + row) * width_ + x0) * 4;
        if (std::memcmp(rgba + offset, previous_.data() + offset, rowBytes) != 0) return true;
    }
    return false;
}

void FrameEncoder::encodeTile(const std::uint8_t* rgba, int tileX, int tileY, std::vector<std::uint8_t>& out,
                              EncodeStats& stats) const {
    const int x0 = tileX * kTileSize;
    const int y0 = tileY * kTileSize;
    const int tileWidth = std::min(kTileSize, width_ - x0);
    const int tileHeight = std::min(kTileSize, height_ - y0);

    put16(out, static_cast<std::uint16_t>(tileY * tilesX_ + tileX));
    const std::size_t modePosition = out.size();

    // SOLID: is every pixel of the tile the same colour?
    const std::uint8_t* first = rgba + (static_cast<std::size_t>(y0) * width_ + x0) * 4;
    bool uniform = true;
    for (int row = 0; row < tileHeight && uniform; ++row) {
        const std::uint8_t* p = rgba + (static_cast<std::size_t>(y0 + row) * width_ + x0) * 4;
        for (int column = 0; column < tileWidth; ++column, p += 4) {
            if (p[0] != first[0] || p[1] != first[1] || p[2] != first[2]) {
                uniform = false;
                break;
            }
        }
    }
    if (uniform) {
        out.push_back(kSolid);
        out.push_back(first[0]);
        out.push_back(first[1]);
        out.push_back(first[2]);
        ++stats.solidTiles;
        return;
    }

    // RLE: walk the tile row by row and merge equal neighbours into runs.
    out.push_back(kRle);
    const std::size_t runCountPosition = out.size();
    put16(out, 0);
    std::uint16_t runs = 0;
    std::uint8_t runR = 0, runG = 0, runB = 0;
    int runLength = 0;
    for (int row = 0; row < tileHeight; ++row) {
        const std::uint8_t* p = rgba + (static_cast<std::size_t>(y0 + row) * width_ + x0) * 4;
        for (int column = 0; column < tileWidth; ++column, p += 4) {
            if (runLength > 0 && runLength < 255 && p[0] == runR && p[1] == runG && p[2] == runB) {
                ++runLength;
                continue;
            }
            if (runLength > 0) {
                out.push_back(static_cast<std::uint8_t>(runLength));
                out.push_back(runR);
                out.push_back(runG);
                out.push_back(runB);
                ++runs;
            }
            runR = p[0];
            runG = p[1];
            runB = p[2];
            runLength = 1;
        }
    }
    out.push_back(static_cast<std::uint8_t>(runLength));
    out.push_back(runR);
    out.push_back(runG);
    out.push_back(runB);
    ++runs;
    write16At(out, runCountPosition, runs);

    const std::size_t rleBytes = 2 + static_cast<std::size_t>(runs) * 4;
    const std::size_t rawBytes = static_cast<std::size_t>(tileWidth) * tileHeight * 3;
    if (rleBytes <= rawBytes) {
        ++stats.rleTiles;
        return;
    }

    // RLE did not help (a busy, noisy tile): store the raw pixels.
    out.resize(modePosition);
    out.push_back(kRaw);
    for (int row = 0; row < tileHeight; ++row) {
        const std::uint8_t* p = rgba + (static_cast<std::size_t>(y0 + row) * width_ + x0) * 4;
        for (int column = 0; column < tileWidth; ++column, p += 4) {
            out.push_back(p[0]);
            out.push_back(p[1]);
            out.push_back(p[2]);
        }
    }
    ++stats.rawTiles;
}

void FrameEncoder::encode(const std::uint8_t* rgba, std::vector<std::uint8_t>* delta,
                          std::vector<std::uint8_t>* key, EncodeStats& stats) {
    stats = EncodeStats();
    stats.totalTiles = tilesX_ * tilesY_;
    if (delta) {
        delta->clear();
        put16(*delta, 0);  // tile count, filled in at the end
    }
    if (key) {
        key->clear();
        put16(*key, static_cast<std::uint16_t>(tilesX_ * tilesY_));
    }

    std::vector<std::uint8_t> tile;
    for (int tileY = 0; tileY < tilesY_; ++tileY) {
        for (int tileX = 0; tileX < tilesX_; ++tileX) {
            const bool changed = tileChanged(rgba, tileX, tileY);
            if (changed) ++stats.changedTiles;
            if (!key && !(delta && changed)) continue;  // nobody needs this tile

            tile.clear();
            encodeTile(rgba, tileX, tileY, tile, stats);
            if (key) key->insert(key->end(), tile.begin(), tile.end());
            if (delta && changed) delta->insert(delta->end(), tile.begin(), tile.end());
        }
    }
    if (delta) write16At(*delta, 0, static_cast<std::uint16_t>(stats.changedTiles));

    std::memcpy(previous_.data(), rgba, previous_.size());
    hasPrevious_ = true;
}

// =================================================================== decoder

FrameDecoder::FrameDecoder(int width, int height)
    : width_(width),
      height_(height),
      tilesX_((width + kTileSize - 1) / kTileSize),
      tilesY_((height + kTileSize - 1) / kTileSize),
      pixels_(static_cast<std::size_t>(width) * height * 4, 255) {}

bool FrameDecoder::apply(const std::uint8_t* data, std::size_t size) {
    std::size_t pos = 0;
    auto need = [&](std::size_t bytes) { return pos + bytes <= size; };
    auto read16 = [&]() {
        const std::uint16_t value = static_cast<std::uint16_t>(data[pos] | (data[pos + 1] << 8));
        pos += 2;
        return value;
    };

    if (!need(2)) return false;
    const int tileCount = read16();
    for (int t = 0; t < tileCount; ++t) {
        if (!need(3)) return false;
        const int index = read16();
        const std::uint8_t mode = data[pos++];
        if (index >= tilesX_ * tilesY_) return false;
        const int x0 = (index % tilesX_) * kTileSize;
        const int y0 = (index / tilesX_) * kTileSize;
        const int tileWidth = std::min(kTileSize, width_ - x0);
        const int tileHeight = std::min(kTileSize, height_ - y0);
        const int pixelCount = tileWidth * tileHeight;

        // Writes pixel number i of the tile (counting row by row).
        auto paint = [&](int i, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
            std::uint8_t* p = &pixels_[(static_cast<std::size_t>(y0 + i / tileWidth) * width_ + x0 + i % tileWidth) * 4];
            p[0] = r;
            p[1] = g;
            p[2] = b;
            p[3] = 255;
        };

        if (mode == kSolid) {
            if (!need(3)) return false;
            for (int i = 0; i < pixelCount; ++i) paint(i, data[pos], data[pos + 1], data[pos + 2]);
            pos += 3;
        } else if (mode == kRle) {
            if (!need(2)) return false;
            const int runs = read16();
            int i = 0;
            for (int r = 0; r < runs; ++r) {
                if (!need(4)) return false;
                const int length = data[pos];
                if (length == 0 || i + length > pixelCount) return false;
                for (int k = 0; k < length; ++k) paint(i++, data[pos + 1], data[pos + 2], data[pos + 3]);
                pos += 4;
            }
            if (i != pixelCount) return false;
        } else if (mode == kRaw) {
            if (!need(static_cast<std::size_t>(pixelCount) * 3)) return false;
            for (int i = 0; i < pixelCount; ++i, pos += 3) paint(i, data[pos], data[pos + 1], data[pos + 2]);
        } else {
            return false;
        }
    }
    return pos == size;
}

}  // namespace cge::codec
