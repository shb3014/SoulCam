#pragma once
// ============================================================================
// SoulCam Store -- Zustand-inspired state management (adapted from Ivy)
//
// Provides typed DP (data-point) storage with:
//   - get<T>(key) / set(key, value) with event notification
//   - JSON file persistence for PERSIST keys
//   - Type-safe setFromJson() that casts incoming values to match the stored type
//   - Thread-safe (mutex-protected)
// ============================================================================

#include "store/store_config.h"

#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace sc {

class Store {
public:
    using ChangeCallback = std::function<void(int key, const StateType& oldVal, const StateType& newVal)>;

    static Store& instance();

    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    void initialize(const std::string& persist_path = "/var/lib/soulcam/store.json");

    // --- get / set -----------------------------------------------------------

    bool set(int key, const StateType& value, bool notify = true);

    /// Type-safe set from a double/bool/string coming from JSON wire protocol.
    /// Casts the incoming value to match the existing type in the cache
    /// (mirrors Ivy's setSecurity pattern).
    bool setFromJson(int key, double json_number);
    bool setFromJson(int key, bool json_bool);
    bool setFromJson(int key, const std::string& json_string);

    template<typename T>
    T get(int key) const {
        std::lock_guard<std::mutex> lk(mu_);
        if (key < 0 || key >= static_cast<int>(cache_.size()))
            return T{};
        T result{};
        std::visit([&result](auto&& arg) {
            using V = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<V, std::string>) {
                if constexpr (std::is_same_v<T, std::string>) result = arg;
            } else {
                if constexpr (!std::is_same_v<T, std::string>) result = static_cast<T>(arg);
            }
        }, cache_[key]);
        return result;
    }

    StateType getRaw(int key) const;

    // --- iteration -----------------------------------------------------------

    void forEachDp(const std::function<void(int key, const StateType& val)>& fn) const;
    int  dpCount() const;

    // --- persistence ---------------------------------------------------------

    bool save();
    bool load();

    // --- change listeners ----------------------------------------------------

    void addChangeListener(ChangeCallback cb);

    // --- key info ------------------------------------------------------------

    static bool isPersist(int key) { return SoulCamDp::isPersist(key); }
    static bool isValid(int key)   { return SoulCamDp::isValid(key); }

private:
    Store() = default;
    void applyDefaults();
    void notifyListeners(int key, const StateType& oldVal, const StateType& newVal);

    mutable std::mutex mu_;
    std::vector<StateType> cache_;
    std::vector<ChangeCallback> listeners_;
    std::string persist_path_;
    bool initialized_ = false;
};

}  // namespace sc
