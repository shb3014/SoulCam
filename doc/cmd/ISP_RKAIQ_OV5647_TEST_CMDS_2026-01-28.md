# OV5647 ISP/RKAIQ Test Commands (RK3566)

Target: `192.168.1.45` (user: `ubuntu`)

## 0) SSH
```bash
ssh ubuntu@192.168.1.45
```

## 1) Baseline: list nodes and formats
```bash
ls /dev/media* /dev/video*
media-ctl -d /dev/media1 -p
v4l2-ctl -d /dev/video8 --list-formats-ext
```

## 2) MP NV12 capture (drop frame 0)
```bash
v4l2-ctl -d /dev/video8 \
  --set-fmt-video=width=640,height=480,pixelformat=NV12 \
  --stream-mmap --stream-count=5 --stream-to=/tmp/nv12.raw

python3 - <<'PY'
W,H=640,480
frame_size=W*H*3//2
Y=W*H
b=open('/tmp/nv12.raw','rb').read()
frames=len(b)//frame_size
for i in range(frames):
    f=b[i*frame_size:(i+1)*frame_size]
    y=f[:Y]
    print(i,'Ymean',sum(y)/len(y),'Ymin',min(y),'Ymax',max(y))
PY
```

## 3) MP UYVY capture (reference brightness)
```bash
v4l2-ctl -d /dev/video8 \
  --set-fmt-video=width=640,height=480,pixelformat=UYVY \
  --stream-mmap --stream-count=5 --stream-to=/tmp/uyvy.raw

python3 - <<'PY'
b=open('/tmp/uyvy.raw','rb').read()
y=b[1::2]
print('UYVY mean',sum(y)/len(y),'min',min(y),'max',max(y))
PY
```

## 3b) Direct ISP capture (UYVY 1280x960 -> JPG)
Use this to validate ISP output quickly. Output goes to `/tmp`.
```bash
W=1280; H=960
v4l2-ctl -d /dev/video8 \
  --set-fmt-video=width=$W,height=$H,pixelformat=UYVY \
  --stream-mmap --stream-count=5 --stream-to=/tmp/isp_uyvy_${W}x${H}.raw

python3 - <<'PY'
W=1280;H=960;frame=W*H*2
b=open('/tmp/isp_uyvy_1280x960.raw','rb').read()
n=len(b)//frame
last=b[(n-1)*frame:n*frame]
open('/tmp/isp_uyvy_1280x960_last.raw','wb').write(last)
PY

ffmpeg -y -f rawvideo -pixel_format uyvy422 -video_size 1280x960 \
  -i /tmp/isp_uyvy_1280x960_last.raw -frames:v 1 /tmp/isp_uyvy_1280x960.jpg
```
Notes:
- Use `--stream-mmap` (do not pass `=4`), or `VIDIOC_REQBUFS` can fail.
- If colors look “gray/purple” on the first few frames, AWB/AE may not have converged yet.
  Prefer the **warmup** variant below.

## 3b-2) Direct ISP capture with warmup (recommended for eye-check)
This reduces “early frames purple/gray” caused by AWB/AE not converged.
```bash
W=1280; H=960
RAW=/tmp/isp_uyvy_${W}x${H}_warm.raw
LAST=/tmp/isp_uyvy_${W}x${H}_warm_last.raw

rm -f "$RAW" "$LAST"
v4l2-ctl -d /dev/video8 \
  --set-fmt-video=width=$W,height=$H,pixelformat=UYVY \
  --stream-mmap --stream-count=90 --stream-to="$RAW"

python3 - <<'PY'
W=1280;H=960;frame=W*H*2
b=open("/tmp/isp_uyvy_1280x960_warm.raw","rb").read()
n=len(b)//frame
last=b[(n-1)*frame:n*frame]
open("/tmp/isp_uyvy_1280x960_warm_last.raw","wb").write(last)
print("frames",n)
PY

ffmpeg -y -f rawvideo -pixel_format uyvy422 -video_size 1280x960 \
  -i "$LAST" -frames:v 1 /tmp/isp_uyvy_${W}x${H}_warm.jpg
```

