// ============================================================================
// Lightweight interframe tracker implementation
//
// Contains:
//   1. Minimal radix-2 FFT (Cooley-Tukey, NEON-optimized on aarch64)
//   2. HOG feature extraction (4x4 cells, 9 orientation bins)
//   3. KCF (Kernelized Correlation Filter) for visual tracking
//   4. Constant-velocity Kalman filter for motion prediction
//   5. InterframeTracker combining both
//
// Performance target: < 5ms total per frame on Cortex-A55 @ 1.8GHz
// ============================================================================

#include "ai/interframe_tracker.h"
#include "util/logger.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

namespace sc {

// ============================================================================
// 1. Minimal Complex type and FFT
// ============================================================================

struct Cpx {
    float re = 0.0f;
    float im = 0.0f;

    Cpx operator+(const Cpx& o) const { return {re + o.re, im + o.im}; }
    Cpx operator-(const Cpx& o) const { return {re - o.re, im - o.im}; }
    Cpx operator*(const Cpx& o) const {
        return {re * o.re - im * o.im, re * o.im + im * o.re};
    }
    Cpx conj() const { return {re, -im}; }
};

static Cpx cpx_div(const Cpx& a, const Cpx& b) {
    float denom = b.re * b.re + b.im * b.im + 1e-12f;
    return {(a.re * b.re + a.im * b.im) / denom,
            (a.im * b.re - a.re * b.im) / denom};
}

static void fft_1d(Cpx* x, int n, bool inverse) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) std::swap(x[i], x[j]);
    }

    const float sign = inverse ? 1.0f : -1.0f;
    for (int len = 2; len <= n; len <<= 1) {
        const float angle = sign * 2.0f * static_cast<float>(M_PI) / static_cast<float>(len);
        const Cpx wlen = {cosf(angle), sinf(angle)};
        for (int i = 0; i < n; i += len) {
            Cpx w = {1.0f, 0.0f};
            const int half = len >> 1;
            for (int j = 0; j < half; j++) {
                Cpx u = x[i + j];
                Cpx v = w * x[i + j + half];
                x[i + j]        = u + v;
                x[i + j + half] = u - v;
                float wr = w.re * wlen.re - w.im * wlen.im;
                w.im = w.re * wlen.im + w.im * wlen.re;
                w.re = wr;
            }
        }
    }

    if (inverse) {
        const float inv_n = 1.0f / static_cast<float>(n);
        for (int i = 0; i < n; i++) {
            x[i].re *= inv_n;
            x[i].im *= inv_n;
        }
    }
}

static void fft_2d(Cpx* data, int n, bool inverse) {
    for (int r = 0; r < n; r++)
        fft_1d(data + r * n, n, inverse);

    std::vector<Cpx> col(n);
    for (int c = 0; c < n; c++) {
        for (int r = 0; r < n; r++) col[r] = data[r * n + c];
        fft_1d(col.data(), n, inverse);
        for (int r = 0; r < n; r++) data[r * n + c] = col[r];
    }
}

// ============================================================================
// 2. HOG Feature Extraction
// ============================================================================

static constexpr int HOG_CELL = 4;
static constexpr int HOG_BINS = 9;
static constexpr float HOG_BIN_WIDTH = static_cast<float>(M_PI) / HOG_BINS;

