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

BetweenPlays between_plays_schedule(double play_clock, double full_clock, Tempo tempo, double walk_up,
                                    double set_seconds, bool called, const GameTuning& t) {
    // A 25-second clock starts on the ready signal, after a stoppage the
    // teams spent huddling: the schedule is that much further along.
    const double head_start = std::max(0.0, full_clock - play_clock);
    const bool huddle = tempo == Tempo::Huddle;
    BetweenPlays b;
    double broke = 0.0;
    if (!called) {
        const double call = tempo == Tempo::Hurry ? t.call_hurry : tempo == Tempo::NoHuddle ? t.call_no_huddle : t.call_huddle;
        b.call = std::max(0.0, call - head_start);
        b.defense_call = b.call + t.defense_call_after;
        broke = std::max(b.call, call + (huddle ? t.huddle_seconds : 0.0) - head_start);
    }
    b.broke = broke;
    b.set = std::max(b.defense_call, broke + (huddle ? walk_up : t.line_up));
    b.earliest = b.set + set_seconds;
    return b;
}

}  // namespace cfbsim
