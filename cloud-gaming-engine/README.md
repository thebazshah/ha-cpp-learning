# Cloud Gaming Engine

A small, hands-on **2D cloud gaming engine** written in **C++17**.

Games run **on the server**. Players only see a video stream of the game in their browser and send their mouse and keyboard input back. This is how services like GeForce Now or Xbox Cloud Gaming work, at toy scale and with every step easy to read:

```
   SERVER                                                            BROWSER (thin client)
 ┌──────────────────────────────────────────────────────┐          ┌──────────────────────────┐
 │ game.update() → game.render() → capture → encode ────┼── TCP ──►│ decode → paint <canvas>  │
 │       ▲            (C++ game plugin)   (tile codec)  │ WebSocket│                          │
 │       └──────────── input queue ◄────────────────────┼◄─────────┤ mouse / keyboard events  │
 └──────────────────────────────────────────────────────┘          └──────────────────────────┘
```

**What's inside:**

* **Game plugins in C++.** Copy a game folder into `games/`. The engine compiles it, test-plays it in a separate process, and offers it in the lobby. Broken games are shown with their compiler errors instead.
* **Frame capture.** Each game draws into an RGBA canvas through a tiny header-only SDK.
* **Video encoding** with a custom lossless tile codec (delta frames, keyframes, run-length encoding), about 200 readable lines.
* **TCP sockets** all the way: an HTTP/1.1 server and WebSocket (RFC 6455) written from scratch, with no third-party libraries.
* **Input forwarding** with sequence numbers, so the real input-to-screen latency can be measured.
* **Latency handling:**
  * TCP_NODELAY,
  * newest-frame-wins frame dropping with keyframe resynchronisation,
  * changed-tiles-only frames,
  * fixed-timestep game loop with overload protection,
  * automatic reconnects that keep your seat.
* **Multiplayer sessions.** Start a session, copy the link, and a friend opens it, enters a name and plays. Extra visitors become spectators.
* **REST API** plus a minimal **Alpine.js** web page that shows the game next to detailed engine, network and game metrics.
* **Tic-Tac-Toe** (`games/tictactoe/`) as the example game.
* **`cge-bot`**, a command-line client that plays over the same protocol (for learning and load tests).

---

## Contents

