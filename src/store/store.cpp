#include "store/store.h"
#include "util/logger.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

namespace sc {

// ============================================================================
// Singleton
// ============================================================================

Store& Store::instance() {
    static Store inst;
    return inst;
}

// ============================================================================
// Initialization
// ============================================================================

void Store::initialize(const std::string& persist_path) {
    std::lock_guard<std::mutex> lk(mu_);
    if (initialized_) return;

    persist_path_ = persist_path;
    cache_.resize(SoulCamDp::END, StateType{(uint32_t)0});

    applyDefaults();

    if (!persist_path_.empty()) {
        // Ensure parent directory exists
        auto last_slash = persist_path_.rfind('/');
        if (last_slash != std::string::npos) {
            std::string dir = persist_path_.substr(0, last_slash);
            mkdir(dir.c_str(), 0755);
        }
    }

    initialized_ = true;
    SC_LOG_INFO("Store initialized (%d cache slots, persist=%s)",
                (int)cache_.size(), persist_path_.c_str());
}

void Store::applyDefaults() {
    auto defaults = SoulCamDp::getDefaultValueMap();
    for (const auto& [key, value] : defaults) {
        if (key >= 0 && key < static_cast<int>(cache_.size())) {
            cache_[key] = value;
        }
    }
}

// ============================================================================
// get / set
// ============================================================================

bool Store::set(int key, const StateType& value, bool notify) {
    if (key < 0 || key >= static_cast<int>(cache_.size())) return false;

    StateType old_value;
    {
        std::lock_guard<std::mutex> lk(mu_);
        old_value = cache_[key];
        if (old_value == value) return true;
        cache_[key] = value;
    }

    if (notify) notifyListeners(key, old_value, value);
    return true;
}

bool Store::setFromJson(int key, double json_number) {
    if (key < 0 || key >= static_cast<int>(cache_.size())) return false;

    StateType converted;
    {
        std::lock_guard<std::mutex> lk(mu_);
        const auto& current = cache_[key];
        std::visit([&converted, json_number](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, uint32_t>)
                converted = static_cast<uint32_t>(json_number);
            else if constexpr (std::is_same_v<T, int>)
                converted = static_cast<int>(json_number);
            else if constexpr (std::is_same_v<T, float>)
                converted = static_cast<float>(json_number);
            else if constexpr (std::is_same_v<T, bool>)
                converted = json_number != 0.0;
            else
                converted = std::to_string(json_number);
        }, current);
    }
    return set(key, converted);
}

bool Store::setFromJson(int key, bool json_bool) {
    if (key < 0 || key >= static_cast<int>(cache_.size())) return false;

    StateType converted;
    {
        std::lock_guard<std::mutex> lk(mu_);
        const auto& current = cache_[key];
        std::visit([&converted, json_bool](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, bool>)
                converted = json_bool;
            else if constexpr (std::is_same_v<T, uint32_t>)
                converted = static_cast<uint32_t>(json_bool ? 1 : 0);
            else if constexpr (std::is_same_v<T, int>)
                converted = json_bool ? 1 : 0;
            else if constexpr (std::is_same_v<T, float>)
                converted = json_bool ? 1.0f : 0.0f;
            else
                converted = std::string(json_bool ? "true" : "false");
        }, current);
    }
    return set(key, converted);
}

bool Store::setFromJson(int key, const std::string& json_string) {
    if (key < 0 || key >= static_cast<int>(cache_.size())) return false;

    StateType converted;
    {
        std::lock_guard<std::mutex> lk(mu_);
        const auto& current = cache_[key];
        std::visit([&converted, &json_string](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::string>) {
                converted = json_string;
            } else if constexpr (std::is_same_v<T, uint32_t>) {
                try { converted = static_cast<uint32_t>(std::stoul(json_string)); }
                catch (...) { converted = (uint32_t)0; }
            } else if constexpr (std::is_same_v<T, int>) {
                try { converted = std::stoi(json_string); }
                catch (...) { converted = 0; }
            } else if constexpr (std::is_same_v<T, float>) {
                try { converted = std::stof(json_string); }
                catch (...) { converted = 0.0f; }
            } else if constexpr (std::is_same_v<T, bool>) {
                converted = (json_string == "true" || json_string == "1");
            }
        }, current);
    }
    return set(key, converted);
}

