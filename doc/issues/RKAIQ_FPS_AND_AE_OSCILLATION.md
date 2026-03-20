# RKAIQ Low FPS & AE Oscillation

Investigation and fix for camera FPS stuck at 14.6 and auto-exposure
brightness flickering on OV5647 + RK3566 (rkisp v21, RKAIQ v6.0x8.0).

**Date**: 2026-03-19
**Device**: RK3566 / OV5647 160° fisheye, ISP selfpath 640×480 NV12
**IQ file**: `/etc/iqfiles/ov5647_rpi-camera-v1p3_calibrated.json`
(symlinked from `ov5647_LMM248_YXC-M804A2.json`)

---

## 1. Symptoms

| Symptom | Description |
|---------|-------------|
| **Low FPS** | Camera stuck at 14.6 fps in bright lighting where 43 fps is expected |
| **AE oscillation** | After light on/off cycle, image flickers bright↔dark every frame |
| **FPS inconsistency** | Same lighting conditions could yield 14.6 or 43 fps across restarts |

---

## 2. Root Causes Found

### 2.1 FpsValue=15 in IQ file (primary cause of low FPS)

The IQ file had `AecFrameRateMode.FpsValue = 15`, telling RKAIQ to target
15 fps. RKAIQ enforced this by inflating the sensor's `vertical_blanking`
register to ~996, even in bright conditions where the exposure was only
89 lines (~2ms) and gain was at minimum (16).

**Evidence:**
```
# Bright room, FPS stuck at 14.6
exposure: 89          ← only 4.5% of max (1964)
analogue_gain: 16     ← minimum possible
vertical_blanking: 996  ← inflated by RKAIQ (min is 24)
```

FPS calculation with the sensor parameters:
```
pixel_rate  = 81,666,700 Hz
line_time   = (1296 + 600) / pixel_rate = 23.22 μs
vblank=24:  frame = (972 + 24)  × 23.22μs = 23.1ms → 43.2 fps
vblank=996: frame = (972 + 996) × 23.22μs = 45.7ms → 21.9 fps
```

The gap between the theoretical 21.9 fps (at vblank=996) and the measured
14.6 fps was caused by anti-flicker quantization (see 2.2).

**Proof:** Manually setting `vblank=24` via v4l2-ctl immediately raised
FPS from 14.6 → 27.8 fps, confirming the vblank was the bottleneck.

### 2.2 Anti-flicker at 50Hz (secondary — quantized FPS downward)

`AecAntiFlicker.enable = 1` with `Frequency = 50Hz` forced exposure times
to multiples of 10ms (half-cycle of 50Hz). With FpsValue=15, the target
frame time of 66.7ms snapped to 70ms → **14.3 fps** (matching observed 14.6).

### 2.3 AE limit cycle (the persistent flickering)

The flickering was caused by an **AE limit cycle** — a periodic
sawtooth oscillation in exposure and gain with ~800ms period. This was
invisible to slow polling (≥300ms between samples) but clearly visible
at 50ms polling intervals:

```
# 50ms rapid poll showing the full oscillation cycle:
exposure: 896 analogue_gain: 38   ← peak (too bright)
exposure: 896 analogue_gain: 32   ← AE ramping down
exposure: 896 analogue_gain: 27
exposure: 896 analogue_gain: 25
exposure: 896 analogue_gain: 20
exposure: 896 analogue_gain: 19   ← approaching target
exposure: 896 analogue_gain: 19
exposure: 834 analogue_gain: 16   ← hit min gain, exposure breaks anti-flicker
exposure: 773 analogue_gain: 16   ← no longer synced to 50Hz
exposure: 710 analogue_gain: 16   ← trough (too dark)
exposure: 896 analogue_gain: 19   ← snap back to anti-flicker quantum
exposure: 896 analogue_gain: 25   ← overshoots
exposure: 896 analogue_gain: 38   ← back to peak → cycle repeats
```

Frame brightness measurement confirmed **83% variation** (47.6 – 157.8
on a 0–255 scale), clearly visible as flickering.

**Mechanism**: The anti-flicker quantum (896 lines ≈ 20ms) at minimum
gain (16 = 1×) produced an image slightly brighter than the AE target.
The AE reduced exposure below the anti-flicker quantum, desynchronizing
from the 50Hz mains frequency. The resulting mains flicker in the stats
caused the AE to overshoot back to peak gain. With 2–3 frames of sensor
register delay and only 0.6 damping (40% correction per frame), the
overshoot was enough to sustain a permanent limit cycle.

