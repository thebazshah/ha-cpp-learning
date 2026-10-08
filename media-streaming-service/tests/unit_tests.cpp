// Unit tests for the parts of the server that can be tested without a
// network: parsers, packetizers, the cache, the ABR logic, ...
//
// Build and run:   cmake --build build && ./build/unit_tests
//
// The MPEG-TS test creates a short real H.264/AAC segment with ffmpeg. If
// ffmpeg is not installed that one test is skipped.

#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "cache/SegmentCache.hpp"
#include "core/Json.hpp"
#include "core/Process.hpp"
#include "core/StringUtils.hpp"
#include "http/HttpMessage.hpp"
#include "http/Router.hpp"
#include "media/Aac.hpp"
#include "media/BitrateLadder.hpp"
#include "media/H264.hpp"
#include "media/HlsPlaylist.hpp"
#include "media/MediaProbe.hpp"
#include "media/TsDemuxer.hpp"
#include "rtsp/AbrController.hpp"
#include "rtsp/Rtcp.hpp"
#include "rtsp/RtpPacketizer.hpp"
#include "rtsp/RtspMessage.hpp"
#include "rtsp/Sdp.hpp"
#include "streaming/HlsService.hpp"

using namespace mss;

// ------------------------------------------------------------ tiny test framework

namespace {
int gFailures = 0;
int gChecks = 0;
std::vector<std::pair<std::string, std::function<void()>>>& registry() {
    static std::vector<std::pair<std::string, std::function<void()>>> tests;
    return tests;
}
struct Register {
    Register(const char* name, std::function<void()> test) { registry().emplace_back(name, std::move(test)); }
};
}  // namespace

#define TEST(name)                                     \
    static void name();                                \
    static Register register_##name(#name, name);      \
    static void name()

#define CHECK(condition)                                                                       \
    do {                                                                                       \
        ++gChecks;                                                                             \
        if (!(condition)) {                                                                    \
            ++gFailures;                                                                       \
            std::cerr << "  FAILED: " << #condition << "  (" << __FILE__ << ":" << __LINE__ << ")\n"; \
        }                                                                                      \
    } while (0)

#define CHECK_EQ(actual, expected)                                                                  \
    do {                                                                                            \
        ++gChecks;                                                                                  \
        const auto actualValue = (actual);                                                          \
        const auto expectedValue = (expected);                                                      \
        if (!(actualValue == expectedValue)) {                                                      \
            ++gFailures;                                                                            \
            std::cerr << "  FAILED: " << #actual << " == " << #expected << "\n    got:      " << actualValue \
                      << "\n    expected: " << expectedValue << "  (" << __FILE__ << ":" << __LINE__ << ")\n"; \
        }                                                                                           \
    } while (0)

// ------------------------------------------------------------ core

TEST(json_writes_objects_arrays_and_escapes) {
    Json item = Json::object();
    item.set("name", "a \"quoted\"\nline").set("size", 42).set("ok", true).set("ratio", 2.5).set("none", nullptr);
    Json list = Json::array();
    list.push(1).push("x");
    item.set("list", list);
    CHECK_EQ(item.dump(),
             std::string(R"({"name":"a \"quoted\"\nline","size":42,"ok":true,"ratio":2.5,"none":null,"list":[1,"x"]})"));
    CHECK_EQ(Json(3.0).dump(), std::string("3"));
    CHECK_EQ(Json(std::uint64_t(18446744073709551615ULL)).type() == Json::Type::Integer, true);
}

