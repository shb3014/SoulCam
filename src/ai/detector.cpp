// ============================================================================
// RKNN detector implementation with YOLOv8 post-processing
//
// Post-processing (DFL decode + NMS) adapted from:
//   YoloV8-NPU/src/postprocess.cpp  (Rockchip / Q-engineering)
//
// When SOULCAM_HAVE_RKNN is defined, uses the real RKNN API.
// Otherwise provides a no-op stub.
// ============================================================================

#include "ai/detector.h"
#include "util/logger.h"

#include <cstring>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <vector>
#include <set>
#include <algorithm>

#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

namespace sc {

// ---- COCO class labels (80 classes) ----
static const char* COCO_LABELS[] = {
    "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train",
    "truck", "boat", "traffic light", "fire hydrant", "stop sign",
    "parking meter", "bench", "bird", "cat", "dog", "horse", "sheep",
    "cow", "elephant", "bear", "zebra", "giraffe", "backpack", "umbrella",
    "handbag", "tie", "suitcase", "frisbee", "skis", "snowboard",
    "sports ball", "kite", "baseball bat", "baseball glove", "skateboard",
    "surfboard", "tennis racket", "bottle", "wine glass", "cup", "fork",
    "knife", "spoon", "bowl", "banana", "apple", "sandwich", "orange",
    "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair",
    "couch", "potted plant", "bed", "dining table", "toilet", "tv",
    "laptop", "mouse", "remote", "keyboard", "cell phone", "microwave",
    "oven", "toaster", "sink", "refrigerator", "book", "clock", "vase",
    "scissors", "teddy bear", "hair drier", "toothbrush"
};
static constexpr int NUM_LABELS    = 80;
static constexpr int MAX_DETECT    = 128;

// =========================================================================
// RKNN implementation
// =========================================================================
#if SOULCAM_HAVE_RKNN

#include "rknn_api.h"

struct Detector {
    rknn_context            ctx = 0;
    rknn_input_output_num   io_num{};
    rknn_tensor_attr*       input_attrs  = nullptr;
    rknn_tensor_attr*       output_attrs = nullptr;
    int  model_width    = 0;
    int  model_height   = 0;
    int  model_channels = 0;
    bool is_quant       = false;
    float conf_thresh   = 0.25f;
    float nms_thresh    = 0.45f;
};

// ---- helpers ----

static unsigned char* load_model_data(const char* path, int& sz) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return nullptr;
    sz = static_cast<int>(f.tellg());
    f.seekg(0, std::ios::beg);
    auto* d = static_cast<unsigned char*>(malloc(sz));
    if (!d) return nullptr;
    f.read(reinterpret_cast<char*>(d), sz);
    return d;
}

inline static int clamp_i(float val, int lo, int hi) {
    return val > lo ? (val < hi ? static_cast<int>(val) : hi) : lo;
}
inline static int32_t clip_i32(float val, float lo, float hi) {
    return static_cast<int32_t>(val <= lo ? lo : (val >= hi ? hi : val));
}
static int8_t qnt_f32_to_affine(float f32, int32_t zp, float scale) {
    return static_cast<int8_t>(clip_i32((f32 / scale) + zp, -128, 127));
}
static float deqnt_affine_to_f32(int8_t qnt, int32_t zp, float scale) {
    return (static_cast<float>(qnt) - static_cast<float>(zp)) * scale;
}

// ---- IoU + NMS ----

static float calc_iou(float x0a, float y0a, float x0b, float y0b,
                       float x1a, float y1a, float x1b, float y1b) {
    float w = fmaxf(0.f, fminf(x0b, x1b) - fmaxf(x0a, x1a) + 1.f);
    float h = fmaxf(0.f, fminf(y0b, y1b) - fmaxf(y0a, y1a) + 1.f);
    float inter = w * h;
    float uni = (x0b-x0a+1.f)*(y0b-y0a+1.f) + (x1b-x1a+1.f)*(y1b-y1a+1.f) - inter;
    return uni <= 0.f ? 0.f : inter / uni;
}

