#pragma once
#include <cstdint>

namespace doorway {
using Milliseconds = std::uint64_t;
using ZoneMask = std::uint16_t;
enum class Zone : unsigned { A = 0, B = 1, C = 2 };
constexpr ZoneMask occupied(Zone zone) { return ZoneMask(1u << unsigned(zone)); }

// Evidence must come from physical single-file control or an upstream tracker.
// An attractive A/B trace alone is NOT evidence of isolation.
enum class Evidence { Unverified, IsolatedPerson, Conflict };
enum class Quality { Valid, Unknown };
struct SensorFrame {
    Milliseconds time_ms = 0;
    ZoneMask occupancy = 0; // All configured zones, atomically sampled.
    Quality quality = Quality::Valid;
    Evidence evidence = Evidence::Unverified;
};
class SensorInput {
public:
    virtual ~SensorInput() = default;
    virtual bool next(SensorFrame& frame) = 0;
};
struct Config {
    unsigned zones = 2; // Ordered outside -> inside; supported range 2..16.
    Milliseconds debounce_ms = 20;
    Milliseconds clear_ms = 100;
    Milliseconds max_frame_gap_ms = 200;
    Milliseconds crossing_timeout_ms = 15000;
};
enum class Outcome { None, Enter, Exit, Ambiguous };
enum class Reason {
    None, Sequence, UnverifiedTraffic, TrafficConflict, UnknownReading,
    MalformedFrame, OutOfOrder, MissingFrames, Timeout
};
struct Detection {
    Outcome outcome = Outcome::None;
    Reason reason = Reason::None;
    Milliseconds time_ms = 0;
};

// Debounce the entire snapshot: never fabricate ordering between simultaneous edges.
class OccupancyFilter {
public:
    explicit OccupancyFilter(Milliseconds debounce) : debounce_(debounce) {}
    ZoneMask update(ZoneMask raw, Milliseconds now);
    void reset();
private:
    Milliseconds debounce_, since_ = 0;
    ZoneMask candidate_ = 0, stable_ = 0;
};

class CrossingDetector {
public:
    explicit CrossingDetector(Config config = {});
    Detection process(const SensorFrame& frame);
    // Watchdog only: never treats elapsed time as evidence that occupancy cleared.
    Detection tick(Milliseconds now);
    void reset(); // Discards partial passage; requires observed clear before rearming.
private:
    enum class Phase { Quarantine, Idle, Crossing, Settling };
    Config config_;
    OccupancyFilter filter_;
    Phase phase_ = Phase::Quarantine;
    bool have_time_ = false, have_frame_ = false, clear_pending_ = false;
    bool reverse_ = false, isolated_ = true;
    Milliseconds last_time_ = 0, last_frame_ = 0, started_ = 0, clear_since_ = 0;
    SensorFrame last_sample_{};
    ZoneMask previous_ = 0;
    unsigned rises_ = 0, falls_ = 0;
    Detection reject(Reason reason, Milliseconds now);
    ZoneMask bit(unsigned index) const;
};

enum class CountUpdate { None, Incremented, Decremented, AtZero, AtMaximum };
class PeopleCount {
public:
    explicit PeopleCount(std::uint32_t initial = 0) : value_(initial) {}
    CountUpdate apply(Outcome outcome);
    std::uint32_t value() const { return value_; }
private:
    std::uint32_t value_;
};
struct CounterResult { Detection detection; CountUpdate update; std::uint32_t count; };
class PeopleCounter {
public:
    explicit PeopleCounter(Config config = {}, std::uint32_t initial = 0)
        : detector_(config), count_(initial) {}
    CounterResult process(const SensorFrame& frame) { return consume(detector_.process(frame)); }
    CounterResult tick(Milliseconds now) { return consume(detector_.tick(now)); }
    void resetDetection() { detector_.reset(); }
    std::uint32_t count() const { return count_.value(); }
private:
    CounterResult consume(Detection d) { return {d, count_.apply(d.outcome), count_.value()}; }
    CrossingDetector detector_;
    PeopleCount count_;
};
} // namespace doorway