TEST(string_helpers) {
    std::string decoded;
    CHECK(str::urlDecode("my%20movie%2B1.mp4", decoded, false));
    CHECK_EQ(decoded, std::string("my movie+1.mp4"));
    CHECK(str::urlDecode("a+b", decoded, true));
    CHECK_EQ(decoded, std::string("a b"));
    CHECK(!str::urlDecode("bad%zz", decoded, false));
    CHECK_EQ(str::urlEncode("a b/c.mp4"), std::string("a%20b%2Fc.mp4"));

    const std::uint8_t man[] = {'M', 'a', 'n'};
    CHECK_EQ(str::base64Encode(man, 3), std::string("TWFu"));
    CHECK_EQ(str::base64Encode(man, 2), std::string("TWE="));
    CHECK_EQ(str::base64Encode(man, 1), std::string("TQ=="));

    long long number = 0;
    CHECK(str::parseInt64("+42", number) && number == 42);
    CHECK(!str::parseInt64("42x", number));
    CHECK_EQ(str::fileExtension("Movie.MP4"), std::string("mp4"));
    CHECK_EQ(str::sanitizeForFileName("a b/c", 10), std::string("a_b_c"));
}

// ------------------------------------------------------------ HTTP

TEST(http_parser_handles_complete_partial_and_pipelined_requests) {
    std::string buffer = "GET /api/media?x=1&name=a%20b HTTP/1.1\r\nHost: localhost\r\nRange: bytes=0-\r\n\r\n";
    buffer += "POST /api/x HTTP/1.1\r\nContent-Length: 5\r\n\r\nhel";  // second request, body incomplete
    HttpRequest request;
    int status = 0;
    std::string message;
    CHECK(parseHttpRequest(buffer, request, status, message) == ParseResult::Complete);
    CHECK_EQ(request.method, std::string("GET"));
    CHECK_EQ(request.path, std::string("/api/media"));
    CHECK_EQ(request.queryParam("name"), std::string("a b"));
    CHECK_EQ(request.header("range").value_or(""), std::string("bytes=0-"));
    CHECK(request.keepAlive());

    CHECK(parseHttpRequest(buffer, request, status, message) == ParseResult::NeedMore);
    buffer += "lo";
    CHECK(parseHttpRequest(buffer, request, status, message) == ParseResult::Complete);
    CHECK_EQ(request.body, std::string("hello"));
    CHECK(buffer.empty());

    std::string bad = "GARBAGE\r\n\r\n";
    CHECK(parseHttpRequest(bad, request, status, message) == ParseResult::Error);
    CHECK_EQ(status, 400);

    std::string wrongVersion = "GET / HTTP/2.0\r\n\r\n";
    CHECK(parseHttpRequest(wrongVersion, request, status, message) == ParseResult::Error);
    CHECK_EQ(status, 505);
}

TEST(router_matches_parameters_and_methods) {
    Router router;
    router.add("GET", "/api/media/{type}/{name}", [](HttpRequest& r) {
        return HttpResponse::text(200, r.params["type"] + "|" + r.params["name"], "text/plain");
    });
    HttpRequest request;
    request.method = "GET";
    request.path = "/api/media/videos/my%20clip%2F1.mp4";
    HttpResponse response;
    CHECK(router.dispatch(request, response));
    CHECK_EQ(response.body, std::string("videos|my clip/1.mp4"));

    request.method = "POST";
    CHECK(router.dispatch(request, response));
    CHECK_EQ(response.status, 405);

    request.path = "/nothing/here";
    CHECK(!router.dispatch(request, response));
}

TEST(byte_ranges) {
    std::uint64_t start = 0;
    std::uint64_t end = 0;
    CHECK_EQ(parseByteRange("bytes=0-99", 1000, 4096, start, end), 1);
    CHECK(start == 0 && end == 99);
    CHECK_EQ(parseByteRange("bytes=500-", 1000, 4096, start, end), 1);
    CHECK(start == 500 && end == 999);
    CHECK_EQ(parseByteRange("bytes=0-", 100000, 4096, start, end), 1);  // open-ended ranges are capped
    CHECK(start == 0 && end == 4095);
    CHECK_EQ(parseByteRange("bytes=-100", 1000, 4096, start, end), 1);
    CHECK(start == 900 && end == 999);
    CHECK_EQ(parseByteRange("bytes=2000-", 1000, 4096, start, end), -1);
    CHECK_EQ(parseByteRange("", 1000, 4096, start, end), 0);
}

// ------------------------------------------------------------ media

