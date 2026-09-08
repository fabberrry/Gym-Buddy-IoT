#pragma once
#include "people_counter/people_counter.hpp"
#include <ostream>
#include <string>

namespace gateway {
enum class Event { None, Entry, Exit, Ambiguous };
enum class Status { Valid, Uncertain };
struct RoomState {
    std::string deviceId, roomId, sessionId;
    std::uint32_t count = 0;
    Event lastEvent = Event::None;
    std::uint64_t timestamp = 0; // UTC Unix milliseconds; supplied by caller.
    Status status = Status::Valid;
    // Negative means unavailable; serialized as null. No invented confidence.
    double confidence = -1;
    std::uint64_t sequence = 0;
};
std::string toJson(const RoomState& state);
class IStatePublisher {
public:
    virtual ~IStatePublisher() = default;
    // Must return promptly (enqueue/cache for network I/O); false means retry.
    virtual bool publish(const RoomState& state) = 0;
};
class ConsolePublisher final : public IStatePublisher {
public:
    explicit ConsolePublisher(std::ostream& stream) : stream_(stream) {}
    bool publish(const RoomState& state) override;
private:
    std::ostream& stream_;
};
// Owns latest state, coalesces failed publications, retries on monotonic schedule.
// Single caller; network adapters must do blocking I/O on a separate worker.
class RoomStateService {
public:
    RoomStateService(RoomState initial, IStatePublisher& publisher,
                     doorway::Milliseconds interval = 1000);
    void observe(const doorway::CounterResult& result, std::uint64_t utc_ms);
    bool flush(doorway::Milliseconds now, std::uint64_t utc_ms, bool force = false);
    const RoomState& latest() const { return state_; }
private:
    RoomState state_;
    IStatePublisher& publisher_;
    doorway::Milliseconds interval_, last_attempt_ = 0;
    bool dirty_ = true, attempted_ = false, failed_ = false;
};
}
