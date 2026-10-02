#include "rules/clock.h"

#include <algorithm>

namespace cfbsim {

ClockAfter clock_after(const ClockEvent& e) {
    ClockAfter c;
    // What the play itself does to the clock.
    if (e.incomplete || e.change_of_possession || e.score) {
        c.start = ClockStart::OnSnap;
    } else if (e.out_of_bounds) {
        c.start = e.late ? ClockStart::OnSnap : ClockStart::OnReady;
    } else if (e.first_down && e.late) {
        c.start = ClockStart::OnReady;
    }
    c.admin = e.change_of_possession || e.score;
    // Administrative stoppages: the clock starts on the ready signal if it
    // would otherwise have been running.
    if (e.penalty || e.injury) {
        if (c.start == ClockStart::Running) c.start = ClockStart::OnReady;
        c.admin = true;
    }
    return c;
}

double runoff_before_snap(const ClockAfter& c, double play_clock_used, const GameTuning& t) {
    switch (c.start) {
        case ClockStart::Running:
            return play_clock_used;
        case ClockStart::OnReady:
            // On a 25-second clock the ready signal starts the play clock too.
            return std::max(0.0, play_clock_used - (c.admin ? 0.0 : t.ready_delay));
        case ClockStart::OnSnap:
            break;
    }
    return 0.0;
}

}  // namespace cfbsim
