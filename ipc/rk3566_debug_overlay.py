#!/usr/bin/env python3
"""
Debug overlay module for Option C.

Reads the CV feed (NV12 640x480 by default) and detection JSON messages from a
debug scene socket, then draws boxes and publishes an annotated stream via shmsink.

This version uses proper GStreamer callbacks to avoid blocking and pipeline stalls.
"""

from __future__ import annotations

import argparse
import json
import os
import socket
import threading
import time
from dataclasses import dataclass
from typing import Any

import cv2
import numpy as np
import gi

gi.require_version("Gst", "1.0")
gi.require_version("GstApp", "1.0")
gi.require_version("GstVideo", "1.0")
from gi.repository import Gst, GLib, GstVideo  # noqa: E402


@dataclass
class DetectionState:
    ts: float = 0.0
    objects: list[dict[str, Any]] | None = None


def _recv_loop(sock_path: str, state: DetectionState, lock: threading.Lock) -> None:
    try:
        os.unlink(sock_path)
    except FileNotFoundError:
        pass
    s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    s.bind(sock_path)
    while True:
        data, _addr = s.recvfrom(1 << 20)
        try:
            msg = json.loads(data.decode("utf-8", errors="replace"))
        except Exception:
            continue
        objects = msg.get("objects", [])
        if not isinstance(objects, list):
            continue
        with lock:
            state.ts = time.time()
            state.objects = objects


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--cv-socket", default="/tmp/soulcam_cv.sock")
    ap.add_argument("--cv-width", type=int, default=640)
    ap.add_argument("--cv-height", type=int, default=480)
    ap.add_argument("--fps", type=int, default=30)
    ap.add_argument("--stream-width", type=int, default=1296)
    ap.add_argument("--stream-height", type=int, default=972)
    ap.add_argument("--out-fps", type=int, default=30)
    ap.add_argument("--out-socket", default="/tmp/soulcam_debug.sock")
    ap.add_argument("--out-shm-size", type=int, default=32 * 1024 * 1024)
    ap.add_argument("--scene-sock", default="/tmp/soulcam_debug_scene.sock")
    ap.add_argument("--ttl-ms", type=int, default=700)
    args = ap.parse_args()

    Gst.init(None)

    state = DetectionState(ts=0.0, objects=[])
    lock = threading.Lock()
    t = threading.Thread(target=_recv_loop, args=(args.scene_sock, state, lock), daemon=True)
    t.start()

    in_pipeline = (
        f"shmsrc socket-path={args.cv_socket} is-live=true do-timestamp=true ! "
        f"video/x-raw,format=NV12,width={args.cv_width},height={args.cv_height},framerate={args.fps}/1 ! "
        "queue max-size-buffers=2 leaky=downstream ! "
        "videoconvert ! video/x-raw,format=BGR ! "
        "queue max-size-buffers=2 leaky=downstream ! "
        "appsink name=in_sink emit-signals=true max-buffers=2 drop=true sync=false"
    )

    try:
        os.unlink(args.out_socket)
    except FileNotFoundError:
        pass

    out_pipeline = (
        "appsrc name=out_src is-live=true format=time do-timestamp=true max-bytes=0 block=false ! "
        f"video/x-raw,format=BGR,width={args.stream_width},height={args.stream_height},framerate={args.out_fps}/1 ! "
        "queue max-size-buffers=2 leaky=downstream ! "
        "videoconvert ! "
        f"video/x-raw,format=NV12,width={args.stream_width},height={args.stream_height} ! "
        f"shmsink socket-path={args.out_socket} shm-size={args.out_shm_size} "
        "wait-for-connection=false sync=false async=false"
    )

    in_pipe = Gst.parse_launch(in_pipeline)
    out_pipe = Gst.parse_launch(out_pipeline)

    in_sink = in_pipe.get_by_name("in_sink")
    out_src = out_pipe.get_by_name("out_src")
    if in_sink is None or out_src is None:
        print("ERROR: failed to create appsink/appsrc.")
        return 1

    scale_x = args.stream_width / float(args.cv_width)
    scale_y = args.stream_height / float(args.cv_height)
    ttl = args.ttl_ms / 1000.0
    frame_duration = int(1_000_000_000 / args.out_fps)
    frame_idx = [0]
    last_push = [0.0]

    def on_new_sample(sink):
        sample = sink.emit("pull-sample")
        if sample is None:
            return Gst.FlowReturn.OK

        buf = sample.get_buffer()
        caps = sample.get_caps()
        if caps is None:
            return Gst.FlowReturn.OK
        vinfo = GstVideo.VideoInfo.new_from_caps(caps)
        if vinfo is None:
            return Gst.FlowReturn.OK
        ok, info = buf.map(Gst.MapFlags.READ)
        if not ok:
            return Gst.FlowReturn.OK

        try:
            arr = np.frombuffer(info.data, dtype=np.uint8)
            width = int(vinfo.width)
            height = int(vinfo.height)
            if width <= 0 or height <= 0:
                buf.unmap(info)
                return Gst.FlowReturn.OK
            fmt = vinfo.finfo.name if vinfo.finfo else ""
            if fmt in ("BGR", "RGB"):
                bpp = 3
            elif fmt in ("BGRx", "BGRA", "RGBx", "RGBA"):
                bpp = 4
            else:
                bpp = 3
            stride = int(vinfo.stride[0]) if vinfo.stride[0] else width * bpp
            if stride < width * bpp:
                stride = width * bpp
            row = arr.reshape((height, stride))
            row = row[:, : width * bpp]
            frame = row.reshape((height, width, bpp))
            if fmt in ("RGB", "RGBx", "RGBA"):
                frame = frame[:, :, :3][:, :, ::-1]
            else:
                frame = frame[:, :, :3]
            frame = cv2.resize(frame, (args.stream_width, args.stream_height), interpolation=cv2.INTER_LINEAR)
        except Exception as exc:
            print(f"Frame processing error: {exc}")
            buf.unmap(info)
            return Gst.FlowReturn.OK
        finally:
            buf.unmap(info)

        now = time.time()
        with lock:
            objs = list(state.objects) if (now - state.ts) <= ttl else []
            last_ts = state.ts

        for obj in objs:
            try:
                box = obj.get("box", {})
                left = int(box.get("left", 0) * scale_x)
                top = int(box.get("top", 0) * scale_y)
                right = int(box.get("right", 0) * scale_x)
                bottom = int(box.get("bottom", 0) * scale_y)
                label = str(obj.get("label", "obj"))
                conf = float(obj.get("conf", 0.0))
            except Exception:
                continue

            cv2.rectangle(frame, (left, top), (right, bottom), (0, 255, 0), 2)
            text = f"{label} {conf:.2f}"
            cv2.putText(frame, text, (left, max(0, top - 5)), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 0, 0), 2)
            cv2.putText(frame, text, (left, max(0, top - 5)), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (255, 255, 255), 1)

        age_ms = int((now - last_ts) * 1000) if last_ts > 0 else -1
        hud = f"det={len(objs)} age_ms={age_ms}"
        cv2.putText(frame, hud, (8, 20), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 0, 0), 2)
        cv2.putText(frame, hud, (8, 20), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 255, 255), 1)

        if now - last_push[0] < (1.0 / float(args.out_fps)):
            return Gst.FlowReturn.OK
        last_push[0] = now

        out_buf = Gst.Buffer.new_allocate(None, frame.nbytes, None)
        out_buf.fill(0, frame.tobytes())
        out_buf.pts = frame_idx[0] * frame_duration
        out_buf.dts = out_buf.pts
        out_buf.duration = frame_duration
        frame_idx[0] += 1

        ret = out_src.emit("push-buffer", out_buf)
        if ret != Gst.FlowReturn.OK:
            print(f"push-buffer returned {ret}")

        return Gst.FlowReturn.OK

    in_sink.connect("new-sample", on_new_sample)

    print(
        "Debug overlay running. "
        f"CV={args.cv_width}x{args.cv_height} -> OUT={args.stream_width}x{args.stream_height}, "
        f"scene={args.scene_sock}, out={args.out_socket}"
    )

    in_pipe.set_state(Gst.State.PLAYING)
    out_pipe.set_state(Gst.State.PLAYING)

    loop = GLib.MainLoop()
    try:
        loop.run()
    except KeyboardInterrupt:
        print("Stopping...")

    in_pipe.set_state(Gst.State.NULL)
    out_pipe.set_state(Gst.State.NULL)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
