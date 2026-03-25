#pragma once
// ============================================================================
// Object Memory Bank -- persistent, unbounded object recognition memory
//
// Stores ObjectRecords with visual embeddings (identity), semantic labels
// (from VLM), and temporal metadata. Supports matching queries against the
// full memory bank partitioned by coarse class for efficiency.
//
// Tiered storage:
//   Hot tier (RAM): recently seen objects, loaded at boot
//   Cold tier (disk): older objects, loaded on-demand per class
//
// The memory bank grows without hard cap; only disk space limits it.
// ============================================================================

#include "ai/embedder.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace sc {

struct ObjectRecord {
    uint32_t object_id = 0;
    std::string coarse_class;

    // Visual identity
    std::vector<float> centroid;
    std::vector<std::vector<float>> exemplars;
    int total_embedding_count = 0;

    // Semantic (populated by VLM)
    std::string name;
    std::string description;
    std::vector<std::string> attributes;
    std::vector<std::string> tags;

    // Temporal
    uint64_t first_seen_ts = 0;
    uint64_t last_seen_ts  = 0;
    uint32_t seen_count    = 0;
    uint32_t session_count = 0;

    float base_interest = 0.0f;

    std::vector<std::string> crop_paths;

    static constexpr int kMaxExemplars = 10;
};

struct MatchResult {
    int       object_id = -1;
    float     score     = 0.0f;
    enum class Confidence { None, Uncertain, Confident };
    Confidence confidence = Confidence::None;
};

struct MemoryConfig {
    std::string storage_dir     = "/var/lib/soulcam/memory";
    int         hot_tier_max    = 1000;
    int         cold_demote_days = 30;

    float       match_confident  = 0.80f;
    float       match_uncertain  = 0.65f;
    float       exemplar_novelty = 0.85f;
    float       merge_threshold  = 0.90f;
};

using ObjectEventCallback = std::function<void(const std::string& event_type,
                                                const ObjectRecord& obj)>;

class ObjectMemory {
public:
    explicit ObjectMemory(const MemoryConfig& cfg = {});
    ~ObjectMemory();

    void set_config(const MemoryConfig& cfg);

    // Match a query embedding against all objects of the given coarse class.
    MatchResult match(const std::vector<float>& query_embedding,
                      const std::string& coarse_class) const;

    // Enroll a new object. Returns the assigned object_id.
    uint32_t enroll(const std::string& coarse_class,
                    const std::vector<std::vector<float>>& exemplar_embeddings,
                    uint64_t timestamp_ms);

    // Update a known object with a new embedding observation.
    void observe(uint32_t object_id,
                 const std::vector<float>& embedding,
                 uint64_t timestamp_ms);

    // Update semantic info from VLM.
    void enrich(uint32_t object_id,
                const std::string& name,
                const std::string& description,
                const std::vector<std::string>& attributes,
                float base_interest);

    // Get an object record (nullptr if not found).
    const ObjectRecord* get(uint32_t object_id) const;
    ObjectRecord* get_mut(uint32_t object_id);

    // Save memory to disk.
    void save() const;

    // Load memory from disk.
    void load();

    // Total number of objects (hot + cold).
    int total_objects() const;

    // Set event callback (enrollment, enrichment, recognition events).
    void set_event_callback(ObjectEventCallback cb);

    // Get all objects for iteration.
    const std::unordered_map<uint32_t, ObjectRecord>& objects() const { return objects_; }

    // Save reference crop images to disk for an object.
    void save_crops(uint32_t object_id,
                    const std::vector<std::pair<std::vector<uint8_t>, std::pair<int,int>>>& crops);

private:
    void emit_event(const std::string& type, const ObjectRecord& obj) const;

    MemoryConfig cfg_;
    std::unordered_map<uint32_t, ObjectRecord> objects_;
    uint32_t next_object_id_ = 1;
    mutable std::mutex mtx_;
    ObjectEventCallback event_cb_;
};

}  // namespace sc
