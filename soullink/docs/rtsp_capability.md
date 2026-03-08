# SoulCam RTSP Capability Contract

## Endpoint

- Default endpoint: `rtsp://<device_ip>:8554/cam`
- Producer: existing `soulcam` runtime (`src/main.cpp` path)
- Consumer-facing publication: Soullink message channel (`m/<clientId>`) health payloads from in-process C++ module

## Runtime readiness rule

RTSP is mandatory for `deviceType=soulcam`.

The native Soullink module reports:

- `rtspOnline=true` only when TCP connectivity to the RTSP port succeeds.
- `ready=true` only when:
  - RTSP is online, and
  - TCP frame receiver is started.

This prevents the client from advertising fully ready while RTSP is down.

## Health payload example

```json
{
  "id": 0,
  "type": "health",
  "deviceType": "soulcam",
  "serviceIdentifier": "soulcam-a1b2c3d4e5",
  "rtspUri": "rtsp://192.168.1.45:8554/cam",
  "rtspOnline": true,
  "ready": true,
  "uptimeSec": 24
}
```

## Open integration items

- Host requirement for RTSP URI registration path (DP/message/separate API).
- Auth policy (`none/basic/token`) for production.
- Final codec/fps/bitrate constraints required by host ingest.

