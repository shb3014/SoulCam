# SoulCam RTSP Capability Contract

## Endpoint

- Default endpoint: `rtsp://<device_ip>:8554/cam`
- Producer: existing `soulcam` runtime (`src/main.cpp` path)
- Consumer-facing health publication: Soullink DP uplink on `out/<clientId>`

## Runtime readiness rule

RTSP is mandatory for `deviceType=soulcam`.

The native Soullink module reports:

- `dp=1001` (`rtspOnline`) is `1` when TCP connectivity to RTSP port succeeds.
- `dp=1002` (`streamSubscribed`) is `1` when stream subscription is active.
- `dp=1003` (`moduleReady`) is `1` only when:
  - RTSP is online, and
  - TCP frame receiver is started.

This prevents the client from advertising fully ready while RTSP is down.

## Health payload example (`out/<clientId>`)

```json
{
  "cmd": 1,
  "data": [
    { "dp": 1001, "value": 1 },
    { "dp": 1002, "value": 0 },
    { "dp": 1003, "value": 1 }
  ]
}
```

## Open integration items

- Host requirement for RTSP URI registration path (DP/message/separate API).
- Auth policy (`none/basic/token`) for production.
- Final codec/fps/bitrate constraints required by host ingest.

