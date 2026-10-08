// =============================================================================
//  Cloud Gaming Engine - Game SDK
// =============================================================================
//
//  This is the ONLY file a game needs. It is "header-only": everything is
//  defined right here, so a game compiles into a self-contained shared
//  library that the engine can load at runtime.
//
//  How a game plugs into the engine:
//
//    1. Write a class that derives from cge::Game and implements
//       input(), update() and render().
//    2. At the end of the file write:
//
//         CGE_EXPORT_GAME(MyGame, {
//             "My Game",              // name shown in the lobby
//             "What the game is about",
//             640, 480,               // canvas width and height in pixels
//             1, 4,                   // minimum and maximum number of players
//             30                      // ticks (update + render) per second
//         })
//
//    3. Put the .cpp file into its own folder inside games/, for example
//       games/mygame/mygame.cpp. The engine notices the new folder,
//       compiles it, tests it, and lists it in the lobby.
//
//  The engine calls your game from ONE thread only (the session's game
//  loop), so your game does not need any locks.
//
//  Every session gets its own Game object, so a game may keep all of its
//  state in member variables.
// =============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace cge {

// Bump when the interface below changes in an incompatible way. The engine
// refuses to load games built for another version.
constexpr int kApiVersion = 1;

// Colours are written as 0xRRGGBB, like in CSS: 0xFF0000 is red.
using Color = std::uint32_t;

// -----------------------------------------------------------------------------
//  Input
// -----------------------------------------------------------------------------

enum class InputKind : std::uint8_t {
    PointerDown = 1,  // mouse button pressed / finger touched
    PointerUp = 2,
    PointerMove = 3,
    KeyDown = 4,
    KeyUp = 5,
};

// One input action from one player, forwarded by the browser to the server.
struct InputEvent {
    InputKind kind = InputKind::PointerDown;
    int player = 0;   // which player slot sent it (0 = first player)
    int x = 0;        // pointer position in canvas pixels (pointer events only)
    int y = 0;
    int button = 0;   // 0 = main (left) button
    std::string key;  // key events: the browser's KeyboardEvent.key, e.g. "a", "5", "ArrowUp", "Enter", " "
};

// -----------------------------------------------------------------------------
//  Game description
// -----------------------------------------------------------------------------

struct GameInfo {
    const char* name;         // shown in the lobby
    const char* description;  // one or two sentences
    int width;                // canvas size in pixels (16 ... 1920)
    int height;               // (16 ... 1080)
    int minPlayers;           // players needed before the game can really start
    int maxPlayers;           // more people can still join, but as spectators
    int ticksPerSecond;       // how often update() + render() run (1 ... 120)
};

// -----------------------------------------------------------------------------
//  Canvas: an RGBA picture the game draws into every tick.
// -----------------------------------------------------------------------------
//
//  The engine "captures" this picture after render(), encodes the parts
//  that changed and streams them to every viewer.
//
//  Tip for good streaming: flat colours compress very well (the encoder uses
//  run-length encoding). Avoid noise and gradients covering the whole screen.
class Canvas {
public:
    Canvas(int width, int height)
        : width_(width), height_(height), pixels_(static_cast<std::size_t>(width) * height * 4, 255) {}

    int width() const { return width_; }
    int height() const { return height_; }

    // Raw pixel memory: 4 bytes per pixel (red, green, blue, alpha), row after row.
    const std::uint8_t* data() const { return pixels_.data(); }
    std::uint8_t* data() { return pixels_.data(); }

    void clear(Color color) { fillRect(0, 0, width_, height_, color); }

    void setPixel(int x, int y, Color color) {
        if (x < 0 || y < 0 || x >= width_ || y >= height_) return;
        std::uint8_t* p = &pixels_[(static_cast<std::size_t>(y) * width_ + x) * 4];
        p[0] = static_cast<std::uint8_t>(color >> 16);
        p[1] = static_cast<std::uint8_t>(color >> 8);
        p[2] = static_cast<std::uint8_t>(color);
        p[3] = 255;
    }

    // A filled rectangle. Parts outside the canvas are clipped.
    void fillRect(int x, int y, int w, int h, Color color) {
        const int x0 = std::max(0, x);
        const int y0 = std::max(0, y);
        const int x1 = std::min(width_, x + w);
        const int y1 = std::min(height_, y + h);
        if (x0 >= x1 || y0 >= y1) return;
        const std::uint8_t r = static_cast<std::uint8_t>(color >> 16);
        const std::uint8_t g = static_cast<std::uint8_t>(color >> 8);
        const std::uint8_t b = static_cast<std::uint8_t>(color);
        for (int row = y0; row < y1; ++row) {
            std::uint8_t* p = &pixels_[(static_cast<std::size_t>(row) * width_ + x0) * 4];
            for (int col = x0; col < x1; ++col) {
                p[0] = r;
                p[1] = g;
                p[2] = b;
                p[3] = 255;
                p += 4;
            }
        }
    }