static void nms(int cnt, std::vector<float>& boxes, std::vector<int>& cls,
                std::vector<int>& order, int filt_cls, float thresh) {
    for (int i = 0; i < cnt; ++i) {
        if (order[i] == -1 || cls[i] != filt_cls) continue;
        int n = order[i];
        for (int j = i + 1; j < cnt; ++j) {
            int m = order[j];
            if (m == -1 || cls[j] != filt_cls) continue;
            float iou = calc_iou(
                boxes[n*4+0], boxes[n*4+1],
                boxes[n*4+0]+boxes[n*4+2], boxes[n*4+1]+boxes[n*4+3],
                boxes[m*4+0], boxes[m*4+1],
                boxes[m*4+0]+boxes[m*4+2], boxes[m*4+1]+boxes[m*4+3]);
            if (iou > thresh) order[j] = -1;
        }
    }
}

// ---- quicksort by score descending ----

static void qsort_desc(std::vector<float>& vals, int lo, int hi,
                         std::vector<int>& idx) {
    if (lo >= hi) return;
    int i = lo, j = hi;
    float key = vals[lo];
    int key_idx = idx[lo];
    while (i < j) {
        while (i < j && vals[j] <= key) --j;
        vals[i] = vals[j]; idx[i] = idx[j];
        while (i < j && vals[i] >= key) ++i;
        vals[j] = vals[i]; idx[j] = idx[i];
    }
    vals[i] = key; idx[i] = key_idx;
    qsort_desc(vals, lo, i - 1, idx);
    qsort_desc(vals, i + 1, hi, idx);
}

// ---- DFL (Distribution Focal Loss) decode ----

static void compute_dfl(float* tensor, int dfl_len, float box[4]) {
    for (int b = 0; b < 4; b++) {
        float exp_sum = 0, acc = 0;
        float et[16]; // stack array (max dfl_len = 16, avoids malloc)
        for (int i = 0; i < dfl_len; i++) {
            et[i] = expf(tensor[i + b * dfl_len]);
            exp_sum += et[i];
        }
        for (int i = 0; i < dfl_len; i++)
            acc += et[i] / exp_sum * i;
        box[b] = acc;
    }
}

// ---- NEON SIMD helpers ----
#ifdef __ARM_NEON

// Fast vectorized exp(x): exp(x) = 2^(x·log₂e) = 2^n · 2^f
// Uses 5th-degree minimax polynomial for 2^f on [0,1).
// Relative error < 0.05% for x ∈ [-87, 88].
static inline float32x4_t neon_exp_f32(float32x4_t x) {
    // Clamp to safe range
    x = vmaxq_f32(x, vdupq_n_f32(-87.33f));
    x = vminq_f32(x, vdupq_n_f32(88.72f));

    // t = x * log2(e)
    float32x4_t t = vmulq_f32(x, vdupq_n_f32(1.44269504089f));

    // n = floor(t)
    float32x4_t n = vrndmq_f32(t);

    // f = t - n (fractional part in [0,1))
    float32x4_t f = vsubq_f32(t, n);

    // 2^f ≈ minimax polynomial degree 5 on [0,1]
    // p = c0 + f*(c1 + f*(c2 + f*(c3 + f*(c4 + f*c5))))
    float32x4_t p = vfmaq_f32(vdupq_n_f32(0.0096181f),
                               vdupq_n_f32(0.0013334f), f);
    p = vfmaq_f32(vdupq_n_f32(0.0555041f), p, f);
    p = vfmaq_f32(vdupq_n_f32(0.2402265f), p, f);
    p = vfmaq_f32(vdupq_n_f32(0.6931472f), p, f);
    p = vfmaq_f32(vdupq_n_f32(1.0f),       p, f);

    // 2^n via IEEE 754 exponent manipulation
    int32x4_t ni = vcvtq_s32_f32(n);
    int32x4_t pow2n = vshlq_n_s32(vaddq_s32(ni, vdupq_n_s32(127)), 23);
    return vmulq_f32(p, vreinterpretq_f32_s32(pow2n));
}

