# RKAIQ OV5647 — Complete Investigation & Calibration Report

_Board: Vicharak (RK3566) · Camera: RPi Camera (G) (OV5647 160° fisheye) · RKAIQ v6.0x8.0_
_Updated: 2026-02-08_

---

## Issues Addressed

| # | Issue | Status | Summary |
|---|-------|--------|---------|
| 1 | `stats dqbuf bytes=0` in RKAIQ logs | **FIXED** | Bug in RKAIQ — `bytesused` never copied from kernel DQBUF |
| 2 | AWB stats invalid / purple color cast | **FIXED** | AWB stats valid; purple caused by miscalibrated IQ file. Gray-world calibrated. |
| 3 | `rkisp-media-setup.service` missing | **FIXED** | Service created and enabled at boot |

---

## 1. Environment

```
Kernel:     6.1.0-1027-rockchip
ISP HW:     v21 (rkisp-vir0)
RKAIQ:      v6.0x8.0 (2024-09-13)
Sensor:     OV5647 @ i2c 0x36, Bayer SGBRG10_1X10, 2592×1944
Camera:     Waveshare RPi Camera (G) — OV5647 + 160° fisheye lens
```

### Device Nodes

| Node | Purpose |
|------|---------|
| `/dev/video7` | rkcif (raw CIF) |
| `/dev/video8` | **rkisp_mainpath** (ISP processed output) |
| `/dev/video9` | rkisp_selfpath |
| `/dev/video16` | rkisp-statistics (3A stats from ISP) |
| `/dev/video17` | rkisp-input-params (3A params to ISP) |
| `/dev/v4l-subdev0` | rkisp-isp-subdev |
| `/dev/v4l-subdev1` | rkisp-csi-subdev |
| `/dev/v4l-subdev2` | ov5647 sensor |
| `/dev/media1` | ISP media device |

### Systemd Services

| Service | Status | Purpose |
|---------|--------|---------|
| `rkaiq-3a.service` | active (running) | RKAIQ 3A server daemon |
| `rkisp-media-setup.service` | active (exited) | Media pad format setup at boot |

---

## 2. Issue #1: `stats dqbuf bytes=0` (RKAIQ Bug)

### Symptom

RKAIQ log showed `stats dqbuf dev(/dev/video16) seq:N bytes:0 len:14750` for every
frame, even though the kernel correctly set `bytesused=14750` in `VIDIOC_DQBUF`.

### Root Cause

In `V4l2Device::dequeue_buffer()` (`rkaiq/xcore/v4l2_device.cpp`), after `VIDIOC_DQBUF`,
the code copies `timestamp`, `timecode`, `sequence`, and `length` from the ioctl result
to the buffer pool entry, but **never copies `bytesused`**. The pool entry retains
`bytesused=0` from initialization.

### Fix (3 files)

**`rkaiq/xcore/v4l2_buffer_proxy.h`** — Added setter:

```cpp
void set_bytesused (const uint32_t value) {
    _buf.bytesused = value;
}
```

**`rkaiq/xcore/v4l2_device.cpp`** — Added after `set_sequence()` in `dequeue_buffer()`:

```cpp
buf->set_bytesused (v4l2_buf.bytesused);
```

**`rkaiq/hwi/isp20/Stream.cpp`** — Removed duplicate `fprintf(stderr, "stats dqbuf ...")`
block that produced the same message twice.

### Verification

After rebuild and deploy of `librkaiq.so`, logs now show `bytes:14750` (correct).

---

## 3. Issue #2: Purple Color Cast (IQ File Miscalibration)

### Investigation Path

1. **Kernel ISP verified OK** — `isp3a_ris=0x1C77` includes RAWAWB (BIT 11),
   `meas_type` BIT(5) set, `bytesused=14750`. All 3A measurements valid.
2. **AWB stats valid during streaming** — "AWB stats invalid" messages are boot-time only.
3. **Source binary verified** — BuildID of source tree matches running `librkaiq.so`.
4. **IQ file identified as root cause** — multiple factors in `ov5647_rpi-camera-v1p3_default.json`.