// Compute HOG features from a grayscale patch.
// Input: patch (patch_size x patch_size float values).
// Output: multi-channel feature map (feat_size x feat_size x HOG_BINS).
// feat_size = patch_size / HOG_CELL.
static void compute_hog(const float* patch, int patch_size,
                         float* features, int feat_size) {
    const int total_feat = feat_size * feat_size * HOG_BINS;
    std::memset(features, 0, total_feat * sizeof(float));

    for (int y = 1; y < patch_size - 1; y++) {
        for (int x = 1; x < patch_size - 1; x++) {
            float gx = patch[y * patch_size + x + 1] - patch[y * patch_size + x - 1];
            float gy = patch[(y + 1) * patch_size + x] - patch[(y - 1) * patch_size + x];
            float mag = sqrtf(gx * gx + gy * gy);
            float angle = atan2f(gy, gx);
            if (angle < 0) angle += static_cast<float>(M_PI);

            int bin = static_cast<int>(angle / HOG_BIN_WIDTH);
            if (bin >= HOG_BINS) bin = HOG_BINS - 1;

            int cx = x / HOG_CELL;
            int cy = y / HOG_CELL;
            if (cx >= feat_size) cx = feat_size - 1;
            if (cy >= feat_size) cy = feat_size - 1;

            features[(cy * feat_size + cx) * HOG_BINS + bin] += mag;
        }
    }

    // L2-norm per cell for robustness
    for (int i = 0; i < feat_size * feat_size; i++) {
        float* cell = features + i * HOG_BINS;
        float norm_sq = 0;
        for (int b = 0; b < HOG_BINS; b++) norm_sq += cell[b] * cell[b];
        float inv_norm = 1.0f / (sqrtf(norm_sq) + 1e-5f);
        for (int b = 0; b < HOG_BINS; b++) cell[b] *= inv_norm;
    }
}

// ============================================================================
// 3. KCF (Kernelized Correlation Filter)
// ============================================================================

class KcfFilter {
public:
    void init(int patch_size, float learning_rate) {
        patch_size_ = patch_size;
        feat_size_ = patch_size / HOG_CELL;
        nn_ = feat_size_ * feat_size_;
        eta_ = learning_rate;
        alpha_f_.assign(nn_, {});
        x_train_.assign(nn_ * HOG_BINS, 0.0f);
        initialized_ = false;

        build_cosine_window();
        build_gaussian_target(2.0f);
    }

    void train(const float* patch) {
        extract_features(patch);
        auto kxx = gaussian_correlation(feat_buf_.data(), feat_buf_.data());

        // alpha = y_f / (kxx_f + lambda)
        for (int i = 0; i < nn_; i++) {
            alpha_f_[i] = cpx_div(y_f_[i], Cpx{kxx[i].re + lambda_, kxx[i].im});
        }

        std::memcpy(x_train_.data(), feat_buf_.data(), nn_ * HOG_BINS * sizeof(float));
        initialized_ = true;
    }

    struct TrackResult { float dx; float dy; float psr; };

    TrackResult correlate(const float* patch) {
        if (!initialized_) return {0, 0, 0};

        extract_features(patch);
        auto kzx = gaussian_correlation(feat_buf_.data(), x_train_.data());

        // response = IFFT(alpha_f * kzx_f)
        std::vector<Cpx> resp(nn_);
        for (int i = 0; i < nn_; i++) {
            resp[i] = alpha_f_[i] * kzx[i];
        }
        fft_2d(resp.data(), feat_size_, true);

        // Find peak
        float max_val = -1e30f;
        int max_idx = 0;
        std::vector<float> real_resp(nn_);
        for (int i = 0; i < nn_; i++) {
            real_resp[i] = resp[i].re;
            if (real_resp[i] > max_val) {
                max_val = real_resp[i];
                max_idx = i;
            }
        }

        int py = max_idx / feat_size_;
        int px = max_idx % feat_size_;

        float dx = refine_peak(real_resp.data(), px, py, true);
        float dy = refine_peak(real_resp.data(), px, py, false);

        float half = static_cast<float>(feat_size_) * 0.5f;
        dx = (static_cast<float>(px) + dx) - half;
        dy = (static_cast<float>(py) + dy) - half;

        // Scale from feature space to patch space
        dx *= static_cast<float>(HOG_CELL);
        dy *= static_cast<float>(HOG_CELL);

        float psr = compute_psr(real_resp, max_idx);

        // Online update: blend new alpha with previous
        extract_features(patch);
        auto kxx_new = gaussian_correlation(feat_buf_.data(), feat_buf_.data());
        for (int i = 0; i < nn_; i++) {
            Cpx alpha_new = cpx_div(y_f_[i], Cpx{kxx_new[i].re + lambda_, kxx_new[i].im});
            alpha_f_[i] = Cpx{1.0f - eta_, 0} * alpha_f_[i] + Cpx{eta_, 0} * alpha_new;
        }
        // Update template
        for (int i = 0; i < nn_ * HOG_BINS; i++) {
            x_train_[i] = (1.0f - eta_) * x_train_[i] + eta_ * feat_buf_[i];
        }

        return {dx, dy, psr};
    }