1. [Quick start](#1-quick-start)
2. [Playing](#2-playing)
3. [How it works](#3-how-it-works)
4. [The video codec](#4-the-video-codec-cgv1)
5. [The network protocol](#5-the-network-protocol)
6. [Latency handling](#6-latency-handling)
7. [How games are loaded](#7-how-games-are-loaded)
8. [Writing your own game](#8-writing-your-own-game)
9. [Sessions, players and spectators](#9-sessions-players-and-spectators)
10. [The metrics panel explained](#10-the-metrics-panel-explained)
11. [REST API reference](#11-rest-api-reference)
12. [Configuration options](#12-configuration-options)
13. [Threads](#13-threads--who-does-what)
14. [Project layout](#14-project-layout)
15. [Testing](#15-testing)
16. [Troubleshooting](#16-troubleshooting)
17. [Limits and ideas](#17-limits-and-ideas-for-extending-it)

---

## 1. Quick start

**You need:**

* a C++17 compiler (Clang ≥ 10 or GCC ≥ 9),
* CMake ≥ 3.16,
* macOS or Linux.

The same compiler is used **at runtime** to build the games.

```bash
# macOS:  xcode-select --install && brew install cmake
# Ubuntu: sudo apt install build-essential cmake

cd cloud-gaming-engine
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/cloud-gaming-engine
```

Open **http://localhost:8090/**. The first start compiles Tic-Tac-Toe, which takes about one second.

**Play with a friend:**

1. Click **Start new session** on Tic-Tac-Toe.
2. Enter your name and click **Join game**. You are player 1 (X).
3. Click **Copy invite link** and send the link to your friend.
   * Same computer: open it in another browser window or tab.
   * Other computer: replace `localhost` with your computer's IP address, for example `http://192.168.1.20:8090/play/abcd2345`.
4. Your friend enters a name and clicks **Join game**. They are player 2 (O).

Press `Ctrl+C` to stop the server.

---

## 2. Playing

**Lobby** (`/`):

* **Games** shows every game folder:
  * playable ones have a **Start new session** button;
  * games with problems show their compiler errors or test failure.
* **Running sessions** lists all sessions with their players. Click **Open** to join one.

**Game page** (`/play/<session-id>`):

* The **invite link** at the top is the session's URL. Anyone who opens it can join.
* **Join game** takes a free player seat. When all seats are taken you join as a **spectator**, who can watch but not play. **Just watch** always makes you a spectator.
* Click the game picture to give it the keyboard focus. Mouse and keyboard input then go to the server.
* **Tic-Tac-Toe controls:**
  * click a square, or press `1`–`9` (1 = top-left … 9 = bottom-right);
  * after a round ends, click anywhere or press `R` to play the next round. The starting player alternates between rounds.
* **Reconnecting:**
  * Reload the page or lose your network, and the page reconnects by itself and gives you **your old seat back**.
  * Seats are kept for 60 seconds after a disconnect (`--reconnect-grace`).
* **Metrics** on the right side: see [section 10](#10-the-metrics-panel-explained).

---

## 3. How it works

### The streaming pipeline (one game tick, 30 times per second)

| Step | Where | What happens |
|------|-------|--------------|
| 1. Input | browser → server | Mouse and key events are sent as small binary WebSocket messages and queued in the session. |
| 2. Update | `GameSession::loop` → game plugin | Queued inputs go to `game->input()`, then `game->update(dt)` advances time. |
| 3. Render = **frame capture** | game plugin | `game->render(canvas)` draws into a 32-bit RGBA canvas held by the engine. |
| 4. **Encode** | `codec/FrameEncoder` | The canvas is compared with the previous frame, 16×16 tile by tile. Changed tiles are compressed (SOLID / RLE / RAW). |
| 5. Package | `protocol/buildFrame` | A small header (frame number, time, size, input acks) is added. The frame is encoded **once** for all viewers. |
| 6. Queue | `Viewer::queueFrame` | Each viewer has a tiny send queue (2 frames). A slow viewer's frame is dropped and marked "needs keyframe". |
| 7. Send | viewer writer thread | The frame is sent as one binary WebSocket message over TCP. |
| 8. **Decode** | browser `applyTiles()` | The tiles are painted into an `ImageData` buffer. |
| 9. Display | `requestAnimationFrame` | The newest picture is put on the `<canvas>` at most once per screen refresh. |

### The pieces

```
src/
 ├─ net/Socket            RAII TCP/UDP sockets, sendAll()
 ├─ http/HttpServer       accept thread + poll() thread + worker pool; hands upgraded sockets to WebSocket code
 ├─ ws/WebSocket          RFC 6455 handshake (SHA-1 + base64) and framing, client and server side
 ├─ codec/FrameCodec      the tile codec (encoder + decoder)
 ├─ protocol/Protocol     binary message layouts (join, input, ping/pong, frame)
 ├─ engine/GameRegistry   watches games/, compiles, validates and dlopen()s games
 ├─ engine/GameLibrary    one loaded plugin (dlopen/dlsym) + the validation run
 ├─ engine/GameSession    the game loop, players, inputs, encoding, broadcasting, metrics
 ├─ engine/Viewer         one WebSocket client: reader thread + writer thread + bounded queue
 ├─ engine/SessionManager creates/finds sessions, removes idle ones
 └─ api/ApiController     REST API, WebSocket entry point, web page
sdk/include/cge/GameApi.hpp   the only header a game needs (Game, Canvas, InputEvent, font)
```

---

## 4. The video codec (CGV1)

Real video codecs such as H.264 are huge. 2D games mostly have **flat colours** and only **small parts change** from frame to frame, so a much simpler codec works very well. It uses the same two ideas real codecs use:

1. **Inter-frame (delta) coding.** The picture is cut into 16×16 tiles, and only tiles that changed since the previous frame are sent.
   * A **keyframe** contains all tiles. Viewers get one when they join or after they missed frames.
2. **Intra-frame compression.** Each tile is stored in the smallest of three forms:

| Mode | When | Size |
|------|------|------|
| SOLID | the whole tile has one colour | 4 bytes |
| RLE | runs of equal pixels, row by row | 4 bytes per run |
| RAW | noisy tiles where RLE would be bigger | 3 bytes per pixel (768 for a full tile) |

**Tile data layout** (little-endian):

```
u16 tileCount
repeat tileCount times:
  u16 tileIndex            row-major: tileY * tilesPerRow + tileX
  u8  mode                 0 SOLID, 1 RLE, 2 RAW
  SOLID: u8 r, g, b
  RLE:   u16 runCount, runCount x (u8 length 1..255, u8 r, g, b)
  RAW:   width*height x (u8 r, g, b)
```

**Real numbers for Tic-Tac-Toe (480×640, 1,200 tiles):**

| | Size |
|---|---|
| Raw RGB picture | 921,600 bytes |
| Keyframe | ≈ 24 KB (about 38× smaller) |
| Typical delta frame (the pulsing turn indicator) | ≈ 100 bytes |
| Frame after a move (the new mark grows in over a few frames) | ≈ 1–3 KB |
| Nothing changing (round over) | **no frames at all**, apart from a 1-per-second heartbeat |

The codec is **lossless**, so the browser shows exactly the pixels the game drew. The unit tests and the bot client check this.

---

## 5. The network protocol

Everything runs over **TCP**.

* **The browser cannot open raw TCP sockets, so we use WebSocket.** It starts as an ordinary HTTP request (`GET /ws/<session>` with `Upgrade: websocket`). After the `101 Switching Protocols` answer, the same TCP connection carries framed messages in both directions.
* **Every socket uses `TCP_NODELAY`.** This disables Nagle's algorithm, which would otherwise hold small packets (inputs!) back for up to ~40 ms.

**Client → server** (binary, little-endian):

| Message | Layout |
|---------|--------|
| JOIN | `u8 0x01, u8 nameLen, name, u8 tokenLen, token, u8 spectate` |
| INPUT | `u8 0x02, u8 kind (1 down, 2 up, 3 move, 4 keydown, 5 keyup), u32 sequence, i16 x, i16 y, u8 button, u8 keyLen, key` |
| PING | `u8 0x03, f64 clientTimeMs, f32 lastRoundTripMs` |
| KEYFRAME | `u8 0x04` (the client asks for a full picture, for example after a decode error) |

**Server → client:**

| Message | Layout |
|---------|--------|
| FRAME (binary) | `u8 0x10, u8 flags (bit0 keyframe), u32 frameNumber, f64 serverTimeMs, u16 width, u16 height, f32 encodeMs, u8 ackCount, ackCount x (u8 player, u32 lastInputSequence), tile data` |
| PONG (binary) | `u8 0x11, f64 clientTimeMs (echo), f64 serverTimeMs` |
| `welcome` (JSON text) | your role, seat, seat token and the game info, sent after JOIN |
| `state` (JSON text) | twice per second: players, game status, game metrics, engine metrics, viewers |
| `error` (JSON text) | for example "This session is full" or a game crash message |

The C++ definitions are in `src/protocol/Protocol.hpp`. The browser side is in `web/app.js` (`buildMessage`, `onBinaryMessage`).

---

## 6. Latency handling

| Technique | What it does | Where |
|-----------|--------------|-------|
| **TCP_NODELAY** | small packets (inputs, small frames) leave immediately | `GameSession::attachViewer`, `ws::connect` |
| **Ping/pong** | round-trip time measured every second, answered right away (not on the next tick) | `onViewerMessage` (kPing) |
| **Input acks** | every frame says which input sequence it already includes, so the client measures true *input → frame* latency | `FrameHeader::acks`, `onInputAcknowledged()` |
| **Fixed-timestep loop** | steady 30 Hz; if the server falls behind by more than 2 ticks it skips ahead instead of racing (counted as "late ticks") | `GameSession::loop` step 8 |
| **Latest frame wins** | at most 2 frames wait per viewer. Newer frames are dropped instead of building a backlog, then **one keyframe** brings the viewer back in sync | `Viewer::queueFrame`, `needsKeyframe` |
| **Encode once** | one encode per tick, whatever the number of viewers | step 4–5 of the loop |
| **Send only changes** | unchanged tiles are never sent; unchanged frames are not sent at all (1 s heartbeat) | `FrameEncoder`, `worthSending` |
| **Paint once per refresh** | many frames between two screen refreshes are decoded but painted once | `paintLoop()` |
| **Pointer-move throttling** | mouse moves are sent at most once per screen refresh | `pendingMove` |
| **Reconnect with seat token** | a dropped connection reconnects with 1, 2, 4, 8 … second pauses and reclaims the same player seat | `onDisconnected()`, `handleJoin()` |
| **Slow-client cut-off** | a write blocked for 5 s means the client is gone; it can never stall the game loop | `setTimeouts(0, 5000)` |

### Where the time goes (measured on one computer, 30 Hz)

| Stage | Typical time |
|-------|-------------|
| network round trip (localhost) | 0.2–1 ms |
| waiting for the next tick | 0–33 ms (17 ms average) |
| update + render + encode | ≈ 1 ms |
| decode in the browser | ≈ 0.05 ms |
| until the next screen refresh | 0–16 ms |
| **input → frame received** (bot client, measured) | **≈ 17–20 ms average** |

The largest part is waiting for the next tick. A game with `ticksPerSecond = 60` halves it.

---

## 7. How games are loaded

```
games/tictactoe/tictactoe.cpp
        │   (folder appears or changes; checked every 3 s)
        ▼
 c++ -std=c++17 -O2 -fPIC -shared -I sdk/include *.cpp -o cache/games/tictactoe-<hash>.so
        │   compiler errors?  → status "build_failed", errors shown in the lobby
        ▼
 cloud-gaming-engine --validate-game cache/games/tictactoe-<hash>.so      (separate process!)
        │   loads it, plays 300 ticks with fake players and random input
        │   crash / exception / endless loop / too slow?  → status "invalid"
        ▼
 dlopen() + dlsym("cge_create_game") in the server   → status "ready", playable
```

* **The fingerprint.** A hash of every source file (size and modification time), the SDK header and the compiler flags. An unchanged game is never compiled twice, even across restarts: the `.so` stays in `cache/games/`.
* **Hot reload.**
  * Editing a game's source rebuilds it automatically. New sessions use the new version; running sessions keep the old one.
  * If the new version fails, the last working version stays playable and the lobby shows the new error.
* **Removing a folder** removes the game (and its compiled files).
* **Prebuilt games.** A folder may contain `game.so` (or `game.dylib`) instead of sources. It is validated and loaded the same way.
* **Why a separate validator process?** A game that crashes (for example by dereferencing a null pointer) would take the whole server down if it were tested inside it. In a child process it only fails its own test.
* **Exceptions during play.** If a game throws a C++ exception, only that session ends: the players see an error message and the server keeps running.

---

## 8. Writing your own game

A game is one or more `.cpp` files in a new folder of `games/`. It includes **`<cge/GameApi.hpp>`** and nothing else from the engine.

### A complete example: "Click Race"

Save this as `games/clickrace/clickrace.cpp` while the server is running. Within a few seconds it shows up in the lobby.

```cpp
#include <cge/GameApi.hpp>
#include <string>

// Up to 4 players race to 20 clicks.
class ClickRace : public cge::Game {
public:
    void start(cge::GameHost& host) override { host_ = &host; }

    void input(const cge::InputEvent& event) override {
        if (event.kind == cge::InputKind::PointerDown && winner_ < 0) {
            if (++clicks_[event.player] == 20) winner_ = event.player;
        }
        if (event.kind == cge::InputKind::KeyDown && event.key == "r") {  // restart
            for (int& c : clicks_) c = 0;
            winner_ = -1;
        }
    }

    void update(double) override {}

    void render(cge::Canvas& canvas) override {
        const cge::Color colors[4] = {0xFF6B6B, 0x4ECDC4, 0xFFE66D, 0xA78BFA};
        canvas.clear(0x1B1F2E);
        canvas.drawTextCentered(200, 16, "CLICK RACE", 3, 0xFFFFFF);
        for (int p = 0; p < 4; ++p) {
            const std::string name = host_->playerName(p);
            if (name.empty()) continue;
            canvas.drawText(16, 64 + p * 40, name, 2, colors[p]);
            canvas.fillRoundRect(140, 60 + p * 40, clicks_[p] * 12, 20, 6, colors[p]);
        }
    }

    std::string status() const override {
        return winner_ >= 0 ? host_->playerName(winner_) + " wins! (press R)" : "Click as fast as you can!";
    }

    std::vector<std::pair<std::string, std::string>> metrics() const override {
        return {{"Leader clicks", std::to_string(std::max(std::max(clicks_[0], clicks_[1]), std::max(clicks_[2], clicks_[3])))}};
    }

private:
    cge::GameHost* host_ = nullptr;
    int clicks_[4] = {0, 0, 0, 0};
    int winner_ = -1;
};

CGE_EXPORT_GAME(ClickRace, {
    "Click Race",                         // name
    "First to 20 clicks wins.",           // description
    400, 240,                             // canvas width, height
    1, 4,                                 // min / max players
    30                                    // ticks per second
})
```

### SDK reference (`sdk/include/cge/GameApi.hpp`)

**`cge::Game`.** Override what you need:

| Method | Called |
|--------|--------|
| `start(GameHost&)` | once, before anything else |
| `playerJoined(player, name)` / `playerLeft(player)` | when a seat is taken or its player disconnects (and again on reconnect) |
| `input(const InputEvent&)` | for every input of a player (spectators cannot send input) |
| `update(double seconds)` | every tick; `seconds` is about `1 / ticksPerSecond` |
| `render(Canvas&)` | every tick, right after `update` |
| `status()` | one line shown above the game |
| `metrics()` | name/value pairs for the metrics panel |

**`cge::InputEvent`:**

* `kind`: `PointerDown`, `PointerUp`, `PointerMove`, `KeyDown`, `KeyUp`
* `player`: the seat number, starting at 0
* `x`, `y`: canvas pixels
* `button`: the mouse button
* `key`: the browser's `KeyboardEvent.key`, for example `"a"`, `"5"`, `"ArrowUp"`, `"Enter"` or `" "`

**`cge::Canvas`** (colours are `0xRRGGBB`):

* `clear`, `setPixel`
* `fillRect`, `strokeRect`, `fillRoundRect`
* `fillCircle`, `strokeCircle` (ring)
* `drawLine` (thick, with round ends)
* `drawText` / `drawTextCentered`, using the built-in 5×7 pixel font with a `scale` factor
* `textWidth`, `data()` (raw RGBA)

**`cge::GameHost`:**

* `playerName(player)`
* `playerConnected(player)`
* `log(message)`, which writes to the server log

**Rules:**

* The engine calls your game from **one thread only**, so you need no locks.
* Each session gets its own `Game` object.
* Keep each tick well below `1000 / ticksPerSecond` ms; the validator rejects games that are too slow.
* Prefer flat colours: they compress best.
* Never let exceptions or crashes escape. The validator tests with random input, so check array indexes and `event.player`.

---

## 9. Sessions, players and spectators

* **A session is one running game.** It has its own game object, game-loop thread, seats and viewers.
* **Seats.** A game declares `maxPlayers`, and the first `maxPlayers` people who click **Join game** take the seats (P1, P2, …). Everyone else watches as a spectator. A game that needs `minPlayers` should wait until enough seats are connected; Tic-Tac-Toe shows "Waiting for an opponent".
* **Seat tokens.** On joining, a player receives a secret token. The page keeps it in `sessionStorage` (per browser tab) and sends it when reconnecting, which gives the player the same seat back. A second tab of the same browser counts as a new person.
* **Disconnects.**
  * When a player disconnects, the game is told (`playerLeft`), and Tic-Tac-Toe shows "Waiting for Bob to reconnect".
  * The seat is reserved for 60 s (`--reconnect-grace`) and is then free for somebody else.
* **Cleanup.**
  * A session nobody has watched for 10 minutes is removed (`--idle-timeout`).
  * A session whose game crashed is removed after 30 s.
  * `DELETE /api/sessions/{id}` ends a session immediately.

---

## 10. The metrics panel explained

**Network (measured in this browser):**

| Metric | Meaning |
|--------|---------|
| Round-trip time | ping → pong time over the WebSocket (pure network delay) |
| Input → frame latency | from sending an input until a frame arrives that already includes it (network + waiting for the tick + processing). Last / average / worst |
| Frames received / displayed | frames per second arriving vs. actually painted (the browser paints once per screen refresh and slows down background tabs) |
| Bandwidth in, Average frame | data received per second, average frame size |
| Keyframes / deltas | complete pictures vs. change-only frames received |
| Decode time | milliseconds to apply one frame's tiles |
| Frame jitter | how much the time between frames varies (standard deviation) |
| Reconnects | how often the connection was re-established |
| Dropped for me | frames the server skipped because this browser was not keeping up |

**Engine (server):**

| Metric | Meaning |
|--------|---------|
| Tick rate | achieved vs. target ticks per second |
| Game update / Render (capture) / Encode | average time per tick for each stage |
| Tick budget used | share of the tick time actually spent working |
| Input wait (queue) | how long inputs waited for the next tick |
| Changed tiles, Last tiles (solid/RLE/raw) | how much of the picture changes, and how tiles were stored |
| Avg delta frame, Last keyframe | encoded sizes |
| Video out | bits per second queued to all viewers together |
| Frames encoded (… key, … idle ticks) | frames produced; idle ticks are ticks where nothing changed and nothing was sent |
| Late ticks | ticks that started more than 2 ticks late (server overloaded) |

**Game metrics** come from the game's `metrics()` method. For Tic-Tac-Toe these are round, moves, wins, draws, turn and last move.

**Connected viewers** lists, for every connection, its round trip, frames sent and dropped, and bytes.

---

## 11. REST API reference

| Method & path | Description |
|---------------|-------------|
| `GET /api/health` | `{"status":"ok","uptimeSeconds":…}` |
| `GET /api/games` | every game folder with its `status` (`ready`, `building`, `build_failed`, `invalid`), `error`, and for ready games `name`, `description`, `width`, `height`, `minPlayers`, `maxPlayers`, `ticksPerSecond`, `activeSessions` |
| `GET /api/games?status=ready` | only the games that are ready to be played |
| `POST /api/games/{gameId}/sessions` | starts a session → `201 {"session": {...}, "url": "/play/<id>", "webSocket": "/ws/<id>"}`; `404` unknown game, `409` game not playable, `503` too many sessions |
| `GET /api/sessions` | running sessions (game, players, viewers, status, uptime) |
| `GET /api/sessions/{id}` | one session including `engine` metrics, `gameMetrics` and `viewerList` |
| `DELETE /api/sessions/{id}` | ends the session |
| `GET /api/stats` | HTTP server, session and game-registry numbers |
| `GET /ws/{id}` | WebSocket upgrade, used to join and play (see section 5) |
| `GET /`, `GET /play/{id}`, `GET /static/{file}` | the web page |

```bash
curl -s localhost:8090/api/games?status=ready | python3 -m json.tool
curl -s -X POST localhost:8090/api/games/tictactoe/sessions
curl -s localhost:8090/api/sessions/<id> | python3 -m json.tool
```

---

## 12. Configuration options

Run `./build/cloud-gaming-engine --help` for the full list.

| Option | Default | Meaning |
|--------|---------|---------|
| `--bind` | `0.0.0.0` | listen address (`127.0.0.1` = this computer only) |
| `--port` | `8090` | HTTP + WebSocket port |
| `--games-dir` | `games` | game folders |
| `--build-dir` | `cache/games` | compiled games |
| `--sdk-dir` | `sdk/include` | folder containing `cge/GameApi.hpp` |
| `--web-dir` | `web` | frontend files |
| `--cxx` | `c++` | compiler used for games |
| `--scan-interval` | `3` | seconds between checks of `games/` |
| `--http-threads` | `16` | HTTP worker threads |
| `--max-sessions` | `100` | sessions at once |
| `--max-viewers` | `32` | players + spectators per session |
| `--idle-timeout` | `600` | remove sessions nobody watched for this many seconds |
| `--reconnect-grace` | `60` | seconds a disconnected player keeps their seat |
| `--log-level` | `info` | `debug`, `info`, `warn`, `error` |

---

## 13. Threads – who does what

| Thread | Count | Job |
|--------|-------|-----|
| main | 1 | starts everything, waits for `Ctrl+C` with `sigwait()` |
| HTTP accept + poll | 2 | accept connections, watch idle keep-alive connections |
| HTTP workers | 16 | handle REST requests and WebSocket handshakes |
| game registry | 1 | scans `games/`, runs the compiler and the validator |
| session janitor | 1 | removes idle and failed sessions |
| game loop | 1 per session | input → update → render → encode → queue, 30 times per second |
| viewer reader | 1 per connection | receives join, input, ping, keyframe requests |
| viewer writer | 1 per connection | sends frames, state messages and pongs |

The game itself only ever runs on its session's game-loop thread. Viewer threads hand data over through mutex-protected queues.

---

## 14. Project layout

```
cloud-gaming-engine/
├── CMakeLists.txt
├── README.md
├── games/
│   └── tictactoe/tictactoe.cpp     the example game (copy more game folders here)
├── sdk/include/cge/GameApi.hpp     the game SDK (header-only)
├── src/
│   ├── main.cpp                    wiring, Ctrl+C handling, --validate-game mode
│   ├── core/                       Config, Logger, Json, StringUtils, ThreadPool, Process (posix_spawn), Sha1
│   ├── net/Socket.*                sockets
│   ├── http/                       HTTP parser, Router, HttpServer (+ upgrade hand-over)
│   ├── ws/WebSocket.*              WebSocket handshake + framing (server and client)
│   ├── codec/FrameCodec.*          tile codec
│   ├── protocol/Protocol.*         binary messages
│   ├── engine/                     GameRegistry, GameLibrary, GameSession, Viewer, SessionManager
│   └── api/ApiController.*         REST API + static files
├── web/                            index.html, app.js (Alpine.js client + decoder), style.css
├── tools/bot_client.cpp            cge-bot command-line client
├── tests/unit_tests.cpp            unit + integration tests
├── scripts/smoke-test.sh           end-to-end check of a running server
└── cache/                          created at runtime: compiled games
```

---

## 15. Testing

```bash
./build/unit_tests          # or: ctest --test-dir build
```

The unit tests cover:

* SHA-1 test vectors and the RFC 6455 accept key;
* a codec round trip (keyframes, deltas, SOLID/RLE/RAW, broken data);
* protocol round trips;
* WebSocket framing over a socket pair (including 64-bit lengths and close);
* **the registry with real compiler runs**: a good game, one with a syntax error, one that crashes, one that throws, and an empty folder;
* **a complete Tic-Tac-Toe round** played through a real `GameSession` by two WebSocket clients, checking that both see identical pixels.

**End-to-end** (start the server first):

```bash
./scripts/smoke-test.sh
```

**Bot client** (play from the command line, measure latency, save the decoded frame):

```bash
./build/cge-bot --session <id> --name Robo --seconds 20
./build/cge-bot --session <id> --spectate --save frame.ppm   # open frame.ppm in an image viewer
```

---

## 16. Troubleshooting

| Problem | Fix |
|---------|-----|
| A game shows **compile error** | Read the compiler output on its card, fix the source, and save. It rebuilds within 3 s. |
| A game shows **rejected** | It crashed, threw, hung or was too slow in the test run. The message says which. Check array indexes and `event.player`, and make sure nothing loops forever. |
| `Cannot run the C++ compiler 'c++'` | Install a compiler, or pass `--cxx clang++` / `--cxx g++`. |
| `SDK header not found` | Start the server from the project folder, or pass `--root` / `--sdk-dir`. |
| A friend on another computer cannot connect | Use your IP address instead of `localhost`, and allow port 8090 in the firewall (macOS asks the first time). |
| Blank page | The page loads Alpine.js from `cdn.jsdelivr.net`, so the browser needs internet access. For offline use, download `cdn.min.js` into `web/` and change the `<script>` URL to `/static/cdn.min.js`. |
| "This session does not exist" | Sessions live in memory: a server restart or 10 idle minutes removes them. Start a new one. |
| Keyboard does nothing | Click the game picture first so it has the keyboard focus. |
| `Address already in use` | Another program uses port 8090. Use `--port 9000`. |

---

## 17. Limits and ideas for extending it

* **Trust.** Games are native code running inside the server process. They are validated first, but a game that crashes later (after validation) still takes the server down. Production engines run each game in its own sandboxed process; that would be a nice next step, since the protocol between session and game is already small.
* **Plain `ws://` only.** No TLS (`wss://`) and no authentication. Put a reverse proxy in front for public use.
* **The codec is lossless and simple.** Ideas:
  * a palette/LZ stage,
  * per-viewer quality levels,
  * motion search for scrolling games,
  * or H.264 via FFmpeg + WebCodecs for photographic content.
* **Faster input response.** Process input right when it arrives ("input-triggered ticks"), or predict locally on the client.
* **More ideas:**
  * audio streaming,
  * gamepad input (Gamepad API),
  * UDP/WebRTC transport,
  * recording and replay,
  * persistent sessions.
