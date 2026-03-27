#include "ai/object_memory.h"
#include "util/logger.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

namespace sc {

// Minimal JSON serializer for ObjectRecord (avoids external dependency)
namespace {

std::string escape_json(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            default:   out += c;
        }
    }
    return out;
}

std::string vec_to_json(const std::vector<float>& v) {
    std::string s = "[";
    for (size_t i = 0; i < v.size(); i++) {
        if (i) s += ",";
        char buf[32];
        snprintf(buf, sizeof(buf), "%.6f", v[i]);
        s += buf;
    }
    s += "]";
    return s;
}

std::string strvec_to_json(const std::vector<std::string>& v) {
    std::string s = "[";
    for (size_t i = 0; i < v.size(); i++) {
        if (i) s += ",";
        s += "\"" + escape_json(v[i]) + "\"";
    }
    s += "]";
    return s;
}

void mkdirs(const std::string& path) {
    std::string dir;
    for (char c : path) {
        dir += c;
        if (c == '/') {
            mkdir(dir.c_str(), 0755);
        }
    }
    mkdir(path.c_str(), 0755);
}

}  // namespace

ObjectMemory::ObjectMemory(const MemoryConfig& cfg)
    : cfg_(cfg) {
    mkdirs(cfg_.storage_dir);
}

ObjectMemory::~ObjectMemory() {
    save();
}

void ObjectMemory::set_config(const MemoryConfig& cfg) {
    std::lock_guard<std::mutex> lock(mtx_);
    cfg_ = cfg;
    mkdirs(cfg_.storage_dir);
}

MatchResult ObjectMemory::match(const std::vector<float>& query_embedding,
                                 const std::string& coarse_class) const {
    std::lock_guard<std::mutex> lock(mtx_);

    MatchResult best;

    for (const auto& [id, obj] : objects_) {
        if (coarse_class != "object" && obj.coarse_class != coarse_class) continue;

        float best_exemplar_sim = 0.0f;
        for (const auto& ex : obj.exemplars) {
            float sim = cosine_similarity(query_embedding, ex);
            best_exemplar_sim = std::max(best_exemplar_sim, sim);
        }

        float centroid_sim = 0.0f;
        if (!obj.centroid.empty()) {
            centroid_sim = cosine_similarity(query_embedding, obj.centroid);
        }

        float score = 0.7f * best_exemplar_sim + 0.3f * centroid_sim;

        if (score > best.score) {
            best.object_id = id;
            best.score = score;
        }
    }

    if (best.score > cfg_.match_confident) {
        best.confidence = MatchResult::Confidence::Confident;
    } else if (best.score > cfg_.match_uncertain) {
        best.confidence = MatchResult::Confidence::Uncertain;
    } else {
        best.confidence = MatchResult::Confidence::None;
        best.object_id = -1;
    }

    return best;
}

uint32_t ObjectMemory::enroll(const std::string& coarse_class,
                               const std::vector<std::vector<float>>& exemplar_embeddings,
                               uint64_t timestamp_ms) {
    std::lock_guard<std::mutex> lock(mtx_);

    ObjectRecord obj;
    obj.object_id = next_object_id_++;
    obj.coarse_class = coarse_class;
    obj.exemplars = exemplar_embeddings;
    obj.total_embedding_count = static_cast<int>(exemplar_embeddings.size());
    obj.first_seen_ts = timestamp_ms;
    obj.last_seen_ts = timestamp_ms;
    obj.seen_count = 1;
    obj.session_count = 1;
    obj.name = coarse_class + " #" + std::to_string(obj.object_id);

    // Compute centroid
    if (!exemplar_embeddings.empty()) {
        int dim = static_cast<int>(exemplar_embeddings[0].size());
        obj.centroid.resize(dim, 0.0f);
        for (const auto& e : exemplar_embeddings) {
            for (int i = 0; i < dim; i++) obj.centroid[i] += e[i];
        }
        float inv_n = 1.0f / exemplar_embeddings.size();
        for (int i = 0; i < dim; i++) obj.centroid[i] *= inv_n;
        l2_normalize(obj.centroid);
    }

    uint32_t oid = obj.object_id;
    emit_event("enrolled", obj);
    objects_[oid] = std::move(obj);

    SC_LOG_INFO("ObjectMemory: enrolled object #%u class=%s (%zu exemplars)",
                oid, coarse_class.c_str(), exemplar_embeddings.size());

    return oid;
}

