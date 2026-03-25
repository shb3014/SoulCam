#include "ai/embedder.h"
#include "util/logger.h"

#include <cmath>
#include <cstring>
#include <vector>

#ifdef SOULCAM_HAVE_RKNN
#include "rknn_api.h"
#endif

#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

namespace sc {

struct Embedder {
#ifdef SOULCAM_HAVE_RKNN
    rknn_context ctx = 0;
    rknn_input_output_num io_num{};
    rknn_tensor_attr input_attr{};
    rknn_tensor_attr output_attr{};
#endif
    int input_size = 128;
    int embed_dim  = 128;
    std::vector<uint8_t> resize_buf;
    bool valid = false;
};

// Simple bilinear resize for crop -> input_size x input_size
static void resize_rgb(const uint8_t* src, int sw, int sh,
                        uint8_t* dst, int dw, int dh) {
    float sx = static_cast<float>(sw) / dw;
    float sy = static_cast<float>(sh) / dh;

    for (int dy = 0; dy < dh; dy++) {
        float fy = (dy + 0.5f) * sy - 0.5f;
        int y0 = std::max(0, std::min(static_cast<int>(fy), sh - 1));
        int y1 = std::min(y0 + 1, sh - 1);
        float wy = fy - y0;
        wy = std::max(0.0f, std::min(wy, 1.0f));

        for (int dx = 0; dx < dw; dx++) {
            float fx = (dx + 0.5f) * sx - 0.5f;
            int x0 = std::max(0, std::min(static_cast<int>(fx), sw - 1));
            int x1 = std::min(x0 + 1, sw - 1);
            float wx = fx - x0;
            wx = std::max(0.0f, std::min(wx, 1.0f));

            for (int c = 0; c < 3; c++) {
                float v00 = src[(y0 * sw + x0) * 3 + c];
                float v01 = src[(y0 * sw + x1) * 3 + c];
                float v10 = src[(y1 * sw + x0) * 3 + c];
                float v11 = src[(y1 * sw + x1) * 3 + c];
                float v = v00 * (1 - wx) * (1 - wy) + v01 * wx * (1 - wy) +
                          v10 * (1 - wx) * wy + v11 * wx * wy;
                dst[(dy * dw + dx) * 3 + c] = static_cast<uint8_t>(
                    std::max(0.0f, std::min(255.0f, v + 0.5f)));
            }
        }
    }
}

Embedder* embedder_create(const EmbedderConfig& cfg) {
    auto* emb = new Embedder();
    emb->input_size = cfg.input_size;
    emb->embed_dim  = cfg.embed_dim;
    emb->resize_buf.resize(cfg.input_size * cfg.input_size * 3);

#ifdef SOULCAM_HAVE_RKNN
    if (cfg.model_path.empty()) {
        SC_LOG_WARN("Embedder: no model path, running in stub mode");
        emb->valid = false;
        return emb;
    }

    // Load model file
    FILE* f = fopen(cfg.model_path.c_str(), "rb");
    if (!f) {
        SC_LOG_ERROR("Embedder: cannot open model: %s", cfg.model_path.c_str());
        emb->valid = false;
        return emb;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> model_data(sz);
    size_t nread = fread(model_data.data(), 1, sz, f);
    fclose(f);
    if (static_cast<long>(nread) != sz) {
        SC_LOG_ERROR("Embedder: short read (%zu / %ld)", nread, sz);
        emb->valid = false;
        return emb;
    }

    int ret = rknn_init(&emb->ctx, model_data.data(), sz, 0, nullptr);
    if (ret < 0) {
        SC_LOG_ERROR("Embedder: rknn_init failed: %d", ret);
        emb->valid = false;
        return emb;
    }

    ret = rknn_query(emb->ctx, RKNN_QUERY_IN_OUT_NUM,
                     &emb->io_num, sizeof(emb->io_num));
    if (ret < 0) {
        SC_LOG_ERROR("Embedder: rknn_query io_num failed: %d", ret);
        rknn_destroy(emb->ctx);
        emb->valid = false;
        return emb;
    }

    emb->input_attr.index = 0;
    ret = rknn_query(emb->ctx, RKNN_QUERY_INPUT_ATTR,
                     &emb->input_attr, sizeof(emb->input_attr));
    if (ret < 0) {
        SC_LOG_ERROR("Embedder: rknn_query input_attr failed: %d", ret);
        rknn_destroy(emb->ctx);
        emb->valid = false;
        return emb;
    }

    emb->output_attr.index = 0;
    ret = rknn_query(emb->ctx, RKNN_QUERY_OUTPUT_ATTR,
                     &emb->output_attr, sizeof(emb->output_attr));
    if (ret < 0) {
        SC_LOG_ERROR("Embedder: rknn_query output_attr failed: %d", ret);
        rknn_destroy(emb->ctx);
        emb->valid = false;
        return emb;
    }

    emb->valid = true;
    SC_LOG_INFO("Embedder: loaded %s (input=%dx%d, output_dim=%d)",
                cfg.model_path.c_str(),
                emb->input_attr.dims[1], emb->input_attr.dims[2],
                emb->output_attr.n_elems);
#else
    SC_LOG_INFO("Embedder: RKNN not available, running in stub mode");
    emb->valid = false;
#endif

    return emb;
}

void embedder_destroy(Embedder* emb) {
    if (!emb) return;
#ifdef SOULCAM_HAVE_RKNN
    if (emb->valid) {
        rknn_destroy(emb->ctx);
    }
#endif
    delete emb;
}

int embedder_infer(Embedder* emb,
                    const uint8_t* crop_rgb, int crop_w, int crop_h,
                    std::vector<float>& out) {
    if (!emb) return -1;

    out.resize(emb->embed_dim);

    // Resize crop to input_size x input_size
    resize_rgb(crop_rgb, crop_w, crop_h,
               emb->resize_buf.data(), emb->input_size, emb->input_size);

#ifdef SOULCAM_HAVE_RKNN
    if (!emb->valid) {
        // Stub: return zero embedding
        std::fill(out.begin(), out.end(), 0.0f);
        return 0;
    }

    rknn_input inputs[1];
    memset(inputs, 0, sizeof(inputs));
    inputs[0].index = 0;
    inputs[0].type = RKNN_TENSOR_UINT8;
    inputs[0].fmt = RKNN_TENSOR_NHWC;
    inputs[0].size = emb->input_size * emb->input_size * 3;
    inputs[0].buf = emb->resize_buf.data();

    int ret = rknn_inputs_set(emb->ctx, 1, inputs);
    if (ret < 0) return -1;

    ret = rknn_run(emb->ctx, nullptr);
    if (ret < 0) return -1;

    rknn_output outputs[1];
    memset(outputs, 0, sizeof(outputs));
    outputs[0].index = 0;
    outputs[0].want_float = 1;

    ret = rknn_outputs_get(emb->ctx, 1, outputs, nullptr);
    if (ret < 0) return -1;

    int n = std::min(emb->embed_dim, static_cast<int>(emb->output_attr.n_elems));
    const float* raw = static_cast<const float*>(outputs[0].buf);
    for (int i = 0; i < n; i++) out[i] = raw[i];
    for (int i = n; i < emb->embed_dim; i++) out[i] = 0.0f;

    rknn_outputs_release(emb->ctx, 1, outputs);

    l2_normalize(out);
    return 0;
#else
    // Stub: compute a simple feature from pixel statistics
    // (not useful for real re-ID but allows the pipeline to function)
    const int npix = emb->input_size * emb->input_size;
    std::fill(out.begin(), out.end(), 0.0f);
    for (int i = 0; i < npix; i += 4) {
        int bucket = (emb->resize_buf[i * 3] / 2) % emb->embed_dim;
        out[bucket] += 1.0f;
    }
    l2_normalize(out);
    return 0;
#endif
}

void l2_normalize(std::vector<float>& vec) {
    float sum = 0.0f;
    const int n = static_cast<int>(vec.size());

#ifdef __ARM_NEON
    float32x4_t vsum = vdupq_n_f32(0);
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        float32x4_t v = vld1q_f32(&vec[i]);
        vsum = vfmaq_f32(vsum, v, v);
    }
    sum = vaddvq_f32(vsum);
    for (; i < n; i++) sum += vec[i] * vec[i];
#else
    for (int i = 0; i < n; i++) sum += vec[i] * vec[i];
#endif

    float inv_norm = (sum > 1e-12f) ? 1.0f / std::sqrt(sum) : 0.0f;

#ifdef __ARM_NEON
    float32x4_t vinv = vdupq_n_f32(inv_norm);
    i = 0;
    for (; i + 4 <= n; i += 4) {
        float32x4_t v = vld1q_f32(&vec[i]);
        vst1q_f32(&vec[i], vmulq_f32(v, vinv));
    }
    for (; i < n; i++) vec[i] *= inv_norm;
#else
    for (int i = 0; i < n; i++) vec[i] *= inv_norm;
#endif
}

float cosine_similarity(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) return 0.0f;
    const int n = static_cast<int>(a.size());
    float dot = 0.0f;

#ifdef __ARM_NEON
    float32x4_t vdot = vdupq_n_f32(0);
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        float32x4_t va = vld1q_f32(&a[i]);
        float32x4_t vb = vld1q_f32(&b[i]);
        vdot = vfmaq_f32(vdot, va, vb);
    }
    dot = vaddvq_f32(vdot);
    for (; i < n; i++) dot += a[i] * b[i];
#else
    for (int i = 0; i < n; i++) dot += a[i] * b[i];
#endif

    return dot;  // a and b are L2-normalized, so dot product = cosine similarity
}

}  // namespace sc
