#include "ai/perception_engine.h"
#include "util/logger.h"

#include <chrono>

namespace sc {

static uint64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

PerceptionEngine::PerceptionEngine(const PerceptionConfig& cfg)
    : cfg_(cfg) {

    associator_    = new MultiObjectAssociator(cfg_.associator);
    tracker_pool_  = new TrackerPool(cfg_.max_tracked_objects, cfg_.tracker);
    crop_extractor_ = new CropExtractor(cfg_.crop);
    memory_        = new ObjectMemory(cfg_.memory);
    interest_      = new InterestScorer(cfg_.interest);

    // Embedder (may be stub if no model path)
    embedder_ = embedder_create(cfg_.embedder);
    embed_queue_ = new EmbeddingQueue(embedder_);

    // VLM client
    vlm_ = new VlmClient(cfg_.vlm);
    vlm_->set_result_callback([this](const VlmEnrichResult& result) {
        if (result.success) {
            memory_->enrich(result.object_id,
                           result.name, result.description,
                           result.attributes, result.base_interest);
        } else {
            SC_LOG_WARN("VLM enrichment failed for #%u: %s",
                        result.object_id, result.error.c_str());
        }
    });

    // Load saved memory
    memory_->load();

    SC_LOG_INFO("PerceptionEngine: initialized (max_tracked=%d, memory=%d objects)",
                cfg_.max_tracked_objects, memory_->total_objects());
}

PerceptionEngine::~PerceptionEngine() {
    memory_->save();

    delete vlm_;
    delete interest_;
    delete memory_;
    delete embed_queue_;
    embedder_destroy(embedder_);
    delete crop_extractor_;
    delete tracker_pool_;
    delete associator_;
}

PerceptionFrame PerceptionEngine::process_yolo_frame(
    const uint8_t* rgb, const uint8_t* gray,
    int img_w, int img_h,
    const std::vector<Detection>& detections,
    float dt) {

    now_ms_ = now_ms();

    // 1. Associate detections to tracks
    associator_->update(detections, now_ms_);

    // 2. Extract crops for all matched tracks
    crop_extractor_->extract(rgb, img_w, img_h, detections,
                             associator_->tracks_mut(), now_ms_);

    // 3. Recognize all tracks against memory bank
    for (auto& track : associator_->tracks_mut()) {
        if (track.embedding.empty()) continue;
        if (track.memory_object_id >= 0) {
            // Already recognized: update observation
            memory_->observe(static_cast<uint32_t>(track.memory_object_id),
                            track.embedding, now_ms_);
            continue;
        }

        MatchResult match = memory_->match(track.embedding, track.coarse_label);
        if (match.confidence == MatchResult::Confidence::Confident) {
            track.memory_object_id = match.object_id;
            track.last_match_similarity = match.score;
            memory_->observe(static_cast<uint32_t>(match.object_id),
                            track.embedding, now_ms_);
        } else if (match.confidence == MatchResult::Confidence::Uncertain) {
            track.last_match_similarity = match.score;
            // Keep trying with more views
        }
    }

    // 4. Try enrollment for unrecognized stable tracks
    for (auto& track : associator_->tracks_mut()) {
        if (track.memory_object_id < 0 &&
            track.stable_frames >= cfg_.associator.enrollment_delay_frames &&
            !track.embedding.empty()) {
            try_enroll(track, now_ms_);
        }
    }

    // 5. Queue embedding extraction for tracks that need it
    for (auto& track : associator_->tracks_mut()) {
        if (track.needs_embedding && track.crop_count > 0) {
            queue_embedding_for_track(track, rgb, img_w, img_h);
        }
    }

    // In YOLO-heavy/YOLO-only mode there may be few or no tracker frames.
    // Drain one embedding request here too so the queue cannot grow unbounded.
    embed_queue_->process_one(associator_->tracks_mut());

    // 6. Score interest and select top-K for tracking
    auto top_k = interest_->rank_and_select(
        associator_->tracks_mut(), *memory_, now_ms_,
        cfg_.max_tracked_objects);

    // 7. Assign KCF tracker slots to top-K
    tracker_pool_->assign_slots(associator_->tracks_mut(), top_k,
                                 gray, img_w, img_h, dt);

    // 8. Reinit active trackers from YOLO detections
    for (auto& track : associator_->tracks_mut()) {
        if (track.is_actively_tracked && track.kcf_slot_index >= 0) {
            Detection det{};
            det.cls_id = track.coarse_class_id;
            det.label = track.coarse_label.c_str();
            det.confidence = 1.0f;
            det.left   = static_cast<int>(track.cx - track.w * 0.5f);
            det.top    = static_cast<int>(track.cy - track.h * 0.5f);
            det.right  = static_cast<int>(track.cx + track.w * 0.5f);
            det.bottom = static_cast<int>(track.cy + track.h * 0.5f);
            tracker_pool_->reinit_slot(track.kcf_slot_index, det,
                                        gray, img_w, img_h, dt);
        }
    }

    return build_output(img_w, img_h);
}

PerceptionFrame PerceptionEngine::process_tracker_frame(
    const uint8_t* gray,
    int img_w, int img_h,
    float dt) {

    now_ms_ = now_ms();

    // 1. Update KCF trackers for actively tracked objects
    tracker_pool_->update_all(associator_->tracks_mut(), gray, img_w, img_h, dt);

    // 2. Process one pending embedding (interleaved NPU usage)
    embed_queue_->process_one(associator_->tracks_mut());

    return build_output(img_w, img_h);
}

PerceptionFrame PerceptionEngine::build_output(int img_w, int img_h) {
    PerceptionFrame frame;
    frame.frame_width = img_w;
    frame.frame_height = img_h;
    frame.total_memory_objects = memory_->total_objects();
    frame.active_trackers = tracker_pool_->active_count();

    for (const auto& track : associator_->tracks()) {
        if (track.miss_count > 0) continue;

        PerceivedObject obj;
        obj.track_id = track.track_id;
        obj.cls_id = track.coarse_class_id;
        obj.cls_label = track.coarse_label;
        obj.confidence = 1.0f;
        obj.left   = static_cast<int>(track.cx - track.w * 0.5f);
        obj.top    = static_cast<int>(track.cy - track.h * 0.5f);
        obj.right  = static_cast<int>(track.cx + track.w * 0.5f);
        obj.bottom = static_cast<int>(track.cy + track.h * 0.5f);
        obj.interest = track.interest_score;
        obj.actively_tracked = track.is_actively_tracked;

        if (track.memory_object_id >= 0) {
            obj.object_id = track.memory_object_id;
            obj.match_confidence = track.last_match_similarity;
            const ObjectRecord* mem = memory_->get(
                static_cast<uint32_t>(track.memory_object_id));
            if (mem) {
                obj.object_name = mem->name;
            }
        }

        frame.objects.push_back(std::move(obj));
    }

    if (callback_) callback_(frame);
    return frame;
}

void PerceptionEngine::try_enroll(TrackSlot& track, uint64_t ts) {
    // Collect exemplar embeddings from crop history
    std::vector<std::vector<float>> exemplars;

    // We need at least one embedding. Use the track's current one.
    if (!track.embedding.empty()) {
        exemplars.push_back(track.embedding);
    }

    if (exemplars.empty()) return;

    uint32_t oid = memory_->enroll(track.coarse_label, exemplars, ts);
    track.memory_object_id = static_cast<int>(oid);
    track.enrollment_status = TrackSlot::Status::Enrolled;

    // Queue VLM enrichment
    if (cfg_.vlm.enabled && track.crop_count > 0) {
        VlmEnrichRequest req;
        req.object_id = oid;
        req.coarse_class = track.coarse_label;

        // Collect best crops (up to 3)
        int n = std::min(3, track.crop_count);
        for (int i = 0; i < n; i++) {
            int idx = (track.crop_cursor - 1 - i + TrackSlot::kMaxCrops) % TrackSlot::kMaxCrops;
            if (idx >= 0 && idx < TrackSlot::kMaxCrops && !track.crops[idx].rgb_data.empty()) {
                req.crop_jpegs.push_back(track.crops[idx].rgb_data);
                req.crop_sizes.push_back({track.crops[idx].width, track.crops[idx].height});
            }
        }

        vlm_->enqueue(req);
    }

    // Save reference crops to disk
    std::vector<std::pair<std::vector<uint8_t>, std::pair<int,int>>> disk_crops;
    int n = std::min(3, track.crop_count);
    for (int i = 0; i < n; i++) {
        int idx = (track.crop_cursor - 1 - i + TrackSlot::kMaxCrops) % TrackSlot::kMaxCrops;
        if (idx >= 0 && idx < TrackSlot::kMaxCrops && !track.crops[idx].rgb_data.empty()) {
            disk_crops.push_back({track.crops[idx].rgb_data,
                                  {track.crops[idx].width, track.crops[idx].height}});
        }
    }
    if (!disk_crops.empty()) {
        memory_->save_crops(oid, disk_crops);
    }
}

void PerceptionEngine::queue_embedding_for_track(TrackSlot& track,
                                                   const uint8_t* rgb,
                                                   int img_w, int img_h) {
    // Find best crop
    int best_idx = -1;
    float best_q = -1;
    for (int i = 0; i < track.crop_count; i++) {
        int idx = (track.crop_cursor - 1 - i + TrackSlot::kMaxCrops) % TrackSlot::kMaxCrops;
        if (track.crops[idx].quality_score > best_q) {
            best_q = track.crops[idx].quality_score;
            best_idx = idx;
        }
    }

    if (best_idx >= 0 && !track.crops[best_idx].rgb_data.empty()) {
        embed_queue_->push(track.track_id,
                           track.crops[best_idx].rgb_data.data(),
                           track.crops[best_idx].width,
                           track.crops[best_idx].height,
                           track.interest_score);
    }
}

const std::vector<TrackSlot>& PerceptionEngine::tracks() const {
    return associator_->tracks();
}

void PerceptionEngine::save_memory() {
    memory_->save();
}

void PerceptionEngine::load_memory() {
    memory_->load();
}

void PerceptionEngine::set_perception_callback(PerceptionCallback cb) {
    callback_ = std::move(cb);
}

}  // namespace sc
