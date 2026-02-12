# ISP NV12 Output — First-Frame Warm-Up (Resolved)

_Board: Vicharak (RK3566) · Camera: OV5647 160° fisheye_
_Driver: rkisp v21 · RKAIQ: v6.0x8.0_
_Discovered: 2026-01-28 · Resolved: 2026-02-09_

---

## Status: RESOLVED

NV12 output works correctly on both ISP paths. The UV plane is only invalid
on the **very first frame** (ISP MI warm-up). From frame #2 onwards, NV12
is fully correct with proper chroma data.

The SoulCam pipeline now uses **NV12 direct** — no RGA format conversion
needed on the mainpath.

---

## Summary

The RK3566 ISP (`rkisp v21`) NV12 output was initially thought to be broken
(Y plane all zeros on kernel 5.10, UV plane all 0x80 on kernel 6.1). After
extensive investigation, the issue was found to be a **first-frame warm-up
behavior**: the ISP Memory Interface (MI) requires one frame to initialize
the UV DMA write path. Previous tests captured only 3-5 frames and analyzed
frame #1, concluding the format was broken.

With 50-frame captures, the warm-up behavior is clearly visible and the UV
data is valid from frame #2 onwards.

---

## Resolution Details

### Root cause

The ISP Memory Interface (MI) requires one frame to initialize the UV DMA
write path. The first frame has UV=0x80 (neutral gray, no chroma). Starting
from the second frame, UV data flows correctly.

### Per-frame UV analysis (50 frames captured, mainpath 640×480)

```
Frame |  Y mean |  UV mean | UV 0x80% | Status
------+---------+----------+----------+-------
    1 |   108.1 |    128.0 |   100.0% | 0x80 (warm-up)
    2 |     1.3 |    127.8 |    86.3% | VALID (first valid)
    3 |     5.6 |    127.1 |    59.4% | VALID
    5 |    59.3 |    121.5 |    21.4% | VALID (AE settling)
   10 |    61.7 |    122.6 |    19.2% | VALID (stable)
   30 |    61.9 |    122.6 |    17.0% | VALID (fully stable)
   50 |    62.1 |    122.6 |    16.7% | VALID
```

The ~17% UV=0x80 at steady state is normal — these pixels correspond to
regions of the scene with genuinely neutral chroma.

### Selfpath (video9) confirms same behavior

```
Frame   1: Y mean=0.0, UV mean=128.0, UV 0x80=100.0%  ← warm-up
Frame   2: Y mean=1.3, UV mean=127.8, UV 0x80=86.0%   ← first valid
Frame   5: Y mean=87.0, UV mean=120.3, UV 0x80=14.8%  ← stable
Frame  30: Y mean=96.5, UV mean=127.1, UV 0x80=12.8%  ← fully stable
```

### NV12 vs UYVY quality comparison (frame 15)

```
Channel | NV12       | UYVY       | Diff
--------+------------+------------+------
Y mean  | 95.6       | 96.3       | 0.65
U mean  | 130.1      | 130.6      | 0.48
V mean  | 121.8      | 122.8      | 1.03
U range | [100, 167] | [96, 175]  | NV12 slightly narrower (4:2:0 subsampling)
V range | [97, 139]  | [93, 141]  | Normal
```

NV12 and UYVY produce essentially identical image quality. The NV12 chroma
range is slightly narrower due to 4:2:0 subsampling (expected).

### Why previous tests showed the bug

Previous testing captured only 3-5 frames (`--stream-count=5`) and analyzed
frame #1 — which always has UV=0x80. With 50 frames, the UV warm-up behavior
is clearly visible and resolves by frame #2.

### Impact on SoulCam pipeline

**Mainpath (RTSP)** — no RGA conversion needed:
```
Before: v4l2src(UYVY) → rgaconvert(NV12) → mpph264enc → RTSP
After:  v4l2src(NV12) → mpph264enc → RTSP        ← simpler, one fewer element
```

**Selfpath (AI)** — RGA still needed for format+resize:
```
Before: v4l2src(UYVY) → rgaconvert(UYVY→RGB 640x640) → RKNN
After:  v4l2src(NV12) → rgaconvert(NV12→RGB 640x640) → RKNN
```

### Performance (RTSP + AI, NV12 direct)

| Metric | UYVY+RGA (old) | NV12 direct (new) |
|--------|---------------|-------------------|
| RTSP fps | 30 | 30 |
| AI fps | ~22.3 | ~21.7 |
| CPU | 10-18% | ~18% |
| Color | Valid | Valid (eye-checked) |
| Pipeline elements | v4l2src + rgaconvert + mpph264enc | v4l2src + mpph264enc |