StateType Store::getRaw(int key) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (key < 0 || key >= static_cast<int>(cache_.size()))
        return StateType{(uint32_t)0};
    return cache_[key];
}

// ============================================================================
// Iteration
// ============================================================================

void Store::forEachDp(const std::function<void(int key, const StateType& val)>& fn) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto defaults = SoulCamDp::getDefaultValueMap();
    for (const auto& [key, _] : defaults) {
        if (key >= 0 && key < static_cast<int>(cache_.size())) {
            fn(key, cache_[key]);
        }
    }
}

int Store::dpCount() const {
    return static_cast<int>(SoulCamDp::getDefaultValueMap().size());
}

// ============================================================================
// Change listeners
// ============================================================================

void Store::addChangeListener(ChangeCallback cb) {
    std::lock_guard<std::mutex> lk(mu_);
    listeners_.push_back(std::move(cb));
}

void Store::notifyListeners(int key, const StateType& oldVal, const StateType& newVal) {
    std::vector<ChangeCallback> cbs;
    {
        std::lock_guard<std::mutex> lk(mu_);
        cbs = listeners_;
    }
    for (const auto& cb : cbs) {
        cb(key, oldVal, newVal);
    }
}

// ============================================================================
// JSON persistence  (minimal hand-written JSON to avoid extra dependencies)
// ============================================================================

namespace {

/// Encode a single StateType value to a JSON fragment
std::string state_to_json_value(const StateType& v) {
    return std::visit([](auto&& arg) -> std::string {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, bool>) {
            return arg ? "true" : "false";
        } else if constexpr (std::is_same_v<T, uint32_t>) {
            return std::to_string(arg);
        } else if constexpr (std::is_same_v<T, int>) {
            return std::to_string(arg);
        } else if constexpr (std::is_same_v<T, float>) {
            std::ostringstream oss;
            oss << arg;
            return oss.str();
        } else if constexpr (std::is_same_v<T, std::string>) {
            std::string out = "\"";
            for (char c : arg) {
                if (c == '"') out += "\\\"";
                else if (c == '\\') out += "\\\\";
                else if (c == '\n') out += "\\n";
                else out += c;
            }
            out += '"';
            return out;
        }
        return "null";
    }, v);
}

/// Minimal JSON string parser: advance past opening '"', return content up to
/// closing '"', handle basic escapes.  pos should point at the opening quote.
std::string parse_json_string(const std::string& text, size_t& pos) {
    if (pos >= text.size() || text[pos] != '"') return {};
    pos++;  // skip opening "
    std::string out;
    while (pos < text.size() && text[pos] != '"') {
        if (text[pos] == '\\' && pos + 1 < text.size()) {
            pos++;
            if (text[pos] == 'n') out += '\n';
            else if (text[pos] == 't') out += '\t';
            else out += text[pos];
        } else {
            out += text[pos];
        }
        pos++;
    }
    if (pos < text.size()) pos++;  // skip closing "
    return out;
}

void skip_whitespace(const std::string& text, size_t& pos) {
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' ||
           text[pos] == '\n' || text[pos] == '\r')) pos++;
}