TEST(hls_playlists) {
    const std::string text =
        "#EXTM3U\n#EXT-X-VERSION:6\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:0\n"
        "#EXT-X-PLAYLIST-TYPE:EVENT\n#EXTINF:2.000000,\nseg_00000.ts\n#EXTINF:2.000000,\nseg_00001.ts\n"
        "#EXTINF:1.500000,\nseg_00002.ts\n#EXT-X-ENDLIST\n";
    HlsMediaPlaylist playlist;
    CHECK(parseMediaPlaylist(text, playlist));
    CHECK_EQ(playlist.segments.size(), std::size_t(3));
    CHECK(playlist.endList);
    CHECK_EQ(playlist.segments[2].uri, std::string("seg_00002.ts"));
    CHECK(playlist.totalDuration() > 5.49 && playlist.totalDuration() < 5.51);
    CHECK_EQ(playlist.segmentIndexAt(3.9), std::size_t(1));
    CHECK_EQ(playlist.segmentIndexAt(100), std::size_t(2));
    CHECK(!parseMediaPlaylist("not a playlist", playlist));

    HlsVariant variant;
    variant.uri = "r0/index.m3u8";
    variant.bandwidth = 1000000;
    variant.codecs = "avc1.4d401e,mp4a.40.2";
    variant.width = 640;
    variant.height = 360;
    const std::string master = buildMasterPlaylist({variant});
    CHECK(master.find("RESOLUTION=640x360") != std::string::npos);
    CHECK(master.find("CODECS=\"avc1.4d401e,mp4a.40.2\"") != std::string::npos);
}

TEST(ffprobe_output_parsing_ignores_cover_art) {
    const std::string text =
        "[STREAM]\nindex=0\ncodec_name=mjpeg\ncodec_type=video\nwidth=500\nheight=500\n"
        "DISPOSITION:attached_pic=1\n[/STREAM]\n"
        "[STREAM]\nindex=1\ncodec_name=mp3\ncodec_type=audio\nsample_rate=44100\nchannels=2\n"
        "DISPOSITION:attached_pic=0\n[/STREAM]\n"
        "[FORMAT]\nformat_name=mp3\nduration=187.5\nbit_rate=320000\n[/FORMAT]\n";
    const MediaInfo info = parseFfprobeOutput(text);
    CHECK(info.ok);
    CHECK(!info.hasVideo);  // the album cover is not a video
    CHECK(info.hasAudio);
    CHECK_EQ(info.audioCodec, std::string("mp3"));
    CHECK_EQ(info.sampleRate, 44100);
    CHECK(info.durationSeconds > 187.4);

    const MediaInfo video = parseFfprobeOutput(
        "[STREAM]\ncodec_name=h264\ncodec_type=video\nwidth=1920\nheight=1080\navg_frame_rate=30000/1001\n"
        "DISPOSITION:attached_pic=0\n[/STREAM]\n[FORMAT]\nduration=10\n[/FORMAT]\n");
    CHECK(video.hasVideo);
    CHECK(video.frameRate > 29.96 && video.frameRate < 29.98);
}

TEST(bitrate_ladder_never_upscales) {
    MediaInfo fullHd;
    fullHd.ok = fullHd.hasVideo = fullHd.hasAudio = true;
    fullHd.width = 1920;
    fullHd.height = 1080;
    std::vector<Rendition> ladder = buildLadder(fullHd, true, 4);
    CHECK_EQ(ladder.size(), std::size_t(4));
    CHECK_EQ(ladder.front().shortSide, 360);
    CHECK_EQ(ladder.back().shortSide, 1080);
    CHECK_EQ(ladder.back().name, std::string("r3"));
    CHECK(ladder.front().peakBandwidth() < ladder.back().peakBandwidth());

    MediaInfo portrait = fullHd;  // phone video: 1080x1920
    portrait.width = 1080;
    portrait.height = 1920;
    CHECK_EQ(buildLadder(portrait, true, 4).back().shortSide, 1080);

    MediaInfo small = fullHd;
    small.width = 320;
    small.height = 180;
    ladder = buildLadder(small, true, 4);
    CHECK_EQ(ladder.size(), std::size_t(1));
    CHECK_EQ(ladder[0].shortSide, 180);

    MediaInfo audioOnly;
    audioOnly.ok = audioOnly.hasAudio = true;
    ladder = buildLadder(audioOnly, false, 4);
    CHECK_EQ(ladder.size(), std::size_t(3));
    CHECK(!ladder[0].hasVideo());
    CHECK_EQ(ladder[2].label(), std::string("192 kbps"));
}

