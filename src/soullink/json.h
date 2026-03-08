#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace sc::soullink {

class JsonValue {
public:
    using Array = std::vector<JsonValue>;
    using Object = std::unordered_map<std::string, JsonValue>;

    JsonValue() = default;
    explicit JsonValue(std::nullptr_t) : value_(nullptr) {}
    explicit JsonValue(bool v) : value_(v) {}
    explicit JsonValue(double v) : value_(v) {}
    explicit JsonValue(int64_t v) : value_(static_cast<double>(v)) {}
    explicit JsonValue(std::string v) : value_(std::move(v)) {}
    explicit JsonValue(const char* v) : value_(std::string(v)) {}
    explicit JsonValue(Array v) : value_(std::move(v)) {}
    explicit JsonValue(Object v) : value_(std::move(v)) {}

    bool is_null() const;
    bool is_bool() const;
    bool is_number() const;
    bool is_string() const;
    bool is_array() const;
    bool is_object() const;

    bool as_bool(bool def = false) const;
    double as_number(double def = 0.0) const;
    int as_int(int def = 0) const;
    const std::string& as_string() const;
    const Array& as_array() const;
    const Object& as_object() const;

    Array& as_array_mut();
    Object& as_object_mut();

    const JsonValue* get(const std::string& key) const;
    JsonValue* get_mut(const std::string& key);
    bool contains(const std::string& key) const;

private:
    using Variant = std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;
    Variant value_{nullptr};
};

bool json_parse(const std::string& text, JsonValue* out, std::string* error);
std::string json_dump(const JsonValue& value);

}  // namespace sc::soullink

