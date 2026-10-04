// Blockers: pass protection and run blocking
// (movement-and-interaction.md#blocking--pass-rush-phase-6). The fight itself
// (control, shed, moving together) is in play/engagements.cpp; these decide
// whom to block and where to be.

#include <algorithm>
#include <cmath>
#include <limits>

#include "agents/behaviors.h"
#include "agents/steer.h"
#include "play/shapes.h"

namespace cfbsim {

namespace {

Vec2 qb_play_pos(const PlayState& play) {
    return play.qb >= 0 ? play.play_pos(play.qb) : Vec2{0.0, -5.0};
}

// Facing the defender he's fighting or going for.
void face(Intent& in, Vec2 from, Vec2 to) {
    const Vec2 d = to - from;
    if (d.length() > 0.05) in.face = d.normalized();
}

void draw_target(const PlayState& play, int self, int target, Json& shapes) {
    if (target < 0 || play.engaged(self)) return;
    Json link = {{"kind", "link"}, {"layer", "protection"}, {"a", self}, {"b", target},
                 {"color", "#b0bec5"}, {"dash", true}};
    shapes.push_back(std::move(link));
}

}  // namespace

// ---------------------------------------------------------------------------
// PassBlocker
// ---------------------------------------------------------------------------

bool PassBlocker::rushing(const PlayState& play, int d, Vec2& pos, Vec2& vel) const {
    // What he sees, a moment late: is this defender coming?
    const BlockingTuning& t = play.tuning().blocking;
    const Seen seen = play.seen(d, s_.lag);
    pos = play.frame.to_play(seen.pos);
    vel = play.frame.dir_to_play(seen.vel);
    const Vec2 to_qb = qb_play_pos(play) - pos;
    const double closing = to_qb.length() > 0.1 ? vel.dot(to_qb.normalized()) : 0.0;
    return pos.y <= t.rushing_line || (closing > t.rushing_speed && pos.y <= t.rushing_depth);
}

double PassBlocker::claim(const PlayState& play, int d) const {
    // How far (sideways) the rusher will be from his area. A back's claim
    // always loses to a lineman's.
    const BlockingTuning& t = play.tuning().blocking;
    Vec2 p, v;
    if (play.knocked_down(d) || !rushing(play, d, p, v)) return kNoClaim;
    const double proj_x = p.x + v.x * t.project_seconds;
    if (s_.back) {
        const double off = std::abs(proj_x - qb_play_pos(play).x);
        return off > t.back_range ? kNoClaim : kBackClaim + off;
    }
    return std::max(0.0, std::abs(proj_x - s_.area_x) - s_.area_half);
}

int PassBlocker::pick_target(const PlayState& play, int self) const {
    const BlockingTuning& t = play.tuning().blocking;
    const Vec2 me = play.play_pos(self);
    const Vec2 qb = qb_play_pos(play);
    const double my_to_qb = (qb - me).length();

    int best = -1, help = -1;
    double best_score = kNoClaim, help_score = kNoClaim, current = kNoClaim;
    for (int d = play.offense_count; d < play.count(); ++d) {
        const double mine = claim(play, d);
        if (mine >= kNoClaim) continue;
        // Already past him on the way to the QB: not his to block anymore.
        Vec2 p, v;
        rushing(play, d, p, v);
        if (d != target_ && (qb - p).length() < my_to_qb - 0.5) continue;

        const Engagement* e = nullptr;
        for (const Engagement& en : play.engagements) {
            if (en.defender == d) e = &en;
        }
        if (e && e->blocker != self && e->helper != self) {
            // Engaged with someone else: a double team if nothing else comes.
            if (!s_.back && mine <= t.help_range && (e->helper < 0 || e->helper == self) &&
                mine < help_score) {
                help = d;
                help_score = mine;
            }
            continue;
        }
        // Another blocker with a better claim is going for him, and can get
        // to him (he's on the rusher's path, not beaten): leave him.
        bool theirs = false;
        for (int j = 0; j < play.offense_count && !theirs; ++j) {
            if (j == self || play.behavior(j)->block_target() != d) continue;
            const auto* other = dynamic_cast<const PassBlocker*>(play.behavior(j));
            const double their = other ? other->claim(play, d) : 0.0;
            const Vec2 pj = play.play_pos(j);
            const bool on_him = (pj - p).length() <= t.on_him ||
                                ((qb - pj).length() < (qb - p).length() &&
                                 std::abs(pj.x - (p.x + v.x * t.project_seconds)) <= t.on_him);
            theirs = on_him && (their < mine || (their == mine && j < self));
        }
        if (theirs) continue;
        if (!s_.back && mine > t.reach_outside) continue;

        const double score = mine + (s_.back ? 0.0 : t.depth_penalty * std::max(0.0, p.y));
        if (d == target_) current = score;
        if (score < best_score) {
            best = d;
            best_score = score;
        }
    }
    // Stay on his man unless another is clearly more his.
    if (target_ >= 0 && current < kNoClaim && current <= best_score + t.switch_margin) return target_;
    return best >= 0 ? best : help;
}

Intent PassBlocker::decide(const PlayState& play, int self, Actions& /*actions*/) {
    const Agent& me = play.agent(self);
    const BlockingTuning& t = play.tuning().blocking;
    const PlayFrame& f = play.frame;
    Intent in;
    in.face = play.downfield();
    if (!play.started(self)) return in;

    if (const Engagement* e = play.engagement_of(self)) {
        // In the fight: he holds his ground (winning a pass set means the
        // rusher goes nowhere, not that he's driven back).
        const Agent& d = play.agent(e->defender);
        target_ = e->defender;
        if (s_.back) picked_up_ = true;
        face(in, me.pos, d.pos);
        return in;
    }
    // A back who has released is done (see done()); one with a rusher stays in.
    if (s_.back && !picked_up_ && play.since_snap() >= s_.release_at) return in;

    target_ = pick_target(play, self);
    if (target_ < 0) {
        // Nobody to block (or his man was picked up by someone else): a back
        // goes back to checking, and releases at his check time.
        if (s_.back) picked_up_ = false;
        in.velocity = steer::arrive(me, s_.home, me.move.top_speed * 0.5, 4.0);
        return in;
    }
    if (s_.back) picked_up_ = true;

    // Set between the rusher (where he'll be in a moment) and the QB.
    // A lineman gives ground (never steps up past his stance); a back meets
    // the rusher in front of the QB.
    const Seen seen = play.seen(target_, s_.lag);
    const Vec2 rusher = f.to_play(seen.pos + seen.vel * t.set_lead);
    const Vec2 qb = qb_play_pos(play);
    const Vec2 to_qb = qb - rusher;
    Vec2 spot = rusher + (to_qb.length() > 0.1 ? to_qb.normalized() : Vec2{0.0, -1.0}) * t.set_front;
    if (s_.back) {
        spot.y = std::min(spot.y, qb.y + t.back_front);
    } else {
        spot.y = std::clamp(spot.y, s_.start_y - s_.max_depth, s_.start_y - t.set_give);
    }
    in.velocity = steer::arrive(me, f.to_field(spot), me.move.top_speed, me.move.decel);
    face(in, me.pos, seen.pos);
    return in;
}

bool PassBlocker::done(const PlayState& play, int self) const {
    // A back releases into his route: at his check time with nobody to block,
    // or at the block release time even from a block (the engagement ends
    // when he stops blocking).
    if (!s_.back) return false;
    const double now = play.since_snap();
    if (now >= s_.block_release_at) return true;
    return !picked_up_ && !play.engaged(self) && now >= s_.release_at;
}

void PassBlocker::draw(const PlayState& play, int self, Json& shapes) const {
    draw_target(play, self, target_, shapes);
}

// ---------------------------------------------------------------------------
// RunBlocker
// ---------------------------------------------------------------------------

std::string_view RunBlocker::name() const {
    if (climbing_) return "climb";
    switch (s_.action) {
        case Action::Drive:
            return "drive";
        case Action::Reach:
            return "reach";
        case Action::Down:
            return "down";
        case Action::Combo:
            return "combo";
        case Action::PullKick:
            return "pull_kick";
        case Action::PullLead:
            return "pull_lead";
        case Action::Stalk:
            return "stalk";
        case Action::Fire:
            break;
    }
    return "run_block";
}

BlockStyle RunBlocker::block_style() const {
    if (climbing_) return BlockStyle::Space;
    switch (s_.action) {
        case Action::PullKick:
        case Action::PullLead:
        case Action::Stalk:
            return BlockStyle::Space;
        default:
            return BlockStyle::Run;
    }
}

Vec2 RunBlocker::drive_direction(const PlayState& play) const {
    const double s = s_.playside;
    Vec2 d{0.0, 1.0};
    if (!climbing_) {
        switch (s_.action) {
            case Action::Reach:
                d = Vec2{-s * 0.5, 0.85};  // turn him back away from the play
                break;
            case Action::Down:
                d = Vec2{-s, 0.35};
                break;
            case Action::PullKick:
                d = Vec2{s, 0.2};  // kick him out
                break;
            default:
                break;
        }
    }
    return play.frame.dir_to_field(d.normalized());
}

Intent RunBlocker::decide(const PlayState& play, int self, Actions& /*actions*/) {
    const Agent& me = play.agent(self);
    const BlockingTuning& t = play.tuning().blocking;
    const PlayFrame& f = play.frame;
    Intent in;
    in.face = play.downfield();
    if (!play.started(self)) return in;

    if (const Engagement* e = play.engagement_of(self)) {
        // A combo climbs to the linebacker once he comes near, or once the
        // other blocker has the down lineman under control.
        const bool doubled = e->helper >= 0;
        if (s_.action == Action::Combo && !climbing_ && s_.climb_to >= 0 && doubled) {
            const Vec2 lb = play.seen(s_.climb_to, s_.lag).pos;
            const double since = static_cast<double>(play.world().tick() - e->helper_tick) * kTickSeconds;
            // The other blocker's control: the engagement's, since both share it.
            if ((lb - me.pos).length() <= t.climb_range ||
                (since >= t.combo_seconds && e->control >= t.combo_control)) {
                climbing_ = true;
                target_ = s_.climb_to;
            }
        }
        if (!climbing_ || target_ == e->defender) {
            in.velocity = drive_direction(play) * t.push_speed;
            face(in, me.pos, play.agent(e->defender).pos);
            return in;
        }
    }

    if (target_ < 0) {
        if (s_.action == Action::Combo && s_.climb_to >= 0) {
            // Nobody to double: straight to the second level.
            climbing_ = true;
            target_ = s_.climb_to;
        } else {
            // No one to block: fire out and take whoever shows up.
            in.velocity = steer::arrive(me, s_.fire_spot, me.move.top_speed * 0.6, 4.0);
            return in;
        }
    }

    // A pull runs its path behind the line first.
    if (path_next_ < s_.path.size()) {
        if ((s_.path[path_next_] - me.pos).length() < 0.8) ++path_next_;
        if (path_next_ < s_.path.size()) {
            in.velocity = steer::seek(me, s_.path[path_next_], me.move.top_speed);
            in.face.reset();
            return in;
        }
    }

    const double s = s_.playside;
    const Vec2 seen = f.to_play(play.seen(target_, s_.lag).pos);
    Vec2 aim = seen;
    if (!climbing_) {
        switch (s_.action) {
            case Action::Reach:
                aim += Vec2{s * t.reach_offset, -0.2};  // his playside shoulder
                break;
            case Action::Down:
                aim += Vec2{s * 0.5, -0.3};
                break;
            case Action::PullKick:
                aim += Vec2{-s * 0.8, -0.3};  // inside-out
                break;
            case Action::Stalk:
                aim += Vec2{0.0, -t.stalk_cushion};
                break;
            default:
                break;
        }
    }
    if (s_.action == Action::Stalk && !climbing_) {
        in.velocity = steer::arrive(me, f.to_field(aim), me.move.top_speed * 0.8, me.move.decel);
    } else {
        in.velocity = steer::seek(me, f.to_field(aim), me.move.top_speed);
    }
    face(in, me.pos, f.to_field(seen));
    return in;
}

void RunBlocker::draw(const PlayState& play, int self, Json& shapes) const {
    draw_target(play, self, target_, shapes);
    if (path_next_ < s_.path.size()) {
        std::vector<Vec2> pts{play.agent(self).pos};
        for (std::size_t k = path_next_; k < s_.path.size(); ++k) pts.push_back(s_.path[k]);
        shapes.push_back(shapes::line("protection", pts, "#b0bec5", true));
    }
}

}  // namespace cfbsim
