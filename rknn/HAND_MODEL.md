# SoulCam 手部检测模型（RK3566 实测可用）

最后验证时间：2026-03-01  
目标设备：`192.168.1.45`（RK3566 / Ubuntu）

---

## 当前结论（基于实机日志）

- 已在目标设备跑通 **INT8** 手部检测，`soulcam` 可持续输出 `[hand]` 检测框。
- 可用 INT8 模型：`/home/ubuntu/models/hand_yolov8n_rk3566_i8_20260301.rknn`
- 该模型在 SoulCam 中显示：`1 inputs, 9 outputs`, `quant=yes`, `classes=1`。
- 当前场景建议阈值：`--conf 0.10`（能稳定出现检测）。
- 要求：`src/ai/detector.cpp` 需使用“logit 兼容后处理”版本（本仓库已更新）。
- 已实现手部单目标跟踪（Phase A + B），多手场景下仅输出 1 个目标手。
- 已实现高级快速切换（Phase C），默认关闭，可通过 `--hand-fast-switch` 启用。

> 说明：单类别手模型的分数分布偏窄，阈值通常要比 COCO 模型更低。

---

## 模型来源

- 权重来源：Hugging Face `Bingsu/adetailer`
- 文件名：`hand_yolov8n.pt`
- 下载到本地：`models/generated/hand_yolov8n.pt`

---

## INT8 输出与后处理说明

INT8 方案采用 9 输出 ONNX（`box/score/score_sum`）：

- `score` 为 **logit**（未 sigmoid）
- `score_sum` 为 `sigmoid(score)` 的和（单类时就是概率）

因此后处理需要：

- 阈值比较时把 `conf` 从概率域映射到 logit 域（只对 `score`）。
- 输出置信度时再把 logit 通过 sigmoid 转回概率。

本仓库当前 `src/ai/detector.cpp` 已实现该逻辑。

---

## 一次性环境（推荐用独立 venv）

```bash
cd ~/embeddedProjects/SoulCam
python3 -m venv tmp/hand_model_venv
. tmp/hand_model_venv/bin/activate
pip install --upgrade pip

# 先装导出依赖
pip install torch==2.1.0 torchvision==0.16.0 ultralytics==8.0.170
pip install "numpy<2" opencv-python==4.8.1.78

# 再装 RKNN 工具链（会自动补齐其依赖）
pip install onnx==1.16.2 protobuf==4.25.4 rknn-toolkit2 huggingface_hub
```

---

## Step 1: 下载手部权重

```bash
cd ~/embeddedProjects/SoulCam
. tmp/hand_model_venv/bin/activate
python3 - <<'PY'
from huggingface_hub import hf_hub_download
p = hf_hub_download(
    repo_id='Bingsu/adetailer',
    filename='hand_yolov8n.pt',
    local_dir='models/generated'
)
print(p)
PY
```

---

## Step 2: 导出 INT8 用 ONNX（logit 版本）

脚本：`rknn/export_hand_yolov8_onnx.py`

```bash
cd ~/embeddedProjects/SoulCam
. tmp/hand_model_venv/bin/activate
python3 rknn/export_hand_yolov8_onnx.py \
  --pt models/generated/hand_yolov8n.pt \
  --onnx models/generated/hand_yolov8n_9out.onnx \
  --score-mode logit
```

导出后应看到类似输出形状：

- `box_0 [1,64,80,80]`
- `score_0 [1,1,80,80]`
- `score_sum_0 [1,1,80,80]`
- ...（共 9 项）

---

## Step 3: 转换 INT8 RKNN（已提供脚本）

脚本：`rknn/convert_hand_onnx_to_rknn.py`

```bash
cd ~/embeddedProjects/SoulCam
. tmp/hand_model_venv/bin/activate
python3 rknn/convert_hand_onnx_to_rknn.py \
  --onnx models/generated/hand_yolov8n_9out.onnx \
  --rknn models/generated/hand_yolov8n_rk3566_i8_20260301.rknn \
  --target rk3566 \
  --quant \
  --dataset models/calib/lists/mixed.txt
```

