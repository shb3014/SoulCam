# SoulCam -> Soullink Client Plan

## 1) Scope and Objectives

Build SoulCam as a **new Soullink device type** client with `deviceType=soulcam` (not `ivy`).

Target outcomes:

- Device discovery via mDNS using SoulFlow-compatible profile rules.
- MQTT command/downlink + uplink data paths (`in/out/m/s`).
- SoulCmd command execution with clear supported/unsupported behavior.
- `syncFiles` protocol implementation with safe incremental updates.
- TCP JPEG frame receiver (`4-byte LE size + JPEG payload`).
- RTSP video streaming as a core device capability (not optional).

This document intentionally allows deep structural changes when needed.

## Progress Update (2026-03-08)

Implemented in repository:

- [x] `soullink/` scaffold created with schema/profile/docs.
- [x] `deviceType=soulcam` profile descriptor + schema validation pipeline.
- [x] Soullink transport migrated into native C++ module under `src/soullink/` and integrated into `src/main.cpp`.
- [x] In-process mDNS + MQTT + SoulCmd dispatch + `syncFiles` + TCP JPEG receiver + RTSP health publication.
- [x] Added Soullink CLI/runtime configuration directly to `soulcam` executable (`--soullink*` options).
- [x] Removed standalone Python Soullink runtime (`soullink/agent`, tests, and service unit).
- [x] Reconciled wire contract against SoulFlow server:
  - SoulCmd numeric mapping aligned to shared enum
  - sync uplink payload (`id/message`) aligned for UI handling
  - syncFiles parsing aligned to `/api/git/get-diff` response shape (`{ data: { A/M/R/D, cdn, commitID } }`)
  - MQTT transport switched to single persistent `libmosquitto` client session
  - downlink subscription covers host command topic shape (`in/<serviceIdentifier>`)
- [x] Re-examined against Ivy Soullink reference (`SoulLinkService`/`SoulLinkClient`) and aligned runtime behavior:
  - added HTTP debug handshake endpoint `GET /debug` (optional `?ip=<host-ip>`) in native module (listens on profile `api_port`, default `5212`) to dynamically retarget MQTT broker
  - switched default MQTT `clientId` mode to Ivy-compatible `<serviceIdentifier>` (composite mode remains configurable via CLI)
  - stream command handlers now honor requested stream index payloads (`subStream`/`unsubStream`/`streaming`)
- [x] Additional runtime alignments:
  - `disconnectServer` (`cmd=12`) implemented to suspend MQTT immediately and keep disconnected until next handshake
  - mDNS lifecycle aligned with Ivy reference: advertise while MQTT disconnected, stop when MQTT connects, restart on disconnect/suspend
  - Fixed SoulFlow `cleanupServices` regression (commit `8ded8b7`) that forced `connected: false` on mDNS timeout — original behavior only clears `found`

Pending external validation:

- [ ] Host allowlist/admission confirmation for `deviceType=soulcam`.
- [ ] MQTT broker interoperability soak run on target.
- [ ] End-to-end subnet discovery verification from host tools.
- [ ] Final host contract confirmations listed in Section 12.

## 2) Naming and Location Rules (Required)

1. Use `soullink` spelling consistently across all paths and modules.
2. Keep Soullink docs/contracts under:
   - `/home/shb3014/embeddedProjects/SoulCam/soullink/`
3. Keep Soullink runtime code inside the SoulCam C++ tree:
   - `/home/shb3014/embeddedProjects/SoulCam/src/soullink/`
4. Do not place Soullink code/docs under `rive-runtime/`.
5. If any Soullink assets are found outside `soullink/` (including `rive-runtime/`), move them into `soullink/`.

Recommended structure:

```text
soullink/
  SOULCAM_SOULLINK_CLIENT_PLAN.md
  schemas/
    profile-descriptor.schema.json
  profiles/
    soulcam.profile.dec.json
  docs/
    protocol_mapping.md
    dp_catalog.md
    rtsp_capability.md
    missing_information.md
src/
  soullink/
    json.h
    json.cpp
    sync_engine.h
    sync_engine.cpp
    module.h
    module.cpp
```

## 3) New Device Type Profile: `soulcam`

This project should onboard `soulcam` as a first-class Soullink profile.

## 3.1 Proposed profile contract (initial draft)

```json
{
  "schemaVersion": "1.0",
  "name": "SoulCam Device Profile",
  "description": "SoulCam Soullink client profile.",
  "profile": {
    "deviceType": "soulcam",
    "mdnsServiceTypes": ["_soulcamDebug._tcp.local"],
    "mqttTopicPrefix": "soulcam/debug/",
    "defaultPorts": {
      "mqtt": 1883,
      "api": 5212,
      "streamTcp": 1234,
      "streamIndex": 0
    },
    "capabilities": {
      "dp": true,
      "message": true,
      "stream": true,
      "sync": true,
      "monitor": true,
      "videoStream": true,
      "directorMode": false
    },
    "channelPolicy": {
      "useCompositeDeviceKey": true
    }
  }
}
```

