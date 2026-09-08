# Future ESP32 input adapter

`pipeline_scaffold.hpp` is a compiled, desktop-tested wiring example using the
same `RoomStateService` and `IStatePublisher` as the simulator. It is not verified
ESP32 firmware. Implement a nonblocking `SensorInput`, supply fresh monotonic
timestamps and UTC milliseconds, then call `Pipeline::poll` from one task.
Initialize RoomState with configured roomId/deviceId, a new sessionId, and the
known initial baseline. The pipeline initializes the counter from that same count.
The stable app payload is defined in [integration-contract.md](../../docs/integration-contract.md).

Implement `doorway::SensorInput` around the chosen driver, or call
`PeopleCounter::process` directly from a serialized acquisition loop.
The C++ core includes no ESP32/Arduino headers. Add only `src/people_counter.cpp`
and `include/` to the firmware build, using C++17. Constructor configuration
validation throws `std::invalid_argument`; enable C++ exceptions for this build
or supply a reviewed non-throwing configuration factory before using a firmware
toolchain with exceptions disabled. Frame processing does not throw or allocate.

Adapter responsibilities:

- Convert valid range/status data to occupied/clear using calibrated per-zone
  baselines, activation and release hysteresis. Retain unknown readings as
  `Quality::Unknown`; never replace a failed return with clear or stale distance.
- Number zones in doorway travel order. With two VL53L1X devices, define A and B
  with overlapping longitudinal sensing regions. Pair readings within a measured
  skew bound; reject stale pairs. Sequential ROI scans need acquisition timestamps
  and a skew check, rather than a fabricated simultaneous snapshot.
- With VL53L5CX, use native grids to retain lateral blobs and map their trajectories
  to at least three ordered depth bands. A frame-wide OR over lateral cells loses
  the multi-person advantage. Calibrate/register two sensors and associate targets
  across views so one person is not counted twice.
- Emit `Evidence::Unverified` by default. `IsolatedPerson` requires a controlled
  lane or independently validated track isolation. Track merging, splitting,
  identity uncertainty or overlapping unrelated occupants means `Conflict`.
  This segmentation/tracking layer is future work, not provided by these drivers.
- Emit one atomic full snapshot per acquired frame, including unchanged states.
  Propagate acquisition failures and detected dropped frames. Never replay cached
  occupancy under a fresh timestamp as if it were a new measurement.
- Extend timer rollover into uint64 milliseconds (or use a native monotonic 64-bit
  clock). Serialize calls. Drive the watchdog even when the sensor stops responding.
- On boot or reset observe a clear doorway before starting. Configure debounce,
  sampling gap, clearance and maximum crossing duration from recorded hardware
  traces. Route `Ambiguous` reasons and count saturation to diagnostics.

For genuinely separate simultaneous tracks, create one `CrossingDetector` per
stable track and apply its one-time accepted results to a shared `PeopleCount`.
Each track still needs a valid initial clear baseline and a confirmed ending;
disappearance/occlusion alone cannot mean clear. Do not treat track replacement
as a new person or create independent counters per overlapping sensor.

Hardware-specific work remaining:

1. Select the ESP32 board/toolchain and wire power, I2C, reset/address pins and
   sensor interrupts. Initialize the actual ToF driver and verify its status codes.
2. Calibrate mounting geometry, background ranges, hysteresis, sensor skew,
   sampling/debounce/timeout limits, and validate recordings against ground truth.
3. Implement range-to-occupancy acquisition, unknown/dropout reporting and, for
   crowded doorways, validated multi-zone person tracking and association.
4. Supply an extended monotonic acquisition clock plus a synchronized UTC mapping;
   schedule acquisition/watchdog independently of network work.
5. Implement the ESP32 network driver/credentials/TLS provisioning and a bounded,
   nonblocking publisher worker or HTTP state cache behind `IStatePublisher`.
   Reuse `toJson` unchanged; publish the newest cached state after reconnect.
6. Validate memory, timing, power-loss behavior and baseline restoration policy on
   the board. The desktop implementation intentionally does not persist counts.

The desktop HTTP adapter, contract, simulation and app integration are usable now.
No Kotlin or app-side algorithm implementation is needed for this hardware work.