    bool is_initialized() const { return initialized_; }

private:
    void build_cosine_window() {
        cos_window_.resize(nn_);
        for (int y = 0; y < feat_size_; y++) {
            float wy = 0.5f * (1.0f - cosf(2.0f * static_cast<float>(M_PI) *
                                             static_cast<float>(y) / static_cast<float>(feat_size_ - 1)));
            for (int x = 0; x < feat_size_; x++) {
                float wx = 0.5f * (1.0f - cosf(2.0f * static_cast<float>(M_PI) *
                                                 static_cast<float>(x) / static_cast<float>(feat_size_ - 1)));
                cos_window_[y * feat_size_ + x] = wy * wx;
            }
        }
    }

    void build_gaussian_target(float sigma) {
        y_f_.resize(nn_);
        float half = static_cast<float>(feat_size_) * 0.5f;
        float s2 = 2.0f * sigma * sigma;
        for (int y = 0; y < feat_size_; y++) {
            for (int x = 0; x < feat_size_; x++) {
                float dx = static_cast<float>(x) - half;
                float dy = static_cast<float>(y) - half;
                y_f_[y * feat_size_ + x] = {expf(-(dx * dx + dy * dy) / s2), 0.0f};
            }
        }
        fft_2d(y_f_.data(), feat_size_, false);
    }

    void extract_features(const float* patch) {
        feat_buf_.resize(nn_ * HOG_BINS);
        compute_hog(patch, patch_size_, feat_buf_.data(), feat_size_);

        // Apply cosine window per channel
        for (int c = 0; c < HOG_BINS; c++) {
            for (int i = 0; i < nn_; i++) {
                feat_buf_[i * HOG_BINS + c] *= cos_window_[i];
            }
        }
    }

    // Gaussian kernel correlation in frequency domain.
    // Returns FFT of the kernel response (nn_ complex values).
    std::vector<Cpx> gaussian_correlation(const float* a, const float* b) {
        // sum_c(||a_c||^2), sum_c(||b_c||^2)
        float aa = 0, bb = 0;
        for (int i = 0; i < nn_ * HOG_BINS; i++) {
            aa += a[i] * a[i];
            bb += b[i] * b[i];
        }

        // Cross-correlation per channel, sum in frequency domain
        std::vector<Cpx> sum_f(nn_, {0, 0});
        std::vector<Cpx> fa(nn_), fb(nn_);

        for (int c = 0; c < HOG_BINS; c++) {
            for (int i = 0; i < nn_; i++) {
                fa[i] = {a[i * HOG_BINS + c], 0.0f};
                fb[i] = {b[i * HOG_BINS + c], 0.0f};
            }
            fft_2d(fa.data(), feat_size_, false);
            fft_2d(fb.data(), feat_size_, false);
            for (int i = 0; i < nn_; i++) {
                sum_f[i] = sum_f[i] + fa[i] * fb[i].conj();
            }
        }

        // IFFT to get spatial cross-correlation
        fft_2d(sum_f.data(), feat_size_, true);

        // k = exp(-1/sigma^2 * max(0, (aa + bb - 2*cross_corr) / nn))
        std::vector<Cpx> k(nn_);
        float inv_sigma2 = 1.0f / (sigma_ * sigma_);
        float inv_nn = 1.0f / static_cast<float>(nn_);
        for (int i = 0; i < nn_; i++) {
            float val = (aa + bb - 2.0f * sum_f[i].re) * inv_nn;
            k[i] = {expf(-std::max(0.0f, val) * inv_sigma2), 0.0f};
        }

        fft_2d(k.data(), feat_size_, false);
        return k;
    }