### Root Causes (4 factors)

| Factor | Details |
|--------|---------|
| **Miscalibrated AWB gains** | Default D65: R=2.05, B=1.35. Correct for this sensor: R≈0.95, B≈1.30. Red channel ~2× too high. |
| **Overly aggressive CCM** | 100% CCM matrices (especially incandescent 'A') amplify mild cast into strong purple. |
| **AWB auto-convergence drift** | UV/XY region calibration in IQ file wrong for this sensor; auto AWB converges to a non-neutral point and drifts over time. |
| **Sensor gain-dependent color shift** | At higher analog gain (darker scenes), color shifts toward warm/purple. Fixed gains can't fully compensate all brightness levels. |

### IQ File Search — No Better Version Exists

All publicly available OV5647 IQ files for RK356x contain **identical** AWB/CCM calibration:

| Source | File | AWB D65 R/B | Same? |
|--------|------|-------------|-------|
| Device (Vicharak) | `ov5647_rpi-camera-v1p3_default.json` | 2.05 / 1.35 | baseline |
| [OpenHD/rockchip-iq-files](https://github.com/OpenHD/rockchip-iq-files) | `rockchip-iqfiles-rk356x/ov5647_rpi-camera-v1p3_default.json` | 2.05 / 1.35 | **identical** |
| [radxa-pkg/rockchip-iqfiles](https://github.com/radxa-pkg/rockchip-iqfiles) | `rockchip-iqfiles-rk356x/ov5647_rpi-camera-v1p3_default.json` | 2.05 / 1.35 | **identical** |
| OpenHD (OKDO module) | `ov5647_OKDO-5MP_default.json` | 2.05 / 1.35 | **identical** |
| TinkerBoard, Toybrick, Khadas, Gitee | Not found / older XML only | — | — |

The `compat.json` on the device has the same calibration data but uses older enum names
(`CALIB_AWB_BLK_STAT_MODE_ALL_V201` instead of `_AL_V201`) and **crashes RKAIQ v6.0x8.0**.

### Fix: Gray-World AWB Calibration

Since no better IQ file exists and no ColorChecker is available, we used a **gray-world
iterative calibration** method, inspired by the
[Waveshare RPi Camera (G) wiki FAQ](https://www.waveshare.net/wiki/RPi_Camera_(G))
which provides a similar approach for Pi cameras (`rg, bg = (1.8, 1.4)` with iterative
adjustment).

#### Method

1. Set IQ file to **manual WB mode**:
   - `wb_v21.control.mode` = `"CALIB_WB_MODE_MANUAL"`
   - `wb_v21.manualPara.mode` = `"CALIB_MWB_MODE_WBGAIN"`
   - `wb_v21.manualPara.cfg.mwbGain` = `[R, 1.0, 1.0, B]`
2. Capture 60 UYVY frames via `/dev/video8` (ISP mainpath)
3. Convert UYVY→RGB, compute per-frame mean R/G and B/G ratios
4. Apply damped gray-world correction:
   - `new_R = old_R / (1 + 0.7 × (R/G − 1))`
   - `new_B = old_B / (1 + 0.7 × (B/G − 1))`
5. Iterate until R/G ≈ 1.0 and B/G ≈ 1.0
6. Average across all brightness levels to compensate for gain-dependent shift
7. Test CCM at 0%, 30%, 50%, 70% to find best tradeoff

#### Key Bug Found During Calibration

The IQ file JSON key for manual WB gains is **`mwbGain`** (lowercase 'w').
Using `mWbGain` (capital 'W') silently creates a new key that RKAIQ ignores — the
original `mwbGain` values are still read. This wasted significant debugging time
because gain changes appeared to have no effect.

#### Convergence Results

| Iteration | R gain | B gain | R/G | B/G | Error |
|-----------|--------|--------|-----|-----|-------|
| Start (from prior tests) | 1.300 | 1.520 | 1.126 | 1.029 | 0.155 |
| Iter 2 | 1.195 | 1.490 | 1.068 | 1.008 | 0.076 |
| Iter 3 | 1.140 | 1.482 | 1.038 | 0.997 | 0.041 |
| **Iter 4 (converged)** | **1.110** | **1.485** | **1.018** | **0.998** | **0.020** |
| Compensated for all brightness | **0.950** | **1.300** | **1.000** | **0.994** | **0.006** |

#### Final Calibrated Configuration

```
Manual WB gains:  R = 0.950,  Gr = 1.0,  Gb = 1.0,  B = 1.300
CCM strength:     30% (identity × 0.7 + original × 0.3)
Default illuminant: D65
IQ file:          /etc/iqfiles/ov5647_rpi-camera-v1p3_calibrated.json
Symlink:          /etc/iqfiles/ov5647_LMM248_YXC-M804A2.json → calibrated.json
```

#### Before vs After (120-frame verification)

| Metric | Default IQ (purple) | Calibrated IQ | Improvement |
|--------|---------------------|---------------|-------------|
| R/G (overall avg) | 1.535 | **1.011** | 97% closer to 1.0 |
| B/G (overall avg) | 1.426 | **1.006** | 99% closer to 1.0 |
| Total error | 0.961 | **0.017** | **56× reduction** |
| R/G (bright, G>100) | ~1.4 | **1.018** | Near-perfect |
| R/G (dark, G<50) | ~1.6 | **1.032** | Very good |
| Visual | Strong purple cast | Natural colors | Purple eliminated |

---

## 4. Issue #3: `rkisp-media-setup.service`

### Problem

No service existed to set up the ISP media pipeline formats at boot. Without this,
the ISP pad formats could be incorrect.

### Fix

Created `/usr/local/bin/rkisp_media_setup_ov5647.sh` and
`/etc/systemd/system/rkisp-media-setup.service`.

Key corrections during setup:
- Changed from `rockchip-csi2-dphy0` to `rockchip-csi2-dphy1` (actual HW)
- Set `rkisp-isp-subdev:2` to full sensor resolution `YUYV8_2X8/2592x1944`
  (not downscaled, which caused `EINVAL`)

---

## 5. RKAIQ AWB Data Flow (Verified Working)

```
Sensor (OV5647, Bayer SGBRG10)
  → rockchip-csi2-dphy1
  → rkisp-csi-subdev
  → rkisp-isp-subdev (pad0: Sink, Bayer in)
  → ISP HW processes Bayer → YUV
  → rkisp-isp-subdev (pad2: Source, YUYV out)
  → rkisp_mainpath (/dev/video8, user captures here)

  ISP also produces:
  → pad3: rkisp-statistics (/dev/video16, 3A stats out)
  ← pad1: rkisp-input-params (/dev/video17, 3A params in)

RKAIQ 3A server:
  ← DQBUF /dev/video16 (stats: AE, AWB, AF measurements)
  → Process 3A algorithms
  → QBUF /dev/video17 (params: AE gains, AWB gains, CCM, etc.)
```

### ISP 3A Interrupt Status

`isp3a_ris = 0x1C77`:
- BIT(0) = AWB done
- BIT(1) = AE done  
- BIT(2) = AF done
- BIT(4) = histogram done
- BIT(6) = RAWAWB done
- BIT(10) = RAWAE(big) done
- BIT(11) = RAWAWB done (v21)
- BIT(12) = RAWAF done (v21)

All measurement engines functional.

---

## 6. Files Modified on Device

### Production files

| Path | Purpose |
|------|---------|
| `/etc/iqfiles/ov5647_rpi-camera-v1p3_calibrated.json` | **Production IQ** — gray-world calibrated |
| `/etc/iqfiles/ov5647_LMM248_YXC-M804A2.json` | Symlink → `calibrated.json` |
| `/usr/local/lib/librkaiq.so` | Rebuilt with `bytes:0` fix |
| `/usr/local/bin/rkisp_media_setup_ov5647.sh` | Media pipeline setup script |
| `/etc/systemd/system/rkisp-media-setup.service` | Systemd unit for boot setup |

### RKAIQ source patches (on device)

| File | Change |
|------|--------|
| `rkaiq/xcore/v4l2_buffer_proxy.h` | Added `set_bytesused()` method |
| `rkaiq/xcore/v4l2_device.cpp` | Copy `bytesused` in `dequeue_buffer()` |
| `rkaiq/hwi/isp20/Stream.cpp` | Removed duplicate `fprintf` |

### Calibration scripts

| File | Purpose |
|------|---------|
| `tmp/manual_awb_calibrate.py` | Gray-world iterative AWB calibration (main script) |
| `tmp/grayworld_calibrate.py` | Earlier auto-AWB calibration attempt |

---

## 7. Remaining Limitations

1. **Fixed WB gains** — Manual mode cannot adapt to different lighting (e.g., tungsten
   vs daylight). Acceptable for fixed-environment cameras.
2. **CCM at 30%** — Provides mild color correction. Increase to 50–70% for more vivid
   colors (slight neutral shift as trade-off).
3. **Auto AWB broken** — The IQ file's UV/XY region calibration data is fundamentally
   wrong for this sensor. Fixing it requires a Macbeth ColorChecker under multiple
   standard illuminants.
4. **Gain-dependent color shift** — OV5647 sensor shifts color at high analog gain
   (darker scenes). Partially compensated by averaging, but dark scenes may still show
   a slight warm tint (R/G ≈ 1.03 vs ideal 1.00).
5. **Non-fatal startup error** — `set stats meta format failed: -2` at RKAIQ startup.
   Stats streaming works correctly despite this.

---

## 8. Quick Reference

### Switch IQ file

```bash
# Apply calibrated IQ (production)
sudo ln -sf /etc/iqfiles/ov5647_rpi-camera-v1p3_calibrated.json \
            /etc/iqfiles/ov5647_LMM248_YXC-M804A2.json
sudo systemctl restart rkaiq-3a.service

# Revert to default IQ (has purple cast)
sudo ln -sf /etc/iqfiles/ov5647_rpi-camera-v1p3_default.json \
            /etc/iqfiles/ov5647_LMM248_YXC-M804A2.json
sudo systemctl restart rkaiq-3a.service
```

### Capture test image

```bash
# UYVY capture (640×480, 30 frames)
v4l2-ctl -d /dev/video8 \
  --set-fmt-video=width=640,height=480,pixelformat=UYVY \
  --stream-mmap --stream-count=30 \
  --stream-to=/dev/shm/test.raw

# Full resolution (2592×1944, 20 frames)
v4l2-ctl -d /dev/video8 \
  --set-fmt-video=width=2592,height=1944,pixelformat=UYVY \
  --stream-mmap --stream-count=20 \
  --stream-to=/dev/shm/fullres.raw
```

### Check 3A health

```bash
# Verify RKAIQ running
systemctl status rkaiq-3a.service

# Check stats output (should show bytes:14750)
journalctl -u rkaiq-3a.service --since "10 sec ago" | grep "stats dqbuf"

# Check AWB validity
journalctl -u rkaiq-3a.service --since "30 sec ago" | grep "AWB stats invalid"
```

### ISP debug

```bash
# Enable ISP kernel debug
echo 1 | sudo tee /sys/module/video_rkisp/parameters/debug

# kprobe ISP 3A interrupt values
echo 'p:s rkisp_stats_send_meas_v21 ris=+8(%x1):u32' | \
  sudo tee /sys/kernel/debug/tracing/kprobe_events

# strace RKAIQ ioctls
sudo strace -f -e ioctl -p $(pgrep rkaiq_3A_server | head -1) 2>&1 | grep -E "DQBUF|QBUF"
```

### Re-run gray-world calibration

```bash
# If lighting changes significantly, re-calibrate:
sudo python3 /dev/shm/manual_awb_calibrate.py
# Or copy from repo:
scp tmp/manual_awb_calibrate.py ubuntu@192.168.1.45:/dev/shm/
```