// NEON DFL decode for common case dfl_len=16.
// Computes softmax + weighted-index sum for 4 box sides.
// Replaces 64 scalar expf() calls with 16 NEON 4-wide exp.
static void neon_compute_dfl_16(const float* tensor, float box[4]) {
    static const float idx_vals[16] = {
        0.f, 1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f,
        8.f, 9.f, 10.f, 11.f, 12.f, 13.f, 14.f, 15.f
    };
    const float32x4_t idx0 = vld1q_f32(idx_vals);
    const float32x4_t idx1 = vld1q_f32(idx_vals + 4);
    const float32x4_t idx2 = vld1q_f32(idx_vals + 8);
    const float32x4_t idx3 = vld1q_f32(idx_vals + 12);

    for (int b = 0; b < 4; b++) {
        const float* base = tensor + b * 16;

        // exp() for 16 values in 4 SIMD batches
        float32x4_t e0 = neon_exp_f32(vld1q_f32(base));
        float32x4_t e1 = neon_exp_f32(vld1q_f32(base + 4));
        float32x4_t e2 = neon_exp_f32(vld1q_f32(base + 8));
        float32x4_t e3 = neon_exp_f32(vld1q_f32(base + 12));

        // Horizontal sum for softmax denominator
        float32x4_t s = vaddq_f32(vaddq_f32(e0, e1), vaddq_f32(e2, e3));
        float32x4_t inv = vdupq_n_f32(1.0f / vaddvq_f32(s));

        // Normalize (softmax)
        e0 = vmulq_f32(e0, inv);
        e1 = vmulq_f32(e1, inv);
        e2 = vmulq_f32(e2, inv);
        e3 = vmulq_f32(e3, inv);

        // Weighted sum: Σ softmax[i] × i
        float32x4_t w = vmulq_f32(e0, idx0);
        w = vfmaq_f32(w, e1, idx1);
        w = vfmaq_f32(w, e2, idx2);
        w = vfmaq_f32(w, e3, idx3);

        box[b] = vaddvq_f32(w);
    }
}

#endif // __ARM_NEON

// ---- process per-branch (i8 quantized) ----

static int process_i8(int8_t* box_t, int32_t box_zp, float box_sc,
                       int8_t* score_t, int32_t score_zp, float score_sc,
                       int8_t* score_sum_t, int32_t ss_zp, float ss_sc,
                       int gh, int gw, int stride, int dfl_len,
                       std::vector<float>& boxes,
                       std::vector<float>& probs,
                       std::vector<int>& cls,
                       float thresh) {
    int valid = 0, glen = gh * gw;
    int8_t thr_i8 = qnt_f32_to_affine(thresh, score_zp, score_sc);
    int8_t ss_thr  = qnt_f32_to_affine(thresh, ss_zp, ss_sc);
    float before_dfl[64]; // stack-allocated (max dfl_len*4 = 16*4 = 64)

    int cell = 0;

#ifdef __ARM_NEON
    // ================================================================
    // NEON path: process 16 grid cells per batch.
    //
    // For a fixed class c, scores of 16 consecutive cells are
    // contiguous in memory (layout [1, C, H, W]), enabling
    // efficient SIMD loads. This reduces the 80-class max-scan
    // from 80 scalar comparisons/cell to 80 NEON ops / 16 cells.
    // ================================================================
    int8x16_t v_ss_thr = vdupq_n_s8(ss_thr);

    for (; cell + 16 <= glen; cell += 16) {
        // Batch score_sum quick-reject: skip if ALL 16 cells below threshold
        if (score_sum_t) {
            int8x16_t ss = vld1q_s8(&score_sum_t[cell]);
            uint8x16_t pass = vcgeq_s8(ss, v_ss_thr);
            if (vmaxvq_u8(pass) == 0) continue;
        }

        // Find max class score & class index for 16 cells simultaneously.
        // For each class, load 16 contiguous scores and update running max.
        int8x16_t max_sc  = vdupq_n_s8(-128);
        uint8x16_t max_cls = vdupq_n_u8(0);

        for (int c = 0; c < NUM_LABELS; c++) {
            int8x16_t sc = vld1q_s8(&score_t[c * glen + cell]);
            uint8x16_t gt = vcgtq_s8(sc, max_sc);
            max_cls = vbslq_u8(gt, vdupq_n_u8(static_cast<uint8_t>(c)), max_cls);
            max_sc  = vmaxq_s8(max_sc, sc);
        }

        // Extract per-lane results
        int8_t  sc_arr[16];
        uint8_t cls_arr[16];
        vst1q_s8(sc_arr, max_sc);
        vst1q_u8(cls_arr, max_cls);

        // Process only cells whose max class score exceeds threshold
        for (int k = 0; k < 16; k++) {
            if (sc_arr[k] <= thr_i8) continue;

            int off = cell + k;

            // Dequantize box tensor (strided access, stride = glen)
            int off2 = off;
            for (int d = 0; d < dfl_len * 4; d++) {
                before_dfl[d] = deqnt_affine_to_f32(box_t[off2], box_zp, box_sc);
                off2 += glen;
            }

            // DFL decode: NEON softmax for dfl_len=16, scalar fallback otherwise
            float box[4];
            if (dfl_len == 16)
                neon_compute_dfl_16(before_dfl, box);
            else
                compute_dfl(before_dfl, dfl_len, box);

            int ci = off / gw, cj = off % gw;
            float x1 = (-box[0] + cj + 0.5f) * stride;
            float y1 = (-box[1] + ci + 0.5f) * stride;
            float x2 = ( box[2] + cj + 0.5f) * stride;
            float y2 = ( box[3] + ci + 0.5f) * stride;
            boxes.push_back(x1); boxes.push_back(y1);
            boxes.push_back(x2 - x1); boxes.push_back(y2 - y1);
            probs.push_back(deqnt_affine_to_f32(sc_arr[k], score_zp, score_sc));
            cls.push_back(static_cast<int>(cls_arr[k]));
            valid++;
        }
    }
#endif // __ARM_NEON

    // ================================================================
    // Scalar path: handles remaining cells (< 16) or non-NEON builds.
    // ================================================================
    for (; cell < glen; cell++) {
        if (score_sum_t && score_sum_t[cell] < ss_thr) continue;
        int8_t max_sc = static_cast<int8_t>(-score_zp);
        int max_c = -1;
        int off2 = cell;
        for (int c = 0; c < NUM_LABELS; c++) {
            if (score_t[off2] > thr_i8 && score_t[off2] > max_sc) {
                max_sc = score_t[off2]; max_c = c;
            }
            off2 += glen;
        }
        if (max_sc <= thr_i8) continue;
        // decode box via DFL
        off2 = cell;
        for (int d = 0; d < dfl_len * 4; d++) {
            before_dfl[d] = deqnt_affine_to_f32(box_t[off2], box_zp, box_sc);
            off2 += glen;
        }
        float box[4];
        compute_dfl(before_dfl, dfl_len, box);
        int ci = cell / gw, cj = cell % gw;
        float x1 = (-box[0] + cj + 0.5f) * stride;
        float y1 = (-box[1] + ci + 0.5f) * stride;
        float x2 = ( box[2] + cj + 0.5f) * stride;
        float y2 = ( box[3] + ci + 0.5f) * stride;
        boxes.push_back(x1); boxes.push_back(y1);
        boxes.push_back(x2 - x1); boxes.push_back(y2 - y1);
        probs.push_back(deqnt_affine_to_f32(max_sc, score_zp, score_sc));
        cls.push_back(max_c);
        valid++;
    }
    return valid;
}

