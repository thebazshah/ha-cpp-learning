#pragma once

#include <cstddef>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace cge {

// A very small JSON value type, used to build REST API responses.
//
// It can only *write* JSON - the server never needs to read JSON.
// Objects keep their keys in the order they were added, so the output is
// easy to read.
//
//   Json item = Json::object();
//   item.set("name", "movie.mp4").set("size", 1234);
//   std::string text = item.dump();   // {"name":"movie.mp4","size":1234}
class Json {
public:
    enum class Type { Null, Bool, Integer, Double, String, Array, Object };

    Json() = default;  // null
    Json(std::nullptr_t) {}
    Json(bool value) : type_(Type::Bool), bool_(value) {}
    Json(double value) : type_(Type::Double), double_(value) {}
    Json(float value) : Json(static_cast<double>(value)) {}
    Json(const char* value) : type_(Type::String), string_(value ? value : "") {}
    Json(std::string value) : type_(Type::String), string_(std::move(value)) {}

    // Accepts every integer type (int, long, size_t, uint64_t, ...).
    // bool has its own constructor above, so it is excluded here.
    template <typename T,
              typename std::enable_if<std::is_integral<T>::value && !std::is_same<T, bool>::value, int>::type = 0>
    Json(T value) : type_(Type::Integer), integer_(static_cast<long long>(value)) {}

    static Json array() {
        Json value;
        value.type_ = Type::Array;
        return value;
    }
    static Json object() {
        Json value;
        value.type_ = Type::Object;
        return value;
    }

    // Appends an element to an array. A null value becomes an array first.
    Json& push(Json value);

    // Sets a key of an object (replacing an existing key with the same name).
    // A null value becomes an object first. Returns *this so calls can be chained.
    Json& set(const std::string& key, Json value);

    Type type() const { return type_; }
    std::size_t size() const;

    // Converts the value to compact JSON text.
    std::string dump() const;

private:
    void dumpTo(std::string& out) const;

    Type type_ = Type::Null;
    bool bool_ = false;
    long long integer_ = 0;
    double double_ = 0.0;
    std::string string_;
    std::vector<Json> items_;                            // array elements
    std::vector<std::pair<std::string, Json>> members_;  // object members, in insertion order
};

// Appends `text` to `out` as a quoted JSON string with all special characters escaped.
void appendJsonString(std::string& out, const std::string& text);

}  // namespace cge
