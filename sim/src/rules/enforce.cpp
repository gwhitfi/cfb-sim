#include "rules/enforce.h"

#include <algorithm>

namespace cfbsim {

namespace {

constexpr double kTryValue = 0.95;  // a touchdown is worth about 6.95 with the try

// The offense's play ended at `yl` with no score: a new set of downs, the
// next down, or a turnover on downs.
Next advance(const Situation& s, double yl, const RulesTable& rules) {
    Next n;
    n.yards = yl - s.yard_line;
    if (yl >= s.line_to_gain() - 1e-9) {
        n.next = first_down_at(yl, rules);
        n.first_down = true;
        return n;
    }
    if (s.down >= 4) {
        n.kind = Next::Kind::ChangeOfPossession;
        n.turnover_on_downs = true;
        n.next = first_down_at(100.0 - yl, rules);
        return n;
    }
    n.next = {yl, s.down + 1, s.line_to_gain() - yl};
    return n;
}

// The other team gets it at `their_yl` (its own yard line); in its end zone
// it's a touchback.
Next change(double their_yl, const RulesTable& rules) {
    Next n;
    n.kind = Next::Kind::ChangeOfPossession;
    n.next = first_down_at(their_yl <= 0.0 ? rules.touchback : their_yl, rules);
    return n;
}

}  // namespace

Situation first_down_at(double yard_line, const RulesTable& rules) {
    return {yard_line, 1, std::min(rules.first_down, 100.0 - yard_line)};
}

Next result_of_play(const Situation& s, const PlayResult& r, const RulesTable& rules) {
    const bool turnover = r.interception || r.fumble_lost;
    const double end = s.yard_line + r.spot_y;  // the offense's yard line where the ball is dead
    if (r.outcome == "touchdown") {
        Next n;
        n.kind = turnover ? Next::Kind::DefenseTouchdown : Next::Kind::Touchdown;
        n.yards = turnover ? 0.0 : 100.0 - s.yard_line;
        return n;
    }
    if (turnover) {
        Next n = change(100.0 - end, rules);
        n.yards = r.yards;
        return n;
    }
    if (r.outcome == "safety" || end <= 0.0) {
        Next n;
        n.kind = Next::Kind::Safety;
        n.yards = -s.yard_line;
        return n;
    }
    if (r.outcome == "incomplete") {
        Next n = advance(s, s.yard_line, rules);
        n.keep_spot = true;
        return n;
    }
    if (end >= 100.0) {
        // A fumble out of the opponent's end zone is their touchback; any
        // other way into the end zone is a touchdown.
        if (r.outcome == "fumble_out_of_bounds") return change(rules.touchback, rules);
        Next n;
        n.kind = Next::Kind::Touchdown;
        n.yards = 100.0 - s.yard_line;
        return n;
    }
    return advance(s, end, rules);
}

Next replay_down(const Situation& s) {
    Next n;
    n.next = s;
    n.keep_spot = true;
    n.replay = true;
    return n;
}

Next enforce(const Situation& s, const PlayResult& r, const Foul& f, const PenaltyRule& rule,
             const RulesTable& rules) {
    const double los = s.yard_line;
    const double foul_yl = los + f.y;

    // The base the play leaves behind for fouls enforced at the end of it.
    Situation base = s;
    double from = los;
    bool end_of_play = false;
    switch (rule.spot) {
        case EnforceSpot::Previous:
        case EnforceSpot::DeadBall:
            break;
        case EnforceSpot::SpotOfFoul:
            from = foul_yl;
            break;
        case EnforceSpot::PreviousOrFoulBeyond:
            from = std::max(los, foul_yl);
            if (f.by_offense && foul_yl <= 0.0) from = foul_yl;  // in his own end zone
            break;
        case EnforceSpot::SpotCapped:
            break;
        case EnforceSpot::EndOfPlay: {
            const Next standing = result_of_play(s, r, rules);
            if (standing.kind == Next::Kind::Continue && r.outcome != "incomplete") {
                end_of_play = true;
                base = standing.next;
                from = base.yard_line;
            }
            break;
        }
    }

    Next n;
    if (f.by_offense && rule.safety_in_end_zone && from <= 0.0) {
        n.kind = Next::Kind::Safety;
        n.yards = -los;
        return n;
    }

    double to = from;
    if (rule.spot == EnforceSpot::SpotCapped && !f.by_offense) {
        // DPI: at the spot of the foul, at most `yards` past the line, never
        // inside the 2 (from inside the 2: half the distance).
        to = std::min(los + rule.yards, std::max(foul_yl, los + 1.0));
        to = std::min(to, 98.0);
        if (to <= los) to = los + (100.0 - los) / 2.0;
    } else if (f.by_offense) {
        const double move = rule.yards > from / 2.0 ? from / 2.0 : rule.yards;
        to = from - move;
    } else {
        const double room = 100.0 - from;
        const double move = rule.yards > room / 2.0 ? room / 2.0 : rule.yards;
        to = from + move;
    }
    n.yards = to - los;

    if (f.by_offense) {
        const double ltg = base.line_to_gain();
        if (rule.loss_of_down) {
            if (base.down >= 4) {
                n.kind = Next::Kind::ChangeOfPossession;
                n.turnover_on_downs = true;
                n.next = first_down_at(100.0 - to, rules);
                return n;
            }
            n.next = {to, base.down + 1, ltg - to};
        } else if (end_of_play) {
            n.next = {to, base.down, std::max(0.5, ltg - to)};
        } else {
            n.next = {to, base.down, ltg - to};  // replay the down; the line to gain stays
        }
        return n;
    }
    if (rule.first_down || to >= base.line_to_gain() - 1e-9) {
        n.next = first_down_at(to, rules);
        n.first_down = true;
        return n;
    }
    n.next = {to, base.down, base.line_to_gain() - to};
    return n;
}

double expected_points(const Situation& s) {
    static constexpr double kDownCost[] = {0.0, 0.35, 0.9, 1.7};
    const int d = std::clamp(s.down, 1, 4);
    double ep = -1.0 + 0.068 * s.yard_line - kDownCost[d - 1];
    ep -= 0.04 * std::max(0.0, s.distance - 10.0);
    return ep;
}

double value_for_offense(const Next& n, const RulesTable& rules) {
    const Situation kickoff = first_down_at(rules.kickoff_touchback, rules);
    switch (n.kind) {
        case Next::Kind::Touchdown:
            return 6.0 + kTryValue - expected_points(kickoff);
        case Next::Kind::DefenseTouchdown:
            return -(6.0 + kTryValue) + expected_points(kickoff);
        case Next::Kind::Safety:
            return -2.0 - expected_points(first_down_at(30.0, rules));
        case Next::Kind::ChangeOfPossession:
            return -expected_points(n.next);
        case Next::Kind::Continue:
            break;
    }
    return expected_points(n.next);
}

}  // namespace cfbsim
