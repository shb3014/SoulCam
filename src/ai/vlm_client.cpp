#include "ai/vlm_client.h"
#include "util/logger.h"

#include <cstdlib>
#include <cstring>
#include <sstream>

// libcurl is optional; if not available, VLM runs in stub mode.
// SOULCAM_HAVE_CURL is defined by CMake when libcurl is found.
#ifdef SOULCAM_HAVE_CURL
#include <curl/curl.h>
#endif

namespace sc {

VlmClient::VlmClient(const VlmConfig& cfg)
    : cfg_(cfg) {
    if (cfg_.enabled) {
        running_ = true;
        worker_ = std::thread(&VlmClient::worker_loop, this);
        SC_LOG_INFO("VlmClient: started (api=%s, model=%s)",
                    cfg_.api_url.c_str(), cfg_.model_name.c_str());
    }
}

VlmClient::~VlmClient() {
    stop();
}

void VlmClient::set_config(const VlmConfig& cfg) {
    std::lock_guard<std::mutex> lock(mtx_);
    cfg_ = cfg;
}

void VlmClient::stop() {
    if (running_) {
        running_ = false;
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }
}

void VlmClient::enqueue(const VlmEnrichRequest& req) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (static_cast<int>(queue_.size()) >= cfg_.max_queue) {
            SC_LOG_WARN("VlmClient: queue full, dropping request for object #%u",
                        req.object_id);
            return;
        }
        queue_.push(req);
    }
    cv_.notify_one();
}

void VlmClient::set_result_callback(VlmResultCallback cb) {
    std::lock_guard<std::mutex> lock(mtx_);
    result_cb_ = std::move(cb);
}

int VlmClient::pending() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return static_cast<int>(queue_.size());
}

void VlmClient::worker_loop() {
    while (running_) {
        VlmEnrichRequest req;
        {
            std::unique_lock<std::mutex> lock(mtx_);
            cv_.wait(lock, [this] { return !queue_.empty() || !running_; });
            if (!running_) break;
            req = std::move(queue_.front());
            queue_.pop();
        }

        VlmEnrichResult result = process_request(req);

        VlmResultCallback cb;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            cb = result_cb_;
        }
        if (cb) cb(result);
    }
}

std::string VlmClient::build_prompt(const std::string& coarse_class) {
    return "You are a visual perception system for a smart camera. "
           "The camera detected an object classified as \"" + coarse_class + "\". "
           "Analyze the provided image(s) and respond with JSON only. "
           "Fields: "
           "name (concise identifier, e.g. \"Gordon's white mug\"), "
           "description (1-2 sentences), "
           "attributes (array of visual properties like color, material, shape), "
           "distinguishing_features (array of features that distinguish this from "
           "similar objects), "
           "base_interest (float 0-1, how inherently noteworthy is this object "
           "in a home/office context -- 0 for mundane stationary items like a wall, "
           "0.5 for moderately interesting items like a plant, 1.0 for highly unusual "
           "or important items like a pet or an unexpected object).";
}

