# Fit-Ai-IoT

C++17 doorway counter with a desktop simulator and HTTP integration endpoint.
No hardware or Android repository required. The counting core is unchanged.

Sensor / Simulator -> preprocessing -> PeopleCounter -> RoomState -> publisher -> app

## Build and test

Windows, with g++ and Python 3.9+ on PATH:

```powershell
./scripts/test.ps1
```

Portable alternative (CMake 3.16+, C++17 compiler, Python 3.9+):

```sh
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

No external libraries or Python packages are required. Tests cover all original
52 counting tests, RoomState and publisher failure/retry, deterministic scenarios,
JSON, and real HTTP integration. With multi-config generators, use the executable
under build/Debug and pass --binary build/Debug/people_counter_gateway.exe below.

## Run locally

Accelerated demo (JSON stdout, event diagnostics stderr):

```powershell
./build/people_counter_gateway.exe --scenario demo
```

```text
ENTRY count=1
ENTRY count=2
AMBIGUOUS count=2
EXIT count=1
```

Start real-time HTTP integration, then query from another terminal:

```powershell
python desktop/people_counter_gateway.py
curl.exe --fail http://127.0.0.1:8080/v1/state
```

On Unix use ./build/people_counter_gateway and curl. Ctrl+C stops the HTTP gateway.
After its traffic scenario it keeps publishing clear-room heartbeats.

Example final payload (session, timestamp and sequence vary):

```json
{"schemaVersion":1,"deviceId":"counter-01","roomId":"room-01","sessionId":"demo-1","sequence":5,"count":1,"event":"exit","timestamp":1725800001920,"status":"uncertain","confidence":null}
```

[config/gateway.json](config/gateway.json) configures identities, HTTP/console mode,
bind host/port, token environment-variable name, initial count and publish/stale
intervals. No hardcoded credentials or MQTT dependency.

Scenarios: empty, entry, exit, entries, exits, close-following, reversal, blockage,
overlap, noise, invalid, demo.

```powershell
python desktop/people_counter_gateway.py --scenario reversal --once
./build/people_counter_gateway.exe --scenario exit --initial-count 3
./build/people_counter_gateway.exe --scenario demo --session-id fixture --epoch-ms 1725800000000
```

--once overrides networking mode with an accelerated console run. The executable
also accepts --device-id, --room-id, --interval-ms and --live. Exits default to an
initial count of zero unless a known baseline is supplied.

## App developer handoff

Give the app developer [integration-contract.md](docs/integration-contract.md)
and [room-state.schema.json](docs/room-state.schema.json). Consume absolute counts,
handle uncertainty and stale data, and recover with one GET. No sensor knowledge
is required. State is memory-only: restart creates a new session and restores the
configured initialCount, not persisted occupancy.

Two basic sensors cannot prove a single person crossed. See
[architecture.md](docs/architecture.md) for the multi-zone hardware recommendation
and limitations. Unverified traffic cannot change the count. Simulation fixtures
explicitly assert isolation; crowded real traffic needs validated spatial tracking.
Ambiguity leaves count unchanged and uncertainty sticky for the session.

## Structure

```text
.
|-- CMakeLists.txt
|-- README.md
|-- .gitignore
|-- include/
|   |-- people_counter/{people_counter.hpp,simulated_input.hpp}
|   `-- gateway/{room_state.hpp,scenarios.hpp}
|-- src/{people_counter.cpp,room_state.cpp}
|-- apps/people_counter_gateway.cpp
|-- desktop/people_counter_gateway.py
|-- config/gateway.json
|-- examples/simulate.cpp
|-- tests/{counter_tests.cpp,room_state_tests.cpp,test_desktop.py}
|-- scripts/test.ps1
|-- docs/{architecture.md,integration-contract.md,room-state.schema.json}
`-- adapters/esp32/{README.md,pipeline_scaffold.hpp}
```

The [ESP32 scaffold](adapters/esp32/README.md) lists hardware work remaining. Its
portable wiring is desktop-tested; no real ESP32 firmware or production internet
service is claimed.