void ObjectMemory::observe(uint32_t object_id,
                            const std::vector<float>& embedding,
                            uint64_t timestamp_ms) {
    std::lock_guard<std::mutex> lock(mtx_);

    auto it = objects_.find(object_id);
    if (it == objects_.end()) return;
    auto& obj = it->second;

    obj.seen_count++;
    obj.last_seen_ts = timestamp_ms;

    // Update centroid as weighted running average
    if (!obj.centroid.empty() && embedding.size() == obj.centroid.size()) {
        float old_weight = static_cast<float>(obj.total_embedding_count);
        float new_weight = 1.0f;
        float total = old_weight + new_weight;
        for (size_t i = 0; i < obj.centroid.size(); i++) {
            obj.centroid[i] = (obj.centroid[i] * old_weight + embedding[i] * new_weight) / total;
        }
        l2_normalize(obj.centroid);
        obj.total_embedding_count++;
    }

    // Add as new exemplar if viewpoint is novel
    float max_sim = 0.0f;
    for (const auto& ex : obj.exemplars) {
        max_sim = std::max(max_sim, cosine_similarity(embedding, ex));
    }

    if (max_sim < cfg_.exemplar_novelty) {
        if (static_cast<int>(obj.exemplars.size()) < ObjectRecord::kMaxExemplars) {
            obj.exemplars.push_back(embedding);
        } else {
            // Replace the exemplar most similar to another (least useful)
            float worst_redundancy = 0.0f;
            int worst_idx = 0;
            for (int i = 0; i < static_cast<int>(obj.exemplars.size()); i++) {
                float best_other = 0.0f;
                for (int j = 0; j < static_cast<int>(obj.exemplars.size()); j++) {
                    if (i == j) continue;
                    best_other = std::max(best_other,
                        cosine_similarity(obj.exemplars[i], obj.exemplars[j]));
                }
                if (best_other > worst_redundancy) {
                    worst_redundancy = best_other;
                    worst_idx = i;
                }
            }
            obj.exemplars[worst_idx] = embedding;
        }
    }
}

void ObjectMemory::enrich(uint32_t object_id,
                           const std::string& name,
                           const std::string& description,
                           const std::vector<std::string>& attributes,
                           float base_interest) {
    std::lock_guard<std::mutex> lock(mtx_);

    auto it = objects_.find(object_id);
    if (it == objects_.end()) return;
    auto& obj = it->second;

    obj.name = name;
    obj.description = description;
    obj.attributes = attributes;
    obj.base_interest = base_interest;

    SC_LOG_INFO("ObjectMemory: enriched #%u -> \"%s\"", object_id, name.c_str());
    emit_event("enriched", obj);
}

const ObjectRecord* ObjectMemory::get(uint32_t object_id) const {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = objects_.find(object_id);
    return (it != objects_.end()) ? &it->second : nullptr;
}

ObjectRecord* ObjectMemory::get_mut(uint32_t object_id) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = objects_.find(object_id);
    return (it != objects_.end()) ? &it->second : nullptr;
}

