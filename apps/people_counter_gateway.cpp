#include "gateway/room_state.hpp"
#include "gateway/scenarios.hpp"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <random>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <csignal>
#endif

namespace {
void pauseFrame() {
#ifdef _WIN32
    Sleep(10);
#else
    usleep(10000);
#endif
}
std::uint64_t number(const std::string& s) {
    if(s.empty() || s.find_first_not_of("0123456789")!=std::string::npos) throw std::invalid_argument("Expected unsigned integer");
    return std::stoull(s);
}
}
int main(int argc, char** argv) {
    try {
#ifndef _WIN32
        std::signal(SIGPIPE,SIG_IGN);
#endif
        gateway::RoomState initial;
        initial.deviceId="counter-01"; initial.roomId="room-01";
        std::random_device random;
        initial.sessionId=std::to_string(random())+"-"+std::to_string(random());
        std::string name="demo"; bool live=false;
        std::uint64_t interval=1000;
        auto epoch=std::uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        for(int i=1;i<argc;++i) {
            const std::string arg=argv[i];
            if(arg=="--live") { live=true; continue; }
            if(i+1>=argc) throw std::invalid_argument("Missing option value");
            const std::string value=argv[++i];
            if(arg=="--device-id") initial.deviceId=value;
            else if(arg=="--room-id") initial.roomId=value;
            else if(arg=="--session-id") initial.sessionId=value;
            else if(arg=="--scenario") name=value;
            else if(arg=="--interval-ms") interval=number(value);
            else if(arg=="--epoch-ms") epoch=number(value);
            else if(arg=="--initial-count") {
                const auto n=number(value);
                if(n>std::numeric_limits<std::uint32_t>::max()) throw std::invalid_argument("Count too large");
                initial.count=std::uint32_t(n);
            } else throw std::invalid_argument("Unknown option: "+arg);
        }
        const auto frames=gateway::scenario(name);
        gateway::ConsolePublisher publisher(std::cout);
        initial.timestamp=epoch;
        gateway::RoomStateService service(initial,publisher,interval);
        doorway::PeopleCounter counter({},initial.count);
        service.flush(0,epoch);
        const auto started=std::chrono::steady_clock::now();
        std::uint64_t now=0;
        for(std::size_t i=0;live || i<frames.size();++i) {
            auto f=i<frames.size()?frames[i]:doorway::SensorFrame{now,0,doorway::Quality::Valid,doorway::Evidence::IsolatedPerson};
            // Live time reflects actual scheduling delays, so stalls trigger the core watchdog.
            if(live) f.time_ms=std::uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now()-started).count());
            now=f.time_ms;
            const auto result=counter.process(f);
            service.observe(result,epoch+now);
            service.flush(now,epoch+now);
            if(result.detection.outcome!=doorway::Outcome::None) {
                const char* event=result.detection.outcome==doorway::Outcome::Enter?"ENTRY":
                    result.detection.outcome==doorway::Outcome::Exit?"EXIT":"AMBIGUOUS";
                std::cerr << event << " count=" << counter.count() << '\n';
            }
            if(live) pauseFrame();
        }
        service.flush(now,epoch+now,true);
        return 0;
    } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
