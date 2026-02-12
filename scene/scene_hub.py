#!/usr/bin/env python3
"""
SoulCam Scene Hub — Detection event consumer

Listens on a Unix datagram socket for detection JSON from soulcam,
and optionally exposes events via a simple HTTP/WebSocket server.

Usage:
  # Listen and print detections to stdout
  python3 scene_hub.py

  # With HTTP API on port 8080
  python3 scene_hub.py --http 8080

  # Custom socket path
  python3 scene_hub.py --sock /tmp/soulcam_scene.sock

JSON format from soulcam:
  {"source":"soulcam","type":"detections","count":3,"objects":[
    {"cls_id":0,"label":"person","conf":0.890,
     "box":{"left":267,"top":162,"right":477,"bottom":493}},
    ...
  ]}
"""

import argparse
import json
import os
import signal
import socket
import sys
import threading
import time
from http.server import HTTPServer, BaseHTTPRequestHandler
from typing import Optional

# ---------------------------------------------------------------------------
# Shared state
# ---------------------------------------------------------------------------
latest_event: dict = {"source": "soulcam", "type": "detections", "count": 0, "objects": []}
latest_lock = threading.Lock()
event_count = 0
start_time = time.time()
running = True


def update_latest(data: dict):
    global latest_event, event_count
    with latest_lock:
        latest_event = data
        event_count += 1


def get_latest() -> dict:
    with latest_lock:
        return latest_event.copy()


# ---------------------------------------------------------------------------
# Unix datagram socket listener
# ---------------------------------------------------------------------------
def socket_listener(sock_path: str):
    global running

    # Remove stale socket
    try:
        os.unlink(sock_path)
    except FileNotFoundError:
        pass

    sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    sock.bind(sock_path)
    sock.settimeout(1.0)
    os.chmod(sock_path, 0o666)

    print(f"[scene_hub] Listening on {sock_path}", file=sys.stderr)

    while running:
        try:
            data, _ = sock.recvfrom(65536)
        except socket.timeout:
            continue
        except OSError:
            break

        try:
            msg = json.loads(data.decode("utf-8"))
        except (json.JSONDecodeError, UnicodeDecodeError) as e:
            print(f"[scene_hub] Bad JSON: {e}", file=sys.stderr)
            continue

        update_latest(msg)

        # Print summary to stdout
        count = msg.get("count", 0)
        objs = msg.get("objects", [])
        labels = ", ".join(
            f"{o['label']} {o['conf']:.0%}" for o in objs[:5]
        )
        print(f"[{time.strftime('%H:%M:%S')}] {count} detections: {labels}")
        sys.stdout.flush()

    sock.close()
    try:
        os.unlink(sock_path)
    except FileNotFoundError:
        pass
    print("[scene_hub] Socket listener stopped", file=sys.stderr)


