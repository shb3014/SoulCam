#!/usr/bin/env python3
"""
Minimal RTSP server for RK3566 SoulCam pipeline.

Why this exists:
- "IPC style" output: RTSP (H.264) that VLC can open.
- Keeps the capture/encode in one process so YOLO can consume the RTSP stream.

Default URL:
  rtsp://<device-ip>:8554/cam
"""

import argparse
import sys

import gi

gi.require_version("Gst", "1.0")
gi.require_version("GstRtspServer", "1.0")

from gi.repository import GLib, Gst, GstRtspServer  # noqa: E402


def _install_log_filter() -> None:
    """
    Filter a known noisy GStreamer-CRITICAL warning seen on some V4L2 drivers:
      gst_value_set_int_range_step: assertion 'end % step == 0' failed

    This does not affect streaming; it just keeps console/log output readable.
    """

    def handler(domain, level, message, user_data=None):  # noqa: ANN001
        msg = message or ""
        if "gst_value_set_int_range_step" in msg and "end % step == 0" in msg:
            return
        # Preserve other critical/warnings to stderr
        dom = domain or "GLib"
        sys.stderr.write(f"{dom}: {msg}\n")

    GLib.log_set_handler(
        "GStreamer",
        GLib.LogLevelFlags.LEVEL_ERROR
        | GLib.LogLevelFlags.LEVEL_CRITICAL
        | GLib.LogLevelFlags.LEVEL_WARNING,
        handler,
        None,
    )