TEST(aac_adts_header_and_config) {
    // ADTS header for AAC-LC, 48 kHz, stereo, frame length 371 bytes.
    const std::uint8_t header[] = {0xFF, 0xF1, 0x4C, 0x80, 0x2E, 0x7F, 0xFC};
    aac::AdtsHeader adts;
    CHECK(aac::parseAdtsHeader(header, sizeof(header), adts));
    CHECK_EQ(adts.audioObjectType, 2);
    CHECK_EQ(adts.sampleRate, 48000);
    CHECK_EQ(adts.channelConfig, 2);
    CHECK_EQ(adts.frameLength, 371);
    CHECK_EQ(adts.headerLength, 7);
    const std::vector<std::uint8_t> config = aac::audioSpecificConfig(adts);
    CHECK_EQ(str::toHex(config.data(), config.size(), true), std::string("1190"));
}

TEST(h264_annexb_and_exp_golomb) {
    const std::uint8_t stream[] = {0, 0, 0, 1, 0x67, 0xAA, 0, 0, 1, 0x68, 0xBB, 0, 0, 0, 1, 0x65, 0x01, 0x02};
    const auto nals = h264::splitAnnexB(stream, sizeof(stream));
    CHECK_EQ(nals.size(), std::size_t(3));
    CHECK_EQ(int(nals[0].type()), 7);
    CHECK_EQ(nals[0].size, std::size_t(2));
    CHECK_EQ(int(nals[2].type()), 5);
    CHECK_EQ(nals[2].size, std::size_t(3));

    const std::uint8_t escaped[] = {0x00, 0x00, 0x03, 0x01, 0x05};
    CHECK_EQ(h264::removeEmulationPrevention(escaped, sizeof(escaped)).size(), std::size_t(4));

    // Bits 1 | 010 | 011 | 00100 -> ue values 0, 1, 2, 3
    const std::uint8_t bits[] = {0xA6, 0x40};
    h264::BitReader reader(bits, sizeof(bits));
    CHECK_EQ(reader.readUnsignedExpGolomb(), 0u);
    CHECK_EQ(reader.readUnsignedExpGolomb(), 1u);
    CHECK_EQ(reader.readUnsignedExpGolomb(), 2u);
    CHECK_EQ(reader.readUnsignedExpGolomb(), 3u);
    CHECK(!reader.failed());
}

// ------------------------------------------------------------ RTP / RTCP / RTSP

