#include "soullink/json.h"

#include <cctype>
#include <cstdlib>
#include <limits>
#include <sstream>

namespace sc::soullink {

namespace {

const JsonValue::Array kEmptyArray;
const JsonValue::Object kEmptyObject;
const std::string kEmptyString;

class Parser {
public:
    explicit Parser(const std::string& text) : text_(text) {}

    bool parse(JsonValue* out, std::string* error) {
        skip_ws();
        if (!parse_value(out, error)) return false;
        skip_ws();
        if (pos_ != text_.size()) {
            if (error) *error = "trailing characters after JSON value";
            return false;
        }
        return true;
    }

private:
    bool parse_value(JsonValue* out, std::string* error) {
        if (pos_ >= text_.size()) {
            if (error) *error = "unexpected end of input";
            return false;
        }
        char c = text_[pos_];
        if (c == '{') return parse_object(out, error);
        if (c == '[') return parse_array(out, error);
        if (c == '"') return parse_string(out, error);
        if (c == 't' || c == 'f' || c == 'n') return parse_literal(out, error);
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) return parse_number(out, error);
        if (error) {
            std::ostringstream oss;
            oss << "unexpected character '" << c << "'";
            *error = oss.str();
        }
        return false;
    }

    bool parse_object(JsonValue* out, std::string* error) {
        JsonValue::Object object;
        pos_++;  // {
        skip_ws();
        if (consume_if('}')) {
            *out = JsonValue(std::move(object));
            return true;
        }
        while (pos_ < text_.size()) {
            JsonValue key_v;
            if (!parse_string(&key_v, error)) return false;
            const std::string& key = key_v.as_string();
            skip_ws();
            if (!consume_if(':')) {
                if (error) *error = "expected ':' after object key";
                return false;
            }
            skip_ws();
            JsonValue val;
            if (!parse_value(&val, error)) return false;
            object[key] = std::move(val);
            skip_ws();
            if (consume_if('}')) {
                *out = JsonValue(std::move(object));
                return true;
            }
            if (!consume_if(',')) {
                if (error) *error = "expected ',' or '}' in object";
                return false;
            }
            skip_ws();
        }
        if (error) *error = "unterminated object";
        return false;
    }

    bool parse_array(JsonValue* out, std::string* error) {
        JsonValue::Array arr;
        pos_++;  // [
        skip_ws();
        if (consume_if(']')) {
            *out = JsonValue(std::move(arr));
            return true;
        }
        while (pos_ < text_.size()) {
            JsonValue val;
            if (!parse_value(&val, error)) return false;
            arr.push_back(std::move(val));
            skip_ws();
            if (consume_if(']')) {
                *out = JsonValue(std::move(arr));
                return true;
            }
            if (!consume_if(',')) {
                if (error) *error = "expected ',' or ']' in array";
                return false;
            }
            skip_ws();
        }
        if (error) *error = "unterminated array";
        return false;
    }

    bool parse_string(JsonValue* out, std::string* error) {
        if (!consume_if('"')) {
            if (error) *error = "expected string";
            return false;
        }
        std::string s;
        while (pos_ < text_.size()) {
            char c = text_[pos_++];
            if (c == '"') {
                *out = JsonValue(std::move(s));
                return true;
            }
            if (c == '\\') {
                if (pos_ >= text_.size()) {
                    if (error) *error = "bad string escape";
                    return false;
                }
                char e = text_[pos_++];
                switch (e) {
                    case '"': s.push_back('"'); break;
                    case '\\': s.push_back('\\'); break;
                    case '/': s.push_back('/'); break;
                    case 'b': s.push_back('\b'); break;
                    case 'f': s.push_back('\f'); break;
                    case 'n': s.push_back('\n'); break;
                    case 'r': s.push_back('\r'); break;
                    case 't': s.push_back('\t'); break;
                    case 'u':
                        // Keep parser small: consume 4 hex chars and emit '?'
                        for (int i = 0; i < 4; ++i) {
                            if (pos_ >= text_.size() ||
                                !std::isxdigit(static_cast<unsigned char>(text_[pos_]))) {
                                if (error) *error = "invalid \\u escape";
                                return false;
                            }
                            pos_++;
                        }
                        s.push_back('?');
                        break;
                    default:
                        if (error) *error = "invalid escape sequence";
                        return false;
                }
            } else {
                s.push_back(c);
            }
        }
        if (error) *error = "unterminated string";
        return false;
    }

    bool parse_literal(JsonValue* out, std::string* error) {
        if (match("true")) {
            *out = JsonValue(true);
            return true;
        }
        if (match("false")) {
            *out = JsonValue(false);
            return true;
        }
        if (match("null")) {
            *out = JsonValue(nullptr);
            return true;
        }
        if (error) *error = "invalid literal";
        return false;
    }

    bool parse_number(JsonValue* out, std::string* error) {
        size_t start = pos_;
        if (peek() == '-') pos_++;
        if (peek() == '0') {
            pos_++;
        } else if (std::isdigit(static_cast<unsigned char>(peek()))) {
            while (std::isdigit(static_cast<unsigned char>(peek()))) pos_++;
        } else {
            if (error) *error = "invalid number";
            return false;
        }
        if (peek() == '.') {
            pos_++;
            if (!std::isdigit(static_cast<unsigned char>(peek()))) {
                if (error) *error = "invalid fractional number";
                return false;
            }
            while (std::isdigit(static_cast<unsigned char>(peek()))) pos_++;
        }
        if (peek() == 'e' || peek() == 'E') {
            pos_++;
            if (peek() == '+' || peek() == '-') pos_++;
            if (!std::isdigit(static_cast<unsigned char>(peek()))) {
                if (error) *error = "invalid exponent";
                return false;
            }
            while (std::isdigit(static_cast<unsigned char>(peek()))) pos_++;
        }
        std::string token = text_.substr(start, pos_ - start);
        char* endp = nullptr;
        double v = std::strtod(token.c_str(), &endp);
        if (endp == nullptr || *endp != '\0') {
            if (error) *error = "failed to parse number";
            return false;
        }
        *out = JsonValue(v);
        return true;
    }

