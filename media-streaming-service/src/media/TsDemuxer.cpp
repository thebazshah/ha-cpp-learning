#include "media/TsDemuxer.hpp"

#include <algorithm>
#include <map>

#include "media/H264.hpp"

namespace mss {
namespace {

constexpr std::size_t kPacketSize = 188;
constexpr std::uint8_t kSyncByte = 0x47;
constexpr std::uint8_t kStreamTypeH264 = 0x1B;
constexpr std::uint8_t kStreamTypeAacAdts = 0x0F;

// Reads a 33-bit PTS/DTS value spread over 5 bytes (with marker bits in between).
std::int64_t readTimestamp(const std::uint8_t* p) {
    return (static_cast<std::int64_t>((p[0] >> 1) & 0x07) << 30) | (static_cast<std::int64_t>(p[1]) << 22) |
           (static_cast<std::int64_t>(p[2] >> 1) << 15) | (static_cast<std::int64_t>(p[3]) << 7) |
           (static_cast<std::int64_t>(p[4]) >> 1);
}

// Collects the payload bytes of one PES packet as it arrives in TS packets.
struct PesStream {
    StreamKind kind = StreamKind::Video;
    std::vector<std::uint8_t> data;
    bool active = false;  // true once we have seen the start of a PES packet
};

class Demuxer {
public:
    explicit Demuxer(DemuxResult& out) : out_(out) {}

    bool run(const std::uint8_t* data, std::size_t size, std::string& error) {
        // Find the first sync byte (normally at offset 0).
        std::size_t offset = 0;
        while (offset < size && data[offset] != kSyncByte) ++offset;

        std::size_t packets = 0;
        while (offset + kPacketSize <= size) {
            if (data[offset] != kSyncByte) {
                // Lost sync (damaged data): skip forward to the next sync byte.
                ++offset;
                while (offset < size && data[offset] != kSyncByte) ++offset;
                continue;
            }
            handlePacket(data + offset);
            offset += kPacketSize;
            ++packets;
        }
        for (auto& entry : streams_) flush(entry.second);  // the last PES packet of each stream

        if (packets == 0) {
            error = "no MPEG-TS packets found";
            return false;
        }
        std::stable_sort(out_.frames.begin(), out_.frames.end(),
                         [](const MediaFrame& a, const MediaFrame& b) { return a.dts < b.dts; });
        return true;
    }

private:
    void handlePacket(const std::uint8_t* packet) {
        const bool payloadStart = (packet[1] & 0x40) != 0;  // a new PES packet / table starts here
        const std::uint16_t pid = static_cast<std::uint16_t>(((packet[1] & 0x1F) << 8) | packet[2]);
        const int adaptationControl = (packet[3] >> 4) & 0x03;

        std::size_t offset = 4;
        if (adaptationControl & 0x02) offset += 1 + packet[4];  // skip the adaptation field
        if (!(adaptationControl & 0x01) || offset >= kPacketSize) return;  // no payload
        const std::uint8_t* payload = packet + offset;
        const std::size_t length = kPacketSize - offset;

        if (pid == 0x1FFF) return;  // null (padding) packet
        if (pid == 0) {
            if (payloadStart) parsePat(payload, length);
            return;
        }
        if (pid == pmtPid_) {
            if (payloadStart) parsePmt(payload, length);
            return;
        }

        auto it = streams_.find(pid);
        if (it == streams_.end()) {
            // No PMT entry for this PID. Fall back to guessing from the PES stream id:
            // 0xE0-0xEF are video streams, 0xC0-0xDF are audio streams.
            if (!payloadStart || length < 4 || payload[0] != 0 || payload[1] != 0 || payload[2] != 1) return;
            const std::uint8_t streamId = payload[3];
            if ((streamId & 0xF0) == 0xE0) {
                it = streams_.emplace(pid, PesStream{StreamKind::Video, {}, false}).first;
            } else if ((streamId & 0xE0) == 0xC0) {
                it = streams_.emplace(pid, PesStream{StreamKind::Audio, {}, false}).first;
            } else {
                return;
            }
        }

        PesStream& stream = it->second;
        if (payloadStart) {
            flush(stream);  // the previous PES packet is complete now
            stream.data.assign(payload, payload + length);
            stream.active = true;
        } else if (stream.active) {
            stream.data.insert(stream.data.end(), payload, payload + length);
        }
    }

    // PAT: maps program numbers to the PID of their PMT.
    void parsePat(const std::uint8_t* payload, std::size_t length) {
        const std::size_t pointer = payload[0];
        if (1 + pointer + 8 > length) return;
        const std::uint8_t* section = payload + 1 + pointer;
        const std::size_t available = length - 1 - pointer;
        const std::size_t sectionLength = ((section[1] & 0x0F) << 8) | section[2];
        if (section[0] != 0x00 || 3 + sectionLength > available || sectionLength < 9) return;
        const std::size_t end = 3 + sectionLength - 4;  // the last 4 bytes are a CRC
        for (std::size_t pos = 8; pos + 4 <= end; pos += 4) {
            const int program = (section[pos] << 8) | section[pos + 1];
            const int pid = ((section[pos + 2] & 0x1F) << 8) | section[pos + 3];
            if (program != 0) {  // program 0 is the network information table, not a real program
                pmtPid_ = pid;
                return;
            }
        }
    }

