# Architecture and inference limits

## Hardware comparison and recommendation

| Sensing layout | Useful evidence | Fundamental limitations |
| --- | --- | --- |
| Two basic ToF sensors, reduced to A/B occupancy | Direction of an isolated continuous passage; observed reversals and partial passage | Boolean occupancy loses person identity and number. Two touching followers or opposing people can alias a single-person trace. |
| Two native multi-zone ToF sensors | Spatial blobs, lateral separation, trajectory continuity, additional views | Occlusion, merged blobs, unsynchronized frames and track swaps remain possible. Two devices alone do not guarantee observability. |
| More than two ordered depth zones | More ordering constraints, middle-zone starts, skipped transitions and reversals become easier to reject | Additional binary depth bands still lose lateral separation and multiplicity. More bands alone cannot resolve simultaneous people. |

Engineering recommendation: start with overhead native multi-zone coverage, using
at least three depth bands ordered outside to inside and retaining lateral cells.
Use a second calibrated view if doorway width or occlusion demands it. Position
and field of view must be validated against mounting height and the actual door.
Extract person blobs, associate them over time, and track each independently.
Reject merged/split/occluded or uncertain associations. Each isolated track may
feed its own detector; accepted track completions feed one shared `PeopleCount`.
Assign stable track IDs and prevent duplicate completions across sensors.
Unresolved interactions must invalidate every affected pending track.

This repository implements the conservative ordered-zone detector, **not** a
depth-image person segmenter or multi-person tracker. Without that future layer,
use a physically controlled single-person lane or leave evidence unverified.
Do not convert unrestricted doorway occupancy to `IsolatedPerson` merely because
there is one connected blob or a plausible A/B order.