## 3.2 Identity policy

- mDNS SRV: `<serviceIdentifier>.<serviceType>`
- MQTT `clientId`:
  - compatibility mode: `<serviceIdentifier>`
  - composite mode: `soulcam:<serviceIdentifier>`

Identity must be stable across reboots and must match discovery binding rules.

## 4) SoulCam Baseline Reuse

Existing reusable components:

- `src/main.cpp` runtime lifecycle + control socket (`/tmp/soulcam_ctrl.sock`)
- `scene/scene_hub.py` event bridge candidate
- `service/soulcam.service` and `service/scene_hub.service`
- `scripts/deploy.sh` (already defaults to target device)

Plan direction: keep media/AI pipeline unchanged; add Soullink transport layer.

## 5) Gap Analysis for `soulcam` Soullink Client

Missing pieces:

1. Descriptor file + schema validation pipeline.
2. mDNS advertiser driven by `soulcam` profile.
3. MQTT client/topic runtime driven by `soulcam/debug/`.
4. SoulCmd dispatcher with explicit command matrix.
5. `syncFiles` worker (`commitID`, `deleteAll`, `204` handling).
6. TCP frame receiver with packet reassembly.
7. End-to-end compatibility test checklist automation.
8. RTSP capability contract publication (endpoint, health, and host-consumable metadata).

## 6) Runtime Architecture

## 6.1 In-process model (implemented)

- `soulcam` (C++) now includes a native `soullink` module:
  - profile-driven identity/topic policy
  - mDNS advertisement
  - MQTT command/downlink + uplink
  - SoulCmd dispatcher
  - `syncFiles` worker
  - TCP JPEG frame receiver
  - RTSP health/readiness publication

## 6.2 Bridge model

- Downlink:
  - `soulcam/debug/in/<clientId>` -> `soullink` C++ module in-process dispatcher.
- Uplink:
  - DP -> `soulcam/debug/out/<clientId>`
  - message -> `soulcam/debug/m/<clientId>`
  - stream -> `soulcam/debug/s/<clientId>/<streamIndex>`
- RTSP:
  - `soulcam` process remains RTSP producer (`rtsp://<device_ip>:8554/cam` by default).
  - in-process `soullink` module publishes RTSP availability/state to host-facing channels.

## 7) Protocol Mapping Plan

## 7.1 MQTT minimum behavior

1. Connect broker (`defaultPorts.mqtt`).
2. Subscribe `soulcam/debug/in/<clientId>`.
3. Parse SoulCmd payload `{ "cmd": <number>, "data": ... }`.
4. Publish result/status to `out/m/s` topics.

## 7.2 SoulCmd support matrix (MVP)

Implement first:

- `setDp`, `getDp`, `getDpAll`
- `subStream`, `unsubStream`, `streaming`
- `syncFiles`

Controlled/guarded:

- `reboot`, `soulReload`, `sysCmd`

Unsupported in phase-1 must return explicit structured message (`id=0`):

- non-applicable commands (for example touch/calibration/UI commands)

## 7.3 Uplink data formats

- DP report:
  - `{ "cmd": 1, "data": [ { "dp": 1001, "value": 1 } ] }`
- Message report:
  - `id=0` normal messages
  - `id=1` sync progress/success
- Stream record (`streamIndex=0`):
  - `uint16 uid + uint8 oldStatus + uint8 newStatus + uint32 timestamp` (LE)

## 7.4 RTSP as core capability

RTSP is a mandatory capability for `deviceType=soulcam`.

Implementation requirements:

1. Keep RTSP service managed by `soulcam` runtime (`src/main.cpp` path).
2. Expose stable RTSP URI contract (default `rtsp://<device_ip>:8554/cam`).
3. Publish RTSP health/status through Soullink uplink messaging (online/offline and error states).
4. Include RTSP checks in startup gating so Soullink does not report fully-ready while RTSP is down.

Planned documentation artifact:

- `soullink/docs/rtsp_capability.md`
  - endpoint format
  - codec/resolution/fps defaults
  - health check policy
  - host integration notes

## 8) `syncFiles` Plan

On `cmd=13`:

1. Read `data.api`.
2. `POST /api/git/get-diff` with `commitID` + `asFile=false`.
3. Handle `204` as success/no-op.
4. Apply `deleteAll`, then `D/R/A/M`.
5. Persist new `commitID`.
6. Publish progress and completion (`id=1`).