TEST(rtp_h264_fragmentation_round_trip) {
    // One big IDR NAL unit (5000 bytes) and one small SEI NAL unit.
    std::vector<std::uint8_t> accessUnit = {0, 0, 0, 1, 0x06, 0x05, 0x10};
    std::vector<std::uint8_t> idr = {0x65};
    for (int i = 0; i < 4999; ++i) idr.push_back(static_cast<std::uint8_t>(i * 7 + 1));
    accessUnit.insert(accessUnit.end(), {0, 0, 0, 1});
    accessUnit.insert(accessUnit.end(), idr.begin(), idr.end());

    RtpPacketizer packetizer(96, 0x11223344, 1000, 1400);
    std::vector<std::vector<std::uint8_t>> packets;
    packetizer.packetizeH264(accessUnit.data(), accessUnit.size(), 90000,
                             [&](const std::uint8_t* p, std::size_t n) { packets.emplace_back(p, p + n); });

    CHECK(packets.size() >= 5);
    CHECK_EQ(int(packets[0][1] & 0x7F), 96);
    CHECK_EQ((packets[0][2] << 8) | packets[0][3], 1000);   // first sequence number
    CHECK_EQ(int(packets[0][12] & 0x1F), 6);                // single NAL packet (SEI)
    CHECK_EQ(int(packets[0][1] & 0x80), 0);                 // marker only on the last packet
    CHECK_EQ(int(packets.back()[1] & 0x80), 0x80);

    // Reassemble the FU-A fragments and compare with the original NAL unit.
    std::vector<std::uint8_t> rebuilt;
    for (std::size_t i = 1; i < packets.size(); ++i) {
        const auto& p = packets[i];
        CHECK(p.size() <= 12 + 1400);
        CHECK_EQ(int(p[12] & 0x1F), 28);  // FU-A
        if (p[13] & 0x80) rebuilt.push_back(static_cast<std::uint8_t>((p[12] & 0xE0) | (p[13] & 0x1F)));
        rebuilt.insert(rebuilt.end(), p.begin() + 14, p.end());
    }
    CHECK(packets[1][13] & 0x80);       // start bit on the first fragment
    CHECK(packets.back()[13] & 0x40);   // end bit on the last fragment
    CHECK(rebuilt == idr);
    CHECK_EQ(packetizer.nextSequence(), static_cast<std::uint16_t>(1000 + packets.size()));
}

TEST(rtp_aac_au_header) {
    RtpPacketizer packetizer(97, 1, 5);
    std::vector<std::uint8_t> frame(300, 0xAB);
    std::vector<std::uint8_t> packet;
    packetizer.packetizeAac(frame.data(), frame.size(), 1024,
                            [&](const std::uint8_t* p, std::size_t n) { packet.assign(p, p + n); });
    CHECK_EQ(packet.size(), std::size_t(12 + 4 + 300));
    CHECK_EQ(int(packet[1] & 0x80), 0x80);                 // marker
    CHECK_EQ((packet[12] << 8) | packet[13], 16);          // AU-headers-length in bits
    CHECK_EQ(((packet[14] << 8) | packet[15]) >> 3, 300);  // AU-size
}

TEST(rtcp_sender_and_receiver_reports) {
    const std::vector<std::uint8_t> report = rtcp::buildSenderReport(0xAABBCCDD, rtcp::ntpNow(), 1234, 10, 5000, "x@y");
    CHECK_EQ(int(report[1]), 200);
    CHECK_EQ(report.size() % 4, std::size_t(0));
    CHECK(rtcp::parseReportBlocks(report.data(), report.size()).empty());  // an SR without report blocks

    // A receiver report with one block: 25% loss (64/256).
    const std::uint8_t rr[] = {0x81, 201, 0, 7,  0, 0, 0, 9,  0xAA, 0xBB, 0xCC, 0xDD, 64, 0, 0, 3,
                               0, 0, 0x10, 0, 0, 0, 0, 5,   0, 0, 0, 0,  0, 0, 0, 0};
    const auto blocks = rtcp::parseReportBlocks(rr, sizeof(rr));
    CHECK_EQ(blocks.size(), std::size_t(1));
    CHECK_EQ(blocks[0].ssrc, 0xAABBCCDDu);
    CHECK(blocks[0].fractionLost > 0.24 && blocks[0].fractionLost < 0.26);
    CHECK_EQ(blocks[0].cumulativeLost, 3u);
}

