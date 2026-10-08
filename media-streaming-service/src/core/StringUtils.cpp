#include "core/StringUtils.hpp"

#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstdlib>

namespace mss::str {

namespace {
bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
}  // namespace

std::string toLower(std::string_view text) {
    std::string out(text);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string trim(std::string_view text) {
    std::size_t start = 0;
    std::size_t end = text.size();
    while (start < end && isSpace(text[start])) ++start;
    while (end > start && isSpace(text[end - 1])) --end;
    return std::string(text.substr(start, end - start));
}

std::vector<std::string> split(std::string_view text, char separator) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (;;) {
        const std::size_t position = text.find(separator, start);
        if (position == std::string_view::npos) {
            parts.emplace_back(text.substr(start));
            return parts;
        }
        parts.emplace_back(text.substr(start, position - start));
        start = position + 1;
    }
}

bool startsWith(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool endsWith(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

bool urlDecode(std::string_view text, std::string& out, bool plusIsSpace) {
    out.clear();
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '%') {
            if (i + 2 >= text.size()) return false;
            const int high = hexValue(text[i + 1]);
            const int low = hexValue(text[i + 2]);
            if (high < 0 || low < 0) return false;
            out += static_cast<char>(high * 16 + low);
            i += 2;
        } else if (c == '+' && plusIsSpace) {
            out += ' ';
        } else {
            out += c;
        }
    }
    return true;
}

std::string urlEncode(std::string_view text) {
    static const char* const kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size() * 3);
    for (unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 0x0F];
        }
    }
    return out;
}

std::string base64Encode(const std::uint8_t* data, std::size_t size) {
    static const char* const kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((size + 2) / 3) * 4);
    std::size_t i = 0;
    // Every 3 input bytes (24 bits) become 4 output characters (6 bits each).
    while (i + 2 < size) {
        const std::uint32_t chunk = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out += kAlphabet[(chunk >> 18) & 0x3F];
        out += kAlphabet[(chunk >> 12) & 0x3F];
        out += kAlphabet[(chunk >> 6) & 0x3F];
        out += kAlphabet[chunk & 0x3F];
        i += 3;
    }
    // Handle the last 1 or 2 bytes and pad with '='.
    const std::size_t remaining = size - i;
    if (remaining == 1) {
        const std::uint32_t chunk = data[i] << 16;
        out += kAlphabet[(chunk >> 18) & 0x3F];
        out += kAlphabet[(chunk >> 12) & 0x3F];
        out += "==";
    } else if (remaining == 2) {
        const std::uint32_t chunk = (data[i] << 16) | (data[i + 1] << 8);
        out += kAlphabet[(chunk >> 18) & 0x3F];
        out += kAlphabet[(chunk >> 12) & 0x3F];
        out += kAlphabet[(chunk >> 6) & 0x3F];
        out += '=';
    }
    return out;
}

std::string toHex(const std::uint8_t* data, std::size_t size, bool upperCase) {
    const char* digits = upperCase ? "0123456789ABCDEF" : "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        out += digits[data[i] >> 4];
        out += digits[data[i] & 0x0F];
    }
    return out;
}

bool parseInt64(std::string_view text, long long& out) {
    if (text.empty()) return false;
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    if (*begin == '+') ++begin;  // from_chars does not accept a leading '+'
    long long value = 0;
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc() || result.ptr != end) return false;
    out = value;
    return true;
}

bool parseDouble(std::string_view text, double& out) {
    if (text.empty()) return false;
    const std::string copy(text);  // strtod needs a zero-terminated string
    char* end = nullptr;
    const double value = std::strtod(copy.c_str(), &end);
    if (end != copy.c_str() + copy.size()) return false;
    out = value;
    return true;
}

std::string formatDouble(double value, int decimals) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    return buffer;
}

std::string httpDate(std::time_t time) {
    std::tm utc{};
    gmtime_r(&time, &utc);
    char buffer[64];
    std::strftime(buffer, sizeof(buffer), "%a, %d %b %Y %H:%M:%S GMT", &utc);
    return buffer;
}

std::string fileExtension(std::string_view fileName) {
    const std::size_t dot = fileName.rfind('.');
    if (dot == std::string_view::npos || dot + 1 >= fileName.size()) return "";
    return toLower(fileName.substr(dot + 1));
}

std::uint64_t fnv1a64(std::string_view text) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (unsigned char c : text) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string sanitizeForFileName(std::string_view text, std::size_t maxLength) {
    std::string out;
    for (unsigned char c : text) {
        if (out.size() >= maxLength) break;
        if (std::isalnum(c) || c == '.' || c == '_' || c == '-') {
            out += static_cast<char>(c);
        } else {
            out += '_';
        }
    }
    return out;
}

}  // namespace mss::str