Safety:

- stage updates before replace
- checksum/size checks where available
- bounded retries + timeout
- minimal restart scope (only impacted services)

## 9) TCP Frame Receiver Plan

- Listen on `defaultPorts.streamTcp` (initially `1234`).
- Parse repeated frame packets:
  - 4-byte LE size
  - JPEG bytes
- Support partial/merged packet reassembly.
- Enforce frame size cap (for example <= 2 MB).
- Forward latest frame to local consumer sink.

## 10) Delivery Phases

## Phase 0 - Foundation

- [x] Create `soullink/` scaffolding.
- [x] Add descriptor schema + `soulcam.profile.dec.json`.
- [x] Add native `src/soullink/` module scaffolding.

## Phase 1 - Discovery and MQTT

- [x] Implement profile loader + mDNS advertiser.
- [x] Implement MQTT connect/subscribe/publish.
- [x] Validate identity binding (stable service ID + compatibility/composite client ID modes).

## Phase 2 - Commands and DP

- [x] Implement SoulCmd dispatcher and MVP command set.
- [x] Implement DP/message uplink.

## Phase 3 - Sync and TCP stream

- [x] Implement `syncFiles` worker.
- [x] Implement TCP frame receiver and stream uplink.

## Phase 4 - Hardening

- [~] Retry/backoff, watchdog, observability, soak tests.
  - retries/timeouts and health messages are implemented
  - watchdog/soak/runtime burn-in still pending target-device run

## 11) Validation Gates

- [ ] Discovery appears within 10s on same subnet. (needs target run)
- [ ] MQTT connection remains stable. (needs broker soak run)
- [x] Required commands execute correctly. (unit-tested command paths)
- [x] `syncFiles` handles `204`, `deleteAll`, `commitID` correctly. (unit-tested)
- [x] TCP frame stream decodes continuously without crashes. (parser/reassembly unit-tested)
- [ ] RTSP endpoint is reachable and stable (`rtsp://<device_ip>:8554/cam` by default). (needs device runtime verification)
- [ ] `deviceType=soulcam` passes host allowlist/admission. (host team dependency)

## 12) Missing Information (Must Confirm Before Final Build)

The following inputs are currently missing or not confirmed for `deviceType=soulcam`:

1. **Host registration/allowlist**
   - Is `soulcam` already registered and allowed in SoulFlow host registry?
   - Exact allowlist source and rollout process for production.

2. **Final mDNS service type**
   - Confirm `_soulcamDebug._tcp.local` naming is accepted.
   - Any alternate naming convention required by host parser.

3. **Final MQTT topic prefix**
   - Confirm `soulcam/debug/` is accepted end-to-end.
   - Confirm whether trailing slash enforcement is strict.

4. **Channel policy**
   - Confirm whether `useCompositeDeviceKey=true` is required for `soulcam`.
   - Confirm runtime mode default: compatibility vs composite `clientId`.

5. **Capability flags**
   - Confirm final values for:
     - `monitor`
     - `videoStream`
     - `directorMode`

6. **DP contract**
   - Definitive DP ID list, types, ranges, and read/write permissions.
   - Which DPs are mandatory for UI/device health.

7. **Command behavior contract**
   - For each SoulCmd, define:
     - supported/unsupported status
     - expected request schema
     - expected response payload schema

8. **Sync file scope and safety rules**
   - Which directories are sync-managed vs protected.
   - Whether binary/service files are allowed in sync payload.
   - Required rollback behavior on partial failure.

9. **TCP frame sink contract**
   - What component consumes received JPEG frames on SoulCam.
   - Required frame rate/latency limits and backpressure behavior.

10. **RTSP host contract**
    - Does host require RTSP URI in DP, message, or separate registration API?
    - Required auth mode for RTSP (none/basic/token) and production policy.
    - Required codec/profile constraints (H.264/H.265, fps/bitrate limits).

11. **Security requirements**
    - MQTT auth/TLS requirements (if any).
    - mDNS exposure policy on production networks.
    - Secret management method for credentials.

12. **Acceptance test ownership**
    - Who signs off host/device interoperability.
    - Required test evidence format for release.

## 13) Immediate Next Actions

1. Run live SoulFlow connect/disconnect flow and verify:
   - connect uses `/debug` handshake to retarget broker
   - `disconnectServer` suspends MQTT promptly
   - reconnect resumes on next handshake.
2. Run subnet validation gates (mDNS visibility, command roundtrip, RTSP readiness).
3. Complete host-team confirmations in Section 12 (allowlist, contracts, security policy).
4. Add soak/watchdog hardening once runtime telemetry is captured on device.
