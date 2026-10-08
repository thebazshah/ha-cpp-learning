#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

// Small text helpers used all over the server.
namespace mss::str {

std::string toLower(std::string_view text);
std::string trim(std::string_view text);

// Splits "a,b,,c" by ',' into {"a", "b", "", "c"}.
std::vector<std::string> split(std::string_view text, char separator);

bool startsWith(std::string_view text, std::string_view prefix);
bool endsWith(std::string_view text, std::string_view suffix);

// Case-insensitive comparison of ASCII text ("Content-Length" == "content-length").
bool iequals(std::string_view a, std::string_view b);

// Decodes "%20"-style escapes. When plusIsSpace is true, '+' becomes ' '
// (that rule only applies to query strings). Returns false for broken escapes.
bool urlDecode(std::string_view text, std::string& out, bool plusIsSpace);

// Escapes every byte that is not a letter, digit or one of "-._~".
std::string urlEncode(std::string_view text);

std::string base64Encode(const std::uint8_t* data, std::size_t size);
std::string toHex(const std::uint8_t* data, std::size_t size, bool upperCase);

// Strict number parsing: the whole text must be a number.
bool parseInt64(std::string_view text, long long& out);
bool parseDouble(std::string_view text, double& out);

// Formats a number with a fixed number of decimals ("1.500").
std::string formatDouble(double value, int decimals);

// Formats a time as an HTTP date: "Wed, 08 Oct 2026 10:00:00 GMT".
std::string httpDate(std::time_t time);

// Returns the lowercase extension without the dot: "Movie.MP4" -> "mp4".
std::string fileExtension(std::string_view fileName);

// A fast, stable 64-bit hash (FNV-1a). Used to build cache folder names.
std::uint64_t fnv1a64(std::string_view text);

// Keeps only [A-Za-z0-9._-] so the result is safe to use as a folder name.
std::string sanitizeForFileName(std::string_view text, std::size_t maxLength);

}  // namespace mss::str