/// Parse a JSON value fragment at pos; return the raw string content and
/// advance pos.  For strings, returns unquoted content.
/// type_hint: 's'=string, 'n'=number, 'b'=bool, '?'=unknown
std::string parse_json_fragment(const std::string& text, size_t& pos, char& type_hint) {
    skip_whitespace(text, pos);
    if (pos >= text.size()) { type_hint = '?'; return {}; }

    if (text[pos] == '"') {
        type_hint = 's';
        return parse_json_string(text, pos);
    }
    if (text[pos] == 't' || text[pos] == 'f') {
        type_hint = 'b';
        size_t start = pos;
        while (pos < text.size() && std::isalpha(static_cast<unsigned char>(text[pos]))) pos++;
        return text.substr(start, pos - start);
    }
    // number
    type_hint = 'n';
    size_t start = pos;
    if (text[pos] == '-') pos++;
    while (pos < text.size() && (std::isdigit(static_cast<unsigned char>(text[pos])) ||
           text[pos] == '.' || text[pos] == 'e' || text[pos] == 'E' ||
           text[pos] == '+' || text[pos] == '-')) pos++;
    return text.substr(start, pos - start);
}

}  // namespace

bool Store::save() {
    if (persist_path_.empty()) return false;

    auto keymap = SoulCamDp::getPersistKeyMap();
    std::ostringstream oss;
    oss << "{\n";
    bool first = true;

    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& [name, key] : keymap) {
        if (key < 0 || key >= static_cast<int>(cache_.size())) continue;
        if (!first) oss << ",\n";
        first = false;
        oss << "  \"" << name << "\": " << state_to_json_value(cache_[key]);
    }
    oss << "\n}\n";

    std::string tmp = persist_path_ + ".tmp";
    std::ofstream out(tmp, std::ios::trunc);
    if (!out.good()) {
        SC_LOG_WARN("Store save: cannot open %s", tmp.c_str());
        return false;
    }
    out << oss.str();
    out.close();
    if (rename(tmp.c_str(), persist_path_.c_str()) != 0) {
        SC_LOG_WARN("Store save: rename failed");
        return false;
    }
    SC_LOG_INFO("Store saved to %s", persist_path_.c_str());
    return true;
}

bool Store::load() {
    if (persist_path_.empty()) return false;

    std::ifstream in(persist_path_);
    if (!in.good()) {
        SC_LOG_INFO("Store load: no persist file at %s (using defaults)", persist_path_.c_str());
        return false;
    }
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    in.close();

    auto keymap = SoulCamDp::getPersistKeyMap();
    int loaded = 0;

    // Minimal JSON object parser: expects {"key": value, ...}
    size_t pos = 0;
    skip_whitespace(content, pos);
    if (pos >= content.size() || content[pos] != '{') {
        SC_LOG_WARN("Store load: invalid JSON");
        return false;
    }
    pos++;  // skip '{'

    while (pos < content.size()) {
        skip_whitespace(content, pos);
        if (pos >= content.size() || content[pos] == '}') break;
        if (content[pos] == ',') { pos++; continue; }

        std::string key_str = parse_json_string(content, pos);
        skip_whitespace(content, pos);
        if (pos < content.size() && content[pos] == ':') pos++;

        char type_hint = '?';
        std::string val_str = parse_json_fragment(content, pos, type_hint);

        auto it = keymap.find(key_str);
        if (it == keymap.end()) continue;
        int dp = it->second;
        if (dp < 0 || dp >= static_cast<int>(cache_.size())) continue;

        // Set value with type matching against the existing default type
        std::lock_guard<std::mutex> lk(mu_);
        std::visit([&](auto&& current) {
            using T = std::decay_t<decltype(current)>;
            if constexpr (std::is_same_v<T, bool>) {
                cache_[dp] = (val_str == "true" || val_str == "1");
            } else if constexpr (std::is_same_v<T, uint32_t>) {
                try { cache_[dp] = static_cast<uint32_t>(std::stoul(val_str)); } catch (...) {}
            } else if constexpr (std::is_same_v<T, int>) {
                try { cache_[dp] = std::stoi(val_str); } catch (...) {}
            } else if constexpr (std::is_same_v<T, float>) {
                try { cache_[dp] = std::stof(val_str); } catch (...) {}
            } else if constexpr (std::is_same_v<T, std::string>) {
                cache_[dp] = val_str;
            }
        }, cache_[dp]);
        loaded++;
    }

    SC_LOG_INFO("Store loaded %d values from %s", loaded, persist_path_.c_str());
    return true;
}

}  // namespace sc
