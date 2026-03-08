#pragma once

#include "soulcam.h"
#include "soullink/json.h"

#include <functional>
#include <string>

namespace sc::soullink {

struct SyncResult {
    bool ok = false;
    std::string commit_id;
    std::string message;
};

class SyncEngine {
public:
    explicit SyncEngine(const sc::SoullinkConfig& cfg);

    SyncResult execute(
        const JsonValue& data,
        const std::function<void(const std::string& stage, const std::string& details)>& progress_cb);

private:
    std::string load_commit_id() const;
    bool store_commit_id(const std::string& commit_id) const;

    const sc::SoullinkConfig cfg_;
};

}  // namespace sc::soullink

