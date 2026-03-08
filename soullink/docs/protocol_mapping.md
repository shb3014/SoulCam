# SoulCam Soullink Protocol Mapping

## MQTT topics

- Prefix: `soulcam/debug/`
- Downlink command topic: `soulcam/debug/in/<clientId>`
- Uplink DP/status topic: `soulcam/debug/out/<clientId>`
- Uplink message topic: `soulcam/debug/m/<clientId>`
- Uplink stream topic: `soulcam/debug/s/<clientId>/<streamIndex>`

## Runtime implementation notes

- Soullink now runs in-process inside `soulcam` (`src/soullink/module.cpp`).
- MQTT transport uses `mosquitto_sub`/`mosquitto_pub` binaries at runtime.
- mDNS advertisement uses `avahi-publish-service`.

## SoulCmd mapping (current implementation)

| Command | Numeric cmd | Behavior |
|---|---:|---|
| `setDp` | 1 | Stores DP values and publishes DP ack on `out`. |
| `getDp` | 2 | Reads requested DP values and publishes on `out`. |
| `getDpAll` | 3 | Publishes full DP map on `out`. |
| `subStream` | 4 | Marks stream subscription active and publishes ack on `out`. |
| `unsubStream` | 5 | Marks stream subscription inactive and publishes ack on `out`. |
| `streaming` | 6 | Publishes current stream subscription state on `out`. |
| `syncFiles` | 13 | Runs sync worker and publishes progress/completion on `m` with `id=1`. |

Guarded commands (`reboot`, `soulReload`, `disconnectServer`, `sysCmd`) currently return `id=0` blocked messages unless explicitly enabled in code.

Unknown or unsupported commands return an explicit structured message on `m`:

```json
{
  "id": 0,
  "type": "unsupported",
  "cmd": "<original-cmd>",
  "details": "SoulCmd is unsupported for soulcam phase-1."
}
```

## Stream payload format

Current stream-status uplink payload on `s/<clientId>/<streamIndex>`:

- Little-endian binary record:
  - `uint16 uid`
  - `uint8 oldStatus`
  - `uint8 newStatus`
  - `uint32 timestamp`

The receiver listens on TCP `streamTcp` (`1234` by profile default) and expects repeated frame packets:

- `4-byte little-endian JPEG size`
- `JPEG payload bytes`