    // PMT: lists the elementary streams (video/audio) of the program and their PIDs.
    void parsePmt(const std::uint8_t* payload, std::size_t length) {
        const std::size_t pointer = payload[0];
        if (1 + pointer + 12 > length) return;
        const std::uint8_t* section = payload + 1 + pointer;
        const std::size_t available = length - 1 - pointer;
        const std::size_t sectionLength = ((section[1] & 0x0F) << 8) | section[2];
        if (section[0] != 0x02 || 3 + sectionLength > available || sectionLength < 13) return;
        const std::size_t programInfoLength = ((section[10] & 0x0F) << 8) | section[11];
        const std::size_t end = 3 + sectionLength - 4;
        std::size_t pos = 12 + programInfoLength;
        while (pos + 5 <= end) {
            const std::uint8_t streamType = section[pos];
            const std::uint16_t pid = static_cast<std::uint16_t>(((section[pos + 1] & 0x1F) << 8) | section[pos + 2]);
            const std::size_t infoLength = ((section[pos + 3] & 0x0F) << 8) | section[pos + 4];
            if (streams_.find(pid) == streams_.end()) {
                if (streamType == kStreamTypeH264) streams_.emplace(pid, PesStream{StreamKind::Video, {}, false});
                if (streamType == kStreamTypeAacAdts) streams_.emplace(pid, PesStream{StreamKind::Audio, {}, false});
            }
            pos += 5 + infoLength;
        }
    }

    // Parses a complete PES packet and turns it into frames.
    void flush(PesStream& stream) {
        if (!stream.active) return;
        stream.active = false;
        std::vector<std::uint8_t>& pes = stream.data;
        if (pes.size() < 9 || pes[0] != 0 || pes[1] != 0 || pes[2] != 1) return;

        // If the PES packet declares its length, ignore any stuffing after it.
        const std::size_t declaredLength = (pes[4] << 8) | pes[5];
        std::size_t size = pes.size();
        if (declaredLength != 0 && 6 + declaredLength < size) size = 6 + declaredLength;

        const int timestampFlags = (pes[7] >> 6) & 0x03;
        const std::size_t headerDataLength = pes[8];
        const std::size_t payloadStart = 9 + headerDataLength;
        if (payloadStart > size) return;

        std::int64_t pts = -1;
        std::int64_t dts = -1;
        if ((timestampFlags & 0x02) && headerDataLength >= 5) pts = readTimestamp(&pes[9]);
        if (timestampFlags == 0x03 && headerDataLength >= 10) dts = readTimestamp(&pes[14]);
        if (pts < 0) return;  // frames without a timestamp cannot be scheduled
        if (dts < 0) dts = pts;

        const std::uint8_t* payload = pes.data() + payloadStart;
        const std::size_t payloadSize = size - payloadStart;
        if (stream.kind == StreamKind::Video) {
            addVideo(pts, dts, payload, payloadSize);
        } else {
            addAudio(pts, payload, payloadSize);
        }
    }

    void addVideo(std::int64_t pts, std::int64_t dts, const std::uint8_t* data, std::size_t size) {
        if (size == 0) return;
        MediaFrame frame;
        frame.kind = StreamKind::Video;
        frame.pts = pts;
        frame.dts = dts;
        frame.data.assign(data, data + size);
        for (const h264::NalUnit& nal : h264::splitAnnexB(data, size)) {
            if (nal.type() == h264::kNalIdr) frame.keyframe = true;
            if (nal.type() == h264::kNalSps && out_.sps.empty()) out_.sps.assign(nal.data, nal.data + nal.size);
            if (nal.type() == h264::kNalPps && out_.pps.empty()) out_.pps.assign(nal.data, nal.data + nal.size);
        }
        out_.hasVideo = true;
        out_.frames.push_back(std::move(frame));
    }

    void addAudio(std::int64_t pts, const std::uint8_t* data, std::size_t size) {
        // One PES packet can hold several ADTS frames. Only the first one has
        // a PTS; the others follow at 1024 samples each.
        std::size_t offset = 0;
        int index = 0;
        while (offset + 7 <= size) {
            aac::AdtsHeader header;
            if (!aac::parseAdtsHeader(data + offset, size - offset, header)) {
                ++offset;  // damaged data: search for the next sync word
                continue;
            }
            if (offset + static_cast<std::size_t>(header.frameLength) > size) break;  // cut-off frame

            MediaFrame frame;
            frame.kind = StreamKind::Audio;
            frame.pts = pts + (static_cast<std::int64_t>(index) * 1024 * 90000) / header.sampleRate;
            frame.dts = frame.pts;
            frame.keyframe = true;  // every AAC frame can be decoded on its own
            frame.data.assign(data + offset + header.headerLength, data + offset + header.frameLength);
            out_.frames.push_back(std::move(frame));

            if (!out_.audioConfigKnown) {
                out_.audioConfig = header;
                out_.audioConfigKnown = true;
            }
            out_.hasAudio = true;
            offset += static_cast<std::size_t>(header.frameLength);
            ++index;
        }
    }

    DemuxResult& out_;
    int pmtPid_ = -1;
    std::map<std::uint16_t, PesStream> streams_;  // PID -> stream being collected
};

}  // namespace

bool demuxTransportStream(const std::uint8_t* data, std::size_t size, DemuxResult& out, std::string& error) {
    out = DemuxResult();
    Demuxer demuxer(out);
    return demuxer.run(data, size, error);
}

}  // namespace mss
