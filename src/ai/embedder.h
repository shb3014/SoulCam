#pragma once
// ============================================================================
// Embedding model for object instance re-identification
//
// Wraps a lightweight CNN (e.g., MobileNetV3-Small) deployed as RKNN to
// produce 128-D L2-normalized feature vectors from object crops. These
// embeddings serve as the identity backbone for the perception pipeline.
//
// On RK3566, runs in ~15-25ms per crop on the NPU.
// Scheduled on tracker frames when the NPU is idle (interleaved with YOLO).
// ============================================================================

#include <cstdint>
#include <string>
#include <vector>

namespace sc {

struct EmbedderConfig {
    std::string model_path;
    int  embed_dim    = 128;
    int  input_size   = 128;  // square input (input_size x input_size)
};

struct Embedder;

Embedder* embedder_create(const EmbedderConfig& cfg);
void      embedder_destroy(Embedder* emb);

// Run embedding inference on a crop.
// crop_rgb: pointer to RGB data (row-major, 3 bytes per pixel)
// crop_w, crop_h: crop dimensions (will be resized internally to input_size)
// out: output embedding vector (resized to embed_dim)
// Returns 0 on success.
int embedder_infer(Embedder* emb,
                   const uint8_t* crop_rgb, int crop_w, int crop_h,
                   std::vector<float>& out);

// L2-normalize a vector in-place.
void l2_normalize(std::vector<float>& vec);

// Cosine similarity between two L2-normalized vectors.
float cosine_similarity(const std::vector<float>& a, const std::vector<float>& b);

}  // namespace sc
