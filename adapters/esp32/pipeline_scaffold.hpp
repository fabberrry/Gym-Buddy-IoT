#pragma once
#include "gateway/room_state.hpp"

namespace esp32_scaffold {
// Portable wiring only, NOT a sensor/network driver or verified ESP32 firmware.
class Pipeline {
public:
    Pipeline(doorway::SensorInput& input, gateway::IStatePublisher& publisher,
             gateway::RoomState initial, doorway::Config config = {},
             doorway::Milliseconds publish_interval = 1000)
        : input_(input), counter_(config, initial.count), state_(initial, publisher, publish_interval) {}
    void poll(doorway::Milliseconds monotonic_now, std::uint64_t utc_ms) {
        doorway::SensorFrame frame;
        // SensorInput::next must be nonblocking; poll often enough for the watchdog.
        if(input_.next(frame)) state_.observe(counter_.process(frame), utc_ms);
        else state_.observe(counter_.tick(monotonic_now), utc_ms);
        state_.flush(monotonic_now, utc_ms);
    }
    const gateway::RoomState& latest() const { return state_.latest(); }
private:
    doorway::SensorInput& input_;
    doorway::PeopleCounter counter_;
    gateway::RoomStateService state_;
};
}
