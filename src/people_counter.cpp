#include "people_counter/people_counter.hpp"
#include <limits>
#include <stdexcept>

namespace doorway {
ZoneMask OccupancyFilter::update(ZoneMask raw, Milliseconds now) {
    if (raw != candidate_) { candidate_ = raw; since_ = now; }
    if (now - since_ >= debounce_) stable_ = candidate_;
    return stable_;
}
void OccupancyFilter::reset() { candidate_ = stable_ = 0; since_ = 0; }

CrossingDetector::CrossingDetector(Config config) : config_(config), filter_(config.debounce_ms) {
    if (config.zones < 2 || config.zones > 16 || config.clear_ms == 0 ||
        config.max_frame_gap_ms == 0 || config.crossing_timeout_ms == 0 ||
        config.debounce_ms >= config.crossing_timeout_ms)
        throw std::invalid_argument("Invalid doorway configuration");
}
void CrossingDetector::reset() {
    phase_ = Phase::Quarantine;
    clear_pending_ = false;
    previous_ = 0;
    rises_ = falls_ = 0;
    isolated_ = true;
    filter_.reset();
}
Detection CrossingDetector::reject(Reason reason, Milliseconds now) {
    reset();
    return {Outcome::Ambiguous, reason, now};
}
ZoneMask CrossingDetector::bit(unsigned index) const {
    return ZoneMask(1u << (reverse_ ? config_.zones - 1 - index : index));
}
Detection CrossingDetector::tick(Milliseconds now) {
    if (have_time_ && now < last_time_) return reject(Reason::OutOfOrder, now);
    have_time_ = true;
    last_time_ = now;
    if (have_frame_ && now - last_frame_ > config_.max_frame_gap_ms) {
        have_frame_ = false;
        return reject(Reason::MissingFrames, now);
    }
    if ((phase_ == Phase::Crossing || phase_ == Phase::Settling) &&
        now - started_ > config_.crossing_timeout_ms) return reject(Reason::Timeout, now);
    return {};
}
Detection CrossingDetector::process(const SensorFrame& f) {
    const Detection watchdog = tick(f.time_ms);
    if (watchdog.outcome != Outcome::None) return watchdog; // Fault frame is discarded.
    if (have_frame_ && f.time_ms == last_frame_) {
        if (f.occupancy == last_sample_.occupancy && f.quality == last_sample_.quality &&
            f.evidence == last_sample_.evidence) return {};
        return reject(Reason::MalformedFrame, f.time_ms);
    }
    last_frame_ = f.time_ms;
    last_sample_ = f;
    have_frame_ = true;
    const auto allowed = (std::uint32_t(1) << config_.zones) - 1;
    if ((f.occupancy & ~allowed) != 0 ||
        (f.quality != Quality::Valid && f.quality != Quality::Unknown) ||
        (f.evidence != Evidence::Unverified && f.evidence != Evidence::IsolatedPerson &&
         f.evidence != Evidence::Conflict)) return reject(Reason::MalformedFrame, f.time_ms);
    if (f.quality == Quality::Unknown) return reject(Reason::UnknownReading, f.time_ms);
    if (f.evidence == Evidence::Conflict) return reject(Reason::TrafficConflict, f.time_ms);

    const ZoneMask current = filter_.update(f.occupancy, f.time_ms);
    if (phase_ == Phase::Quarantine) {
        if (f.occupancy != 0 || current != 0) { clear_pending_ = false; return {}; }
        if (!clear_pending_) { clear_pending_ = true; clear_since_ = f.time_ms; }
        if (f.time_ms - clear_since_ >= config_.clear_ms) {
            phase_ = Phase::Idle;
            clear_pending_ = false;
        }
        return {};
    }
    // Include evidence on pre-debounce frames; never launder conflict/uncertainty.
    if (f.occupancy != 0 || phase_ == Phase::Crossing || phase_ == Phase::Settling)
        isolated_ = isolated_ && f.evidence == Evidence::IsolatedPerson;
    if (phase_ == Phase::Idle) {
        if (current == 0) {
            if (f.occupancy == 0) isolated_ = true;
            return {};
        }
        if (current != 1 && current != ZoneMask(1u << (config_.zones - 1)))
            return reject(Reason::Sequence, f.time_ms);
        reverse_ = current != 1;
        started_ = f.time_ms;
        rises_ = 1;
        falls_ = 0;
        previous_ = current;
        phase_ = Phase::Crossing;
        return {};
    }
    if (phase_ == Phase::Settling) {
        // Even a brief new activation invalidates the pending completion.
        if (f.occupancy != 0 || current != 0) return reject(Reason::Sequence, f.time_ms);
        if (f.time_ms - clear_since_ < config_.clear_ms) return {};
        const bool trusted = isolated_;
        phase_ = Phase::Idle;
        isolated_ = true;
        return {trusted ? (reverse_ ? Outcome::Exit : Outcome::Enter) : Outcome::Ambiguous,
                trusted ? Reason::None : Reason::UnverifiedTraffic, f.time_ms};
    }
    if (current == previous_) return {};
    const ZoneMask changed = current ^ previous_;
    if ((changed & (changed - 1)) != 0) return reject(Reason::Sequence, f.time_ms);
    if ((current & changed) != 0) {
        if (rises_ >= config_.zones || changed != bit(rises_))
            return reject(Reason::Sequence, f.time_ms);
        ++rises_;
    } else {
        if (falls_ >= config_.zones || changed != bit(falls_) ||
            (falls_ + 1 < config_.zones && rises_ <= falls_ + 1))
            return reject(Reason::Sequence, f.time_ms);
        ++falls_;
    }
    previous_ = current;
    if (current == 0) {
        if (rises_ != config_.zones || falls_ != config_.zones)
            return reject(Reason::Sequence, f.time_ms);
        phase_ = Phase::Settling;
        clear_since_ = f.time_ms;
    }
    return {};
}
CountUpdate PeopleCount::apply(Outcome outcome) {
    if (outcome == Outcome::Enter) {
        if (value_ == std::numeric_limits<std::uint32_t>::max()) return CountUpdate::AtMaximum;
        ++value_;
        return CountUpdate::Incremented;
    }
    if (outcome == Outcome::Exit) {
        if (value_ == 0) return CountUpdate::AtZero;
        --value_;
        return CountUpdate::Decremented;
    }
    return CountUpdate::None;
}
} // namespace doorway
