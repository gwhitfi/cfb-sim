// Stand-in blocking: with no defenders yet, blockers just take their first
// steps and hold (real blocking is phase 6).

#include "agents/behaviors.h"
#include "agents/steer.h"

namespace cfbsim {

std::string_view StandInBlocker::name() const {
    switch (kind_) {
        case Kind::PassSet:
            return "pass_set";
        case Kind::RunBlock:
            return "run_block";
        case Kind::Stalk:
            return "stalk";
        case Kind::CheckRelease:
            return "check_release";
    }
    return "block";
}

Intent StandInBlocker::decide(const PlayState& play, int self, Actions& /*actions*/) {
    Intent in;
    in.face = play.downfield();
    if (!play.started(self)) return in;
    const Agent& me = play.agent(self);
    // Short, controlled steps: a lineman never sprints to his set point.
    const double speed = me.move.top_speed * (kind_ == Kind::Stalk ? 0.7 : 0.5);
    in.velocity = steer::arrive(me, spot_, speed, 4.0);
    return in;
}

Key StandInBlocker::key() const {
    switch (kind_) {
        case Kind::PassSet:
        case Kind::CheckRelease:
            return Key::PassSet;
        case Kind::RunBlock:
            return Key::RunBlock;
        case Kind::Stalk:
            break;
    }
    return Key::None;
}

bool StandInBlocker::done(const PlayState& play, int /*self*/) const {
    return kind_ == Kind::CheckRelease && play.since_snap() >= until_;
}

}  // namespace cfbsim