### 2.4 Damping too aggressive + tolerance too tight

With `DampOver/Under = 0.6`, the AE adjusted by 40% of the error per
frame. At 42fps with 2–3 frame sensor delay, the effective loop delay
caused systematic overshoot. The `ToleranceIn/Out = 10` (10%) was too
tight — the AE continuously corrected small brightness errors instead of
accepting them. The `DyDamp.SlowRange = 15` was too narrow — the
oscillation amplitude exceeded 15%, so the slow damping (0.95) never
activated.

### 2.5 isFpsFix=1 at FpsValue=30 locked FPS too low

Setting `isFpsFix=1, FpsValue=30` successfully stopped vblank oscillation
(locked at 522 for ~30fps sensor rate). But the slower camera rate
combined with YOLO inference backpressure reduced the effective AI
pipeline rate to 19.2 fps.

---

## 3. Final Fix

Changes to the IQ file at
`/etc/iqfiles/ov5647_rpi-camera-v1p3_calibrated.json`:

### 3.1 IQ file changes (current working configuration)

| Parameter | Path in JSON | Original | Final | Why |
|-----------|-------------|----------|-------|-----|
| FpsValue | `...AecFrameRateMode.FpsValue` | 15 | **43** | Target max sensor FPS; minimizes vblank |
| isFpsFix | `...AecFrameRateMode.isFpsFix` | 0 | **1** | Lock vblank to minimum — prevents AE from inflating it |
| AntiFlicker enable | `...AecAntiFlicker.enable` | 1 | **1** | Keep 50Hz sync to prevent mains flicker |
| AntiFlicker Mode | `...AecAntiFlicker.Mode` | AUTO | **NORMAL** | Strict quantization — AE can never break the anti-flicker quantum |
| DampOver | `...AecSpeed.DampOver` | 0.15 | **0.98** | Only 2% correction per frame — prevents overshoot |
| DampUnder | `...AecSpeed.DampUnder` | 0.15 | **0.98** | Same |
| DampDark2Bright | `...AecSpeed.DampDark2Bright` | 0.15 | **0.98** | Same |
| DampBright2Dark | `...AecSpeed.DampBright2Dark` | 0.15 | **0.98** | Same |
| DyDamp.SlowRange | `...AecSpeed.DyDamp.SlowRange` | 15 | **100** | Always use slow damping (covers full error range) |
| DyDamp.SlowDamp | `...AecSpeed.DyDamp.SlowDamp` | 0.95 | **0.995** | Near target: only 0.5% correction per frame |
| ToleranceIn | `...LinearAeCtrl.ToleranceIn` | 10 | **25** | Accept 25% brightness error without correction |
| ToleranceOut | `...LinearAeCtrl.ToleranceOut` | 10 | **25** | Same |
| CISMinFps | `sensor_calib.CISMinFps` | 10 | **15** | Prevent AE from dropping below 15fps in very low light |

The key insight: the limit cycle was caused by the AE breaking the
anti-flicker exposure quantum when it ran out of gain headroom. Switching
anti-flicker from AUTO to NORMAL mode forces strict quantization — the
AE can never drop exposure below the 50Hz-synced value. Combined with
extreme damping (0.98), the AE converges slowly (~10–15s from startup)
but never overshoots.

### 3.2 Code change (isp_config.cpp)

Added sensor state diagnostic logging at startup. Reports vblank, exposure,
gain, and estimated FPS after ISP configuration. Warns if vblank is high
despite short exposure (indicates IQ file misconfiguration).

```
Sensor state: vblank=24 exposure=134 gain=16 → est 43.2 fps (requested vblank=24 → 43.2 fps)
```

### 3.3 How to apply

