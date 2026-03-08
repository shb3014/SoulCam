# SoulCam DP Catalog (Initial)

This catalog documents the currently implemented DP behavior in the native Soullink C++ module.

## Implemented DP IDs

The phase-1 implementation allows dynamic DP keys and does not hardcode a closed DP list yet.

### Common defaults used by the module

- `1001` - RTSP online (`0|1`)
- `1002` - Stream subscription active (`0|1`)
- `1003` - Module ready (`0|1`)

## Command behavior

- `setDp` (`cmd=1`): accepts `data` list of `{ "dp": <int>, "value": <any> }` and updates in-memory state.
- `getDp` (`cmd=2`): accepts list of DP IDs (or `{dp:<id>}` objects) and returns matching values.
- `getDpAll` (`cmd=3`): returns all in-memory DP values.

## Pending host-team confirmation

- Final authoritative DP ID list.
- Types/ranges and read-write policy for each DP.
- Mandatory DP set required for host UI and health dashboards.

