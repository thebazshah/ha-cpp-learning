#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// A small, lossless video codec made for 2D games ("CGV1").
//
// Why not H.264? Real video codecs are huge. This one is about 200 lines,
// easy to follow, needs no libraries, and works very well for 2D games with
// flat colours. It still uses the two big ideas of real codecs:
//
//  1. Inter-frame (delta) coding: the picture is cut into 16x16 pixel
//     tiles. Only tiles that changed since the previous frame are sent.
//     A "keyframe" contains every tile and lets a viewer start from scratch
//     (for example right after joining, or after frames were dropped).
//
//  2. Intra-frame compression: each tile is stored in the smallest of three
//     forms:
//       SOLID - the whole tile has one colour      -> 4 bytes
//       RLE   - runs of equal pixels (row by row)  -> 4 bytes per run
//       RAW   - every pixel as R,G,B               -> 3 bytes per pixel
//
// Encoded tile data layout (all numbers little-endian):
//
//   u16 tileCount
//   repeated tileCount times:
//     u16 tileIndex        (tileY * tilesPerRow + tileX)
//     u8  mode             (0 = SOLID, 1 = RLE, 2 = RAW)
//     SOLID: u8 r, u8 g, u8 b
//     RLE:   u16 runCount, then runCount x (u8 length 1..255, u8 r, u8 g, u8 b)
//     RAW:   width*height x (u8 r, u8 g, u8 b)
//
// Tiles on the right and bottom edge can be smaller than 16x16 when the
// picture size is not a multiple of 16.
namespace cge::codec {

constexpr int kTileSize = 16;
enum TileMode : std::uint8_t { kSolid = 0, kRle = 1, kRaw = 2 };

struct EncodeStats {
    int totalTiles = 0;
    int changedTiles = 0;
    int solidTiles = 0;  // how the encoded tiles were stored (keyframe and delta together)
    int rleTiles = 0;
    int rawTiles = 0;
};

class FrameEncoder {
public:
    FrameEncoder(int width, int height);

    // Encodes one captured frame (RGBA, 4 bytes per pixel).
    //  * delta (optional): only the tiles that differ from the previous frame
    //  * key   (optional): all tiles
    // Afterwards this frame becomes the "previous frame" for the next call.
    void encode(const std::uint8_t* rgba, std::vector<std::uint8_t>* delta, std::vector<std::uint8_t>* key,
                EncodeStats& stats);

    int width() const { return width_; }
    int height() const { return height_; }
    int tileCount() const { return tilesX_ * tilesY_; }

private:
    bool tileChanged(const std::uint8_t* rgba, int tileX, int tileY) const;
    void encodeTile(const std::uint8_t* rgba, int tileX, int tileY, std::vector<std::uint8_t>& out,
                    EncodeStats& stats) const;

    int width_;
    int height_;
    int tilesX_;
    int tilesY_;
    std::vector<std::uint8_t> previous_;
    bool hasPrevious_ = false;
};

// Rebuilds the picture from encoded tile data (used by the native client
// and the tests; the browser has the same logic in JavaScript).
class FrameDecoder {
public:
    FrameDecoder(int width, int height);

    // Paints the tiles onto the current picture. Returns false if the data is broken.
    bool apply(const std::uint8_t* data, std::size_t size);

    const std::vector<std::uint8_t>& rgba() const { return pixels_; }
    int width() const { return width_; }
    int height() const { return height_; }

private:
    int width_;
    int height_;
    int tilesX_;
    int tilesY_;
    std::vector<std::uint8_t> pixels_;
};

}  // namespace cge::codec