输出文件：

- `models/generated/hand_yolov8n_rk3566_i8_20260301.rknn`（约 4.5MB）

---

## Step 4: 部署到设备

```bash
cd ~/embeddedProjects/SoulCam
sshpass -p 'shb084ww' ssh -o StrictHostKeyChecking=no ubuntu@192.168.1.45 \
  'mkdir -p /home/ubuntu/models'

sshpass -p 'shb084ww' scp -o StrictHostKeyChecking=no \
  models/generated/hand_yolov8n_rk3566_i8_20260301.rknn \
  ubuntu@192.168.1.45:/home/ubuntu/models/
```

---

## Step 5: 在设备运行 SoulCam

```bash
sshpass -p 'shb084ww' ssh -o StrictHostKeyChecking=no ubuntu@192.168.1.45 \
  "cd /home/ubuntu/SoulCam && sudo systemctl stop soulcam || true && \
   sudo ./build/soulcam --ai \
     --model /home/ubuntu/models/hand_yolov8n_rk3566_i8_20260301.rknn \
     --labels hand --conf 0.10 -v"
```

---

## 运行模式与推荐命令

### 1) 默认单目标跟踪（Phase A + B）

```bash
sshpass -p 'shb084ww' ssh -o StrictHostKeyChecking=no ubuntu@192.168.1.45 \
  "cd /home/ubuntu/SoulCam && sudo systemctl stop soulcam || true && \
   sudo ./build/soulcam --ai \
     --model /home/ubuntu/models/hand_yolov8n_rk3566_i8_20260301.rknn \
     --labels hand --conf 0.10 -v"
```

### 2) 启用高级快速切换（Phase C，默认参数）

```bash
sshpass -p 'shb084ww' ssh -o StrictHostKeyChecking=no ubuntu@192.168.1.45 \
  "cd /home/ubuntu/SoulCam && sudo systemctl stop soulcam || true && \
   sudo ./build/soulcam --ai \
     --model /home/ubuntu/models/hand_yolov8n_rk3566_i8_20260301.rknn \
     --labels hand --conf 0.10 -v \
     --hand-fast-switch"
```

### 3) 启用高级快速切换（自定义参数示例）

```bash
sshpass -p 'shb084ww' ssh -o StrictHostKeyChecking=no ubuntu@192.168.1.45 \
  "cd /home/ubuntu/SoulCam && sudo systemctl stop soulcam || true && \
   sudo ./build/soulcam --ai \
     --model /home/ubuntu/models/hand_yolov8n_rk3566_i8_20260301.rknn \
     --labels hand --conf 0.10 -v \
     --hand-fast-switch \
     --hand-fast-growth 0.16 \
     --hand-fast-area-ratio 0.88 \
     --hand-fast-hold 2"
```

---

## 快速验收命令

```bash
sshpass -p 'shb084ww' ssh -o StrictHostKeyChecking=no ubuntu@192.168.1.45 \
  "cd /home/ubuntu/SoulCam && timeout 15s sudo ./build/soulcam --ai \
    --model /home/ubuntu/models/hand_yolov8n_rk3566_i8_20260301.rknn \
    --labels hand --conf 0.10 -v 2>&1 | rg 'RKNN SDK|Model:|Model input|Inference:|\\[hand\\]'"
```

若成功，日志应包含：

- `RKNN SDK: ...`
- `Model: 1 inputs, 9 outputs`
- `Model input: 640x640x3, quant=yes, classes=1`
- `Inference: N candidates -> M detections`
- `[hand] ...`

---

## 单目标手部跟踪策略（已实现）

为满足“多手场景下优先跟踪一个目标手”的需求，主流程已增加单目标跟踪器：