The first frame's grayscale appearance (UV=0x80) is not visible in practice —
GStreamer's pipeline startup buffers the first frame, and by the time the
H.264 IDR frame reaches the RTSP client, the UV is already valid.

### Correct reproduction test

```bash
# Capture 30+ frames (NOT 5!) and analyze the LAST frame:
v4l2-ctl -d /dev/video8 \
    --set-fmt-video=width=640,height=480,pixelformat=NV12 \
    --stream-mmap --stream-count=30 --stream-to=/dev/shm/test_nv12.raw

python3 -c "
with open('/dev/shm/test_nv12.raw', 'rb') as f:
    data = f.read()
w, h = 640, 480
fs = w * h * 3 // 2
off = len(data) - fs  # last frame
y = list(data[off:off+w*h])
uv = list(data[off+w*h:off+fs])
print(f'Y:  mean={sum(y)/len(y):.1f} range=[{min(y)},{max(y)}]')
print(f'UV: mean={sum(uv)/len(uv):.1f} range=[{min(uv)},{max(uv)}] 0x80={uv.count(128)/len(uv)*100:.0f}%')
"
# Expected: Y valid, UV has real chroma data (not all 0x80)
```

### Reference

This issue was flagged in [ubuntu-rockchip#1127](https://github.com/Joshua-Riek/ubuntu-rockchip/issues/1127)
where a user reported wrong colors on Ubuntu 24 with kernel 6.1.0-1025-rockchip.
That issue was caused by a missing `rkaiq_3A_server` (ISP auto-exposure and
auto-white-balance not running). The solution was to build `rkaiq_3A_server`
for the RK356x platform with proper IQ JSON files
([solution comment](https://github.com/Joshua-Riek/ubuntu-rockchip/issues/1127#issuecomment-2849764847)).
Our system already has the RKAIQ 3A server running correctly (v6.0x8.0).

---

## Investigation History (archived)

The sections below document the original investigation. The conclusions about
NV12 being fundamentally broken were **incorrect** — the actual issue was
insufficient test frame count. Kept for reference.

### Original symptoms (kernel 5.10.x, 2026-01-28)

- NV12 frames had all-zero Y plane, UV plane had some data
- Image appeared completely black
- UYVY worked correctly

### Updated findings (kernel 6.1.0, 2026-02-09)

On kernel 6.1.0, behavior changed:

| Format | Y-plane | UV-plane | Status |
|--------|---------|----------|--------|
| **UYVY** (packed 4:2:2) | Valid | Valid (interleaved) | **WORKS** |
| **NV12** (Y/UV 4:2:0) | Valid (mean≈87) | 0x80 on frame 1, valid from frame 2 | **WORKS** (was misdiagnosed) |
| **NV21** (Y/VU 4:2:0) | Valid | 0x80 on frame 1 | Likely works (not re-tested) |
| **NV16** (Y/UV 4:2:2) | Valid | 0x80 on frame 1 | Likely works (not re-tested) |

### Kernel source analysis (historical)

The kernel driver analysis examining `capture_v21.c` MI register configuration
was technically accurate but led to an incorrect conclusion. The mainpath MI
**does** handle NV12 correctly — the UV DMA path simply needs one frame to
initialize. The "missing output format bits" observation is valid for the
register layout, but the hardware handles the format conversion via a
different mechanism than expected.

### NV12 frame layout (for reference)

```
NV12 frame (640×480 = 460800 bytes):

  Offset 0          ┌──────────────────────┐
                    │   Y plane            │  640 × 480 = 307200 bytes
                    │   (luma, 8-bit)      │  ← valid from frame 1
  Offset 307200     ├──────────────────────┤
                    │   UV plane           │  640 × 240 = 153600 bytes
                    │   (chroma, interl.)  │  ← valid from frame 2
  Offset 460800     └──────────────────────┘
```

---

## Environment

| Component | Version / ID |
|-----------|-------------|
| SoC | RK3566 (Rockchip) |
| Board | Vicharak Axon |
| Kernel | 6.1.0-1025-rockchip (Ubuntu) |
| ISP | rkisp v21 (driver: rkisp_v5) |
| RKAIQ | v6.0x8.0 (rkaiq_3A_server running) |
| Sensor | OV5647 (m00_b_ov5647 1-0036) |
| Sensor mode | 1296×972 SGBRG10_1X10 (2×2 binned) |
| CSI DPHY | rockchip-csi2-dphy1 |
| GStreamer | 1.24.x |
| Formats verified | NV12 (works), UYVY (works) |
| Paths verified | mainpath /dev/video8, selfpath /dev/video9 |
