#include "gateway/room_state.hpp"
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace gateway {
namespace {
std::string quote(const std::string& value) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : value) {
        if (c=='"' || c=='\\') out << '\\' << char(c);
        else if (c<32 || c>=127) out << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << unsigned(c);
        else out << char(c);
    }
    out << '"'; return out.str();
}
bool identifier(const std::string& s) {
    if(s.empty() || s.size()>128) return false;
    for(unsigned char c:s) if(!((c>='a'&&c<='z') || (c>='A'&&c<='Z') ||
        (c>='0'&&c<='9') || c=='-' || c=='_' || c=='.' || c==':')) return false;
    return true;
}
}
std::string toJson(const RoomState& s) {
    const char* event = "none";
    switch(s.lastEvent) {
        case Event::None: break;
        case Event::Entry: event="entry"; break;
        case Event::Exit: event="exit"; break;
        case Event::Ambiguous: event="ambiguous"; break;
    }
    std::ostringstream out; out.imbue(std::locale::classic());
    out << "{\"schemaVersion\":1,\"deviceId\":" << quote(s.deviceId)
        << ",\"roomId\":" << quote(s.roomId) << ",\"sessionId\":" << quote(s.sessionId)
        << ",\"sequence\":" << s.sequence << ",\"count\":" << s.count
        << ",\"event\":\"" << event << "\",\"timestamp\":" << s.timestamp
        << ",\"status\":\"" << (s.status==Status::Valid?"valid":"uncertain") << "\",\"confidence\":";
    if(std::isfinite(s.confidence) && s.confidence>=0 && s.confidence<=1) out << std::setprecision(17) << s.confidence;
    else out << "null";
    out << '}'; return out.str();
}
bool ConsolePublisher::publish(const RoomState& s) {
    stream_ << toJson(s) << std::endl;
    return bool(stream_);
}
RoomStateService::RoomStateService(RoomState initial, IStatePublisher& publisher, doorway::Milliseconds interval)
    : state_(std::move(initial)), publisher_(publisher), interval_(interval) {
    if(!identifier(state_.deviceId) || !identifier(state_.roomId) || !identifier(state_.sessionId) || interval==0)
        throw std::invalid_argument("IDs must use 1..128 ASCII letters/digits/._:-; interval must be positive");
}
void RoomStateService::observe(const doorway::CounterResult& r, std::uint64_t utc_ms) {
    if(r.detection.outcome==doorway::Outcome::None) return;
    state_.count=r.count;
    switch(r.detection.outcome) {
        case doorway::Outcome::Enter: state_.lastEvent=Event::Entry; break;
        case doorway::Outcome::Exit: state_.lastEvent=Event::Exit; break;
        case doorway::Outcome::Ambiguous: state_.lastEvent=Event::Ambiguous; state_.status=Status::Uncertain; break;
        case doorway::Outcome::None: break;
    }
    if(r.update==doorway::CountUpdate::AtZero || r.update==doorway::CountUpdate::AtMaximum)
        state_.status=Status::Uncertain;
    state_.confidence=-1;
    state_.timestamp=utc_ms;
    ++state_.sequence;
    dirty_=true;
}
bool RoomStateService::flush(doorway::Milliseconds now, std::uint64_t utc_ms, bool force) {
    const bool due=!attempted_ || (now>=last_attempt_ && now-last_attempt_>=interval_);
    if(!force && ((!dirty_ && !due) || (failed_ && !due))) return false;
    if(!dirty_ && !failed_) { state_.timestamp=utc_ms; ++state_.sequence; }
    last_attempt_=now; attempted_=true;
    bool ok=false;
    try { ok=publisher_.publish(state_); } catch(...) { ok=false; }
    failed_=!ok;
    if(ok) dirty_=false;
    return ok;
}
}
