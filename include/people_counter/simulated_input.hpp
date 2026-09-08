#pragma once
#include "people_counter.hpp"
#include <utility>
#include <vector>

namespace doorway {
// Laptop-only convenience; core processing performs no heap allocation.
class SimulatedInput final : public SensorInput {
public:
    explicit SimulatedInput(std::vector<SensorFrame> frames) : frames_(std::move(frames)) {}
    bool next(SensorFrame& frame) override {
        if (cursor_ == frames_.size()) return false;
        frame = frames_[cursor_++];
        return true;
    }
private:
    std::vector<SensorFrame> frames_;
    std::size_t cursor_ = 0;
};
}