```bash
# Edit IQ file on device
sudo python3 -c "
import json
path = '/etc/iqfiles/ov5647_rpi-camera-v1p3_calibrated.json'
with open(path) as f:
    iq = json.load(f)
ae = iq['main_scene'][0]['sub_scene'][0]['scene_isp21']['ae_calib']
ae['CommCtrl']['AecFrameRateMode'] = {'isFpsFix': 1, 'FpsValue': 43}
ae['CommCtrl']['AecAntiFlicker'] = {
    'enable': 1,
    'Frequency': 'AECV2_FLICKER_FREQUENCY_50HZ',
    'Mode': 'AECV2_ANTIFLICKER_NORMAL_MODE'
}
ae['CommCtrl']['AecSpeed'] = {
    'SmoothEn': 1,
    'DampOver': 0.98, 'DampUnder': 0.98,
    'DampDark2Bright': 0.98, 'DampBright2Dark': 0.98,
    'DyDamp': {
        'DyDampEn': 1, 'SlowOPType': 'RK_AIQ_OP_MODE_AUTO',
        'SlowRange': 100, 'SlowDamp': 0.995
    }
}
linear = ae.get('LinearAeCtrl', {})
linear['ToleranceIn'] = 25
linear['ToleranceOut'] = 25
iq['sensor_calib']['CISMinFps'] = 15
with open(path, 'w') as f:
    json.dump(iq, f, indent=2)
print('Done')
"

# IMPORTANT: clean restart sequence (order matters)
sudo systemctl stop soulcam
sleep 1
sudo systemctl restart rkaiq-3a
sleep 2
sudo dmesg -C
sudo systemctl start soulcam
```

---

## 4. Results

| Metric | Original | After initial fix | After comprehensive fix |
|--------|----------|-------------------|------------------------|
| Camera FPS (bright) | 14.6 | 28 | **41.0** |
| YOLO FPS | 1.6–1.8 | 3.2–4.2 | 5.6–8.6 |
| Tracker FPS | 12.8–13.0 | 24–25 | 33–35 |
| Total AI pipeline FPS | ~14.6 | ~28 | **41.0** |
| AE stability | 3.3× oscillation | Improved | **1.9% variation (stable)** |
| Brightness range (0-255) | N/A | N/A | 157.9–160.9 |
| vblank (bright) | 996 (inflated) | 24 (free) | 70 (locked by isFpsFix=1) |
| AE convergence from startup | instant | instant | ~10–15s (tradeoff for stability) |

---

## 5. Tradeoffs & Known Limitations

### Slow AE convergence from startup (~10–15s)

With damping at 0.98, the AE converges exponentially: each frame corrects
only 2% of the remaining error. From the initial exposure point to steady
state takes approximately 10–15 seconds. During this time, the image
brightness ramps smoothly (no oscillation) from init values to the correct
level. This is acceptable for a persistent surveillance camera.

### Reduced AE responsiveness to lighting changes

The extreme damping means the camera adapts slowly to sudden lighting
changes (e.g., lights on/off). A step change takes ~5–10 seconds to fully
settle. The `DyDamp.SlowRange=100` ensures slow damping applies across
the full error range, preventing the fast mode from causing overshoot.

### Low-light FPS reduction

With `isFpsFix=1, FpsValue=43`, the vblank is locked low, so FPS stays
near 43 fps regardless of lighting. However, in very dark conditions,
exposure time + gain may max out (40ms exposure, 15.5× gain), and the
image will be dark. The `CISMinFps=15` setting acts as a fallback floor.

### YOLO backpressure limits effective FPS

Even at 43fps sensor rate, the effective AI pipeline rate is ~41fps because
YOLO inference (~100ms on NPU) blocks the appsink callback, causing
GStreamer to drop frames (`max-buffers=2 drop=true`). This is by design —
the tracker fills the gap between YOLO frames.

---

## 6. Diagnostic Commands