TEST(rtsp_messages_transport_and_urls) {
    std::string buffer = "SETUP rtsp://h:8554/videos/a%20b.mp4/trackID=1 RTSP/1.0\r\nCSeq: 3\r\n"
                         "Transport: RTP/AVP/TCP;unicast;interleaved=2-3\r\nSession: ABC;timeout=60\r\n\r\n";
    RtspRequest request;
    std::string error;
    CHECK(parseRtspRequest(buffer, request, error) == ParseResult::Complete);
    CHECK_EQ(request.method, std::string("SETUP"));
    CHECK_EQ(request.cseq(), 3);
    CHECK_EQ(request.sessionId(), std::string("ABC"));

    TransportSpec transport;
    CHECK(parseTransport(request.header("transport").value_or(""), transport));
    CHECK(transport.tcp && transport.rtpChannel == 2 && transport.rtcpChannel == 3);
    CHECK(parseTransport("RTP/AVP;unicast;client_port=5000-5001", transport));
    CHECK(!transport.tcp && transport.clientRtpPort == 5000 && transport.clientRtcpPort == 5001);
    CHECK(!parseTransport("RTP/AVP;multicast", transport));
    CHECK(parseTransport("RTP/SAVP;unicast;client_port=1-2, RTP/AVP/TCP;interleaved=0-1", transport));
    CHECK(transport.tcp);

    RtspTarget target;
    CHECK(parseRtspUrl(request.url, target));
    CHECK(target.kind == MediaKind::Video && target.name == "a b.mp4" && target.trackId == 1);
    CHECK(parseRtspUrl("rtsp://h/audios/song.mp3?rendition=2/trackID=0", target));
    CHECK(target.kind == MediaKind::Audio && target.rendition == 2 && target.trackId == 0);
    CHECK(!parseRtspUrl("rtsp://h/other/x.mp4", target));

    CHECK(parseNptStart("npt=12.5-").value_or(-1) == 12.5);
    CHECK(!parseNptStart("npt=now-").has_value());
}

TEST(sdp_contains_codec_parameters) {
    SdpDescription d;
    d.serverIp = "127.0.0.1";
    d.hasVideo = true;
    d.profileLevelId = "4D401E";
    d.sps = {0x67, 0x4D};
    d.pps = {0x68, 0xEE};
    d.hasAudio = true;
    d.audioTrackId = 1;
    d.audioConfig.audioObjectType = 2;
    d.audioConfig.samplingIndex = 3;
    d.audioConfig.sampleRate = 48000;
    d.audioConfig.channelConfig = 2;
    const std::string sdp = buildSdp(d);
    CHECK(sdp.find("a=rtpmap:96 H264/90000") != std::string::npos);
    CHECK(sdp.find("sprop-parameter-sets=Z00=,aO4=") != std::string::npos);
    CHECK(sdp.find("a=rtpmap:97 MPEG4-GENERIC/48000/2") != std::string::npos);
    CHECK(sdp.find("config=1190") != std::string::npos);
    CHECK(sdp.find("a=control:trackID=1") != std::string::npos);
}

TEST(abr_controller_reacts_to_congestion) {
    AbrController abr(4, 2, true);
    abr.reportLateness(0.0);
    CHECK_EQ(abr.chooseNext(), std::size_t(2));  // healthy, but too early to go up
    abr.reportLoss(0.20);
    CHECK_EQ(abr.chooseNext(), std::size_t(1));  // heavy loss: down
    abr.reportStall();
    CHECK_EQ(abr.chooseNext(), std::size_t(0));  // stall: down again
    abr.reportStall();
    CHECK_EQ(abr.chooseNext(), std::size_t(0));  // cannot go below the lowest level

    AbrController fixed(4, 3, false);  // a forced rendition never changes
    fixed.reportStall();
    CHECK_EQ(fixed.chooseNext(), std::size_t(3));
}

// ------------------------------------------------------------ cache