ST documents native 4x4/8x8 spatial output for
[VL53L5CX](https://www.st.com/en/imaging-and-photonics-solutions/vl53l5cx.html).
Its advertised fastest mode is 4x4 at 60 Hz; consult the
[datasheet](https://www.st.com/resource/en/datasheet/vl53l5cx.pdf) for resolution,
timing and operating constraints. Choose resolution and rate together; a fast
person must occupy each meaningful state for several acquired frames.
[VL53L1X](https://www.st.com/en/imaging-and-photonics-solutions/vl53l1x.html)
offers a programmable ROI. Scanning ROIs between measurements is sequential, as
described in [ST AN5191](https://www.st.com/resource/en/application_note/dm00516219-using-the-programmable-region-of-interest-roi-with-the-vl53l1x-stmicroelectronics.pdf);
it must not be represented as a simultaneous native grid.

## Layers and assumptions

1. `SensorInput` produces full `SensorFrame` occupancy snapshots. Bits represent
   ordered zones; A is outside and the final zone is inside. All zones must be
   valid and temporally coherent. Unknown is a quality flag, never a clear zone.
2. `OccupancyFilter` debounces whole masks, preserving simultaneous changes.
   Quality and conflict checks bypass debounce. Evidence uncertainty is sticky
   throughout a crossing, including activation candidates before debounce.
3. `CrossingDetector` validates time, requires initial clear, accepts ordered
   rising and falling edges, and waits for clear confirmation before emitting.
4. `PeopleCount` applies Enter/Exit and reports saturation at zero or UINT32_MAX.
   `PeopleCounter` combines one detector with one count. No heap allocation in
   frame processing; up to 16 zones; constant storage and work per frame.
5. Future ESP32 adapters own drivers, range thresholds, calibration and tracking.
   Only the simulation helper allocates a vector for replay.

Timestamps are monotonic uint64 milliseconds from acquisition, not wall clock or
arrival order. Repeated identical timestamps/frames are ignored; different frames
at the same timestamp are malformed. Backward times invalidate the pending
passage and do not rewind the watermark. Frames after a watchdog fault are
discarded. Resume with newer timestamps and a confirmed clear interval. Explicit
reset preserves the count and clock watermark; reconstruct on clock-epoch change.
Call `process` for periodic snapshots even when nothing changes. Call `tick`
periodically if the input stalls. Tick can invalidate, never establish clearance.
One caller must serialize input and watchdog calls; classes are not thread safe.

## State machine

| State | Action |
| --- | --- |
| Quarantine | Startup/reset/fault: ignore occupied tails, require valid raw and filtered clear for `clear_ms`. |
| Idle | Only first or last zone may initiate a crossing. |
| Crossing | Each zone must rise once in travel order and fall once in the same order. A zone cannot fall before the next rises, except the final zone. Exactly one bit changes per accepted transition. |
| Settling | All zones visited and cleared. Require another `clear_ms` of observed clearance; any raw activation invalidates the pending result. |

For two zones the only directional paths after removing repeats are
`clear, A, AB, B, clear` and its reverse. A then B without observed overlap is
ambiguous. For three zones, both `A, AB, B, BC, C, clear` and
`A, AB, ABC, BC, C, clear` are valid under the isolation assumption.
Zones must physically support observed overlap: this detector deliberately does
not bridge an unsensed gap by guessing a person's identity.

Any unexpected transition emits `Ambiguous`, never a count adjustment, and
quarantines the remainder. Well-formed but unverified completed traffic also
emits `Ambiguous`. No numerical confidence score implies an unmeasured accuracy.
`Reason` identifies malformed input, missing frames, conflict, unknown data,
timeout, unverified traffic or sequence failure.
An exit at zero remains a detected Exit with `CountUpdate::AtZero`; external
code can recognize the count discrepancy without unsigned underflow.

## Timing and filtering

Defaults are simulation examples, not calibrated hardware promises:

| Setting | Default | Meaning |
| --- | --- | --- |
| zones | 2 | Supported range 2..16, outside to inside |
| debounce_ms | 20 | Unchanged candidate mask duration before acceptance; zero disables |
| clear_ms | 100 | Confirmed clearance for startup/recovery and completion |
| max_frame_gap_ms | 200 | Larger inter-frame/watchdog gap invalidates state |
| crossing_timeout_ms | 15000 | Maximum complete passage including final clearance |

Debounce accepts at `>=`; watchdog/timeout fail at `>`. Passage time begins on
the first accepted activation. Long dwell is valid within the configured timeout;
expiry produces ambiguity, never an automatic direction. Continued occupied input
after expiry cannot start another crossing. No-observation periods never satisfy
clearance. Duplicate packets do not advance debounce time.

Fast motion below sampling/debounce resolution may be suppressed entirely and
produce no event. Recognized skipped transitions are ambiguous. An entirely
unseen person cannot be flagged by software. Likewise a missing sample within
the configured maximum gap cannot be detected unless it changes the observed
sequence or the adapter marks quality unknown. Filtering can hide a short real
reversal just as it hides noise. Reduce thresholds only with measured noise and
speed data; there is no parameter that eliminates this observability limit.

## Scenario policy and validation

Tests cover entries/exits, bounds, partial passages, both reversals, backing out,
simultaneous activation, bounce, duplicates, fast/slow movement, standing timeout,
followers with and without clear gaps, conflicting traffic, missing data,
out-of-order/malformed frames, startup, recovery, three-zone motion and simulation.
They also enumerate all 4096 six-snapshot two-zone traces with and without
isolation evidence. These are logic checks, not a measured real-world accuracy.

Close followers are countable separately only when their passages are isolated
and a full clear interval separates them (or a future tracker isolates them).
Followers without that separation invalidate the combined episode. A merged
pair, simultaneous people, or opposing traffic can produce exactly a valid
single-person trace: tests pass that trace as unverified and expect Ambiguous.
The core has no way to detect a falsely asserted isolation flag.

Future validation needs labeled hardware recordings including children, bags,
mobility aids, lighting, height, door movement, crowded traffic and sensor failure.
Measure false counts, missed counts and rejection rate separately. Conservative
rejection sacrifices recall and creates count drift; reconcile occupancy through
an independent known baseline. No accuracy percentage is claimed here.