void ObjectMemory::save() const {
    std::lock_guard<std::mutex> lock(mtx_);

    std::string path = cfg_.storage_dir + "/memory.json";
    std::ofstream out(path);
    if (!out.is_open()) {
        SC_LOG_ERROR("ObjectMemory: cannot save to %s", path.c_str());
        return;
    }

    out << "{\n\"next_id\":" << next_object_id_ << ",\n\"objects\":[\n";

    bool first = true;
    for (const auto& [id, obj] : objects_) {
        if (!first) out << ",\n";
        first = false;

        out << "{\"id\":" << obj.object_id
            << ",\"class\":\"" << escape_json(obj.coarse_class) << "\""
            << ",\"name\":\"" << escape_json(obj.name) << "\""
            << ",\"desc\":\"" << escape_json(obj.description) << "\""
            << ",\"attrs\":" << strvec_to_json(obj.attributes)
            << ",\"tags\":" << strvec_to_json(obj.tags)
            << ",\"first_seen\":" << obj.first_seen_ts
            << ",\"last_seen\":" << obj.last_seen_ts
            << ",\"seen_count\":" << obj.seen_count
            << ",\"session_count\":" << obj.session_count
            << ",\"base_interest\":" << obj.base_interest
            << ",\"embed_count\":" << obj.total_embedding_count
            << ",\"centroid\":" << vec_to_json(obj.centroid);

        out << ",\"exemplars\":[";
        for (size_t i = 0; i < obj.exemplars.size(); i++) {
            if (i) out << ",";
            out << vec_to_json(obj.exemplars[i]);
        }
        out << "]";

        out << ",\"crops\":" << strvec_to_json(obj.crop_paths);
        out << "}";
    }

    out << "\n]}\n";
    out.close();

    SC_LOG_INFO("ObjectMemory: saved %zu objects to %s",
                objects_.size(), path.c_str());
}

void ObjectMemory::load() {
    std::lock_guard<std::mutex> lock(mtx_);

    std::string path = cfg_.storage_dir + "/memory.json";
    std::ifstream in(path);
    if (!in.is_open()) {
        SC_LOG_INFO("ObjectMemory: no saved memory at %s (starting fresh)", path.c_str());
        return;
    }

    // Parse with minimal JSON reader (the file is machine-generated)
    std::stringstream ss;
    ss << in.rdbuf();
    std::string content = ss.str();
    in.close();

    // Extract next_id
    auto nid_pos = content.find("\"next_id\":");
    if (nid_pos != std::string::npos) {
        next_object_id_ = std::stoul(content.substr(nid_pos + 10));
    }

    // For robust loading, we'd need a proper JSON parser.
    // For now, log that we found the file and note the limitation.
    SC_LOG_INFO("ObjectMemory: found saved memory at %s (full JSON parsing TBD, %zu bytes)",
                path.c_str(), content.size());

    // TODO: Full JSON parser for loading. The save format is well-defined;
    // loading needs a lightweight JSON parser (e.g., nlohmann/json or a
    // minimal custom parser). For now, the system starts fresh each session
    // and saves at shutdown for inspection.
}

int ObjectMemory::total_objects() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return static_cast<int>(objects_.size());
}

void ObjectMemory::set_event_callback(ObjectEventCallback cb) {
    std::lock_guard<std::mutex> lock(mtx_);
    event_cb_ = std::move(cb);
}

void ObjectMemory::save_crops(uint32_t object_id,
    const std::vector<std::pair<std::vector<uint8_t>, std::pair<int,int>>>& crops) {
    std::lock_guard<std::mutex> lock(mtx_);

    auto it = objects_.find(object_id);
    if (it == objects_.end()) return;
    auto& obj = it->second;

    std::string crop_dir = cfg_.storage_dir + "/crops/" + std::to_string(object_id);
    mkdirs(crop_dir);

    for (size_t i = 0; i < crops.size(); i++) {
        std::string fname = crop_dir + "/view_" + std::to_string(i) + ".rgb";
        std::ofstream f(fname, std::ios::binary);
        if (f.is_open()) {
            f.write(reinterpret_cast<const char*>(crops[i].first.data()),
                    crops[i].first.size());
            obj.crop_paths.push_back(fname);
        }
    }
}

void ObjectMemory::emit_event(const std::string& type, const ObjectRecord& obj) const {
    if (event_cb_) event_cb_(type, obj);
}

}  // namespace sc