// ---- process per-branch (fp32) ----

static int process_fp32(float* box_t, float* score_t, float* score_sum_t,
                         int gh, int gw, int stride, int dfl_len,
                         std::vector<float>& boxes,
                         std::vector<float>& probs,
                         std::vector<int>& cls,
                         float thresh) {
    int valid = 0, glen = gh * gw;
    for (int i = 0; i < gh; i++) {
        for (int j = 0; j < gw; j++) {
            int off = i * gw + j;
            if (score_sum_t && score_sum_t[off] < thresh) continue;
            float max_sc = 0; int max_c = -1;
            int off2 = off;
            for (int c = 0; c < NUM_LABELS; c++) {
                if (score_t[off2] > thresh && score_t[off2] > max_sc) {
                    max_sc = score_t[off2]; max_c = c;
                }
                off2 += glen;
            }
            if (max_sc <= thresh) continue;
            off2 = off;
            float before_dfl[64]; // stack (max dfl_len*4 = 16*4 = 64)
            for (int k = 0; k < dfl_len * 4; k++) {
                before_dfl[k] = box_t[off2]; off2 += glen;
            }
            float box[4];
            compute_dfl(before_dfl, dfl_len, box);
            float x1 = (-box[0] + j + 0.5f) * stride;
            float y1 = (-box[1] + i + 0.5f) * stride;
            float x2 = ( box[2] + j + 0.5f) * stride;
            float y2 = ( box[3] + i + 0.5f) * stride;
            boxes.push_back(x1); boxes.push_back(y1);
            boxes.push_back(x2 - x1); boxes.push_back(y2 - y1);
            probs.push_back(max_sc);
            cls.push_back(max_c);
            valid++;
        }
    }
    return valid;
}

// =========================================================================
// Public API
// =========================================================================

