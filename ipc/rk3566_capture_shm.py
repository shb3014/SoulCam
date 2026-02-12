#!/usr/bin/env python3
"""
SoulCam RK3566 capture daemon (Option C).

Captures from a single V4L2 device once, then fans out raw frames via shared memory:
- "stream" feed: NV12 at stream resolution (e.g. 1296x972) for RTSP encoding/viewers
- "cv" feed:     NV12 downscaled (e.g. 640x480) for CV workers (YOLO, tracking, face, ...)

This avoids multiple processes fighting over /dev/video8 and avoids RTSP decode for CV.
"""

from __future__ import annotations

import argparse
import os
import signal
import sys

import gi

gi.require_version("Gst", "1.0")
from gi.repository import GLib, Gst  # noqa: E402


def _on_bus_message(bus: Gst.Bus, message: Gst.Message, loop: GLib.MainLoop) -> None:
    msg_type = message.type
    if msg_type == Gst.MessageType.ERROR:
        err, debug = message.parse_error()
        sys.stderr.write(f"GStreamer ERROR: {err}\n")
        if debug:
            sys.stderr.write(f"Debug: {debug}\n")
        loop.quit()
    elif msg_type == Gst.MessageType.EOS:
        loop.quit()


def build_pipeline(
    *,
    device: str,
    stream_width: int,
    stream_height: int,
    fps: int,
    source_format: str,
    stream_format: str,
    stream_socket: str,
    cv_width: int,
    cv_height: int,
    cv_socket: str,
    cv2_width: int,
    cv2_height: int,
    cv2_socket: str,
    stream_shm_size: int,
    cv_shm_size: int,
    cv2_shm_size: int,
    use_rga: bool,
) -> str:
    # Notes:
    # - Keep conversion/scaling in *this* daemon, once, so multiple consumers don't each pay the cost.
    # - Use leaky queues so slow consumers don't stall capture.
    #
    # We request a concrete raw format from v4l2src to keep negotiation stable on this stack.
    source_format = source_format.upper()
    stream_format = stream_format.upper()
    if source_format not in ("NV12", "UYVY"):
        raise ValueError(f"Unsupported source_format: {source_format}")
    if stream_format not in ("NV12", "NV21", "UYVY"):
        raise ValueError(f"Unsupported stream_format: {stream_format}")

    if source_format == "NV12" and use_rga:
        use_rga = False
    if use_rga:
        convert = "rgaconvert"
        scale_chain = f"{convert} !"
    else:
        # Ensure full chroma conversion when using software paths.
        convert = "videoconvert chroma-mode=full matrix-mode=full"
        scale_chain = "videoconvert chroma-mode=full matrix-mode=full ! videoscale ! videoconvert chroma-mode=full matrix-mode=full !"
    pipeline = (
        "v4l2src "
        f"device={device} io-mode=2 do-timestamp=true ! "
        f"video/x-raw,format={source_format},width={stream_width},height={stream_height} ! "
        "queue leaky=downstream max-size-buffers=2 ! "
        "tee name=t "
    )

    # Stream feed (UYVY or NV12 @ stream res)
    if stream_format == source_format:
        pipeline += (
            "t. ! queue leaky=downstream max-size-buffers=2 ! "
            f"video/x-raw,format={stream_format},width={stream_width},height={stream_height},framerate={fps}/1 ! "
            f"shmsink socket-path={stream_socket} shm-size={stream_shm_size} "
            "wait-for-connection=false sync=false "
        )
    elif stream_format == "UYVY":
        pipeline += (
            "t. ! queue leaky=downstream max-size-buffers=2 ! "
            f"video/x-raw,format=UYVY,width={stream_width},height={stream_height},framerate={fps}/1 ! "
            f"shmsink socket-path={stream_socket} shm-size={stream_shm_size} "
            "wait-for-connection=false sync=false "
        )
    else:
        pipeline += (
            "t. ! queue leaky=downstream max-size-buffers=2 ! "
            f"{convert} ! "
            f"video/x-raw,format={stream_format},width={stream_width},height={stream_height},framerate={fps}/1,colorimetry=bt709,range=full ! "
            f"shmsink socket-path={stream_socket} shm-size={stream_shm_size} "
            "wait-for-connection=false sync=false "
        )

    # CV feed (NV12 @ downscaled res)
    pipeline += (
        "t. ! queue leaky=downstream max-size-buffers=2 ! "
        f"{scale_chain} "
        f"video/x-raw,format=NV12,width={cv_width},height={cv_height},framerate={fps}/1 ! "
        f"shmsink socket-path={cv_socket} shm-size={cv_shm_size} "
        "wait-for-connection=false sync=false"
    )

    if cv2_socket:
        pipeline += (
            " t. ! queue leaky=downstream max-size-buffers=2 ! "
            f"{scale_chain} "
            f"video/x-raw,format=NV12,width={cv2_width},height={cv2_height},framerate={fps}/1 ! "
            f"shmsink socket-path={cv2_socket} shm-size={cv2_shm_size} "
            "wait-for-connection=false sync=false"
        )

    return pipeline


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--device", default="/dev/video8")
    ap.add_argument("--fps", type=int, default=30)
    ap.add_argument("--stream-width", type=int, default=1296)
    ap.add_argument("--stream-height", type=int, default=972)
    ap.add_argument("--source-format", default="UYVY")
    ap.add_argument("--stream-format", default="NV12")
    ap.add_argument("--use-rga", action="store_true")
    ap.add_argument("--stream-socket", default="/tmp/soulcam_stream.sock")
    ap.add_argument("--stream-shm-size", type=int, default=32 * 1024 * 1024)
    ap.add_argument("--cv-width", type=int, default=640)
    ap.add_argument("--cv-height", type=int, default=480)
    ap.add_argument("--cv-socket", default="/tmp/soulcam_cv.sock")
    ap.add_argument("--cv-shm-size", type=int, default=8 * 1024 * 1024)
    ap.add_argument("--cv2-width", type=int, default=0)
    ap.add_argument("--cv2-height", type=int, default=0)
    ap.add_argument("--cv2-socket", default="")
    ap.add_argument("--cv2-shm-size", type=int, default=8 * 1024 * 1024)
    args = ap.parse_args()
    args.source_format = args.source_format.upper()
    args.stream_format = args.stream_format.upper()
    if args.source_format == "NV12" and args.use_rga:
        print("WARN: SOURCE_FORMAT=NV12; disabling RGA to avoid color issues")
        args.use_rga = False

    # Clean up stale sockets so shmsink can bind.
    sockets = [args.stream_socket, args.cv_socket]
    if args.cv2_socket:
        sockets.append(args.cv2_socket)
    for p in sockets:
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass

    Gst.init(None)
    loop = GLib.MainLoop()

    launch = build_pipeline(
        device=args.device,
        stream_width=args.stream_width,
        stream_height=args.stream_height,
        fps=args.fps,
        source_format=args.source_format,
        stream_format=args.stream_format,
        stream_socket=args.stream_socket,
        cv_width=args.cv_width,
        cv_height=args.cv_height,
        cv_socket=args.cv_socket,
        cv2_width=args.cv2_width if args.cv2_width else args.cv_width,
        cv2_height=args.cv2_height if args.cv2_height else args.cv_height,
        cv2_socket=args.cv2_socket,
        stream_shm_size=args.stream_shm_size,
        cv_shm_size=args.cv_shm_size,
        cv2_shm_size=args.cv2_shm_size,
        use_rga=bool(args.use_rga),
    )

    pipeline = Gst.parse_launch(launch)
    bus = pipeline.get_bus()
    bus.add_signal_watch()
    bus.connect("message", _on_bus_message, loop)

    def _shutdown(*_args) -> None:  # noqa: ANN001
        try:
            pipeline.set_state(Gst.State.NULL)
        finally:
            loop.quit()

    signal.signal(signal.SIGINT, _shutdown)
    signal.signal(signal.SIGTERM, _shutdown)

    ret = pipeline.set_state(Gst.State.PLAYING)
    if ret == Gst.StateChangeReturn.FAILURE:
        sys.stderr.write("Failed to start capture pipeline.\n")
        pipeline.set_state(Gst.State.NULL)
        return 1

    print("SoulCam capture daemon running.")
    print(f"- source: {args.source_format}")
    print(f"- stream: {args.stream_format} {args.stream_width}x{args.stream_height}@{args.fps} -> {args.stream_socket}")
    print(f"- cv:     NV12 {args.cv_width}x{args.cv_height}@{args.fps} -> {args.cv_socket}")
    print(f"- rga:    {'enabled' if args.use_rga else 'disabled'}")
    print("Press Ctrl+C to stop.")

    loop.run()
    pipeline.set_state(Gst.State.NULL)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