    // Rectangle outline with the given line thickness (drawn inside the rectangle).
    void strokeRect(int x, int y, int w, int h, int thickness, Color color) {
        fillRect(x, y, w, thickness, color);
        fillRect(x, y + h - thickness, w, thickness, color);
        fillRect(x, y, thickness, h, color);
        fillRect(x + w - thickness, y, thickness, h, color);
    }

    // Rectangle with rounded corners.
    void fillRoundRect(int x, int y, int w, int h, int radius, Color color) {
        radius = std::max(0, std::min(radius, std::min(w, h) / 2));
        fillRect(x, y + radius, w, h - 2 * radius, color);
        for (int i = 0; i < radius; ++i) {
            // Width of the rounded part on this row, from the circle equation.
            const double dy = radius - i - 0.5;
            const int inset = radius - static_cast<int>(std::sqrt(static_cast<double>(radius) * radius - dy * dy) + 0.5);
            fillRect(x + inset, y + i, w - 2 * inset, 1, color);
            fillRect(x + inset, y + h - 1 - i, w - 2 * inset, 1, color);
        }
    }

    void fillCircle(int cx, int cy, int radius, Color color) {
        if (radius <= 0) return;
        for (int dy = -radius; dy <= radius; ++dy) {
            const int dx = static_cast<int>(std::sqrt(static_cast<double>(radius) * radius - dy * dy));
            fillRect(cx - dx, cy + dy, 2 * dx + 1, 1, color);
        }
    }

    // A ring: everything between radius - thickness and radius.
    void strokeCircle(int cx, int cy, int radius, int thickness, Color color) {
        if (radius <= 0) return;
        const int inner = std::max(0, radius - thickness);
        for (int dy = -radius; dy <= radius; ++dy) {
            const int outer = static_cast<int>(std::sqrt(static_cast<double>(radius) * radius - dy * dy));
            if (std::abs(dy) >= inner) {
                fillRect(cx - outer, cy + dy, 2 * outer + 1, 1, color);
            } else {
                const int hole = static_cast<int>(std::sqrt(static_cast<double>(inner) * inner - dy * dy));
                fillRect(cx - outer, cy + dy, outer - hole, 1, color);
                fillRect(cx + hole + 1, cy + dy, outer - hole, 1, color);
            }
        }
    }

    // A thick line with round ends. Every pixel whose distance to the line
    // segment is at most thickness/2 is painted.
    void drawLine(int x0, int y0, int x1, int y1, int thickness, Color color) {
        const double half = std::max(0.5, thickness / 2.0);
        const int minX = static_cast<int>(std::floor(std::min(x0, x1) - half));
        const int maxX = static_cast<int>(std::ceil(std::max(x0, x1) + half));
        const int minY = static_cast<int>(std::floor(std::min(y0, y1) - half));
        const int maxY = static_cast<int>(std::ceil(std::max(y0, y1) + half));
        const double dx = x1 - x0;
        const double dy = y1 - y0;
        const double lengthSquared = dx * dx + dy * dy;
        for (int y = std::max(0, minY); y <= std::min(height_ - 1, maxY); ++y) {
            for (int x = std::max(0, minX); x <= std::min(width_ - 1, maxX); ++x) {
                // Closest point of the segment to (x, y), as a fraction t of its length.
                double t = lengthSquared > 0 ? ((x - x0) * dx + (y - y0) * dy) / lengthSquared : 0.0;
                t = std::max(0.0, std::min(1.0, t));
                const double px = x0 + t * dx - x;
                const double py = y0 + t * dy - y;
                if (px * px + py * py <= half * half) setPixel(x, y, color);
            }
        }
    }

    // Text with the built-in 5x7 pixel font. `scale` makes every font pixel
    // scale x scale screen pixels. Returns the width of the drawn text.
    int drawText(int x, int y, const std::string& text, int scale, Color color) {
        int cursor = x;
        for (unsigned char c : text) {
            if (c < 32 || c > 126) c = '?';
            const std::uint8_t* glyph = fontGlyph(c);
            for (int column = 0; column < 5; ++column) {
                for (int row = 0; row < 8; ++row) {
                    if (glyph[column] & (1 << row)) fillRect(cursor + column * scale, y + row * scale, scale, scale, color);
                }
            }
            cursor += 6 * scale;
        }
        return cursor - x - (text.empty() ? 0 : scale);
    }

    static int textWidth(const std::string& text, int scale) {
        return text.empty() ? 0 : static_cast<int>(text.size()) * 6 * scale - scale;
    }
    static int textHeight(int scale) { return 7 * scale; }