- 实现位置：`src/ai/hand_target_tracker.h/.cpp`
- 接入位置：`src/main.cpp` 的 `on_detections()` 回调
- 行为：`detector` 仍可检出多手，但 overlay/ONVIF/scene hub 只发布 1 个目标手
- 启用条件：`--labels` 为单标签 `hand`（避免影响非手部通用检测流程）

默认策略（Phase A + B）：

- 多手时只锁定 1 个目标（初始优先更大框）
- 目标延续：优先通过 IoU/中心距离匹配当前目标，短时丢失可容忍
- 切换条件：仅当候选手框面积持续大于当前目标（默认 `1.25x`）才允许切换
- 防抖机制：需连续满足若干帧（默认 `6` 帧）才切换
- 冷却机制：切换后进入冷却（默认 `10` 帧）防止来回跳变

高级策略（Phase C，可选启用，默认关闭）：

- “快速靠近”提前切换（基于面积增长率）已实现
- 启用开关：`--hand-fast-switch`
- 可调参数：
  - `--hand-fast-growth`（默认 `0.18`，面积相对增长阈值）
  - `--hand-fast-area-ratio`（默认 `0.90`，候选面积/当前面积比值阈值）
  - `--hand-fast-hold`（默认 `3`，连续满足帧数）

---

## 跟踪参数说明（简版）

- `--labels hand`
  - 单目标手跟踪生效前提（当前实现要求单标签 `hand`）。
- `--hand-fast-switch`
  - 开启 Phase C；不开启时仅执行 Phase A + B 策略。
- `--hand-fast-growth`
  - 候选目标每帧面积相对增长阈值，越小越敏感，越大越保守。
- `--hand-fast-area-ratio`
  - 候选面积与当前面积比阈值，越小越容易提前切换，越大越稳。
- `--hand-fast-hold`
  - 连续满足快速条件的帧数，越小响应越快，越大抖动越小。

---

## 代码同步与设备构建状态（本次）

- 本次改动已同步到：`/home/ubuntu/SoulCam`
- 设备端已执行：`cmake -S src -B build && cmake --build build -j4`
- 构建成功：`[100%] Built target soulcam`
- 生成程序：`/home/ubuntu/SoulCam/build/soulcam`

---

## 已知限制与后续优化方向

- 当前模型在本场景置信度偏低，需要 `--conf 0.10` 左右。
- 会有一定误检（尤其在手部不明显、画面纹理复杂时）。
- 若要提升稳定性，建议后续做：
  - 更贴近你相机数据域的微调训练（手部数据集 + 现场采样）
  - 引入关键点模型二级确认（先检测手框，再做 hand landmarks）

---

## 本次生成/使用的关键文件

- `models/generated/hand_yolov8n.pt`
- `models/generated/hand_yolov8n_9out.onnx`
- `models/generated/hand_yolov8n_rk3566_i8_20260301.rknn`
- `models/calib/cam`, `models/calib/hand`, `models/calib/hand_v2`, `models/calib/hands`, `models/calib/base`
- `models/calib/lists/mixed.txt`（及其他校准列表）
- `rknn/export_hand_yolov8_onnx.py`
- `rknn/convert_hand_onnx_to_rknn.py`
- `src/ai/detector.cpp`（logit 兼容后处理）
- `src/ai/hand_target_tracker.h`
- `src/ai/hand_target_tracker.cpp`
- `src/main.cpp`（单目标跟踪接入）

---

## 故障排查：`Failed to attach RTSP server`

如果启动时出现：

- `Failed to attach RTSP server`
- `Failed to start RTSP server`

通常是 `8554` 端口已被另一个 `soulcam` 进程占用（常见于 systemd 后台服务仍在运行）。

先释放端口再启动：

```bash
sudo systemctl stop soulcam
sudo pkill -f '/home/ubuntu/SoulCam/build/soulcam' || true
sudo ss -lntp | rg 8554 || true
```

确认 `8554` 没有监听后，再运行前台命令。
