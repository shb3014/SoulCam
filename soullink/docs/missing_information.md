# Missing Information Tracker

This file tracks unresolved host-integration requirements before production rollout.

## Unresolved items

1. Host-side allowlist/admission for `deviceType=soulcam`.
2. Final accepted mDNS service type string.
3. Final accepted MQTT topic prefix and slash policy.
4. Final channel policy default (`compatibility` vs `composite` client ID mode).
5. Definitive capability flags (`monitor`, `videoStream`, `directorMode`).
6. Authoritative DP contract.
7. Per-command request/response schema contract for all SoulCmd values.
8. Sync-managed directory allowlist and rollback policy.
9. JPEG TCP frame consumer contract and performance budget.
10. RTSP auth and codec requirements from host.
11. MQTT auth/TLS + secret management policy.
12. Acceptance test ownership/sign-off process.

## Current working assumptions

- mDNS type: `_soulcamDebug._tcp.local`
- MQTT prefix: `soulcam/debug/`
- RTSP endpoint: `rtsp://<device_ip>:8554/cam`
- Compatibility client ID is default (`<serviceIdentifier>`); composite remains optional.
- Discovery exposure remains continuous over mDNS to satisfy current SoulFlow service timeout/cleanup behavior.