TEST(segment_cache_lru_and_coalescing) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("mss-cache-test-" + std::to_string(getpid()));
    fs::create_directories(dir);
    for (int i = 0; i < 4; ++i) {
        std::ofstream(dir / ("f" + std::to_string(i))) << std::string(100, static_cast<char>('a' + i));
    }

    SegmentCache cache(1000);  // files bigger than 250 bytes are not kept, 100-byte files are
    CHECK(cache.get((dir / "f0").string()) != nullptr);
    CHECK(cache.get((dir / "f0").string()) != nullptr);
    CHECK(cache.get((dir / "missing").string()) == nullptr);
    const std::string stats = cache.statsJson().dump();
    CHECK(stats.find("\"hits\":1") != std::string::npos);
    CHECK(stats.find("\"misses\":2") != std::string::npos);

    // Many threads asking for the same file at once share one disk read.
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) threads.emplace_back([&] { CHECK(cache.get((dir / "f1").string()) != nullptr); });
    for (auto& thread : threads) thread.join();
    CHECK(cache.statsJson().dump().find("\"misses\":3") != std::string::npos);

    SegmentCache small(450);  // room for 4 x 100 bytes; the 5th insert evicts the oldest
    for (int i = 0; i < 4; ++i) small.get((dir / ("f" + std::to_string(i))).string());
    CHECK(small.statsJson().dump().find("\"entries\":4") != std::string::npos);
    std::ofstream(dir / "f4") << std::string(100, 'z');
    small.get((dir / "f4").string());
    CHECK(small.statsJson().dump().find("\"evictions\":1") != std::string::npos);

    fs::remove_all(dir);
}

// ------------------------------------------------------------ real media (needs ffmpeg)

TEST(transport_stream_demuxer_reads_real_ffmpeg_output) {
    namespace fs = std::filesystem;
    const fs::path file = fs::temp_directory_path() / ("mss-ts-test-" + std::to_string(getpid()) + ".ts");
    const ProcessResult made = Process::run(
        {"ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i", "testsrc2=size=640x360:rate=30",
         "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000", "-t", "2", "-c:v", "libx264", "-profile:v",
         "main", "-bf", "0", "-pix_fmt", "yuv420p", "-c:a", "aac", "-ac", "2", "-f", "mpegts", file.string()},
        60);
    if (!made.started || made.exitCode != 0) {
        std::cout << "  (skipped: ffmpeg with libx264 is not available)\n";
        return;
    }

    std::string data;
    CHECK(readWholeFile(file.string(), data));
    DemuxResult result;
    std::string error;
    CHECK(demuxTransportStream(reinterpret_cast<const std::uint8_t*>(data.data()), data.size(), result, error));
    CHECK(result.hasVideo && result.hasAudio);
    CHECK(!result.sps.empty() && !result.pps.empty());

    h264::SpsInfo sps;
    CHECK(h264::parseSps(result.sps.data(), result.sps.size(), sps));
    CHECK_EQ(sps.width, 640);
    CHECK_EQ(sps.height, 360);
    CHECK_EQ(sps.profileIdc, 77);  // Main profile
    CHECK_EQ(h264::codecString(sps).substr(0, 7), std::string("avc1.4d"));

    CHECK(result.audioConfigKnown);
    CHECK_EQ(result.audioConfig.sampleRate, 48000);
    CHECK_EQ(result.audioConfig.channelConfig, 2);

    int videoFrames = 0;
    int audioFrames = 0;
    bool sorted = true;
    for (std::size_t i = 0; i < result.frames.size(); ++i) {
        if (result.frames[i].kind == StreamKind::Video) ++videoFrames;
        else ++audioFrames;
        if (i > 0 && result.frames[i].dts < result.frames[i - 1].dts) sorted = false;
    }
    CHECK_EQ(videoFrames, 60);           // 2 seconds at 30 fps
    CHECK(audioFrames >= 90);            // ~94 AAC frames of 1024 samples at 48 kHz
    CHECK(sorted);
    for (const MediaFrame& frame : result.frames) {
        if (frame.kind == StreamKind::Video) {
            CHECK(frame.keyframe);       // the first picture is a key frame
            break;
        }
    }
    fs::remove(file);
}

// ------------------------------------------------------------ main

int main() {
    for (const auto& test : registry()) {
        const int before = gFailures;
        std::cout << "[ RUN  ] " << test.first << "\n";
        test.second();
        std::cout << (gFailures == before ? "[  OK  ] " : "[ FAIL ] ") << test.first << "\n";
    }
    std::cout << "\n" << registry().size() << " tests, " << gChecks << " checks, " << gFailures << " failures\n";
    return gFailures == 0 ? 0 : 1;
}