Detector* detector_create(const RknnConfig& cfg) {
    if (cfg.model_path.empty()) return nullptr;

    auto* det = new Detector();
    det->conf_thresh = cfg.conf_threshold;
    det->nms_thresh  = cfg.nms_threshold;

    int model_sz = 0;
    unsigned char* model_data = load_model_data(cfg.model_path.c_str(), model_sz);
    if (!model_data) {
        SC_LOG_ERROR("Failed to load RKNN model: %s", cfg.model_path.c_str());
        delete det; return nullptr;
    }
    int ret = rknn_init(&det->ctx, model_data, model_sz, 0, nullptr);
    free(model_data);
    if (ret < 0) {
        SC_LOG_ERROR("rknn_init failed: %d", ret);
        delete det; return nullptr;
    }

    rknn_sdk_version ver;
    rknn_query(det->ctx, RKNN_QUERY_SDK_VERSION, &ver, sizeof(ver));
    SC_LOG_INFO("RKNN SDK: %s, driver: %s", ver.api_version, ver.drv_version);

    rknn_query(det->ctx, RKNN_QUERY_IN_OUT_NUM, &det->io_num, sizeof(det->io_num));
    SC_LOG_INFO("Model: %d inputs, %d outputs", det->io_num.n_input, det->io_num.n_output);

    det->input_attrs = new rknn_tensor_attr[det->io_num.n_input]{};
    for (uint32_t i = 0; i < det->io_num.n_input; i++) {
        det->input_attrs[i].index = i;
        rknn_query(det->ctx, RKNN_QUERY_INPUT_ATTR, &det->input_attrs[i], sizeof(rknn_tensor_attr));
    }
    det->output_attrs = new rknn_tensor_attr[det->io_num.n_output]{};
    for (uint32_t i = 0; i < det->io_num.n_output; i++) {
        det->output_attrs[i].index = i;
        rknn_query(det->ctx, RKNN_QUERY_OUTPUT_ATTR, &det->output_attrs[i], sizeof(rknn_tensor_attr));
    }

    if (det->input_attrs[0].fmt == RKNN_TENSOR_NCHW) {
        det->model_channels = det->input_attrs[0].dims[1];
        det->model_height   = det->input_attrs[0].dims[2];
        det->model_width    = det->input_attrs[0].dims[3];
    } else {
        det->model_height   = det->input_attrs[0].dims[1];
        det->model_width    = det->input_attrs[0].dims[2];
        det->model_channels = det->input_attrs[0].dims[3];
    }
    det->is_quant = (det->output_attrs[0].qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC &&
                     det->output_attrs[0].type != RKNN_TENSOR_FLOAT16);

    SC_LOG_INFO("Model input: %dx%dx%d, quant=%s",
                det->model_width, det->model_height, det->model_channels,
                det->is_quant ? "yes" : "no");
#ifdef __ARM_NEON
    SC_LOG_INFO("NEON SIMD: enabled (batch score scan + fast DFL softmax)");
#else
    SC_LOG_INFO("NEON SIMD: not available (scalar fallback)");
#endif
    return det;
}

void detector_destroy(Detector* det) {
    if (!det) return;
    if (det->ctx) rknn_destroy(det->ctx);
    delete[] det->input_attrs;
    delete[] det->output_attrs;
    delete det;
}

