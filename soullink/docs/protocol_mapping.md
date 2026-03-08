# SoulCam Soullink Protocol Mapping

## Runtime transport and discovery

- Soullink runs in-process in `soulcam` (`src/soullink/module.cpp`).
- MQTT transport uses one persistent `libmosquitto` client session.
- mDNS advertisement uses `avahi-publish-service`.
- Discovery lifecycle (aligned with Ivy reference):
  - mDNS advertises while MQTT is not connected (discovery phase)
  - mDNS stops when MQTT connects (Ivy-compatible behavior)
  - mDNS restarts on disconnect, suspend (`disconnectServer`), or process crash (watchdog)
- HTTP handshake endpoint:
  - `GET /debug` (uses requester source IP as broker host)
  - optional override: `GET /debug?ip=<host-ip>`

## MQTT topics

- Prefix: `soulcam/debug/`
- Downlink subscribe topics:
  - `soulcam/debug/in/<serviceIdentifier>` (primary)
  - `soulcam/debug/in/<clientId>` (compat, when different)
- Uplink topics:
  - DP/status: `soulcam/debug/out/<clientId>`
  - Messages/notifications/sync: `soulcam/debug/m/<clientId>`
  - Stream state: `soulcam/debug/s/<clientId>/<streamIndex>`

Default client ID mode is compatibility (`<serviceIdentifier>`). Composite mode (`soulcam:<serviceIdentifier>`) is optional via CLI.

## SoulCmd mapping (current implementation)

| Command | Numeric cmd | Behavior |
|---|---:|---|
| `setDp` | 0 | Stores DP values and publishes DP report (`cmd=1`) on `out`. |
| `getDp` | 1 | Reads requested DPs and publishes report (`cmd=1`) on `out`. |
| `getDpAll` | 2 | Publishes all in-memory DPs (`cmd=1`) on `out`. |
| `subStream` | 4 | Sets subscribed=true and publishes stream-state binary on `s/<clientId>/<index>`. |
| `unsubStream` | 5 | Sets subscribed=false and publishes stream-state binary on `s/<clientId>/<index>`. |
| `disconnectServer` | 12 | Suspends MQTT until next `/debug` handshake reconnects. |
| `syncFiles` | 13 | Runs sync worker and publishes progress/result on `m` with `id=1`. |
| `streaming` | 18 | Publishes current stream-state binary on `s/<clientId>/<index>`. |

Currently not implemented and reported as warnings:
- `soulReload` (`14`)
- `directorPlay` (`20`)
- `sysCmd` (`21`)

Unknown/unsupported commands are reported on `m` as:

```json
{
  "id": 0,
  "message": {
    "text": "Unsupported command",
    "type": "warning",
    "data": { "cmd": "<original-cmd>" }
  }
}
```

## Stream and TCP frame payloads

Stream-state uplink payload on `s/<clientId>/<streamIndex>`:
- Little-endian binary record:
  - `uint16 uid`
  - `uint8 oldStatus`
  - `uint8 newStatus`
  - `uint32 timestamp`

TCP JPEG receiver (`streamTcp`, default `1234`) input format:
- `4-byte little-endian JPEG size`
- `JPEG payload bytes`