    // Draws text so that its middle is at centerX.
    void drawTextCentered(int centerX, int y, const std::string& text, int scale, Color color) {
        drawText(centerX - textWidth(text, scale) / 2, y, text, scale, color);
    }

private:
    // Classic 5x8 LCD font for ASCII 32..126. Each glyph is 5 columns; in each
    // column byte, bit 0 is the top pixel and bit 7 the bottom (descenders).
    static const std::uint8_t* fontGlyph(unsigned char c) {
        static const std::uint8_t kFont[95][5] = {
            {0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x5F, 0x00, 0x00}, {0x00, 0x07, 0x00, 0x07, 0x00},  //   ! "
            {0x14, 0x7F, 0x14, 0x7F, 0x14}, {0x24, 0x2A, 0x7F, 0x2A, 0x12}, {0x23, 0x13, 0x08, 0x64, 0x62},  // # $ %
            {0x36, 0x49, 0x56, 0x20, 0x50}, {0x00, 0x08, 0x07, 0x03, 0x00}, {0x00, 0x1C, 0x22, 0x41, 0x00},  // & ' (
            {0x00, 0x41, 0x22, 0x1C, 0x00}, {0x2A, 0x1C, 0x7F, 0x1C, 0x2A}, {0x08, 0x08, 0x3E, 0x08, 0x08},  // ) * +
            {0x00, 0x80, 0x70, 0x30, 0x00}, {0x08, 0x08, 0x08, 0x08, 0x08}, {0x00, 0x00, 0x60, 0x60, 0x00},  // , - .
            {0x20, 0x10, 0x08, 0x04, 0x02}, {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00},  // / 0 1
            {0x72, 0x49, 0x49, 0x49, 0x46}, {0x21, 0x41, 0x49, 0x4D, 0x33}, {0x18, 0x14, 0x12, 0x7F, 0x10},  // 2 3 4
            {0x27, 0x45, 0x45, 0x45, 0x39}, {0x3C, 0x4A, 0x49, 0x49, 0x31}, {0x41, 0x21, 0x11, 0x09, 0x07},  // 5 6 7
            {0x36, 0x49, 0x49, 0x49, 0x36}, {0x46, 0x49, 0x49, 0x29, 0x1E}, {0x00, 0x00, 0x14, 0x00, 0x00},  // 8 9 :
            {0x00, 0x40, 0x34, 0x00, 0x00}, {0x00, 0x08, 0x14, 0x22, 0x41}, {0x14, 0x14, 0x14, 0x14, 0x14},  // ; < =
            {0x00, 0x41, 0x22, 0x14, 0x08}, {0x02, 0x01, 0x59, 0x09, 0x06}, {0x3E, 0x41, 0x5D, 0x59, 0x4E},  // > ? @
            {0x7C, 0x12, 0x11, 0x12, 0x7C}, {0x7F, 0x49, 0x49, 0x49, 0x36}, {0x3E, 0x41, 0x41, 0x41, 0x22},  // A B C
            {0x7F, 0x41, 0x41, 0x41, 0x3E}, {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x09, 0x01},  // D E F
            {0x3E, 0x41, 0x41, 0x51, 0x73}, {0x7F, 0x08, 0x08, 0x08, 0x7F}, {0x00, 0x41, 0x7F, 0x41, 0x00},  // G H I
            {0x20, 0x40, 0x41, 0x3F, 0x01}, {0x7F, 0x08, 0x14, 0x22, 0x41}, {0x7F, 0x40, 0x40, 0x40, 0x40},  // J K L
            {0x7F, 0x02, 0x1C, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F}, {0x3E, 0x41, 0x41, 0x41, 0x3E},  // M N O
            {0x7F, 0x09, 0x09, 0x09, 0x06}, {0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46},  // P Q R
            {0x26, 0x49, 0x49, 0x49, 0x32}, {0x03, 0x01, 0x7F, 0x01, 0x03}, {0x3F, 0x40, 0x40, 0x40, 0x3F},  // S T U
            {0x1F, 0x20, 0x40, 0x20, 0x1F}, {0x3F, 0x40, 0x38, 0x40, 0x3F}, {0x63, 0x14, 0x08, 0x14, 0x63},  // V W X
            {0x03, 0x04, 0x78, 0x04, 0x03}, {0x61, 0x59, 0x49, 0x4D, 0x43}, {0x00, 0x7F, 0x41, 0x41, 0x41},  // Y Z [
            {0x02, 0x04, 0x08, 0x10, 0x20}, {0x00, 0x41, 0x41, 0x41, 0x7F}, {0x04, 0x02, 0x01, 0x02, 0x04},  // \ ] ^
            {0x40, 0x40, 0x40, 0x40, 0x40}, {0x00, 0x03, 0x07, 0x08, 0x00}, {0x20, 0x54, 0x54, 0x78, 0x40},  // _ ` a
            {0x7F, 0x28, 0x44, 0x44, 0x38}, {0x38, 0x44, 0x44, 0x44, 0x28}, {0x38, 0x44, 0x44, 0x28, 0x7F},  // b c d
            {0x38, 0x54, 0x54, 0x54, 0x18}, {0x00, 0x08, 0x7E, 0x09, 0x02}, {0x18, 0xA4, 0xA4, 0x9C, 0x78},  // e f g
            {0x7F, 0x08, 0x04, 0x04, 0x78}, {0x00, 0x44, 0x7D, 0x40, 0x00}, {0x20, 0x40, 0x40, 0x3D, 0x00},  // h i j
            {0x7F, 0x10, 0x28, 0x44, 0x00}, {0x00, 0x41, 0x7F, 0x40, 0x00}, {0x7C, 0x04, 0x78, 0x04, 0x78},  // k l m
            {0x7C, 0x08, 0x04, 0x04, 0x78}, {0x38, 0x44, 0x44, 0x44, 0x38}, {0xFC, 0x18, 0x24, 0x24, 0x18},  // n o p
            {0x18, 0x24, 0x24, 0x18, 0xFC}, {0x7C, 0x08, 0x04, 0x04, 0x08}, {0x48, 0x54, 0x54, 0x54, 0x24},  // q r s
            {0x04, 0x04, 0x3F, 0x44, 0x24}, {0x3C, 0x40, 0x40, 0x20, 0x7C}, {0x1C, 0x20, 0x40, 0x20, 0x1C},  // t u v
            {0x3C, 0x40, 0x30, 0x40, 0x3C}, {0x44, 0x28, 0x10, 0x28, 0x44}, {0x4C, 0x90, 0x90, 0x90, 0x7C},  // w x y
            {0x44, 0x64, 0x54, 0x4C, 0x44}, {0x00, 0x08, 0x36, 0x41, 0x00}, {0x00, 0x00, 0x77, 0x00, 0x00},  // z { |
            {0x00, 0x41, 0x36, 0x08, 0x00}, {0x02, 0x01, 0x02, 0x04, 0x02},                                  // } ~
        };
        return kFont[c - 32];
    }