```bash
# Check current sensor AE state
v4l2-ctl -d /dev/v4l-subdev2 --get-ctrl=vertical_blanking,exposure,analogue_gain

# CRITICAL: rapid poll at 50ms to detect AE limit cycles
# (slower polls ≥300ms will miss the ~800ms oscillation cycle)
for i in $(seq 1 60); do
  v4l2-ctl -d /dev/v4l-subdev2 \
    --get-ctrl=exposure,analogue_gain 2>&1 | tr '\n' ' '
  echo; sleep 0.05
done

# Measure actual frame brightness (objective flicker test)
python3 -c "
import subprocess, time
bs = []
for i in range(15):
    fn = f'/tmp/frame_{i}.raw'
    subprocess.run(['timeout','3','ffmpeg','-y','-rtsp_transport','tcp',
        '-i','rtsp://127.0.0.1:8554/cam','-vframes','1',
        '-vf','format=gray','-f','rawvideo',fn],
        capture_output=True, timeout=5)
    with open(fn,'rb') as f: d=f.read()
    if len(d)>100:
        avg=sum(d)/len(d); bs.append(avg)
        print(f'Frame {i}: brightness={avg:.1f}')
    time.sleep(0.05)
mn,mx=min(bs),max(bs); avg=sum(bs)/len(bs)
print(f'Range: {mx-mn:.1f} ({(mx-mn)/avg*100:.1f}%)')
print('STABLE' if (mx-mn)/avg*100<5 else 'FLICKERING')
"

# Check AI pipeline FPS
sudo journalctl -u soulcam --since "15s ago" | grep "AI pipeline"

# Check RKAIQ errors
sudo journalctl -u rkaiq-3a --since "30s ago" | tail -20

# Read current IQ file AE config
python3 -c "
import json
with open('/etc/iqfiles/ov5647_rpi-camera-v1p3_calibrated.json') as f:
    iq = json.load(f)
ae = iq['main_scene'][0]['sub_scene'][0]['scene_isp21']['ae_calib']
c = ae['CommCtrl']
print('FpsValue:', c['AecFrameRateMode']['FpsValue'])
print('isFpsFix:', c['AecFrameRateMode']['isFpsFix'])
print('AntiFlicker:', c['AecAntiFlicker'])
print('AecSpeed:', c['AecSpeed'])
linear = ae.get('LinearAeCtrl', {})
print('ToleranceIn:', linear.get('ToleranceIn'))
print('ToleranceOut:', linear.get('ToleranceOut'))
print('CISMinFps:', iq['sensor_calib']['CISMinFps'])
"
```

---

## 7. Key Learnings

1. **RKAIQ controls sensor FPS via vblank**, not via V4L2 framerate caps.
   The `framerate=30/1` in GStreamer v4l2src is ignored by the ISP driver.
   Actual FPS is determined by `vertical_blanking` which RKAIQ sets based
   on the IQ file's `AecFrameRateMode.FpsValue`.

2. **Anti-flicker AUTO mode can break its own quantization.** When the AE
   can't reach the target brightness within the anti-flicker constraints
   (e.g., at minimum gain with exposure locked to a 50Hz quantum), AUTO
   mode silently drops anti-flicker and allows intermediate exposure values.
   This desynchronizes from mains frequency, causing stats to oscillate,
   which creates a sustained limit cycle. **Use NORMAL mode** to force
   strict quantization — the AE must stay at the quantum even if it can't
   perfectly reach the target brightness.

3. **Poll at ≥20Hz to detect AE oscillation.** The limit cycle had an
   ~800ms period (1.25Hz). Polling at 0.3s (3.3Hz) barely captured it;
   0.5s polls missed it entirely. The gain appeared "stuck at 248" in a
   single reading because the poll happened to hit the same phase. Always
   use 50ms polls (20Hz) for at least 3 seconds to reliably detect
   oscillation patterns.

4. **Damping of 0.6 is dangerously aggressive at >30fps.** With 2–3
   frames of sensor register delay, 0.6 damping (40% correction/frame)
   causes overshoot at high frame rates. Use 0.95–0.98 for stable
   convergence. The tradeoff (slow startup convergence) is acceptable for
   always-on cameras.

5. **The code's vblank=24 in isp_config.cpp is overridden by RKAIQ**
   within seconds of startup. The IQ file is the authoritative source for
   sensor timing parameters.

6. **Higher camera FPS improves effective AI pipeline FPS** even beyond
   what the AI processing can consume, because more frames are available
   in the appsink queue after YOLO inference completes (fewer dropped).

7. **Measure frame brightness, not just sensor registers.** V4L2 controls
   only show sensor-side AE. ISP-internal processing (digital gain, AWB,
   gamma) can also cause brightness variation. Use ffmpeg to capture RTSP
   frames and compare grayscale averages for objective flicker detection.

8. **Clean restart sequence is critical.** Always stop soulcam first, then
   restart rkaiq-3a, then start soulcam. Restarting rkaiq while soulcam is
   running corrupts ISP state (`rkisp: no bay3d buffer available`) and can
   cut the RTSP stream.
