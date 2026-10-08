#include "core/Json.hpp"

#include <cmath>
#include <cstdio>

namespace mss {

Json& Json::push(Json value) {
    if (type_ == Type::Null) type_ = Type::Array;
    items_.push_back(std::move(value));
    return *this;
}

Json& Json::set(const std::string& key, Json value) {
    if (type_ == Type::Null) type_ = Type::Object;
    for (auto& member : members_) {
        if (member.first == key) {
            member.second = std::move(value);
            return *this;
        }
    }
    members_.emplace_back(key, std::move(value));
    return *this;
}

std::size_t Json::size() const {
    if (type_ == Type::Array) return items_.size();
    if (type_ == Type::Object) return members_.size();
    return 0;
}

std::string Json::dump() const {
    std::string out;
    dumpTo(out);
    return out;
}

void Json::dumpTo(std::string& out) const {
    switch (type_) {
        case Type::Null:
            out += "null";
            break;
        case Type::Bool:
            out += bool_ ? "true" : "false";
            break;
        case Type::Integer:
            out += std::to_string(integer_);
            break;
        case Type::Double: {
            // JSON has no NaN or Infinity, so we write null for them.
            if (!std::isfinite(double_)) {
                out += "null";
                break;
            }
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "%.6f", double_);
            std::string text(buffer);
            // Remove useless trailing zeros: "2.500000" -> "2.5", "3.000000" -> "3".
            while (!text.empty() && text.back() == '0') text.pop_back();
            if (!text.empty() && text.back() == '.') text.pop_back();
            if (text.empty() || text == "-") text = "0";
            if (text == "-0") text = "0";
            out += text;
            break;
        }
        case Type::String:
            appendJsonString(out, string_);
            break;
        case Type::Array: {
            out += '[';
            for (std::size_t i = 0; i < items_.size(); ++i) {
                if (i > 0) out += ',';
                items_[i].dumpTo(out);
            }
            out += ']';
            break;
        }
        case Type::Object: {
            out += '{';
            for (std::size_t i = 0; i < members_.size(); ++i) {
                if (i > 0) out += ',';
                appendJsonString(out, members_[i].first);
                out += ':';
                members_[i].second.dumpTo(out);
            }
            out += '}';
            break;
        }
    }
}

void appendJsonString(std::string& out, const std::string& text) {
    out += '"';
    for (unsigned char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    // Other control characters must be written as \u00XX.
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                    out += buffer;
                } else {
                    out += static_cast<char>(c);  // normal character (UTF-8 bytes pass through)
                }
        }
    }
    out += '"';
}

}  // namespace mss