def build_launch(
    *,
    source: str,
    device: str,
    shm_socket: str,
    shm_format: str,
    width: int,
    height: int,
    fps: int,
    bitrate_kbps: int,
    source_fps_caps: bool,
    use_videorate: bool,
    encoder: str,
    swap_uv: bool,
) -> str:
    # Notes:
    # - rkisp mainpath can output multiple formats (often UYVY by default).
    #   Don't hard-require NV12 here; request size/fps and let v4l2src pick a supported format,
    #   then convert to I420 for x264enc.
    # - ultrafast+zerolatency keeps CPU and latency down (but still software encode).
    # - key-int-max ~1s for decent seeking / quick join.
    shm_format = shm_format.upper()
    if shm_format not in ("NV12", "NV21", "UYVY", "YUYV"):
        raise ValueError(f"Unsupported shm_format: {shm_format}")
    src_caps = (
        f"video/x-raw,format={shm_format},width={width},height={height},"
        "colorimetry=bt709,range=full"
    )

    post_convert = " videoconvert chroma-mode=full matrix-mode=full ! "
    encoder = encoder.lower()
    if encoder == "mpp":
        if use_videorate:
            post_convert += f" videorate ! video/x-raw,format=NV12,framerate={fps}/1 ! "
        else:
            post_convert += " video/x-raw,format=NV12 ! "
    else:
        if use_videorate:
            post_convert += f" videorate ! video/x-raw,format=I420,framerate={fps}/1,colorimetry=bt709,range=full ! "
        else:
            post_convert += " video/x-raw,format=I420,colorimetry=bt709,range=full ! "

    if source == "v4l2":
        src = f"v4l2src device={device} io-mode=2 do-timestamp=true"
        # On this stack, forcing a concrete source format improves negotiation stability.
        src_caps = src_caps.replace("format=NV12", "format=UYVY")
        if source_fps_caps:
            src_caps += f",framerate={fps}/1"
    elif source == "shm":
        # Consumes raw frames from the capture daemon (Option C).
        # shmsrc is-live/do-timestamp helps keep RTSP latency reasonable.
        src = f"shmsrc socket-path={shm_socket} is-live=true do-timestamp=true"
        # IMPORTANT: shmsrc commonly reports framerate as a wide range unless downstream fixes it.
        # RTSP server needs fixed caps to preroll reliably.
        src_caps += f",framerate={fps}/1"
    else:
        raise ValueError(f"Unknown source: {source}")

    if encoder == "mpp":
        enc = f" mpph264enc bps={bitrate_kbps * 1000} gop={fps} ! "
    elif encoder == "openh264":
        enc = f" openh264enc bitrate={bitrate_kbps * 1000} gop-size={fps} ! "
    elif encoder == "x264":
        enc = f" x264enc tune=zerolatency speed-preset=ultrafast bitrate={bitrate_kbps} key-int-max={fps} ! "
    else:
        raise ValueError(f"Unknown encoder: {encoder}")

    swap_uv = bool(swap_uv)
    swap_chain = ""
    if swap_uv and shm_format in ("NV12", "NV21"):
        target = "NV21" if shm_format == "NV12" else "NV12"
        swap_chain = f" videoconvert ! video/x-raw,format={target} ! "

    return (
        "("
        f" {src} ! {src_caps} ! "
        " queue leaky=downstream max-size-buffers=2 ! "
        f"{swap_chain}"
        # Optional CFR: videorate duplicates/drops to hit the requested FPS. Keep it default-on for
        # compatibility, but you may want it off when validating "true" camera FPS.
        f"{post_convert}"
        f"{enc}"
        " h264parse config-interval=1 ! "
        " rtph264pay name=pay0 pt=96 config-interval=1 "
        ")"
    )


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--device", default="/dev/video8", help="V4L2 device (rkisp mainpath), default: /dev/video8")
    ap.add_argument(
        "--source",
        choices=("v4l2", "shm"),
        default="v4l2",
        help="Video source: v4l2 (direct camera) or shm (from capture daemon)",
    )
    ap.add_argument("--shm-socket", default="/tmp/soulcam_stream.sock", help="shmsrc socket path when --source=shm")
    ap.add_argument("--width", type=int, default=1296)
    ap.add_argument("--height", type=int, default=972)
    ap.add_argument("--shm-format", default="NV12", choices=("NV12", "NV21", "UYVY", "YUYV"))
    ap.add_argument("--fps", type=int, default=30)
    ap.add_argument("--bitrate-kbps", type=int, default=4000)
    ap.add_argument("--encoder", choices=("x264", "openh264", "mpp"), default="mpp")
    ap.add_argument("--swap-uv", action="store_true", help="Swap UV planes for NV12/NV21 sources.")
    ap.add_argument("--port", type=int, default=8554)
    ap.add_argument("--mount", default="/cam")
    ap.add_argument("--debug-shm-socket", default="")
    ap.add_argument("--debug-mount", default="/cam_debug")
    ap.add_argument("--debug-width", type=int, default=0)
    ap.add_argument("--debug-height", type=int, default=0)
    ap.add_argument("--debug-fps", type=int, default=0)
    ap.add_argument(
        "--source-fps-caps",
        action="store_true",
        help="Request FPS at v4l2src caps (may improve true FPS; can also break negotiation on some drivers).",
    )
    ap.add_argument(
        "--no-videorate",
        action="store_true",
        help="Disable videorate (use device timestamps; helps validate true FPS; may produce variable cadence).",
    )
    args = ap.parse_args()

    _install_log_filter()
    Gst.init(None)

    server = GstRtspServer.RTSPServer()
    server.set_service(str(args.port))

    factory = GstRtspServer.RTSPMediaFactory()
    factory.set_shared(True)
    factory.set_launch(
        build_launch(
            source=str(args.source),
            device=str(args.device),
            shm_socket=str(args.shm_socket),
            shm_format=str(args.shm_format),
            width=int(args.width),
            height=int(args.height),
            fps=int(args.fps),
            bitrate_kbps=int(args.bitrate_kbps),
            source_fps_caps=bool(args.source_fps_caps),
            use_videorate=not bool(args.no_videorate),
            encoder=str(args.encoder),
            swap_uv=bool(args.swap_uv),
        )
    )

    mounts = server.get_mount_points()
    mounts.add_factory(args.mount, factory)

    if args.debug_shm_socket:
        dbg_width = int(args.debug_width) if args.debug_width else int(args.width)
        dbg_height = int(args.debug_height) if args.debug_height else int(args.height)
        dbg_fps = int(args.debug_fps) if args.debug_fps else int(args.fps)
        debug_factory = GstRtspServer.RTSPMediaFactory()
        debug_factory.set_shared(True)
        debug_factory.set_launch(
            build_launch(
                source="shm",
                device=str(args.device),
                shm_socket=str(args.debug_shm_socket),
                width=dbg_width,
                height=dbg_height,
                fps=dbg_fps,
                bitrate_kbps=int(args.bitrate_kbps),
                source_fps_caps=False,
                use_videorate=not bool(args.no_videorate),
            )
        )
        mounts.add_factory(args.debug_mount, debug_factory)

    server.attach(None)

    print(f"RTSP ready: rtsp://0.0.0.0:{args.port}{args.mount}")
    if args.debug_shm_socket:
        print(f"RTSP debug: rtsp://0.0.0.0:{args.port}{args.debug_mount}")
    print("Press Ctrl+C to stop.")

    loop = GLib.MainLoop()
    loop.run()


if __name__ == "__main__":
    main()