VlmEnrichResult VlmClient::parse_response(uint32_t object_id,
                                            const std::string& response) {
    VlmEnrichResult result;
    result.object_id = object_id;

    // Minimal JSON field extraction (avoid external JSON dependency)
    auto extract_string = [&](const std::string& key) -> std::string {
        std::string search = "\"" + key + "\"";
        auto pos = response.find(search);
        if (pos == std::string::npos) return "";
        pos = response.find("\"", pos + search.size() + 1);
        if (pos == std::string::npos) return "";
        pos++;
        auto end = response.find("\"", pos);
        if (end == std::string::npos) return "";
        return response.substr(pos, end - pos);
    };

    auto extract_float = [&](const std::string& key) -> float {
        std::string search = "\"" + key + "\"";
        auto pos = response.find(search);
        if (pos == std::string::npos) return 0.0f;
        pos = response.find(":", pos);
        if (pos == std::string::npos) return 0.0f;
        pos++;
        while (pos < response.size() && (response[pos] == ' ' || response[pos] == '\t')) pos++;
        return std::strtof(response.c_str() + pos, nullptr);
    };

    auto extract_string_array = [&](const std::string& key) -> std::vector<std::string> {
        std::vector<std::string> arr;
        std::string search = "\"" + key + "\"";
        auto pos = response.find(search);
        if (pos == std::string::npos) return arr;
        pos = response.find("[", pos);
        if (pos == std::string::npos) return arr;
        auto end = response.find("]", pos);
        if (end == std::string::npos) return arr;
        std::string segment = response.substr(pos + 1, end - pos - 1);
        // Extract quoted strings
        size_t p = 0;
        while (p < segment.size()) {
            auto q1 = segment.find("\"", p);
            if (q1 == std::string::npos) break;
            auto q2 = segment.find("\"", q1 + 1);
            if (q2 == std::string::npos) break;
            arr.push_back(segment.substr(q1 + 1, q2 - q1 - 1));
            p = q2 + 1;
        }
        return arr;
    };

    result.name = extract_string("name");
    result.description = extract_string("description");
    result.attributes = extract_string_array("attributes");
    result.base_interest = extract_float("base_interest");
    result.success = !result.name.empty();

    if (!result.success) {
        result.error = "Failed to parse VLM response";
    }

    return result;
}

VlmEnrichResult VlmClient::process_request(const VlmEnrichRequest& req) {
    VlmEnrichResult result;
    result.object_id = req.object_id;

#ifdef SOULCAM_HAVE_CURL
    if (cfg_.api_url.empty() || cfg_.api_key.empty()) {
        result.success = false;
        result.error = "VLM API URL or key not configured";
        return result;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        result.error = "curl_easy_init failed";
        return result;
    }

    std::string prompt = build_prompt(req.coarse_class);

    // Build JSON request body (text-only for now; image upload requires
    // base64 encoding which is a TODO for multi-modal API integration)
    std::string body = "{\"model\":\"" + cfg_.model_name + "\","
                       "\"messages\":[{\"role\":\"user\",\"content\":\"" +
                       prompt + "\"}],"
                       "\"max_tokens\":500}";

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    std::string auth_header = "Authorization: Bearer " + cfg_.api_key;
    headers = curl_slist_append(headers, auth_header.c_str());

    std::string response_body;
    auto write_cb = +[](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
        auto* s = static_cast<std::string*>(userdata);
        s->append(ptr, size * nmemb);
        return size * nmemb;
    };

    curl_easy_setopt(curl, CURLOPT_URL, cfg_.api_url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(cfg_.timeout_sec));

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        result.error = std::string("curl error: ") + curl_easy_strerror(res);
        SC_LOG_WARN("VlmClient: request failed for #%u: %s",
                    req.object_id, result.error.c_str());
    } else {
        // Extract content from the OpenAI-style response
        auto content_pos = response_body.find("\"content\"");
        if (content_pos != std::string::npos) {
            auto start = response_body.find("\"", content_pos + 10);
            if (start != std::string::npos) {
                start++;
                // Find the matching closing quote (handle escaped quotes)
                std::string content;
                bool escape = false;
                for (size_t i = start; i < response_body.size(); i++) {
                    if (escape) {
                        content += response_body[i];
                        escape = false;
                    } else if (response_body[i] == '\\') {
                        escape = true;
                    } else if (response_body[i] == '"') {
                        break;
                    } else {
                        content += response_body[i];
                    }
                }
                result = parse_response(req.object_id, content);
            }
        }
        if (!result.success && result.error.empty()) {
            result.error = "Could not extract content from API response";
        }
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
#else
    // Stub mode: generate basic labels from coarse class
    result.name = req.coarse_class + " #" + std::to_string(req.object_id);
    result.description = "A " + req.coarse_class + " detected by SoulCam.";
    result.attributes = {req.coarse_class};
    result.base_interest = 0.3f;
    result.success = true;
    SC_LOG_INFO("VlmClient: stub enrichment for #%u -> \"%s\"",
                req.object_id, result.name.c_str());
#endif

    return result;
}

}  // namespace sc