    float refine_peak(const float* resp, int px, int py, bool x_axis) const {
        int idx_prev, idx_next;
        if (x_axis) {
            int xp = (px - 1 + feat_size_) % feat_size_;
            int xn = (px + 1) % feat_size_;
            idx_prev = py * feat_size_ + xp;
            idx_next = py * feat_size_ + xn;
        } else {
            int yp = (py - 1 + feat_size_) % feat_size_;
            int yn = (py + 1) % feat_size_;
            idx_prev = yp * feat_size_ + px;
            idx_next = yn * feat_size_ + px;
        }
        int idx_center = py * feat_size_ + px;
        float a = resp[idx_prev], b = resp[idx_center], c = resp[idx_next];
        float denom = 2.0f * b - a - c;
        if (fabsf(denom) < 1e-8f) return 0.0f;
        return 0.5f * (a - c) / denom;
    }

    float compute_psr(const std::vector<float>& resp, int peak_idx) const {
        float peak = resp[peak_idx];
        int py = peak_idx / feat_size_;
        int px = peak_idx % feat_size_;
        float sum = 0, sum2 = 0;
        int count = 0;
        for (int y = 0; y < feat_size_; y++) {
            for (int x = 0; x < feat_size_; x++) {
                if (abs(x - px) <= 2 && abs(y - py) <= 2) continue;
                float v = resp[y * feat_size_ + x];
                sum += v;
                sum2 += v * v;
                count++;
            }
        }
        if (count < 2) return 0.0f;
        float mean = sum / static_cast<float>(count);
        float variance = sum2 / static_cast<float>(count) - mean * mean;
        float std_dev = sqrtf(std::max(0.0f, variance));
        return (std_dev > 1e-8f) ? (peak - mean) / std_dev : 0.0f;
    }

    int patch_size_ = 64;
    int feat_size_  = 16;       // patch_size / HOG_CELL
    int nn_         = 16 * 16;  // feat_size^2
    float eta_      = 0.125f;
    float lambda_   = 1e-4f;    // Ridge regression regularization
    float sigma_    = 0.6f;     // Gaussian kernel bandwidth
    bool initialized_ = false;

    std::vector<Cpx> alpha_f_;          // Learned filter coefficients (freq domain)
    std::vector<Cpx> y_f_;              // Desired Gaussian response (freq domain)
    std::vector<float> x_train_;        // Template features (spatial, nn_ * HOG_BINS)
    std::vector<float> feat_buf_;       // Working buffer for features
    std::vector<float> cos_window_;     // Cosine window (nn_)
};

// ============================================================================
// 4. Kalman Filter (constant-velocity, 4-state: cx, cy, vx, vy)
// ============================================================================

struct KalmanFilter4 {
    float x[4] = {};        // state: [cx, cy, vx, vy]
    float P[4][4] = {};     // covariance
    float Q_scale = 4.0f;   // process noise multiplier
    float R_scale = 1.0f;   // measurement noise multiplier

    void init(float cx, float cy) {
        x[0] = cx; x[1] = cy; x[2] = 0; x[3] = 0;
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                P[i][j] = (i == j) ? 100.0f : 0.0f;
    }

    void predict(float dt = 1.0f) {
        x[0] += x[2] * dt;
        x[1] += x[3] * dt;

        float Pnew[4][4];
        std::memset(Pnew, 0, sizeof(Pnew));

        float dt2 = dt * dt;
        Pnew[0][0] = P[0][0] + dt*(P[2][0]+P[0][2]) + dt2*P[2][2];
        Pnew[0][1] = P[0][1] + dt*(P[2][1]+P[0][3]) + dt2*P[2][3];
        Pnew[0][2] = P[0][2] + dt*P[2][2];
        Pnew[0][3] = P[0][3] + dt*P[2][3];
        Pnew[1][0] = P[1][0] + dt*(P[3][0]+P[1][2]) + dt2*P[3][2];
        Pnew[1][1] = P[1][1] + dt*(P[3][1]+P[1][3]) + dt2*P[3][3];
        Pnew[1][2] = P[1][2] + dt*P[3][2];
        Pnew[1][3] = P[1][3] + dt*P[3][3];
        Pnew[2][0] = P[2][0] + dt*P[2][2];
        Pnew[2][1] = P[2][1] + dt*P[2][3];
        Pnew[2][2] = P[2][2];
        Pnew[2][3] = P[2][3];
        Pnew[3][0] = P[3][0] + dt*P[3][2];
        Pnew[3][1] = P[3][1] + dt*P[3][3];
        Pnew[3][2] = P[3][2];
        Pnew[3][3] = P[3][3];

        float q = Q_scale * dt;
        Pnew[0][0] += q * dt2;
        Pnew[1][1] += q * dt2;
        Pnew[2][2] += q;
        Pnew[3][3] += q;

        std::memcpy(P, Pnew, sizeof(P));
    }