    int width_;
    int height_;
    std::vector<std::uint8_t> pixels_;
};

// -----------------------------------------------------------------------------
//  Services the engine offers to a game
// -----------------------------------------------------------------------------
class GameHost {
public:
    virtual ~GameHost() = default;
    // Display name of a player slot ("" if nobody has joined that slot).
    virtual std::string playerName(int player) const = 0;
    // True if the player is joined and currently connected.
    virtual bool playerConnected(int player) const = 0;
    // Writes a line to the server log (prefixed with the session id).
    virtual void log(const std::string& message) = 0;
};

// -----------------------------------------------------------------------------
//  The game interface
// -----------------------------------------------------------------------------
class Game {
public:
    virtual ~Game() = default;

    // Called once, before anything else. Keep the host pointer if you need it.
    virtual void start(GameHost& host) { (void)host; }

    // A player took a slot (also called again when a player reconnects).
    virtual void playerJoined(int player, const std::string& name) { (void)player; (void)name; }
    // A player disconnected. The slot stays reserved for a while so they can come back.
    virtual void playerLeft(int player) { (void)player; }

    // One input event from a player (spectators cannot send input).
    virtual void input(const InputEvent& event) = 0;

    // Advance the game by `seconds` (about 1 / ticksPerSecond).
    virtual void update(double seconds) = 0;

    // Draw the current state. Called right after update().
    virtual void render(Canvas& canvas) = 0;

    // One line describing the situation, shown above the game ("Bob's turn").
    virtual std::string status() const { return ""; }

    // Game-specific numbers for the metrics panel, as name/value pairs:
    //   return {{"Round", "3"}, {"Moves", "17"}};
    virtual std::vector<std::pair<std::string, std::string>> metrics() const { return {}; }
};

}  // namespace cge

// -----------------------------------------------------------------------------
//  Export macro: creates the four C functions the engine looks for when it
//  loads the shared library (with dlopen/dlsym). C functions are used because
//  their names are not "mangled" by the C++ compiler, so the engine can find them.
// -----------------------------------------------------------------------------
#define CGE_EXPORT_GAME(GameClass, ...)                                                                  \
    extern "C" __attribute__((visibility("default"))) int cge_api_version() { return cge::kApiVersion; } \
    extern "C" __attribute__((visibility("default"))) const cge::GameInfo* cge_game_info() {             \
        static const cge::GameInfo info = __VA_ARGS__;                                                     \
        return &info;                                                                                      \
    }                                                                                                      \
    extern "C" __attribute__((visibility("default"))) cge::Game* cge_create_game() { return new GameClass(); } \
    extern "C" __attribute__((visibility("default"))) void cge_destroy_game(cge::Game* game) { delete game; }