## 3c) UYVY chroma sanity (U/V std + pct around 128)
Use this to validate whether U/V are “nearly constant” (grayscale-like).
```bash
W=1280; H=960
OUT=/tmp/uyvy_${W}x${H}.raw
LAST=/tmp/uyvy_${W}x${H}_last.raw
rm -f "$OUT" "$LAST"

v4l2-ctl -d /dev/video8 \
  --set-fmt-video=width=$W,height=$H,pixelformat=UYVY \
  --stream-mmap --stream-count=5 --stream-to="$OUT"

python3 - <<'PY'
import math
W=1280;H=960
frame=W*H*2
b=open("/tmp/uyvy_1280x960.raw","rb").read()
n=len(b)//frame
last=b[(n-1)*frame:n*frame]
open("/tmp/uyvy_1280x960_last.raw","wb").write(last)

U=last[0::4]
V=last[2::4]
Y=last[1::2]

def stats(name, arr):
    mean=sum(arr)/len(arr)
    std=(sum((x-mean)**2 for x in arr)/len(arr))**0.5
    cnt=sum(1 for x in arr if 126<=x<=130)
    print(name,"mean",round(mean,3),"std",round(std,3),
          "min",min(arr),"max",max(arr),
          "pct_128_pm2",round(cnt/len(arr)*100,2))

stats("U", U)
stats("V", V)
stats("Y", Y)
PY
```

## 3d) UYVY gray-world ratio (R/G, B/G) from last frame
This is a file-level metric; use it to compare different Bayer settings.
```bash
W=1280; H=960
LAST=/tmp/uyvy_${W}x${H}_last.raw
RGB=/tmp/uyvy_${W}x${H}_last.rgb
rm -f "$RGB"

ffmpeg -hide_banner -loglevel error -y \
  -f rawvideo -pixel_format uyvy422 -video_size ${W}x${H} \
  -i "$LAST" -frames:v 1 -f rawvideo -pix_fmt rgb24 "$RGB"

python3 - <<'PY'
rgb=open("/tmp/uyvy_1280x960_last.rgb","rb").read()
R=rgb[0::3]; G=rgb[1::3]; B=rgb[2::3]
Rm=sum(R)/len(R); Gm=sum(G)/len(G); Bm=sum(B)/len(B)
print("RGB_mean",round(Rm,3),round(Gm,3),round(Bm,3),
      "R/G",round(Rm/Gm,3),
      "B/G",round(Bm/Gm,3))
PY
```

## 4) Start/stop AIQ (3A server)
```bash
sudo /usr/local/bin/rkaiq_3A_server --silent >/tmp/rkaiq_3A_server.log 2>&1 &
sleep 2
pgrep -a rkaiq_3A_server
tail -n 20 /tmp/rkaiq_3A_server.log
```

Stop:
```bash
sudo pkill -f rkaiq_3A_server
```

## 4b) Fix purple cast via IQ file
If UYVY/NV12 look magenta/purple, point the IQ symlink to the compat file.
```bash
sudo ln -sf /etc/iqfiles/ov5647_rpi-camera-v1p3_compat.json \
  /etc/iqfiles/ov5647_LMM248_YXC-M804A2.json
sudo systemctl restart rkaiq-3a.service
```

## 4c) Root fix: IQ schema mismatch (AIQ v6 parser "unknown enum")
If `rkaiq-3a.service` logs show `unknown enum name` and/or:
- `AWB:E:JsonPara2HwPara blkMeasureMode:-1 is invaild!!!`
- `AWB:E:no latest params !`
- `CAMHW:E:impossible, no effect isp params!`

then the IQ JSON is **not compatible with the current rkaiq build**, and 3A params may not be applied at all.

On target, verify current parser-supported enum names:
```bash
rg -n 'unknown enum name|blkMeasureMode:-1|no latest params|impossible, no effect' \
  <(journalctl -u rkaiq-3a.service --since "10 min ago" --no-pager) || true

# optional: enum definitions (authoritative for this rkaiq build)
python3 - <<'PY'
p="/home/ubuntu/external_camera_engine_rkaiq/build/rkaiq/iq_parser_v2/output.h"
print("output.h exists?", __import__("os").path.exists(p))
PY
```

