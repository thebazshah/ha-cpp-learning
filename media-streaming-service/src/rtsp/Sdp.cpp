#include "rtsp/Sdp.hpp"

#include <chrono>
#include <sstream>

#include "core/StringUtils.hpp"

namespace mss {

std::string buildSdp(const SdpDescription& d) {
    const auto sessionVersion = std::chrono::duration_cast<std::chrono::seconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count();
    std::ostringstream sdp;
    sdp << "v=0\r\n";
    sdp << "o=- " << sessionVersion << " 1 IN IP4 " << d.serverIp << "\r\n";
    sdp << "s=" << (d.sessionName.empty() ? "Media Streaming Service" : d.sessionName) << "\r\n";
    sdp << "c=IN IP4 0.0.0.0\r\n";
    sdp << "t=0 0\r\n";
    sdp << "a=tool:MediaStreamingService\r\n";
    sdp << "a=control:*\r\n";  // PLAY/PAUSE apply to all tracks at once
    if (d.durationSeconds > 0) {
        sdp << "a=range:npt=0-" << str::formatDouble(d.durationSeconds, 3) << "\r\n";
    }

    if (d.hasVideo) {
        sdp << "m=video 0 RTP/AVP " << d.videoPayloadType << "\r\n";
        sdp << "a=rtpmap:" << d.videoPayloadType << " H264/90000\r\n";
        sdp << "a=fmtp:" << d.videoPayloadType << " packetization-mode=1";
        if (!d.profileLevelId.empty()) sdp << ";profile-level-id=" << d.profileLevelId;
        if (!d.sps.empty() && !d.pps.empty()) {
            // The decoder needs SPS/PPS before the first picture. Sending them
            // in the SDP (base64) lets playback start immediately.
            sdp << ";sprop-parameter-sets=" << str::base64Encode(d.sps.data(), d.sps.size()) << ","
                << str::base64Encode(d.pps.data(), d.pps.size());
        }
        sdp << "\r\n";
        sdp << "a=control:trackID=" << d.videoTrackId << "\r\n";
    }

    if (d.hasAudio) {
        const std::vector<std::uint8_t> config = aac::audioSpecificConfig(d.audioConfig);
        const int channels = d.audioConfig.channelConfig > 0 ? d.audioConfig.channelConfig : 2;
        sdp << "m=audio 0 RTP/AVP " << d.audioPayloadType << "\r\n";
        sdp << "a=rtpmap:" << d.audioPayloadType << " MPEG4-GENERIC/" << d.audioConfig.sampleRate << "/" << channels
            << "\r\n";
        // RFC 3640 "AAC-hbr" mode: each RTP packet carries one AAC frame with
        // a small header (13 bits size + 3 bits index).
        sdp << "a=fmtp:" << d.audioPayloadType
            << " streamtype=5;profile-level-id=1;mode=AAC-hbr;sizelength=13;indexlength=3;indexdeltalength=3;config="
            << str::toHex(config.data(), config.size(), true) << "\r\n";
        sdp << "a=control:trackID=" << d.audioTrackId << "\r\n";
    }
    return sdp.str();
}

}  // namespace mss