    void skip_ws() {
        while (pos_ < text_.size() &&
               std::isspace(static_cast<unsigned char>(text_[pos_]))) {
            pos_++;
        }
    }

    bool consume_if(char c) {
        if (pos_ < text_.size() && text_[pos_] == c) {
            pos_++;
            return true;
        }
        return false;
    }

    bool match(const char* kw) {
        size_t n = std::char_traits<char>::length(kw);
        if (text_.compare(pos_, n, kw) == 0) {
            pos_ += n;
            return true;
        }
        return false;
    }

    char peek() const {
        if (pos_ >= text_.size()) return '\0';
        return text_[pos_];
    }

    const std::string& text_;
    size_t pos_ = 0;
};

static void append_escaped(const std::string& input, std::string* out) {
    out->push_back('"');
    for (char c : input) {
        switch (c) {
            case '"': out->append("\\\""); break;
            case '\\': out->append("\\\\"); break;
            case '\b': out->append("\\b"); break;
            case '\f': out->append("\\f"); break;
            case '\n': out->append("\\n"); break;
            case '\r': out->append("\\r"); break;
            case '\t': out->append("\\t"); break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out->append("?");
                } else {
                    out->push_back(c);
                }
        }
    }
    out->push_back('"');
}

static void dump_json(const JsonValue& value, std::string* out) {
    if (value.is_null()) {
        out->append("null");
        return;
    }
    if (value.is_bool()) {
        out->append(value.as_bool() ? "true" : "false");
        return;
    }
    if (value.is_number()) {
        std::ostringstream oss;
        oss.precision(std::numeric_limits<double>::digits10 + 1);
        oss << value.as_number();
        out->append(oss.str());
        return;
    }
    if (value.is_string()) {
        append_escaped(value.as_string(), out);
        return;
    }
    if (value.is_array()) {
        out->push_back('[');
        const auto& arr = value.as_array();
        for (size_t i = 0; i < arr.size(); ++i) {
            if (i) out->push_back(',');
            dump_json(arr[i], out);
        }
        out->push_back(']');
        return;
    }
    out->push_back('{');
    bool first = true;
    for (const auto& [k, v] : value.as_object()) {
        if (!first) out->push_back(',');
        first = false;
        append_escaped(k, out);
        out->push_back(':');
        dump_json(v, out);
    }
    out->push_back('}');
}

}  // namespace

bool JsonValue::is_null() const { return std::holds_alternative<std::nullptr_t>(value_); }
bool JsonValue::is_bool() const { return std::holds_alternative<bool>(value_); }
bool JsonValue::is_number() const { return std::holds_alternative<double>(value_); }
bool JsonValue::is_string() const { return std::holds_alternative<std::string>(value_); }
bool JsonValue::is_array() const { return std::holds_alternative<Array>(value_); }
bool JsonValue::is_object() const { return std::holds_alternative<Object>(value_); }

bool JsonValue::as_bool(bool def) const {
    if (!is_bool()) return def;
    return std::get<bool>(value_);
}

double JsonValue::as_number(double def) const {
    if (!is_number()) return def;
    return std::get<double>(value_);
}

int JsonValue::as_int(int def) const {
    if (!is_number()) return def;
    return static_cast<int>(std::get<double>(value_));
}

const std::string& JsonValue::as_string() const {
    if (!is_string()) return kEmptyString;
    return std::get<std::string>(value_);
}

const JsonValue::Array& JsonValue::as_array() const {
    if (!is_array()) return kEmptyArray;
    return std::get<Array>(value_);
}

const JsonValue::Object& JsonValue::as_object() const {
    if (!is_object()) return kEmptyObject;
    return std::get<Object>(value_);
}

JsonValue::Array& JsonValue::as_array_mut() {
    if (!is_array()) value_ = Array{};
    return std::get<Array>(value_);
}

JsonValue::Object& JsonValue::as_object_mut() {
    if (!is_object()) value_ = Object{};
    return std::get<Object>(value_);
}

const JsonValue* JsonValue::get(const std::string& key) const {
    if (!is_object()) return nullptr;
    const auto& obj = std::get<Object>(value_);
    auto it = obj.find(key);
    if (it == obj.end()) return nullptr;
    return &it->second;
}

JsonValue* JsonValue::get_mut(const std::string& key) {
    if (!is_object()) return nullptr;
    auto& obj = std::get<Object>(value_);
    auto it = obj.find(key);
    if (it == obj.end()) return nullptr;
    return &it->second;
}

bool JsonValue::contains(const std::string& key) const {
    return get(key) != nullptr;
}

bool json_parse(const std::string& text, JsonValue* out, std::string* error) {
    if (out == nullptr) {
        if (error) *error = "json_parse output pointer is null";
        return false;
    }
    Parser parser(text);
    return parser.parse(out, error);
}

std::string json_dump(const JsonValue& value) {
    std::string out;
    out.reserve(256);
    dump_json(value, &out);
    return out;
}

}  // namespace sc::soullink

