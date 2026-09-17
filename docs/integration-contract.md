# App integration contract — RoomState v1

The app consumes room snapshots; it needs no sensor or algorithm knowledge.
The canonical machine-readable JSON Schema is [room-state.schema.json](room-state.schema.json)
([JSON Schema 2020-12](https://json-schema.org/draft/2020-12/json-schema-validation)).
All successful transports use that exact object without a transport envelope.
Breaking changes require a new schemaVersion and endpoint version.

## Fields / schema summary

| Field | JSON type | Meaning |
| --- | --- | --- |
| schemaVersion | integer, constant 1 | Contract version |
| deviceId | string | Stable configured identity of the authoritative counter |
| roomId | string | Stable room identity agreed with the app; not a display label |
| sessionId | string | New random identity on each gateway start; distinguishes resets |
| sequence | nonnegative integer | Increasing snapshot revision within one session |
| count | integer, 0..4294967295 | Absolute current occupancy estimate; never a delta |
| event | entry / exit / ambiguous / none | Last detected event (C++ `lastEvent`); retained on heartbeats |
| timestamp | nonnegative integer | UTC Unix **milliseconds**, snapshot observation time |
| status | valid / uncertain | Count consistency, not networking connectivity |
| confidence | number 0..1 or null, optional | Unavailable in this implementation: always null |

All fields except confidence are required. IDs contain 1..128 ASCII letters,
digits, `.`, `_`, `:`, or `-`. Parse timestamps/sequence as 64-bit integers.
Sequence orders state even if the wall clock changes; retries can repeat a revision.
Timestamp is not the event occurrence time: heartbeats refresh it while retaining
the last event. Simulation uses a captured UTC origin plus monotonic elapsed time;
`--epoch-ms` supplies a fixed origin for reproducible fixtures. Never send raw
ESP32 uptime as UTC. Future firmware must establish a valid UTC mapping before
publishing; connectivity/freshness errors are distinct from count uncertainty.

## Count and event semantics

- `none`: initial state, no detected event yet. No count change.
- `entry`: accepted passage, normally increases count by one.
- `exit`: accepted passage, normally decreases count by one.
- `ambiguous`: unresolved passage or invalid input; **does not change count**.
- Counts saturate at zero and UINT32_MAX. An exit at zero still reports `exit`,
  count zero and status uncertain; an entry at the maximum behaves similarly.
- Ambiguity or saturation makes status uncertain for the rest of the session.
  Later valid crossings still update the estimate but cannot repair earlier drift.
- `valid` means consistent with the configured initial baseline and accepted input
  assumptions, not a guarantee of measured accuracy. No confidence percentage is
  invented. Initial count defaults to zero; configure a known baseline explicitly.

Use `count` directly. Do not increment/decrement locally from `event`, because
heartbeats retain events and polls can skip intermediate crossings. This is a
latest-state protocol, not a durable event log. One configured device is the
authority for a room. Do not sum multiple overlapping devices' room snapshots;
multi-door aggregation requires one authority upstream of this contract.

Initial example:

```json
{"schemaVersion":1,"deviceId":"counter-01","roomId":"room-01","sessionId":"demo-1","sequence":0,"count":0,"event":"none","timestamp":1725800000000,"status":"valid","confidence":null}
```

After entry:

```json
{"schemaVersion":1,"deviceId":"counter-01","roomId":"room-01","sessionId":"demo-1","sequence":1,"count":1,"event":"entry","timestamp":1725800000620,"status":"valid","confidence":null}
```

Ambiguity after two entries (count stays two):

```json
{"schemaVersion":1,"deviceId":"counter-01","roomId":"room-01","sessionId":"demo-1","sequence":4,"count":2,"event":"ambiguous","timestamp":1725800001220,"status":"uncertain","confidence":null}
```

Subsequent exit:

```json
{"schemaVersion":1,"deviceId":"counter-01","roomId":"room-01","sessionId":"demo-1","sequence":5,"count":1,"event":"exit","timestamp":1725800001920,"status":"uncertain","confidence":null}
```

## Networking and frequency

- HTTP: `GET http://127.0.0.1:8080/v1/state` returns 200 with one RoomState.
  `Content-Type: application/json; charset=utf-8`, `Cache-Control: no-store`.
  It reads an atomic latest-state cache; slow/disconnected clients do not block
  the counting process. No write endpoint or CORS configuration is provided.
- Health: `GET /health` returns 200 `{"status":"ok"}` while the HTTP server is
  running. It does not certify that the producer is fresh: use `/v1/state` for
  that. If Bearer authentication is configured, it applies to both endpoints.
- Console: one identical JSON object per stdout line (NDJSON). Human-readable
  `ENTRY count=1` diagnostics go to stderr. The HTTP adapter consumes this stream.
- MQTT is not implemented or required. A future publisher can carry the same
  JSON as a retained latest-state message behind `IStatePublisher`; topic/QoS
  settings would belong to that adapter, not the counting core.

Startup and each detection publish immediately when the publisher is healthy.
Heartbeats publish every `publishIntervalMs` (1000 default) while the simulator is
live, including after its finite traffic scenario finishes. Failed publications
retry at that interval, coalescing to the latest state; no unbounded event queue.
`flush(..., force=true)` republishes the latest state on a transport reconnect.
The C++ publisher interface requires prompt return: real network adapters must
enqueue to a bounded worker/cache. The desktop stdout sink is for a drained local
pipe/console, not a substitute for a nonblocking firmware network driver.

Poll about once per second. Faster polls may return the same revision. HTTP has
no server-side client session and needs no reconnect handshake. Each GET obtains
the newest cached state. Slow polling is safe for occupancy but cannot capture
every event.

## Reconnect, restart and errors

After reconnect, GET `/v1/state`, validate version/identity and replace the app's
displayed count. Within one session, ignore older revisions; equal revisions are
duplicates. With a new sessionId, discard the old sequence watermark and accept
the new baseline. Reconnect alone does not reset the counter. **Process restart
does reset it to initialCount**; state is memory-only, not durable storage. Do not
silently interpret restart as proof that the room emptied. The operator must
choose the correct baseline; the app should flag a changed session for review.

| Response / failure | Consumer action |
| --- | --- |
| 200 / status uncertain | Show the estimate with uncertainty; do not replace it with zero |
| 401 | Supply configured Bearer token; resolve credentials before retrying |
| 404 | Correct endpoint path |
| 405 on POST | Use GET; this endpoint is read-only |
| 503 / state_unavailable | No initial state, producer stopped, or stream stale; honor Retry-After: 1 |
| Network timeout / malformed JSON / wrong schema | Retain last displayed count marked stale; retry with backoff |

Error bodies are `{ "error": "code" }`, not RoomState. Other unsupported HTTP
methods may use the standard server's 501 response. A producer stream older than
`staleAfterMs` (5000 default) returns 503 rather than serving stale success. The
app should also use its own monotonic time since last successful fresh revision
to detect loss; heartbeat timestamps and revisions advance while live.
Retry transient failures at 1, 2, 4 seconds, capped at 30 seconds with jitter.
No auto-restart of the producer conceals a count reset: restart the gateway and
reconcile the initial baseline explicitly.

## Copy-paste local integration

From the repository root:

```powershell
./scripts/test.ps1
python desktop/people_counter_gateway.py
```

In another terminal:

```powershell
curl.exe --fail http://127.0.0.1:8080/v1/state
```

Unix uses `curl` in place of `curl.exe`. For another device on your LAN, set host
to the appropriate interface or `0.0.0.0` and use the laptop's LAN IP in the app;
localhost on a phone refers to the phone. Configure the host firewall as needed.
The checked-in development config binds to `0.0.0.0`; change `host` to
`127.0.0.1` for laptop-only access. Run `--scenario slow-demo` to expose
`0 → 1 → 2 → 3 → 2 → 3` for app polling. This uses actual RoomState updates
from the simulator and counter, with four seconds of scripted clear frames
between crossings. Wall time varies with the host scheduler (about 30 seconds
on the tested Windows laptop).

`config/gateway.json` selects deviceId, roomId, mode (`http`/`console`), bind host,
port, initialCount, scenario, publishIntervalMs and staleAfterMs. This HTTP option
is a server, so host is a bind address, not a remote URL or MQTT broker.
To enable authentication, set `authTokenEnv` to an environment-variable name,
provide its value outside source control, and send `Authorization: Bearer <value>`.
A configured but empty variable fails startup. No credentials are hardcoded.

This desktop endpoint is for local/LAN development. Python explicitly describes
[http.server](https://docs.python.org/3/library/http.server.html) as unsuitable for
production; deployed internet access requires an appropriate HTTPS server/proxy.
No real ESP32 networking or sensor support is claimed by this desktop service.
