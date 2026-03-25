#pragma once
// ============================================================================
// VLM client -- async cloud API integration for semantic enrichment
//
// Sends object reference crops to a Vision-Language Model API and receives
// rich descriptions, attributes, and a semantic interest signal.
//
// Runs asynchronously on a background thread; does not block the real-time
// perception pipeline. Results are delivered via callback.
// ============================================================================

#include "ai/object_memory.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace sc {

struct VlmConfig {
    std::string api_url;            // e.g. "https://api.openai.com/v1/chat/completions"
    std::string api_key;
    std::string model_name = "gpt-4o";
    int         timeout_sec = 30;
    int         max_queue   = 50;
    bool        enabled     = false;
};

struct VlmEnrichRequest {
    uint32_t object_id;
    std::string coarse_class;
    std::vector<std::vector<uint8_t>> crop_jpegs;  // 1-3 JPEG-encoded crops
    std::vector<std::pair<int,int>> crop_sizes;     // width, height for each crop
};

struct VlmEnrichResult {
    uint32_t object_id;
    std::string name;
    std::string description;
    std::vector<std::string> attributes;
    float base_interest = 0.0f;
    bool  success = false;
    std::string error;
};

using VlmResultCallback = std::function<void(const VlmEnrichResult&)>;

class VlmClient {
public:
    explicit VlmClient(const VlmConfig& cfg = {});
    ~VlmClient();

    VlmClient(const VlmClient&) = delete;
    VlmClient& operator=(const VlmClient&) = delete;

    void set_config(const VlmConfig& cfg);

    // Queue an enrichment request (non-blocking).
    void enqueue(const VlmEnrichRequest& req);

    // Set callback for completed results.
    void set_result_callback(VlmResultCallback cb);

    // Number of pending requests.
    int pending() const;

    void stop();

private:
    void worker_loop();
    VlmEnrichResult process_request(const VlmEnrichRequest& req);
    std::string build_prompt(const std::string& coarse_class);
    VlmEnrichResult parse_response(uint32_t object_id, const std::string& response);

    VlmConfig cfg_;
    VlmResultCallback result_cb_;

    std::thread worker_;
    std::atomic<bool> running_{false};
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::queue<VlmEnrichRequest> queue_;
};

}  // namespace sc