int detector_infer(Detector* det,
                   const uint8_t* data, size_t size,
                   int width, int height, int channels,
                   std::vector<Detection>& out) {
    if (!det || !det->ctx || !data) return -1;

    // ---- set input ----
    rknn_input inputs[1]{};
    inputs[0].index = 0;
    inputs[0].type  = RKNN_TENSOR_UINT8;
    inputs[0].fmt   = RKNN_TENSOR_NHWC;
    inputs[0].size  = det->model_width * det->model_height * det->model_channels;
    inputs[0].buf   = const_cast<uint8_t*>(data);

    int ret = rknn_inputs_set(det->ctx, det->io_num.n_input, inputs);
    if (ret < 0) { SC_LOG_ERROR("rknn_inputs_set: %d", ret); return ret; }

    ret = rknn_run(det->ctx, nullptr);
    if (ret < 0) { SC_LOG_ERROR("rknn_run: %d", ret); return ret; }

    // ---- get outputs ----
    std::vector<rknn_output> outputs(det->io_num.n_output);
    memset(outputs.data(), 0, outputs.size() * sizeof(rknn_output));
    for (uint32_t i = 0; i < det->io_num.n_output; i++) {
        outputs[i].index = i;
        outputs[i].want_float = (!det->is_quant);
    }
    ret = rknn_outputs_get(det->ctx, det->io_num.n_output, outputs.data(), nullptr);
    if (ret < 0) { SC_LOG_ERROR("rknn_outputs_get: %d", ret); return ret; }

    // ---- YOLOv8 post-processing (3-branch DFL decode + NMS) ----
    std::vector<float> filterBoxes;
    std::vector<float> objProbs;
    std::vector<int>   classIds;
    int validCount = 0;

    int model_w = det->model_width;
    int model_h = det->model_height;
    int dfl_len = det->output_attrs[0].dims[1] / 4;
    int per_branch = det->io_num.n_output / 3;

    for (int b = 0; b < 3; b++) {
        int box_idx   = b * per_branch;
        int score_idx = b * per_branch + 1;
        void*   score_sum = nullptr;
        int32_t ss_zp = 0; float ss_sc = 1.0f;
        if (per_branch == 3) {
            score_sum = outputs[b * per_branch + 2].buf;
            ss_zp  = det->output_attrs[b * per_branch + 2].zp;
            ss_sc  = det->output_attrs[b * per_branch + 2].scale;
        }
        int gh = det->output_attrs[box_idx].dims[2];
        int gw = det->output_attrs[box_idx].dims[3];
        int stride = model_h / gh;

        if (det->is_quant) {
            validCount += process_i8(
                (int8_t*)outputs[box_idx].buf,
                det->output_attrs[box_idx].zp, det->output_attrs[box_idx].scale,
                (int8_t*)outputs[score_idx].buf,
                det->output_attrs[score_idx].zp, det->output_attrs[score_idx].scale,
                (int8_t*)score_sum, ss_zp, ss_sc,
                gh, gw, stride, dfl_len,
                filterBoxes, objProbs, classIds, det->conf_thresh);
        } else {
            validCount += process_fp32(
                (float*)outputs[box_idx].buf,
                (float*)outputs[score_idx].buf,
                (float*)score_sum,
                gh, gw, stride, dfl_len,
                filterBoxes, objProbs, classIds, det->conf_thresh);
        }
    }

    if (validCount > 0) {
        // sort by confidence descending
        std::vector<int> indexArr(validCount);
        for (int i = 0; i < validCount; i++) indexArr[i] = i;
        qsort_desc(objProbs, 0, validCount - 1, indexArr);

        // NMS per class
        std::set<int> cls_set(classIds.begin(), classIds.end());
        for (int c : cls_set)
            nms(validCount, filterBoxes, classIds, indexArr, c, det->nms_thresh);

        // scale from model coords to frame coords
        float sx = static_cast<float>(det->model_width)  / width;
        float sy = static_cast<float>(det->model_height) / height;

        for (int i = 0; i < validCount && static_cast<int>(out.size()) < MAX_DETECT; i++) {
            if (indexArr[i] == -1) continue;
            int n = indexArr[i];
            float x1 = filterBoxes[n * 4 + 0];
            float y1 = filterBoxes[n * 4 + 1];
            float x2 = x1 + filterBoxes[n * 4 + 2];
            float y2 = y1 + filterBoxes[n * 4 + 3];
            int cid = classIds[n];

            Detection d;
            d.cls_id     = cid;
            d.label      = (cid >= 0 && cid < NUM_LABELS) ? COCO_LABELS[cid] : "?";
            d.confidence = objProbs[i];
            d.left       = clamp_i(x1 / sx, 0, width);
            d.top        = clamp_i(y1 / sy, 0, height);
            d.right      = clamp_i(x2 / sx, 0, width);
            d.bottom     = clamp_i(y2 / sy, 0, height);
            out.push_back(d);
        }
    }

    rknn_outputs_release(det->ctx, det->io_num.n_output, outputs.data());

    SC_LOG_DEBUG("Inference: %d candidates -> %zu detections", validCount, out.size());
    return 0;
}

void detector_get_input_size(const Detector* det, int& w, int& h, int& c) {
    if (det) { w = det->model_width; h = det->model_height; c = det->model_channels; }
}

#else  // ---- Stub (no RKNN) ----

struct Detector { float conf = 0.25f; };
Detector* detector_create(const RknnConfig&) {
    SC_LOG_WARN("RKNN not available -- detector is a no-op stub");
    return new Detector();
}
void detector_destroy(Detector* d) { delete d; }
int detector_infer(Detector*, const uint8_t*, size_t, int, int, int,
                   std::vector<Detection>&) { return 0; }
void detector_get_input_size(const Detector*, int& w, int& h, int& c) {
    w = 640; h = 640; c = 3;
}

#endif  // SOULCAM_HAVE_RKNN

}  // namespace sc