Applied fix (2026-02-07):
- Install patched IQ: `/etc/iqfiles/ov5647_rpi-camera-v1p3_compat_rkaiq609fix.json`
- Point symlink used by rkaiq: `/etc/iqfiles/ov5647_LMM248_YXC-M804A2.json -> ..._rkaiq609fix.json`
- Restart:
```bash
sudo systemctl restart rkaiq-3a.service
journalctl -u rkaiq-3a.service --since "1 min ago" --no-pager | rg -n 'unknown enum name|blkMeasureMode:-1|no latest params' || true
```

## 5) NV12 with AIQ (auto controls)
```bash
v4l2-ctl -d /dev/video8 \
  --set-ctrl=auto_exposure=0 \
  --set-ctrl=gain_automatic=1 \
  --set-ctrl=white_balance_automatic=1

sudo /usr/local/bin/rkaiq_3A_server --silent >/tmp/rkaiq_3A_server.log 2>&1 &
sleep 2

v4l2-ctl -d /dev/video8 \
  --set-fmt-video=width=640,height=480,pixelformat=NV12 \
  --stream-mmap --stream-count=5 --stream-to=/tmp/nv12_aiq.raw

python3 - <<'PY'
W,H=640,480
frame_size=W*H*3//2
Y=W*H
b=open('/tmp/nv12_aiq.raw','rb').read()
frames=len(b)//frame_size
means=[]
for i in range(1,frames):
    f=b[i*frame_size:(i+1)*frame_size]
    y=f[:Y]
    means.append(sum(y)/len(y))
print('NV12 mean frames 1-4',means,'avg',sum(means)/len(means))
PY

sudo pkill -f rkaiq_3A_server
```

## 6) Stats capture sanity check (while MP is streaming)
```bash
rm -f /tmp/rkisp_stats.bin
v4l2-ctl -d /dev/video8 \
  --set-fmt-video=width=640,height=480,pixelformat=UYVY \
  --stream-mmap --stream-count=5 --stream-to=/tmp/uyvy.raw &
pid=$!
sleep 0.5
timeout 5 v4l2-ctl -d /dev/video16 --stream-mmap --stream-count=5 \
  --stream-to=/tmp/rkisp_stats.bin --verbose
wait $pid || true

python3 - <<'PY'
import os
p='/tmp/rkisp_stats.bin'
print('exists',os.path.exists(p))
if os.path.exists(p):
    d=open(p,'rb').read()
    print('size',len(d),'sum',sum(d),'unique',len(set(d)))
PY
```

## 7) Check controls state
```bash
v4l2-ctl -d /dev/video8 --all | rg -A5 -n 'auto_exposure|gain_automatic|white_balance_automatic|exposure'
```

## 8) RGA (rgaconvert) UYVY -> NV12 validation
Note: uses the rebuilt `rgaconvert` with UYVY support and `imcvtcolor_t`.

Rebuild & install plugin on device:
```bash
cd /home/ubuntu/build/gstreamer-rgaconvert
ninja -C build
sudo cp -f build/plugins/libgstrgaconvert.so /usr/local/lib/aarch64-linux-gnu/gstreamer-1.0/libgstrgaconvert.so
rm -f ~/.cache/gstreamer-1.0/registry.aarch64.bin
GST_PLUGIN_PATH=/usr/local/lib/aarch64-linux-gnu/gstreamer-1.0 gst-inspect-1.0 rgaconvert | rg -n 'UYVY|format' | head -n 6
```

File-based conversion test (no camera):
```bash
v4l2-ctl -d /dev/video8 \
  --set-fmt-video=width=1280,height=960,pixelformat=UYVY \
  --stream-mmap --stream-count=2 --stream-to=/tmp/uyvy_test.raw

GST_PLUGIN_PATH=/usr/local/lib/aarch64-linux-gnu/gstreamer-1.0 \
  gst-launch-1.0 -v filesrc location=/tmp/uyvy_test.raw ! \
  rawvideoparse format=uyvy width=1280 height=960 framerate=30/1 ! \
  rgaconvert ! video/x-raw,format=NV12,width=1280,height=960 ! fakesink
```

Live camera pipeline test:
```bash
GST_PLUGIN_PATH=/usr/local/lib/aarch64-linux-gnu/gstreamer-1.0 \
  gst-launch-1.0 -v v4l2src device=/dev/video8 io-mode=2 num-buffers=30 \
  ! video/x-raw,format=UYVY,width=1280,height=960,framerate=30/1 \
  ! rgaconvert ! video/x-raw,format=NV12,width=1280,height=960 ! fakesink
```

