#include "gateway/room_state.hpp"
#include "gateway/scenarios.hpp"
#include "../adapters/esp32/pipeline_scaffold.hpp"
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>
using namespace gateway;
void require(bool ok) { if(!ok) throw std::runtime_error("integration assertion failed"); }
struct Publisher : IStatePublisher {
    bool fail=false, throws=false;
    unsigned calls=0;
    std::vector<RoomState> states;
    bool publish(const RoomState& s) override {
        ++calls;
        if(throws) throw std::runtime_error("disconnected");
        if(fail) return false;
        states.push_back(s); return true;
    }
};
RoomState initial() { RoomState s; s.deviceId="d"; s.roomId="r"; s.sessionId="s"; return s; }
int main() {
    try {
        Publisher p; RoomStateService service(initial(),p,100);
        doorway::PeopleCounter counter;
        std::vector<Event> events;
        std::vector<std::uint32_t> counts;
        service.flush(0,1000);
        for(const auto& f:scenario("demo")) {
            const auto r=counter.process(f);
            service.observe(r,1000+f.time_ms); service.flush(f.time_ms,1000+f.time_ms);
            if(r.detection.outcome!=doorway::Outcome::None) {
                events.push_back(service.latest().lastEvent); counts.push_back(service.latest().count);
                require(service.latest().count==counter.count());
            }
        }
        require(events==std::vector<Event>{Event::Entry,Event::Entry,Event::Ambiguous,Event::Exit});
        require(counts==std::vector<std::uint32_t>{1,2,2,1});
        require(service.latest().status==Status::Uncertain);
        for(std::size_t i=1;i<p.states.size();++i) require(p.states[i].sequence>p.states[i-1].sequence);
        Publisher flaky; flaky.fail=true;
        RoomStateService retry(initial(),flaky,100);
        require(!retry.flush(0,1000));
        retry.observe({{doorway::Outcome::Enter,doorway::Reason::None,1},doorway::CountUpdate::Incremented,1},1001);
        require(!retry.flush(1,1001) && flaky.calls==1);
        retry.observe({{doorway::Outcome::Enter,doorway::Reason::None,2},doorway::CountUpdate::Incremented,2},1002);
        flaky.fail=false; flaky.throws=true;
        require(!retry.flush(100,1100));
        require(retry.latest().count==2);
        flaky.throws=false;
        require(retry.flush(200,1200));
        require(flaky.states.size()==1 && flaky.states.back().count==2);
        const auto seq=retry.latest().sequence;
        require(retry.flush(201,1201,true));
        require(retry.latest().count==2 && retry.latest().sequence>seq);
        Publisher lost; lost.fail=true; RoomStateService offline(initial(),lost,100);
        doorway::PeopleCounter active;
        for(const auto& f:scenario("entries")) {
            const auto r=active.process(f); offline.observe(r,1000+f.time_ms); offline.flush(f.time_ms,1000+f.time_ms);
        }
        require(active.count()==3 && offline.latest().count==3);
        lost.fail=false; require(offline.flush(10000,11000,true)); require(lost.states.back().count==3);
        Publisher zero; RoomStateService bounded(initial(),zero); doorway::PeopleCounter empty;
        for(const auto& f:scenario("exits")) bounded.observe(empty.process(f),1000+f.time_ms);
        require(bounded.latest().count==0 && bounded.latest().status==Status::Uncertain);
        auto s=initial(); s.confidence=0.5;
        require(toJson(s)=="{\"schemaVersion\":1,\"deviceId\":\"d\",\"roomId\":\"r\",\"sessionId\":\"s\",\"sequence\":0,\"count\":0,\"event\":\"none\",\"timestamp\":0,\"status\":\"valid\",\"confidence\":0.5}");
        s.deviceId="a\"\\\n"; require(toJson(s).find("a\\\"\\\\\\u000a")!=std::string::npos);
        std::ostringstream stream; ConsolePublisher console(stream); require(console.publish(initial()));
        require(stream.str()==toJson(initial())+"\n");
        bool rejected=false; try { RoomStateService bad(s,p); } catch(const std::invalid_argument&) { rejected=true; }
        require(rejected);
        doorway::SimulatedInput input(scenario("entry")); Publisher wiring;
        esp32_scaffold::Pipeline pipeline(input,wiring,initial());
        for(doorway::Milliseconds t=0;t<700;t+=10) pipeline.poll(t,1000+t);
        require(pipeline.latest().count==1 && pipeline.latest().lastEvent==Event::Entry);
        std::cout << "RoomState integration tests passed\n";
        return 0;
    } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
