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

BetweenPlays between_plays_schedule(double play_clock, double full_clock, Tempo tempo, bool called,
                                    const GameTuning& t) {
    BetweenPlays b;
    if (called) return b;
    // A 25-second clock starts on the ready signal, after a stoppage the
    // teams spent gathering: the schedule is that much further along.
    const double head_start = std::max(0.0, full_clock - play_clock);
    double call = t.call_huddle;
    switch (tempo) {
        case Tempo::Huddle:
            break;
        case Tempo::Sugar:
            call = t.call_sugar;
            break;
        case Tempo::NoHuddle:
            call = t.call_no_huddle;
            break;
        case Tempo::Hurry:
            call = t.call_hurry;
            break;
    }
    b.call = std::max(0.0, call - head_start);
    b.defense_call = b.call + t.defense_call_after;
    return b;
}

}  // namespace cfbsim