## 9) Systemd services (AIQ + auto controls)
```bash
systemctl is-active rkaiq-3a.service
systemctl status rkaiq-3a.service --no-pager -n 5

systemctl is-enabled rkisp-auto-controls.service
systemctl status rkisp-auto-controls.service --no-pager -n 5
```

Restart services:
```bash
sudo systemctl restart rkaiq-3a.service
sudo systemctl restart rkisp-auto-controls.service
```

## 10) Persistent Bayer fix (no kernel rebuild)
If you see a persistent magenta/purple cast, the sensor may be reporting the wrong Bayer order.
On kernel `6.1.75+` we observed that **forcing `horizontal_flip=1` can double-apply**
the built-in wiring compensation and make colors worse. The current best-known baseline is:
- `horizontal_flip=0`
- `BAYER=SBGGR10_1X10`

Recommended: install a oneshot service that runs `media-ctl` at boot (and can be restarted anytime).

Install on target:
```bash
sudo tee /usr/local/bin/rkisp_media_setup_ov5647.sh >/dev/null <<'SH'
#!/usr/bin/env bash
set -euo pipefail

MEDIA_DEV="${MEDIA_DEV:-/dev/media1}"
SENSOR_W="${SENSOR_W:-2592}"
SENSOR_H="${SENSOR_H:-1944}"
OUT_W="${OUT_W:-1280}"
OUT_H="${OUT_H:-960}"
BAYER="${BAYER:-SBGGR10_1X10}"

# 6.1.75+ baseline: do NOT force hflip=1 (can double-compensate)
v4l2-ctl -d /dev/v4l-subdev3 --set-ctrl=horizontal_flip=0 || true

media-ctl -d "$MEDIA_DEV" --set-v4l2 "\"m00_b_ov5647 1-0036\":0[fmt:${BAYER}/${SENSOR_W}x${SENSOR_H} field:none]"
media-ctl -d "$MEDIA_DEV" --set-v4l2 "\"rockchip-csi2-dphy0\":0[fmt:${BAYER}/${SENSOR_W}x${SENSOR_H} field:none]"
media-ctl -d "$MEDIA_DEV" --set-v4l2 "\"rkisp-csi-subdev\":0[fmt:${BAYER}/${SENSOR_W}x${SENSOR_H} field:none]"
media-ctl -d "$MEDIA_DEV" --set-v4l2 "\"rkisp-csi-subdev\":1[fmt:${BAYER}/${SENSOR_W}x${SENSOR_H} field:none]"
media-ctl -d "$MEDIA_DEV" --set-v4l2 "\"rkisp-isp-subdev\":0[fmt:${BAYER}/${SENSOR_W}x${SENSOR_H} field:none crop:(0,0)/${SENSOR_W}x${SENSOR_H}]"
media-ctl -d "$MEDIA_DEV" --set-v4l2 "\"rkisp-isp-subdev\":2[fmt:YUYV8_2X8/${OUT_W}x${OUT_H} crop:(0,0)/${OUT_W}x${OUT_H}]"
v4l2-ctl -d /dev/video8 --set-fmt-video=width=${OUT_W},height=${OUT_H},pixelformat=UYVY
SH
sudo chmod 0755 /usr/local/bin/rkisp_media_setup_ov5647.sh

sudo tee /etc/systemd/system/rkisp-media-setup.service >/dev/null <<'UNIT'
[Unit]
Description=RKISP media-ctl setup for OV5647
After=multi-user.target

[Service]
Type=oneshot
ExecStart=/usr/local/bin/rkisp_media_setup_ov5647.sh
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
UNIT

sudo systemctl daemon-reload
sudo systemctl enable --now rkisp-media-setup.service
```

Verify:
```bash
systemctl is-active rkisp-media-setup.service
media-ctl -d /dev/media1 --get-v4l2 "\"rkisp-isp-subdev\":0"
```

