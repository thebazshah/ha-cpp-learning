#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace cge {

// SHA-1 hash (FIPS 180-1). The WebSocket handshake needs it: the server
// proves it understood the request by hashing the client's key.
// (SHA-1 is no longer secure for signatures, but the handshake only uses it
// as a checksum, which is fine.)
std::array<std::uint8_t, 20> sha1(const std::string& data);

}  // namespace cge
