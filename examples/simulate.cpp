#include "people_counter/simulated_input.hpp"
#include <iostream>

int main() {
    using namespace doorway;
    std::vector<SensorFrame> frames;
    // Controlled, isolated person: clear -> A -> AB -> B -> clear.
    for (Milliseconds t = 0; t <= 800; t += 10) {
        ZoneMask mask = t < 200 ? 0 : t < 300 ? occupied(Zone::A) :
                        t < 400 ? occupied(Zone::A) | occupied(Zone::B) :
                        t < 500 ? occupied(Zone::B) : 0;
        frames.push_back({t, mask, Quality::Valid, Evidence::IsolatedPerson});
    }
    SimulatedInput input(std::move(frames));
    PeopleCounter counter;
    SensorFrame frame;
    while (input.next(frame)) {
        const auto result = counter.process(frame);
        if (result.detection.outcome == Outcome::Enter)
            std::cout << "ENTER at " << frame.time_ms << " ms; count=" << result.count << '\n';
    }
    return counter.count() == 1 ? 0 : 1;
}