## 10c) Ensure rkaiq prestart hflip matches baseline
If `rkaiq-3a.service` sets `horizontal_flip` in `ExecStartPre`, make sure it matches
the baseline above (for `6.1.75+`, use `horizontal_flip=0`):
```bash
sudo systemctl edit rkaiq-3a.service
# add:
# [Service]
# ExecStartPre=
# ExecStartPre=/usr/bin/v4l2-ctl -d /dev/v4l-subdev3 --set-ctrl=horizontal_flip=0
sudo systemctl daemon-reload
sudo systemctl restart rkaiq-3a.service
```

## 10d) Snapshot collection (new/old image diff)
在切换镜像前后，用同一套命令采集“系统/服务/媒体链路/IQ/日志/文件 hash”，便于做 diff：

```bash
OUT=/tmp/target_snapshot.txt
: > "$OUT"
{
  echo "## sys"; date; uname -a; cat /etc/os-release 2>/dev/null || true; echo
  echo "## cmdline"; cat /proc/cmdline; echo
  echo "## extlinux (selected)"; python3 - <<'PY' || true
import re
p="/boot/extlinux/extlinux.conf"
try:
  for ln in open(p,errors="ignore"):
    if re.match(r"^(default|label|linux|fdt|append)\b", ln.strip()):
      print(ln.rstrip())
except FileNotFoundError:
  pass
PY
  echo
  echo "## rkaiq bin"; ls -lh /usr/local/bin/rkaiq_3A_server 2>/dev/null || true; readlink -f /usr/local/bin/rkaiq_3A_server 2>/dev/null || true; echo
  echo "## ldd"; ldd /usr/local/bin/rkaiq_3A_server 2>/dev/null || true; echo
  echo "## iqfiles list"; ls -la /etc/iqfiles 2>/dev/null | head -n 200 || true; echo
  echo "## current iq link"; ls -l /etc/iqfiles/ov5647_LMM248_YXC-M804A2.json 2>/dev/null || true; echo
  echo "## services"; systemctl cat rkaiq-3a.service 2>/dev/null || true; echo; systemctl cat rkisp-media-setup.service 2>/dev/null || true; echo
  echo "## media graph"; media-ctl -d /dev/media1 -p 2>/dev/null | sed -n "1,260p" || true; echo
  echo "## v4l2 video8"; v4l2-ctl -d /dev/video8 --all 2>/dev/null | sed -n "1,260p" || true; echo
  echo "## v4l2 sensor subdev3"; v4l2-ctl -d /dev/v4l-subdev3 --all 2>/dev/null | sed -n "1,220p" || true; echo
} >> "$OUT"
cat "$OUT"
```

## 10e) Force-check Bayer code vs horizontal_flip
**经验结论（来自 2026-02-08 的新镜像实测）**：OV5647 的 sensor pad Bayer code 会随 `horizontal_flip` 变化，且 sensor pad 不能稳定被 `media-ctl --set-v4l2` 强制到任意 Bayer。

```bash
echo "=== hflip=0 ==="
sudo v4l2-ctl -d /dev/v4l-subdev3 --set-ctrl=horizontal_flip=0
media-ctl -d /dev/media1 --get-v4l2 "\"m00_b_ov5647 1-0036\":0"
media-ctl -d /dev/media1 --get-v4l2 "\"rkisp-isp-subdev\":0" | head -n 2

echo "=== hflip=1 ==="
sudo v4l2-ctl -d /dev/v4l-subdev3 --set-ctrl=horizontal_flip=1
media-ctl -d /dev/media1 --get-v4l2 "\"m00_b_ov5647 1-0036\":0"
media-ctl -d /dev/media1 --get-v4l2 "\"rkisp-isp-subdev\":0" | head -n 2
```

## 10f) Switch default boot entry (u-boot extlinux)
将默认内核从 `l0` 切到 `l1`（或反之）：

```bash
grep -n '^U_BOOT_DEFAULT' /etc/default/u-boot
sudo cp -a /etc/default/u-boot /etc/default/u-boot.bak.$(date +%s)
sudo python3 - <<'PY'
p="/etc/default/u-boot"
s=open(p).read().splitlines(True)
out=[]
for ln in s:
  if ln.startswith("U_BOOT_DEFAULT="):
    out.append('U_BOOT_DEFAULT="l1"\n')  # or "l0"
  else:
    out.append(ln)
open("/tmp/u-boot.new","w").writelines(out)
PY
sudo mv /tmp/u-boot.new /etc/default/u-boot
sudo u-boot-update
grep -n '^default' /boot/extlinux/extlinux.conf | head -n 3
sudo reboot
```