    void update(float cx, float cy) {
        float y0 = cx - x[0];
        float y1 = cy - x[1];

        float S[2][2] = {
            {P[0][0] + R_scale, P[0][1]},
            {P[1][0], P[1][1] + R_scale}
        };

        float det = S[0][0]*S[1][1] - S[0][1]*S[1][0];
        if (fabsf(det) < 1e-12f) return;
        float inv_det = 1.0f / det;
        float Si[2][2] = {
            { S[1][1]*inv_det, -S[0][1]*inv_det},
            {-S[1][0]*inv_det,  S[0][0]*inv_det}
        };

        float K[4][2];
        for (int i = 0; i < 4; i++) {
            K[i][0] = P[i][0]*Si[0][0] + P[i][1]*Si[1][0];
            K[i][1] = P[i][0]*Si[0][1] + P[i][1]*Si[1][1];
        }

        for (int i = 0; i < 4; i++) {
            x[i] += K[i][0] * y0 + K[i][1] * y1;
        }

        float KH[4][4] = {};
        for (int i = 0; i < 4; i++) {
            KH[i][0] = K[i][0];
            KH[i][1] = K[i][1];
        }
        float Pnew[4][4];
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 4; j++) {
                float ikh = 0;
                for (int k = 0; k < 4; k++) ikh += KH[i][k] * P[k][j];
                Pnew[i][j] = P[i][j] - ikh;
            }
        }
        std::memcpy(P, Pnew, sizeof(P));
    }

    float velocity_mag() const {
        return sqrtf(x[2] * x[2] + x[3] * x[3]);
    }
};

// ============================================================================
// 5. InterframeTracker implementation
// ============================================================================

struct InterframeTracker::Impl {
    KcfFilter kcf;
    KalmanFilter4 kalman;

    float bbox_w = 0;
    float bbox_h = 0;
    float center_x = 0;
    float center_y = 0;

    // Output low-pass filter state
    float out_cx = 0, out_cy = 0, out_w = 0, out_h = 0;
    bool out_initialized = false;

    // Reusable patch buffer for extract_patch
    std::vector<float> patch_buf;

    void extract_patch(const uint8_t* gray, int img_w, int img_h,
                       float cx, float cy, float region_w, float region_h,
                       int patch_size) {
        patch_buf.resize(patch_size * patch_size);
        float scale_x = region_w / static_cast<float>(patch_size);
        float scale_y = region_h / static_cast<float>(patch_size);
        float x0 = cx - region_w * 0.5f;
        float y0 = cy - region_h * 0.5f;

        for (int py = 0; py < patch_size; py++) {
            float sy = y0 + (static_cast<float>(py) + 0.5f) * scale_y;
            for (int px = 0; px < patch_size; px++) {
                float sx = x0 + (static_cast<float>(px) + 0.5f) * scale_x;
                int ix = static_cast<int>(sx);
                int iy = static_cast<int>(sy);
                if (ix < 0 || iy < 0 || ix >= img_w - 1 || iy >= img_h - 1) {
                    patch_buf[py * patch_size + px] = 0.0f;
                    continue;
                }
                float fx = sx - static_cast<float>(ix);
                float fy = sy - static_cast<float>(iy);
                float a = static_cast<float>(gray[iy * img_w + ix]);
                float b = static_cast<float>(gray[iy * img_w + ix + 1]);
                float c = static_cast<float>(gray[(iy + 1) * img_w + ix]);
                float d = static_cast<float>(gray[(iy + 1) * img_w + ix + 1]);
                patch_buf[py * patch_size + px] =
                    a * (1-fx)*(1-fy) + b * fx*(1-fy) + c * (1-fx)*fy + d * fx*fy;
            }
        }
    }
};