# ---------------------------------------------------------------------------
# HTTP API (optional)
# ---------------------------------------------------------------------------
def format_onvif_xml(data: dict) -> str:
    """Format detection data as ONVIF tt:MetadataStream XML."""
    import datetime
    ts = datetime.datetime.utcnow().strftime("%Y-%m-%dT%H:%M:%SZ")
    objects = data.get("objects", [])

    # Default frame dimensions (AI model space)
    frame_w = 640
    frame_h = 640

    xml = '<?xml version="1.0" encoding="UTF-8"?>\n'
    xml += '<tt:MetadataStream xmlns:tt="http://www.onvif.org/ver10/schema">\n'
    xml += '  <tt:VideoAnalytics>\n'
    xml += f'    <tt:Frame UtcTime="{ts}">\n'
    xml += '      <tt:Transformation>\n'
    xml += f'        <tt:Translate x="-1.0" y="-1.0"/>\n'
    xml += f'        <tt:Scale x="{2.0/frame_w:.6f}" y="{2.0/frame_h:.6f}"/>\n'
    xml += '      </tt:Transformation>\n'

    for i, obj in enumerate(objects, 1):
        box = obj.get("box", {})
        left = (2.0 * box.get("left", 0) / frame_w) - 1.0
        top = (2.0 * box.get("top", 0) / frame_h) - 1.0
        right = (2.0 * box.get("right", 0) / frame_w) - 1.0
        bottom = (2.0 * box.get("bottom", 0) / frame_h) - 1.0
        label = obj.get("label", "unknown")
        conf = obj.get("conf", 0.0)

        xml += f'      <tt:Object ObjectId="{i}">\n'
        xml += '        <tt:Appearance>\n'
        xml += '          <tt:Shape>\n'
        xml += f'            <tt:BoundingBox left="{left:.4f}" top="{top:.4f}" right="{right:.4f}" bottom="{bottom:.4f}"/>\n'
        xml += '          </tt:Shape>\n'
        xml += '          <tt:Class>\n'
        xml += f'            <tt:Type Likelihood="{conf:.3f}">{label}</tt:Type>\n'
        xml += '          </tt:Class>\n'
        xml += '        </tt:Appearance>\n'
        xml += '      </tt:Object>\n'

    xml += '    </tt:Frame>\n'
    xml += '  </tt:VideoAnalytics>\n'
    xml += '</tt:MetadataStream>\n'
    return xml


class SceneHandler(BaseHTTPRequestHandler):
    """Simple HTTP handler for detection state."""

    def do_GET(self):
        if self.path == "/detections" or self.path == "/":
            data = get_latest()
            body = json.dumps(data, indent=2).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(body)

        elif self.path == "/onvif/metadata":
            data = get_latest()
            body = format_onvif_xml(data).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/xml")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(body)

        elif self.path == "/status":
            uptime = time.time() - start_time
            status = {
                "service": "scene_hub",
                "uptime_s": round(uptime, 1),
                "events_received": event_count,
                "events_per_sec": round(event_count / max(uptime, 1), 1),
            }
            body = json.dumps(status, indent=2).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(body)

        else:
            self.send_error(404)

    def log_message(self, format, *args):
        pass  # suppress request logging


def http_server(port: int):
    server = HTTPServer(("0.0.0.0", port), SceneHandler)
    server.timeout = 1.0
    print(f"[scene_hub] HTTP API on http://0.0.0.0:{port}/detections", file=sys.stderr)
    while running:
        server.handle_request()
    server.server_close()
    print("[scene_hub] HTTP server stopped", file=sys.stderr)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    global running

    parser = argparse.ArgumentParser(description="SoulCam Scene Hub")
    parser.add_argument("--sock", default="/tmp/soulcam_scene.sock",
                        help="Unix socket path (default: /tmp/soulcam_scene.sock)")
    parser.add_argument("--http", type=int, default=0,
                        help="HTTP API port (0=disabled, e.g. 8080)")
    args = parser.parse_args()

    def shutdown(signum, frame):
        global running
        running = False
        print(f"\n[scene_hub] Signal {signum}, shutting down...", file=sys.stderr)

    signal.signal(signal.SIGINT, shutdown)
    signal.signal(signal.SIGTERM, shutdown)

    # Start socket listener thread
    sock_thread = threading.Thread(target=socket_listener, args=(args.sock,), daemon=True)
    sock_thread.start()

    # Start HTTP server thread (optional)
    http_thread: Optional[threading.Thread] = None
    if args.http > 0:
        http_thread = threading.Thread(target=http_server, args=(args.http,), daemon=True)
        http_thread.start()

    # Wait for shutdown
    print(f"[scene_hub] Ready. Ctrl+C to stop.", file=sys.stderr)
    try:
        while running:
            time.sleep(0.5)
    except KeyboardInterrupt:
        running = False

    sock_thread.join(timeout=3)
    if http_thread:
        http_thread.join(timeout=3)

    print(f"[scene_hub] Total events: {event_count}", file=sys.stderr)


if __name__ == "__main__":
    main()
