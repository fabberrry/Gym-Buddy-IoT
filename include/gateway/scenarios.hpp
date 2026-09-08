#pragma once
#include "people_counter/simulated_input.hpp"
#include <stdexcept>
#include <string>

namespace gateway {
inline std::vector<doorway::SensorFrame> scenario(const std::string& name) {
    using namespace doorway;
    std::vector<SensorFrame> frames;
    Milliseconds t=0;
    auto hold=[&](ZoneMask mask, Milliseconds duration, Evidence e=Evidence::IsolatedPerson,
                  Quality q=Quality::Valid) {
        for(Milliseconds end=t+duration;t<end;t+=10) frames.push_back({t,mask,q,e});
    };
    auto entry=[&] { hold(1,100); hold(3,100); hold(2,100); hold(0,200); };
    auto leave=[&] { hold(2,100); hold(3,100); hold(1,100); hold(0,200); };
    auto overlap=[&] { hold(3,100); hold(0,200); };
    hold(0,200);
    if(name=="empty") hold(0,1000);
    else if(name=="entry") entry();
    else if(name=="exit") leave();
    else if(name=="entries") { entry(); entry(); entry(); }
    else if(name=="exits") { leave(); leave(); leave(); }
    else if(name=="close-following") { hold(1,100); hold(3,100); hold(2,100); hold(3,100); hold(2,100); hold(0,200); }
    else if(name=="reversal") { hold(1,100); hold(3,100); hold(1,100); hold(0,200); }
    else if(name=="blockage") { hold(1,16000); hold(0,200); }
    else if(name=="overlap") overlap();
    else if(name=="noise") { hold(1,10); hold(0,10); hold(1,10); hold(0,30); entry(); }
    else if(name=="invalid") { hold(4,10); hold(0,200); }
    else if(name=="demo") { entry(); entry(); overlap(); leave(); }
    else throw std::invalid_argument("Unknown scenario: "+name);
    return frames;
}
}