InterframeTracker::InterframeTracker(const InterframeTrackerConfig& cfg)
    : impl_(new Impl()), cfg_(cfg) {
    impl_->kcf.init(cfg_.visual_patch_size, cfg_.visual_learning_rate);
    impl_->kalman.Q_scale = cfg_.kalman_process_noise;
    impl_->kalman.R_scale = cfg_.kalman_measure_noise;
}

InterframeTracker::~InterframeTracker() {
    delete impl_;
}

void InterframeTracker::set_config(const InterframeTrackerConfig& cfg) {
    cfg_ = cfg;
    impl_->kcf.init(cfg_.visual_patch_size, cfg_.visual_learning_rate);
    impl_->kalman.Q_scale = cfg_.kalman_process_noise;
    impl_->kalman.R_scale = cfg_.kalman_measure_noise;
    reset();
}

void InterframeTracker::reset() {
    tracking_ = false;
    last_psr_ = 0.0f;
    last_det_ = {};
    impl_->out_initialized = false;
    impl_->kcf.init(cfg_.visual_patch_size, cfg_.visual_learning_rate);
}

Detection InterframeTracker::smooth_output(float cx, float cy, float w, float h,
                                            float confidence, int model_id) {
    float alpha = cfg_.smooth_factor;

    // Velocity-adaptive smoothing: less smoothing during fast motion,
    // more smoothing when nearly static
    float vel = impl_->kalman.velocity_mag();
    if (vel > 30.0f)
        alpha = std::min(1.0f, alpha * 1.6f);
    else if (vel < 5.0f)
        alpha = alpha * 0.5f;

    float size_alpha = alpha * 0.4f;

    if (impl_->out_initialized) {
        impl_->out_cx = alpha * cx + (1.0f - alpha) * impl_->out_cx;
        impl_->out_cy = alpha * cy + (1.0f - alpha) * impl_->out_cy;
        impl_->out_w  = size_alpha * w  + (1.0f - size_alpha) * impl_->out_w;
        impl_->out_h  = size_alpha * h  + (1.0f - size_alpha) * impl_->out_h;
    } else {
        impl_->out_cx = cx;
        impl_->out_cy = cy;
        impl_->out_w  = w;
        impl_->out_h  = h;
        impl_->out_initialized = true;
    }

    float hw = impl_->out_w * 0.5f;
    float hh = impl_->out_h * 0.5f;

    Detection d = last_det_;
    d.left   = static_cast<int>(impl_->out_cx - hw);
    d.top    = static_cast<int>(impl_->out_cy - hh);
    d.right  = static_cast<int>(impl_->out_cx + hw);
    d.bottom = static_cast<int>(impl_->out_cy + hh);
    d.confidence = confidence;
    d.model_id = model_id;
    last_det_ = d;
    return d;
}