## 10g) Kernel-side fix: OV5647 HFLIP invert removal (Bayer baseline restore)
当观测到 **`horizontal_flip=0` 时 sensor pad 报 `SBGGR10`，而旧镜像/预期是 `SGBRG10`**，优先考虑驱动里对 HFLIP 的“反向补偿/双重补偿”。

### 10g-1) 验证：Bayer 是否随 hflip 反向切换

```bash
uname -r
for v in 0 1; do
  echo "=== hflip=$v ==="
  sudo v4l2-ctl -d /dev/v4l-subdev3 --set-ctrl=horizontal_flip=$v
  media-ctl -d /dev/media1 --get-v4l2 "\"m00_b_ov5647 1-0036\":0"
done
```

期望（旧镜像基线）：
- `hflip=0` → `SGBRG10_1X10`
- `hflip=1` → `SBGGR10_1X10`

### 10g-2) 内核源码修复点（repo 内）
- 文件：`ubuntu-rockchip-2.4.0/build/linux-rockchip/drivers/media/i2c/ov5647.c`
- 关键点：`V4L2_CID_HFLIP` 控制处理
  - 将 `ov5647_s_flip(..., !ctrl->val)` 改为 `ov5647_s_flip(..., ctrl->val)`

### 10g-3)（本项目实践）构建/部署产物命名建议
为了避免覆盖现有内核，建议用 localversion：
- 例如：`6.1.75-ov5647fix1`
- `extlinux.conf` 增加一个新 label（例如 `l0fix`），验证通过后再切默认。

## 10b) Quick Bayer sweep (compare U/V stats + gray-world)
Requires the installed `/usr/local/bin/rkisp_media_setup_ov5647.sh`.
```bash
run_case(){
  bayer="$1"
  echo "=== BAYER=$bayer ==="
  sudo BAYER="$bayer" /usr/local/bin/rkisp_media_setup_ov5647.sh >/dev/null

  # capture + stats
  bash -lc "$(sed -n '1,999p' /dev/null)" 2>/dev/null || true
  W=1280; H=960
  OUT=/tmp/uyvy_${bayer}.raw
  LAST=/tmp/uyvy_${bayer}_last.raw
  RGB=/tmp/uyvy_${bayer}_last.rgb
  rm -f "$OUT" "$LAST" "$RGB"
  v4l2-ctl -d /dev/video8 --set-fmt-video=width=$W,height=$H,pixelformat=UYVY \
    --stream-mmap --stream-count=5 --stream-to="$OUT"

  python3 - <<PY
import math,subprocess
W=1280;H=960
frame=W*H*2
out="/tmp/uyvy_${bayer}.raw"
lastp="/tmp/uyvy_${bayer}_last.raw"
b=open(out,"rb").read()
n=len(b)//frame
last=b[(n-1)*frame:n*frame]
open(lastp,"wb").write(last)
U=last[0::4]; V=last[2::4]
for name,arr in [("U",U),("V",V)]:
    mean=sum(arr)/len(arr)
    std=(sum((x-mean)**2 for x in arr)/len(arr))**0.5
    cnt=sum(1 for x in arr if 126<=x<=130)
    print(name,"std",round(std,3),"pct_128_pm2",round(cnt/len(arr)*100,2))
rgbp="/tmp/uyvy_${bayer}_last.rgb"
subprocess.check_call(["ffmpeg","-hide_banner","-loglevel","error","-y",
  "-f","rawvideo","-pixel_format","uyvy422","-video_size",f"{W}x{H}",
  "-i",lastp,"-frames:v","1","-f","rawvideo","-pix_fmt","rgb24",rgbp])
rgb=open(rgbp,"rb").read()
R=rgb[0::3]; G=rgb[1::3]; B=rgb[2::3]
Rm=sum(R)/len(R); Gm=sum(G)/len(G); Bm=sum(B)/len(B)
print("R/G",round(Rm/Gm,3),"B/G",round(Bm/Gm,3))
PY
}

run_case SBGGR10_1X10
run_case SGBRG10_1X10
run_case SGRBG10_1X10
run_case SRGGB10_1X10
```
