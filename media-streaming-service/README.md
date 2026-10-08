# Media Streaming Service

A high-performance media streaming server written in **C++17**. It streams the videos and audio files you put into two folders:

* **HLS** (HTTP Live Streaming) with **adaptive bitrate**. It plays in every browser and is used by the built-in web page.
* **RTSP / RTP / RTCP**, for players like VLC and ffplay. The server itself picks the quality (**server-side adaptive bitrate**).
* **Direct file playback** with HTTP Range requests (progressive download).

All networking is written from scratch on top of POSIX sockets:

* a multithreaded HTTP/1.1 server,
* an RTSP server,
* RTP packetizers for H.264 and AAC,
* RTCP sender/receiver reports.

**FFmpeg** encodes the H.264/AAC quality ladder. Our own C++ code reads the results at the bitstream level, using its own MPEG-TS demuxer, H.264 SPS parser and AAC ADTS parser.

A small **REST API** lists your media and serves a minimal **Alpine.js** web page for testing.

---

## Contents

1. [Quick start](#1-quick-start)
2. [Using the web page](#2-using-the-web-page)
3. [Watching with VLC / ffplay over RTSP](#3-watching-with-vlc--ffplay-over-rtsp)
4. [How it works – the big picture](#4-how-it-works--the-big-picture)
5. [Adaptive bitrate streaming (ABR)](#5-adaptive-bitrate-streaming-abr)
6. [Codec integration: H.264 and AAC](#6-codec-integration-h264-and-aac)
7. [Buffering, caching and low latency](#7-buffering-caching-and-low-latency)
8. [Handling network interruptions](#8-handling-network-interruptions)
9. [Threads – who does what](#9-threads--who-does-what)
10. [REST API reference](#10-rest-api-reference)
11. [Configuration options](#11-configuration-options)
12. [Project layout](#12-project-layout)
13. [Testing](#13-testing)
14. [Troubleshooting](#14-troubleshooting)
15. [Limits and ideas for extending it](#15-limits-and-ideas-for-extending-it)

---

## 1. Quick start

### Requirements

| Tool | Why | Install |
|------|-----|---------|
| A C++17 compiler (Clang ≥ 10 or GCC ≥ 9) | builds the server | macOS: `xcode-select --install`; Ubuntu: `sudo apt install build-essential` |
| CMake ≥ 3.16 | build system | macOS: `brew install cmake`; Ubuntu: `sudo apt install cmake` |
| FFmpeg (with libx264) | H.264/AAC encoding and reading media details | macOS: `brew install ffmpeg`; Ubuntu: `sudo apt install ffmpeg` |

The server runs on **macOS and Linux**. It uses POSIX sockets, `poll()` and `posix_spawn()`. There are no other libraries to install: the server has no third-party C++ dependencies.

### Build

```bash
cd media-streaming-service
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

This produces two programs:

* `build/media-streaming-service` – the server
* `build/unit_tests` – the unit tests

### Add media

Copy your files into the two folders:

```
media-streaming-service/
├── videos/   ← .mp4 .mkv .mov .webm .avi .ts .m4v .flv .wmv .mpg ...
└── audios/   ← .mp3 .m4a .aac .wav .flac .ogg .opus ...
```

Don't have any files handy? `./scripts/generate-test-media.sh` creates a few test files with FFmpeg.

### Run

```bash
./build/media-streaming-service
```

Then open **http://localhost:8080/** in your browser. Press `Ctrl+C` to stop the server cleanly.

The server finds the project folder automatically: it looks for the folder that contains `web/index.html`. You can start it from the project folder or from inside `build/`.

New files appear without a restart. Press the ⟳ button on the page to rescan the folders.

---

## 2. Using the web page

The page has three columns:

**Library (left).** Tabs for *Videos* and *Audios*. Each file shows its duration, size and resolution, plus a badge:

| Badge | Meaning |
|-------|---------|
| `not prepared` | the adaptive stream has not been created yet |
| `encoding 42%` | FFmpeg is creating the quality ladder (you can already play!) |
| `ready` | all qualities are encoded and stored in the `cache/` folder |
| `unreadable` | ffprobe could not read the file (not a media file, or broken) |

**Player (middle).**

* **Mode:**
  * *HLS adaptive*: the H.264/AAC ladder, with automatic quality switching.
  * *Original file*: plays your file as it is, using HTTP Range requests. This only works for formats the browser understands, such as MP4/H.264 and MP3.
* **Quality:** *Auto* (adaptive) or a fixed level, for example `720p · 3837 kbps`.
* **Status line:** shows what is happening: preparing, buffering, playing, connection problems, recovery.
* **RTSP box:** the `rtsp://` URL of the same file, with a copy button and ready-made commands.
* **Player events:** quality switches, network errors and reconnects.

**Statistics (right).** Updated every second:

* **Playback:** position, buffer ahead, current quality and its bitrate, bandwidth estimate, the last segment's size and download time, startup time, quality switches, rebuffering events, recovered errors, reconnects and dropped frames.
* **Server:** HTTP requests and connections, busy worker threads, data sent, segment cache usage and hit ratio, running transcodes, encoder, RTSP sessions, plus a card for every active RTSP session showing its quality, bitrate and ABR decision.

> **First playback of a file:** the server starts FFmpeg and playback begins as soon as the *first 2-second segment* of every quality exists. On a modern computer that takes about 0.5–1.5 s. You don't have to wait for the whole file to be encoded.

---

## 3. Watching with VLC / ffplay over RTSP

Browsers cannot play RTSP, so use a media player. RTSP URLs look like this:

```
rtsp://<server>:8554/videos/<file name>
rtsp://<server>:8554/audios/<file name>
```

Spaces and special characters in the file name must be URL-encoded (`%20` for a space). The copy button on the web page does this for you.

```bash
# ffplay, with RTP inside the RTSP TCP connection (works through most firewalls)
ffplay -rtsp_transport tcp "rtsp://localhost:8554/videos/my%20movie.mp4"

# ffplay, with RTP over UDP
ffplay -rtsp_transport udp "rtsp://localhost:8554/videos/my%20movie.mp4"

# start 60 seconds in (sends "Range: npt=60-")
ffplay -ss 60 -rtsp_transport tcp "rtsp://localhost:8554/videos/my%20movie.mp4"

# force one quality level (0 = lowest) instead of adaptive
ffplay -rtsp_transport tcp "rtsp://localhost:8554/videos/my%20movie.mp4?rendition=0"
```

In **VLC**, use *Media → Open Network Stream* and paste the URL.

Supported RTSP methods: `OPTIONS`, `DESCRIBE`, `SETUP` (UDP or TCP-interleaved), `PLAY` (with `Range: npt=`), `PAUSE`, `TEARDOWN`, and `GET_PARAMETER`/`SET_PARAMETER` (keep-alive).

---

## 4. How it works – the big picture

```
                    ┌────────────────────────── media-streaming-service ─────────────────────────────┐
                    │                                                                                │
 videos/ audios/ ──►│ MediaLibrary ──► TranscodeManager ──► ffmpeg ──► cache/hls/<file>/r0..rN/*.ts │
 (your files)       │  (scans, ffprobe)  (jobs, queue)      (H.264 + AAC ladder, HLS segments)        │
                    │                         │                                   │                  │
                    │                         ▼                                   ▼                  │
                    │                  SegmentCache (LRU in RAM, shared by everyone below)           │
                    │                    │                                  │                        │
                    │      ┌─────────────┴─────────────┐        ┌───────────┴───────────────┐        │
                    │      │ HlsService + ApiController│        │ RtspServer / RtspSession  │        │
                    │      │ (playlists, segments,     │        │ TsDemuxer → RtpPacketizer │        │
                    │      │  REST API, web page)      │        │ RTCP, AbrController       │        │
                    │      └─────────────┬─────────────┘        └───────────┬───────────────┘        │
                    │                    │ HttpServer (port 8080)           │ RTSP (8554) + RTP/RTCP │
                    └────────────────────┼──────────────────────────────────┼────────────────────────┘
                                         ▼                                  ▼
                              Browser (Alpine.js + hls.js)           VLC, ffplay, ...
```

### What happens when you press play in the browser

1. The page calls `POST /api/media/videos/movie.mp4/prepare`.
2. `TranscodeManager` checks the `cache/` folder.
   * If this file was already encoded (with the same size, modification date and settings), the old result is reused immediately.
   * Otherwise it starts **one** FFmpeg process. That process decodes the file once and encodes it into up to 4 qualities in parallel. Each quality is written as 2-second MPEG-TS segments plus a growing playlist.
3. As soon as every quality has its first segment, the job is **ready**. The page loads `/hls/videos/movie.mp4/master.m3u8`.
4. The server builds the master playlist on the fly. It reads segment 0 of every quality with its own MPEG-TS demuxer and H.264 SPS parser to get the exact resolution and codec string (for example `avc1.4d401f`).
5. hls.js downloads the segments. Each segment goes through the **SegmentCache**: the first request reads the disk, and later requests come from RAM. The next segment is prefetched in the background.
6. hls.js measures the download speed and switches quality whenever needed.

### What happens when VLC opens the RTSP URL

1. **DESCRIBE**: the server makes sure the stream is prepared, as above, and answers with an **SDP** description. The SDP contains the H.264 SPS/PPS (base64) and the AAC configuration, both extracted by our parsers.
2. **SETUP** (once per track): the client chooses UDP (with its two ports) or TCP-interleaved. The server opens a UDP port pair or reuses the TCP connection.
3. **PLAY**: a new **sender thread** starts for the session. It reads HLS segments through the cache and demuxes them into frames. It sends each frame at exactly the right moment (real-time pacing), cut into RTP packets. It also sends RTCP sender reports every second.
4. At every segment boundary, the **ABR controller** may switch quality (see below).
5. At the end of the media the server sends an RTCP **BYE**, so the player knows the stream is complete.

---

## 5. Adaptive bitrate streaming (ABR)

### The bitrate ladder

Each video is encoded into several qualities ("renditions"). We never upscale. If the source has a short side of 720 pixels, the ladder stops at 720p.

| Level | Short side | Video | Audio | Used for |
|-------|-----------|-------|-------|----------|
| 240p  | 240 px  | 400 kbps  | 64 kbps  | very slow networks |
| 360p  | 360 px  | 800 kbps  | 96 kbps  | |
| 480p  | 480 px  | 1400 kbps | 128 kbps | |
| 720p  | 720 px  | 2800 kbps | 128 kbps | |
| 1080p | 1080 px | 5000 kbps | 160 kbps | fast networks |

By default the best 4 levels that fit the source are used (`--max-renditions`). Audio-only files get three AAC levels: 64, 128 and 192 kbps.

Portrait videos (phones) work too. The "p" number is always the **short** side, so a 1080×1920 video is 1080p.

**Key frames are aligned:** every quality has a key frame at exactly the same moments (every 2 s), so segment *N* covers the same time in every quality. That is what makes seamless switching possible.

### Client-side ABR (HLS)

The browser plays the master playlist with hls.js. hls.js estimates the bandwidth from how fast segments download and picks the highest level the network can sustain. Safari does the same natively. You can lock a level in the *Quality* menu.

### Server-side ABR (RTSP)

With RTSP the **server** pushes the media, so the server has to decide. `AbrController` watches two signals:

1. **Lateness.** Every frame has a scheduled send time. If a TCP client cannot keep up, `send()` blocks and the sender falls behind schedule. Growing lateness means "the network is slower than this bitrate".
2. **Packet loss.** RTCP receiver reports from the player tell us what fraction of the UDP packets was lost.

At each segment boundary it decides:

| Situation | Decision |
|-----------|----------|
| loss > 8 %, or lateness grew by > 0.25 s within one segment, or lateness > 2 s, or a stall | go **one level down** right away |
| loss < 2 % and lateness < 0.15 s, for at least the "hold time" | try **one level up** |
| otherwise | stay |

After every downgrade the hold time doubles (6 s → 12 s → 24 s …, up to 60 s). This *exponential back-off* stops the stream from bouncing up and down between two levels. After a long healthy period the hold time goes back to 6 s.

You can watch this happen with the slow test client:

```bash
python3 scripts/slow_rtsp_client.py "rtsp://127.0.0.1:8554/videos/movie.mp4" --kbps 600 --seconds 45
# server log:  Session ...: switching quality 480p -> 360p
#              Session ...: switching quality 360p -> 240p
```

---

## 6. Codec integration: H.264 and AAC

**Encoding** is done by FFmpeg (libx264 + the native AAC encoder), started by `TranscodeManager` with `posix_spawn`. The exact command line is built in `TranscodeManager::buildCommand()`. The important settings are:

| Setting | Why |
|---------|-----|
| `split` + `scale` filter graph | decode once, scale into every quality (cheaper than one FFmpeg per quality) |
| `-profile:v main -bf 0` | Main profile, no B-frames → lower decoding delay and simple RTP timing (PTS = DTS) |
| `-force_key_frames expr:gte(t,n_forced*2)`, `-sc_threshold 0` | key frames exactly every 2 s in every quality → aligned segments |
| `-maxrate` = 1.2 × bitrate, `-bufsize` = 2 × bitrate | caps short bitrate peaks so the HLS `BANDWIDTH` value stays honest |
| `-c:a aac -ac 2 -ar 48000` | the same audio format in every quality, so switching never changes the audio setup |
| `-hls_playlist_type event`, `-hls_flags temp_file` | the playlist grows while encoding, and segments appear only when complete |

If libx264 is missing, the server automatically falls back to `h264_videotoolbox` (macOS hardware encoder) or `libopenh264`.

**Reading the bitstream** is done by our own code, without FFmpeg libraries:

| File | What it understands |
|------|---------------------|
| `media/TsDemuxer.cpp` | MPEG-TS packets (188 bytes), PAT/PMT tables and PES packets with 33-bit PTS/DTS timestamps. It splits several AAC frames out of one PES packet. |
| `media/H264.cpp` | Annex-B start codes, NAL unit types, emulation-prevention bytes, an Exp-Golomb bit reader, and a full **SPS parser** (profile, level, width and height with cropping, High-profile scaling lists). Builds the `avc1.PPCCLL` codec string. |
| `media/Aac.cpp` | ADTS headers (profile, sample rate, channels, frame length) and the 2-byte **AudioSpecificConfig** used in SDP (`config=1190`). |
| `rtsp/RtpPacketizer.cpp` | **RFC 6184** H.264 over RTP: single NAL unit packets plus **FU-A fragmentation** for big NAL units, with the marker bit on the last packet of a picture. **RFC 3640** AAC over RTP (AAC-hbr mode with AU headers). |
| `rtsp/Rtcp.cpp` | RTCP **Sender Reports** (NTP ↔ RTP time mapping, used for lip-sync), SDES/CNAME, **BYE**, and parsing of **Receiver Reports** (packet loss, jitter). |
| `rtsp/Sdp.cpp` | SDP with `sprop-parameter-sets`, `profile-level-id` and the MPEG4-GENERIC `fmtp` line. |

After a quality switch, `RtspSession` makes sure every key frame carries SPS/PPS, so the decoder can follow the change of resolution.

---

## 7. Buffering, caching and low latency

| Layer | What it does | Where |
|-------|--------------|-------|
| **Disk cache of transcoded output** | Every file is encoded only once. The results are kept in `cache/hls/` and reused, even after a restart. A changed file (new size or date) or changed settings produce a new folder. | `TranscodeManager` |
| **Play while encoding** | A stream is "ready" as soon as the first segment of every quality exists, so you don't wait for the whole encode. | `TranscodeJob::markReady` |
| **In-memory segment cache** | An LRU cache with a memory budget (256 MB by default, `--cache-mb`). Popular segments are served from RAM. | `cache/SegmentCache.cpp` |
| **Request coalescing** | If many clients request the same segment at the same moment, the disk is read once and everyone shares the result. | `SegmentCache::get` |
| **Zero-copy sharing** | Cached segments are `shared_ptr<const string>`. HTTP responses and RTSP sessions use the same bytes without copying. | `HttpResponse::sharedBody` |
| **Prefetching** | When segment N is requested, N+1 is loaded into the cache in the background. | `HlsService::renditionFile`, `RtspSession::loadSegment` |
| **Short segments** | 2-second segments mean a quicker start and quicker quality switches (`--segment-seconds`). | settings |
| **RTP send-ahead window** | RTSP sessions send media up to 0.5 s ahead of real time. The player gets a small cushion against jitter, which helps startup. | `kSendAhead` in `RtspSession.cpp` |
| **Real-time pacing** | RTP packets leave at media speed, not in bursts, so player buffers and network queues stay small. | `RtspSession::senderLoop` |
| **Player buffer** | hls.js keeps up to 30 s downloaded ahead, which bridges network hiccups. | `web/app.js` |
| **Capped range responses** | "Give me everything from byte N" is answered with at most 4 MB. Players ask for the next part, and a slow client never ties up a worker thread for a whole download. | `HlsService::directFile` |

---

## 8. Handling network interruptions

**In the browser (`web/app.js`):**

* **Retries with growing delays:** each failed playlist or segment download is retried up to 6 times, waiting 1, 2, 4, 8, 8, 8 s.
* **Reconnecting:** if hls.js finally gives up, the page reconnects by itself with exponential back-off (1, 2, 4, 8, 15 s …). It resumes at the same position and shows *"Connection restored"* when segments arrive again.
* **Playing from the buffer:** playback continues from the buffer during the outage. A short server or Wi-Fi drop is often not visible at all.
* **Decoder errors** are recovered with `hls.recoverMediaError()`.
* The browser's `offline` and `online` events are shown, and coming back online triggers an immediate retry.
* *Tested:* stop the server while playing, seek forward, start the server again. Playback resumes automatically.

**In the server:**

* **Stalls:** if an RTSP session falls more than 3 s behind (the network stalled), it does not burst the backlog. It restarts its pacing clock, reports a stall to the ABR controller (→ lower quality), and continues.
* **Dead clients:**
  * A TCP client that disappears is noticed at once (failed send or closed connection), and its session is closed.
  * A UDP client is noticed when no RTCP report or RTSP request arrives for 60 s (`--rtsp-session-timeout`), and its session is then closed.
* **Encoding not finished yet:** if an RTSP client reaches the end of what FFmpeg has produced so far, the session simply waits for the next segment instead of failing.
* **Socket timeouts:** a client that stops reading cannot block a thread forever. Writes give up after 5 s (RTSP) or 30 s (HTTP).
* **Clean shutdown:** `Ctrl+C`/`SIGTERM` stops FFmpeg, closes all sessions with BYE, finishes running requests and exits. Interrupted encodes are redone on the next run.

---

## 9. Threads – who does what

| Thread(s) | Count | Job |
|-----------|-------|-----|
| main | 1 | starts everything, then waits for `SIGINT`/`SIGTERM` with `sigwait()` |
| HTTP accept | 1 | accepts TCP connections on port 8080 |
| HTTP poll | 1 | watches all idle keep-alive connections with `poll()` and hands busy ones to a worker |
| HTTP workers | 32 (`--http-threads`) | parse requests, run handlers, write responses |
| background pool | 4 (`--background-threads`) | runs ffprobe on new files (in parallel) and prefetches segments |
| transcode job | one per running FFmpeg (max 2, `--max-transcodes`) | starts FFmpeg, reads its progress, detects "ready" and "finished" |
| RTSP accept | 1 | accepts RTSP connections on port 8554 |
| RTSP connection | one per client | reads RTSP requests and interleaved RTCP, sends responses |
| RTSP sender | one per playing session | pacing, RTP packetization, RTCP, ABR |
| RTSP janitor | 1 | closes timed-out sessions and cleans up finished connections |

Shared data is protected with mutexes. Counters use `std::atomic`. Long operations (disk reads, network sends) are always done **outside** of locks.

The HTTP server is a small *reactor*: idle connections cost no thread, only a `poll()` entry. So a few dozen workers can serve hundreds of open browser connections. In the tests here, 480 segment requests (50 at a time) were served in about 0.6 s.

---

## 10. REST API reference

All responses are JSON unless noted. `{type}` is `videos` or `audios`. `{name}` is the URL-encoded file name.

| Method & path | Description |
|---------------|-------------|
| `GET /api/health` | `{"status":"ok","uptimeSeconds":12.3}` |
| `GET /api/config` | ports, segment length, encoder, folder paths |
| `GET /api/media` | `{"videos":[...], "audios":[...]}` – every file with its details |
| `GET /api/media/{type}/{name}` | one file with its details |
| `POST /api/media/{type}/{name}/prepare` | starts encoding if needed. Answers `200` (ready to play) or `202` (still preparing) with the job status. |
| `GET /api/stats` | live metrics: `http`, `rtsp` (incl. sessions), `cache`, `transcoder` (incl. jobs) |
| `GET /hls/{type}/{name}/master.m3u8` | HLS master playlist (starts encoding; `503` + `Retry-After` if not ready within 8 s) |
| `GET /hls/{type}/{name}/r{N}/index.m3u8` | media playlist of quality level N |
| `GET /hls/{type}/{name}/r{N}/seg_00000.ts` | one MPEG-TS segment |
| `GET /media/{type}/{name}` | the original file, with `Range` support (`206 Partial Content`) |
| `GET /` and `GET /static/{file}` | the web frontend |

Example media item (shortened):

```json
{
  "id": "videos/holiday.mp4",
  "type": "video",
  "name": "holiday.mp4",
  "sizeBytes": 14451637,
  "durationSeconds": 20,
  "playable": true,
  "video": { "codec": "h264", "width": 1920, "height": 1080, "frameRate": 30 },
  "audio": { "codec": "aac", "sampleRate": 48000, "channels": 2 },
  "urls": {
    "hls": "/hls/videos/holiday.mp4/master.m3u8",
    "direct": "/media/videos/holiday.mp4",
    "rtspPath": "/videos/holiday.mp4",
    "prepare": "/api/media/videos/holiday.mp4/prepare"
  },
  "stream": {
    "state": "running", "progress": 0.42, "ready": true,
    "renditions": [ { "name": "r0", "label": "360p", "videoKbps": 800, "audioKbps": 96, "width": 640, "height": 360, "codecs": "avc1.4d401e,mp4a.40.2" } ]
  }
}
```

Try it:

```bash
curl -s localhost:8080/api/media | python3 -m json.tool
curl -s -X POST localhost:8080/api/media/videos/holiday.mp4/prepare
curl -s localhost:8080/hls/videos/holiday.mp4/master.m3u8
curl -s localhost:8080/api/stats | python3 -m json.tool
```

---

## 11. Configuration options

Run `./build/media-streaming-service --help` for the full list. Both `--flag value` and `--flag=value` work.

| Option | Default | Meaning |
|--------|---------|---------|
| `--bind` | `0.0.0.0` | network address to listen on (`127.0.0.1` = this computer only) |
| `--http-port` | `8080` | REST API, HLS and web page |
| `--rtsp-port` | `8554` | RTSP |
| `--rtp-port-min` / `--rtp-port-max` | `50000` / `50999` | UDP ports for RTP/RTCP (2 per track) |
| `--root` | auto | project folder (contains `web/`) |
| `--videos-dir` / `--audios-dir` | `videos` / `audios` | media folders |
| `--cache-dir` | `cache` | transcoded HLS output (safe to delete while the server is stopped) |
| `--web-dir` | `web` | frontend files |
| `--http-threads` | `32` | HTTP worker threads |
| `--background-threads` | `4` | probe/prefetch threads |
| `--cache-mb` | `256` | RAM budget for the segment cache |
| `--max-rtsp-sessions` | `64` | concurrent RTSP sessions |
| `--rtsp-session-timeout` | `60` | seconds before a silent UDP session is closed |
| `--ffmpeg` / `--ffprobe` | `ffmpeg` / `ffprobe` | program paths |
| `--video-encoder` | `auto` | `libx264`, `h264_videotoolbox`, ... |
| `--x264-preset` | `veryfast` | libx264 speed/quality trade-off (`ultrafast` … `slow`) |
| `--segment-seconds` | `2` | HLS segment length (1–10) |
| `--max-renditions` | `4` | quality levels per video (1–5) |
| `--max-transcodes` | `2` | FFmpeg processes at the same time |
| `--log-level` | `info` | `debug` also logs every HTTP and RTSP request |

---

## 12. Project layout

```
media-streaming-service/
├── CMakeLists.txt              build definition (library + server + unit tests)
├── README.md                   this file
├── videos/  audios/            put your media here
├── cache/                      created at runtime: transcoded HLS output
├── web/
│   ├── index.html              Alpine.js page (layout and bindings)
│   ├── app.js                  player logic: hls.js, retries, statistics
│   └── style.css               styles (light and dark mode)
├── scripts/
│   ├── generate-test-media.sh  creates test videos/audio with FFmpeg
│   ├── smoke-test.sh           end-to-end check of a running server
│   └── slow_rtsp_client.py     throttled RTSP client for testing server-side ABR
├── tests/unit_tests.cpp        unit tests (parsers, packetizers, cache, ABR, ...)
└── src/
    ├── main.cpp                wires everything together, handles Ctrl+C
    ├── core/                   Config, Logger, ThreadPool, Json writer, string helpers, Process (posix_spawn)
    ├── net/Socket.*            RAII sockets, TCP listener, UDP sockets, sendAll()
    ├── http/                   HTTP parser/serializer, Router, multithreaded HttpServer
    ├── cache/SegmentCache.*    LRU segment cache with request coalescing and prefetching
    ├── media/
    │   ├── MediaLibrary.*      scans the folders, caches ffprobe results
    │   ├── MediaProbe.*        runs and parses ffprobe
    │   ├── BitrateLadder.*     chooses the quality levels
    │   ├── TranscodeManager.*  FFmpeg jobs: queue, progress, readiness, disk cache
    │   ├── HlsPlaylist.*       parses media playlists, builds master playlists
    │   ├── TsDemuxer.*         MPEG-TS demuxer
    │   ├── H264.*              NAL units, SPS parser, codec strings
    │   └── Aac.*               ADTS headers, AudioSpecificConfig
    ├── streaming/HlsService.*  HLS + direct-file HTTP endpoints, Range requests
    ├── api/ApiController.*     REST API and static web files
    └── rtsp/
        ├── RtspServer.*        accepts connections, RTSP request handling, session registry
        ├── RtspSession.*       one playback: pacing thread, RTP/RTCP, UDP/TCP transport
        ├── RtspMessage.*       RTSP parsing, Transport header, URLs, Range
        ├── RtpPacketizer.*     H.264 (RFC 6184) and AAC (RFC 3640) packetization
        ├── Rtcp.*              sender reports, BYE, receiver report parsing
        ├── Sdp.*               session descriptions
        └── AbrController.*     server-side adaptive bitrate decisions
```

---

## 13. Testing

**Unit tests** cover the HTTP parser, router, Range parsing, JSON, base64 and URL encoding, HLS playlists, ffprobe parsing, the bitrate ladder, ADTS/AudioSpecificConfig, H.264 Annex-B and Exp-Golomb, FU-A fragmentation round trip, AAC AU headers, RTCP SR/RR, RTSP Transport/URL parsing, SDP, the ABR controller, and the LRU cache with coalescing. They also demux a **real** FFmpeg H.264/AAC stream and parse its SPS.

```bash
./build/unit_tests            # or: ctest --test-dir build
```

**End-to-end smoke test** (start the server first):

```bash
./scripts/smoke-test.sh
```

It checks health, the media list, prepare, the master and media playlists, a segment, a Range request and 5 seconds of RTSP playback.

**Manual checks:**

```bash
# decode the whole HLS stream (quality level 3) without errors
ffmpeg -v error -i "http://localhost:8080/hls/videos/movie.mp4/master.m3u8" -map 0:p:3 -f null -

# decode the whole RTSP stream over TCP and over UDP
ffmpeg -v error -rtsp_transport tcp -i "rtsp://localhost:8554/videos/movie.mp4" -f null -
ffmpeg -v error -rtsp_transport udp -i "rtsp://localhost:8554/videos/movie.mp4" -f null -

# watch server-side ABR react to a slow network
python3 scripts/slow_rtsp_client.py "rtsp://127.0.0.1:8554/videos/movie.mp4" --kbps 600
```

---

## 14. Troubleshooting

| Problem | Fix |
|---------|-----|
| `ffmpeg/ffprobe not found` at start | Install FFmpeg, or pass `--ffmpeg /path/to/ffmpeg --ffprobe /path/to/ffprobe`. |
| `cannot listen on 0.0.0.0:8080: Address already in use` | Another program uses the port. Stop it or use `--http-port 9090` (and/or `--rtsp-port`). |
| A file shows `unreadable` | ffprobe cannot read it. Check it with `ffprobe yourfile`. |
| A file shows `failed` | Look at `cache/hls/<file>-<hash>/ffmpeg.log` for FFmpeg's error message. Failed jobs are retried 30 s later. |
| *Original file* mode doesn't play | The browser can't decode that format (for example MKV with HEVC). Use HLS mode, which re-encodes to H.264/AAC. |
| RTSP over UDP shows nothing (other computer) | A firewall blocks UDP ports 50000–50999. Use TCP (`-rtsp_transport tcp`, or in VLC: *Preferences → Input/Codecs → RTP over RTSP (TCP)*). On macOS, allow incoming connections when asked. |
| The web page shows a blank player | The page loads Alpine.js and hls.js from `cdn.jsdelivr.net`, so the browser needs internet access. For offline use, download both files into `web/` and change the two `<script>` URLs in `index.html` to `/static/...`. |
| Long files take a while to show "ready" | They don't: playback starts after the first segments. Seeking is limited to the part that is already encoded until the job finishes. |
| The cache folder grows | Delete `cache/` while the server is stopped. It is rebuilt on demand. |

---

## 15. Limits and ideas for extending it

* IPv4 only. No authentication and no HTTPS. Put it behind a reverse proxy (nginx, Caddy) for public use.
* The HTTP server reads requests with blocking calls after `poll()` reports data. An `epoll`/`kqueue` edge-triggered reactor would scale to tens of thousands of connections.
* Each playing RTSP session has its own sender thread. That is simple and works well for hundreds of sessions; thousands would need a shared timer-wheel scheduler.
* Ideas:
  * LL-HLS (partial segments) for even lower latency;
  * fMP4/CMAF segments (DASH support);
  * HEVC/AV1 ladders;
  * multicast RTP;
  * live inputs (cameras or RTMP ingest) through the same pipeline;
  * thumbnails and sprite sheets for the seek bar.