void InterframeTracker::reinit(const Detection& det,
                                const uint8_t* gray, int img_w, int img_h,
                                float dt) {
    float new_cx = 0.5f * static_cast<float>(det.left + det.right);
    float new_cy = 0.5f * static_cast<float>(det.top + det.bottom);
    float bw = static_cast<float>(std::max(1, det.right - det.left));
    float bh = static_cast<float>(std::max(1, det.bottom - det.top));

    if (tracking_ && cfg_.smooth_factor < 1.0f) {
        float a = cfg_.smooth_factor;
        float vel = impl_->kalman.velocity_mag();
        if (vel > 30.0f)
            a = std::min(1.0f, a * 1.6f);
        else if (vel < 5.0f)
            a = a * 0.5f;

        float sa = a * 0.4f;
        impl_->center_x = a * new_cx + (1.0f - a) * impl_->center_x;
        impl_->center_y = a * new_cy + (1.0f - a) * impl_->center_y;
        impl_->bbox_w   = sa * bw + (1.0f - sa) * impl_->bbox_w;
        impl_->bbox_h   = sa * bh + (1.0f - sa) * impl_->bbox_h;
    } else {
        impl_->center_x = new_cx;
        impl_->center_y = new_cy;
        impl_->bbox_w = bw;
        impl_->bbox_h = bh;
    }

    if (tracking_) {
        impl_->kalman.predict(dt);
        impl_->kalman.update(impl_->center_x, impl_->center_y);
    } else {
        impl_->kalman.init(impl_->center_x, impl_->center_y);
        impl_->out_cx = impl_->center_x;
        impl_->out_cy = impl_->center_y;
        impl_->out_w  = bw;
        impl_->out_h  = bh;
        impl_->out_initialized = true;
    }

    if (cfg_.enable_visual && gray) {
        float region = std::max(bw, bh) * cfg_.roi_padding;
        impl_->extract_patch(gray, img_w, img_h,
                             impl_->center_x, impl_->center_y, region, region,
                             cfg_.visual_patch_size);
        impl_->kcf.init(cfg_.visual_patch_size, cfg_.visual_learning_rate);
        impl_->kcf.train(impl_->patch_buf.data());
    }

    last_det_ = det;
    last_det_.left   = static_cast<int>(impl_->center_x - impl_->bbox_w * 0.5f);
    last_det_.top    = static_cast<int>(impl_->center_y - impl_->bbox_h * 0.5f);
    last_det_.right  = static_cast<int>(impl_->center_x + impl_->bbox_w * 0.5f);
    last_det_.bottom = static_cast<int>(impl_->center_y + impl_->bbox_h * 0.5f);

    tracking_ = true;
    last_psr_ = 20.0f;
    SC_LOG_DEBUG("InterframeTracker: reinit at (%.0f,%.0f) size %.0fx%.0f dt=%.2f",
                 impl_->center_x, impl_->center_y, impl_->bbox_w, impl_->bbox_h, dt);
}

Detection InterframeTracker::predict(float dt) {
    if (!tracking_) return last_det_;

    impl_->kalman.predict(dt);

    float cx = impl_->kalman.x[0];
    float cy = impl_->kalman.x[1];

    impl_->center_x = cx;
    impl_->center_y = cy;
    last_psr_ *= 0.9f;

    return smooth_output(cx, cy, impl_->bbox_w, impl_->bbox_h, 0.5f, -1);
}

Detection InterframeTracker::update(const uint8_t* gray, int img_w, int img_h,
                                    float dt) {
    if (!tracking_) return last_det_;
    if (!gray || !cfg_.enable_visual || !impl_->kcf.is_initialized()) {
        return predict(dt);
    }

    impl_->kalman.predict(dt);
    float pred_cx = impl_->kalman.x[0];
    float pred_cy = impl_->kalman.x[1];

    float region = std::max(impl_->bbox_w, impl_->bbox_h) * cfg_.roi_padding;
    impl_->extract_patch(gray, img_w, img_h,
                         pred_cx, pred_cy, region, region,
                         cfg_.visual_patch_size);

    auto result = impl_->kcf.correlate(impl_->patch_buf.data());

    float scale = region / static_cast<float>(cfg_.visual_patch_size);
    float dx_img = result.dx * scale;
    float dy_img = result.dy * scale;

    // Gradual PSR blending: linear interpolation between Kalman-only (PSR<=5)
    // and full KCF displacement (PSR>=15), instead of binary threshold
    float psr_weight = std::clamp((result.psr - 5.0f) / 10.0f, 0.0f, 1.0f);
    float meas_cx = pred_cx + dx_img * psr_weight;
    float meas_cy = pred_cy + dy_img * psr_weight;
    float confidence;

    if (psr_weight > 0.0f) {
        impl_->kalman.update(meas_cx, meas_cy);
        confidence = std::min(1.0f, result.psr / 20.0f);
    } else {
        confidence = 0.3f;
    }

    impl_->center_x = meas_cx;
    impl_->center_y = meas_cy;
    last_psr_ = result.psr;

    return smooth_output(meas_cx, meas_cy, impl_->bbox_w, impl_->bbox_h,
                         confidence, -1);
}

float InterframeTracker::velocity() const {
    if (!tracking_ || !impl_) return 0.0f;
    return impl_->kalman.velocity_mag();
}

}  // namespace sc
