#include "people_counter/people_counter.hpp"
#include "people_counter/simulated_input.hpp"
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace doorway;
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
constexpr ZoneMask A = occupied(Zone::A), B = occupied(Zone::B), C = occupied(Zone::C);
struct Rig {
    PeopleCounter counter;
    Milliseconds now = 0;
    std::vector<CounterResult> events;
    explicit Rig(Config config = {}, std::uint32_t initial = 0) : counter(config, initial) { hold(0, 150); }
    void save(CounterResult r) { if (r.detection.outcome != Outcome::None) events.push_back(r); }
    void send(ZoneMask mask, Evidence evidence = Evidence::IsolatedPerson,
              Quality quality = Quality::Valid) {
        save(counter.process({now, mask, quality, evidence}));
        now += 5;
    }
    void hold(ZoneMask mask, Milliseconds duration = 100, Evidence e = Evidence::IsolatedPerson) {
        const auto end = now + duration;
        while (now < end) send(mask, e);
    }
    void path(std::initializer_list<ZoneMask> masks, Milliseconds duration = 100,
              Evidence evidence = Evidence::IsolatedPerson) {
        for (auto mask : masks) hold(mask, duration, evidence);
        hold(0, 150, evidence);
    }
    void expect(Outcome outcome, std::uint32_t count) {
        check(events.size() == 1, "expected exactly one outcome");
        check(events[0].detection.outcome == outcome, "wrong outcome");
        check(counter.count() == count, "wrong count");
    }
};
int main() {
    unsigned passed = 0, failed = 0;
    auto test = [&](const std::string& name, const std::function<void()>& run) {
        try { run(); ++passed; std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& e) { ++failed; std::cerr << "FAIL " << name << ": " << e.what() << '\n'; }
    };
    test("one entry at zero", [] { Rig r; r.path({A, A|B, B, 0}); r.expect(Outcome::Enter, 1); });
    test("one exit", [] { Rig r({}, 2); r.path({B, A|B, A, 0}); r.expect(Outcome::Exit, 1); });
    test("repeated entries", [] {
        Rig r; for (int i=0; i<5; ++i) r.path({A,A|B,B,0});
        check(r.events.size()==5 && r.counter.count()==5, "entries lost");
    });
    test("repeated exits saturate at zero", [] {
        Rig r({}, 2); for (int i=0; i<5; ++i) r.path({B,A|B,A,0});
        check(r.events.size()==5 && r.counter.count()==0, "exit bounds");
        check(r.events.back().update==CountUpdate::AtZero, "underflow must be reported");
    });
    test("A only partial passage", [] { Rig r; r.path({A,0}); r.expect(Outcome::Ambiguous,0); });
    test("B only partial passage", [] { Rig r; r.path({B,0}); r.expect(Outcome::Ambiguous,0); });
    test("A-B-A reversal", [] { Rig r; r.path({A,A|B,A,0}); r.expect(Outcome::Ambiguous,0); });
    test("B-A-B reversal", [] { Rig r; r.path({B,A|B,B,0}); r.expect(Outcome::Ambiguous,0); });
    test("backs out after reaching far zone", [] {
        Rig r; r.path({A,A|B,B,A|B,A,0}); r.expect(Outcome::Ambiguous,0);
    });
    test("simultaneous A/B", [] { Rig r; r.path({A|B,0}); r.expect(Outcome::Ambiguous,0); });
    test("bouncing activation filtered", [] {
        Rig r; r.hold(A,5); r.hold(0,5); r.hold(A,5); r.hold(0,30);
        r.hold(A); r.hold(0,5); r.hold(A,30); r.path({A|B,B,0}); r.expect(Outcome::Enter,1);
    });
    test("exact duplicate readings ignored", [] {
        Rig r; r.hold(A);
        auto frame = SensorFrame{r.now,A,Quality::Valid,Evidence::IsolatedPerson};
        r.save(r.counter.process(frame)); r.save(r.counter.process(frame)); r.now+=5;
        r.path({A|B,B,0}); r.expect(Outcome::Enter,1);
    });
    test("very slow continuous crossing", [] {
        Config c; c.crossing_timeout_ms=30000; Rig r(c);
        r.path({A,A|B,B,0},5000); r.expect(Outcome::Enter,1);
    });
    test("fast resolved crossing", [] {
        Config c; c.debounce_ms=5; Rig r(c); r.path({A,A|B,B,0},15); r.expect(Outcome::Enter,1);
    });
    test("fast unresolved crossing", [] {
        Config c; c.debounce_ms=0; Rig r(c); r.path({A,B,0},5); r.expect(Outcome::Ambiguous,0);
    });
    test("standing timeout", [] {
        Config c; c.crossing_timeout_ms=300; Rig r(c); r.hold(A,700);
        r.expect(Outcome::Ambiguous,0); check(r.events[0].detection.reason==Reason::Timeout,"timeout reason");
    });
    test("A never followed by B times out", [] {
        Config c; c.crossing_timeout_ms=200; Rig r(c); r.hold(A,500); r.expect(Outcome::Ambiguous,0);
    });
    test("B never followed by A times out", [] {
        Config c; c.crossing_timeout_ms=200; Rig r(c); r.hold(B,500); r.expect(Outcome::Ambiguous,0);
    });
    test("two followers with resolved clear gap", [] {
        Rig r; r.path({A,A|B,B,0}); r.path({A,A|B,B,0});
        check(r.events.size()==2 && r.counter.count()==2,"separated followers");
    });
    test("close followers without clear gap", [] {
        Rig r; r.path({A,A|B,B,A|B,B,0}); r.expect(Outcome::Ambiguous,0);
    });
    test("follower during clear confirmation", [] {
        Rig r; r.hold(A); r.hold(A|B); r.hold(B); r.hold(0,50);
        r.path({A,A|B,B,0}); r.expect(Outcome::Ambiguous,0);
    });
    for (const auto* name : {"unresolved tailgating", "opposing traffic alias", "multiple people alias"})
        test(name, [] {
            Rig r; r.path({A,A|B,B,0},100,Evidence::Unverified); r.expect(Outcome::Ambiguous,0);
            check(r.events[0].detection.reason==Reason::UnverifiedTraffic,"evidence required");
        });
    test("opposite direction simultaneous crossing", [] {
        Rig r; r.path({A|B,A,0}); r.expect(Outcome::Ambiguous,0);
    });
    test("overlap reported by tracker", [] {
        Rig r; r.hold(A); r.send(A|B,Evidence::Conflict); r.path({B,0}); r.expect(Outcome::Ambiguous,0);
    });
    test("uncertainty cannot be restored mid passage", [] {
        Rig r; r.hold(A,5,Evidence::Unverified); r.path({A,A|B,B,0}); r.expect(Outcome::Ambiguous,0);
    });
    test("missed transition", [] { Rig r; r.path({A,B,0}); r.expect(Outcome::Ambiguous,0); });
    test("missing sample watchdog", [] {
        Rig r; r.hold(A); r.now+=300; r.save(r.counter.tick(r.now));
        r.expect(Outcome::Ambiguous,0); check(r.events[0].detection.reason==Reason::MissingFrames,"gap reason");
        r.now+=300; r.save(r.counter.tick(r.now)); check(r.events.size()==1,"watchdog repeated");
    });
    test("frame arriving after sampling gap discarded", [] {
        Rig r; r.hold(A); r.now+=300; r.send(A|B); r.path({B,0}); r.expect(Outcome::Ambiguous,0);
    });
    test("unknown reading is not clear", [] {
        Rig r; r.hold(A); r.send(0,Evidence::IsolatedPerson,Quality::Unknown);
        r.path({A|B,B,0}); r.expect(Outcome::Ambiguous,0);
    });
    test("out of order timestamp", [] {
        Rig r; r.hold(A); r.save(r.counter.process({1,A|B,Quality::Valid,Evidence::IsolatedPerson}));
        r.path({A|B,B,0}); r.expect(Outcome::Ambiguous,0);
    });
    test("conflicting same timestamp", [] {
        Rig r; r.send(A); r.save(r.counter.process({r.now-5,B,Quality::Valid,Evidence::IsolatedPerson}));
        r.expect(Outcome::Ambiguous,0);
    });
    test("invalid mask", [] { Rig r; r.send(C); r.expect(Outcome::Ambiguous,0); });
    test("invalid enum", [] {
        Rig r; r.send(A,static_cast<Evidence>(99)); r.expect(Outcome::Ambiguous,0);
    });
    test("startup occupied never counted", [] {
        PeopleCounter c; for (Milliseconds t=0;t<100;t+=5)
            check(c.process({t,A,Quality::Valid,Evidence::IsolatedPerson}).detection.outcome==Outcome::None,"startup");
        check(c.count()==0,"startup count");
    });
    test("reset requires clear and retains count", [] {
        Rig r({},3); r.hold(A); r.counter.resetDetection(); r.path({A|B,B,0});
        check(r.events.empty(),"reset tail counted"); r.path({A,A|B,B,0}); r.expect(Outcome::Enter,4);
    });
    test("fault recovery", [] {
        Rig r; r.path({A|B,0}); r.events.clear(); r.path({A,A|B,B,0}); r.expect(Outcome::Enter,1);
    });
    test("no early commit", [] {
        Rig r; r.hold(A); r.hold(A|B); r.hold(B); r.hold(0,50); check(r.counter.count()==0,"early commit");
        r.hold(0,100); r.expect(Outcome::Enter,1);
    });
    test("tick cannot confirm clear", [] {
        Rig r; r.hold(A); r.hold(A|B); r.hold(B); r.hold(0,50);
        r.save(r.counter.tick(r.now+100)); check(r.counter.count()==0,"synthetic clear");
    });
    test("three zones moving window entry", [] {
        Config c; c.zones=3; Rig r(c); r.path({A,A|B,B,B|C,C,0}); r.expect(Outcome::Enter,1);
    });
    test("three zones full span exit", [] {
        Config c; c.zones=3; Rig r(c,1); r.path({C,B|C,A|B|C,A|B,A,0}); r.expect(Outcome::Exit,0);
    });
    test("three zone middle start", [] {
        Config c; c.zones=3; Rig r(c); r.path({B,B|C,C,0}); r.expect(Outcome::Ambiguous,0);
    });
    test("three zone skipped middle", [] {
        Config c; c.zones=3; Rig r(c); r.path({A,A|C,C,0}); r.expect(Outcome::Ambiguous,0);
    });
    test("count overflow guarded", [] {
        PeopleCount c(std::numeric_limits<std::uint32_t>::max());
        check(c.apply(Outcome::Enter)==CountUpdate::AtMaximum,"overflow");
        check(c.value()==std::numeric_limits<std::uint32_t>::max(),"wrapped count");
    });
    test("invalid config", [] {
        for (unsigned n : {0u,1u,17u}) {
            Config c; c.zones=n; bool threw=false;
            try { PeopleCounter p(c); } catch (const std::invalid_argument&) { threw=true; }
            check(threw,"bad config accepted");
        }
    });
    test("sixteen zone boundary in both directions", [] {
        Config c; c.zones=16; Rig r(c);
        for (unsigned i=0;i<16;++i) {
            if (i!=0) r.hold(ZoneMask((1u<<(i-1)) | (1u<<i)));
            r.hold(ZoneMask(1u<<i));
        }
        r.hold(0,150); r.expect(Outcome::Enter,1); r.events.clear();
        for (int i=15;i>=0;--i) {
            if (i!=15) r.hold(ZoneMask((1u<<(i+1)) | (1u<<i)));
            r.hold(ZoneMask(1u<<i));
        }
        r.hold(0,150); r.expect(Outcome::Exit,0);
    });
    test("brief conflict bypasses debounce", [] {
        Rig r; r.hold(A); r.send(A,Evidence::Conflict); r.path({A|B,B,0});
        r.expect(Outcome::Ambiguous,0);
    });
    test("timeout includes final clear wait", [] {
        Config c; c.crossing_timeout_ms=350; Rig r(c); r.path({A,A|B,B,0});
        r.expect(Outcome::Ambiguous,0);
    });
    test("64 bit timestamps", [] {
        PeopleCounter c; const Milliseconds base=Milliseconds(1)<<40;
        std::vector<Outcome> results;
        for (Milliseconds dt=0;dt<=800;dt+=5) {
            const ZoneMask mask=dt<200 ? 0 : dt<300 ? A : dt<400 ? A|B : dt<500 ? B : 0;
            const auto r=c.process({base+dt,mask,Quality::Valid,Evidence::IsolatedPerson});
            if(r.detection.outcome!=Outcome::None) results.push_back(r.detection.outcome);
        }
        check(results==std::vector<Outcome>{Outcome::Enter} && c.count()==1,"timestamp truncated");
    });
    test("simulated input end of stream", [] {
        SimulatedInput s({{0,0,Quality::Valid,Evidence::Unverified}}); SensorFrame f;
        check(s.next(f) && !s.next(f),"simulation cursor");
    });
    // Exhaustively enumerate all short two-zone traces. Only the two complete
    // ordered paths can count; all unverified paths must preserve occupancy.
    test("exhaustive short traces and evidence gate", [] {
        Config c; c.debounce_ms=0;
        for (unsigned code=0;code<4096;++code) {
            Rig trusted(c), unverified(c); unsigned x=code;
            std::vector<ZoneMask> compressed{0};
            for (unsigned step=0;step<6;++step) {
                const auto mask=ZoneMask(x%4); x/=4;
                if(mask!=compressed.back()) compressed.push_back(mask);
                trusted.hold(mask,25); unverified.hold(mask,25,Evidence::Unverified);
            }
            trusted.hold(0,150); unverified.hold(0,150,Evidence::Unverified);
            if(compressed.back()!=0) compressed.push_back(0);
            const bool enter=compressed==std::vector<ZoneMask>{0,A,A|B,B,0};
            const bool leave=compressed==std::vector<ZoneMask>{0,B,A|B,A,0};
            unsigned directions=0;
            for(const auto& e: trusted.events) {
                if(e.detection.outcome==Outcome::Enter) { ++directions; check(enter,"false enter"); }
                if(e.detection.outcome==Outcome::Exit) { ++directions; check(leave,"false exit"); }
            }
            check(directions==unsigned(enter || leave),"resolved trace lost");
            check(unverified.counter.count()==0,"unverified trace counted");
            check(trusted.counter.count()<=1,"short trace double counted");
            for (const auto& e : unverified.events)
                check(e.detection.outcome==Outcome::Ambiguous,"unverified directional outcome");
        }
    });
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
