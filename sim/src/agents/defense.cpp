// Defenders: run/pass recognition, man and zone coverage, playing the ball,
// and the stand-ins for the rush and run fits
// (movement-and-interaction.md#defense--coverage-phase-5).

#include <algorithm>
#include <cmath>
#include <numbers>

#include "agents/behaviors.h"
#include "agents/steer.h"
#include "play/shapes.h"

namespace cfbsim {

namespace {

// Facing the QB, but never more than `max_deg` off the way he's going, so
// he keeps (nearly) full speed while he watches the QB.
Vec2 face_toward(Vec2 moving, Vec2 to_qb, double max_deg) {
    if (moving.length() < 1e-6) return to_qb;
    const Vec2 dir = moving.normalized();
    const double angle = signed_angle(dir, to_qb);
    const double max_rad = max_deg * std::numbers::pi / 180.0;
    return dir.rotated(std::clamp(angle, -max_rad, max_rad));
}

Vec2 toward_qb(const PlayState& play, const Agent& me) {
    if (play.qb < 0) return play.downfield() * -1.0;
    const Vec2 d = play.agent(play.qb).pos - me.pos;
    return d.length() > 0.1 ? d.normalized() : play.downfield() * -1.0;
}

}  // namespace

// ---------------------------------------------------------------------------
// Defender
// ---------------------------------------------------------------------------

Defender::Defender(std::unique_ptr<Behavior> run_job, std::unique_ptr<Behavior> pass_job,
                   double read_delay, double read_rate, double prior)
    : run_job_(std::move(run_job)),
      pass_job_(std::move(pass_job)),
      read_delay_(read_delay),
      read_rate_(read_rate),
      p_pass_(prior) {}

void Defender::update_belief(const PlayState& play, int self) {
    const DefenseTuning& t = play.tuning().defense;
    // Facts he has noticed beat any key.
    if (play.sees_throw(self)) {
        p_pass_ = 1.0;
        return;
    }
    if (play.sees_handoff(self)) {
        p_pass_ = 0.0;
        return;
    }
    if (play.since_snap() < read_delay_) return;

    // The keys: what the line and the QB are showing.
    int pass = 0, run = 0;
    for (int i = 0; i < play.offense_count; ++i) {
        switch (play.behavior(i)->key()) {
            case Key::PassSet:
            case Key::Drop:
                ++pass;
                break;
            case Key::RunBlock:
            case Key::Handoff:
                ++run;
                break;
            case Key::None:
                break;
        }
    }
    if (pass == run) return;
    const double target = pass > run ? t.pass_target : t.run_target;
    p_pass_ += read_rate_ * kTickSeconds * (target - p_pass_);
}

Intent Defender::decide(const PlayState& play, int self, Actions& actions) {
    update_belief(play, self);
    const Agent& me = play.agent(self);
    const Ball& ball = play.world().ball();

    // A teammate intercepted it: the play is going the other way.
    const int carrier = play.carrier();
    if (carrier >= 0 && !play.is_offense(carrier)) return Intent{};

    // The ball is in the air and he has seen it: go get it.
    if (ball.state == Ball::State::InFlight && play.pass && !play.pass->away &&
        play.sees_throw(self)) {
        if (const auto spot = ball_drop_spot(play)) {
            Intent in;
            in.velocity = steer::seek(me, spot->first, me.move.top_speed);
            const Vec2 to_ball = ball.pos.ground() - me.pos;
            if (spot->second < 0.3 && to_ball.length() > 0.1) in.face = to_ball.normalized();
            return in;
        }
    }

    // Someone on offense has the ball (not the QB in the pocket): after a
    // catch, chase him; on a run, once the carrier is through the line or on
    // top of him.
    if (carrier >= 0 && carrier != play.qb) {
        const bool caught = play.pass.has_value();
        const bool through = play.play_pos(carrier).y > 0.0 ||
                             (play.agent(carrier).pos - me.pos).length() < 3.0;
        if (caught || (!playing_pass() && through)) return pursue_.decide(play, self, actions);
    }

    Behavior& job = playing_pass() ? *pass_job_ : *run_job_;
    return job.decide(play, self, actions);
}

void Defender::draw(const PlayState& play, int self, Json& shapes) const {
    (playing_pass() ? pass_job_ : run_job_)->draw(play, self, shapes);
    // His read, until he's sure.
    if (p_pass_ > 0.1 && p_pass_ < 0.9) {
        char text[16];
        std::snprintf(text, sizeof text, "%s %.2f", playing_pass() ? "P" : "R", p_pass_);
        shapes.push_back(shapes::tag("belief", self, text, playing_pass() ? "#7fb2ff" : "#ffb37f"));
    }
}

// ---------------------------------------------------------------------------
// Man coverage
// ---------------------------------------------------------------------------

Intent ManCoverage::decide(const PlayState& play, int self, Actions& /*actions*/) {
    const Agent& me = play.agent(self);
    const DefenseTuning& t = play.tuning().defense;
    Intent in;
    in.face = toward_qb(play, me);
    if (!start_y_) start_y_ = play.play_pos(target_).y;
    if (!play.started(self)) return in;

    // He sees the receiver late, and later still just after a well-run break.
    double lag = lag_;
    const double since_break = play.since_snap() - play.behavior(target_)->last_break();
    if (since_break >= 0.0 && since_break < t.sell_window) {
        lag += t.sell(play.agent(target_).ratings[Attr::RouteRunning]);
    }
    const Seen seen = play.seen(target_, lag);
    const Vec2 rec = play.frame.to_play(seen.pos);

    // His spot: leverage to one side, a cushion downfield that shrinks as
    // the receiver eats it up.
    const double center_x = play.offense ? play.offense->center_x : 0.0;
    const double outside = rec.x >= center_x ? 1.0 : -1.0;
    const double gained = std::max(0.0, rec.y - *start_y_);
    const double cushion = std::max(t.min_cushion, cushion_ - t.cushion_shrink * gained);
    const Vec2 spot = play.frame.to_field(rec + Vec2{leverage_ * outside * t.leverage, cushion});

    Vec2 want = seen.vel + (spot - me.pos) * t.man_gain;
    if (want.length() > me.move.top_speed) want = want.normalized() * me.move.top_speed;
    in.velocity = want;

    // Backpedal facing the QB while the receiver is in front of him and slow
    // enough; otherwise open the hips and run.
    const bool in_front = rec.y < play.play_pos(self).y - 1.0;
    const double backpedal =
        me.move.top_speed * play.tuning().movement.backpedal_factor * t.backpedal_share;
    if (!(in_front && want.length() < backpedal)) in.face.reset();
    return in;
}

void ManCoverage::draw(const PlayState& /*play*/, int self, Json& shapes) const {
    Json link = {{"kind", "link"}, {"layer", "coverage"}, {"a", self}, {"b", target_}};
    shapes.push_back(std::move(link));
}

// ---------------------------------------------------------------------------
// Zone coverage
// ---------------------------------------------------------------------------

Vec2 ZoneCoverage::spot(const PlayState& play, int self) {
    const DefenseTuning& t = play.tuning().defense;
    const PlayFrame& frame = play.frame;
    const int eyes = play.qb_eyes();
    const Vec2 me = play.play_pos(self);

    // The threats: receivers he sees in the zone, or about to enter it.
    struct Threat {
        int player;
        Vec2 pos;
        double score;
    };
    std::vector<Threat> threats;
    const auto consider = [&](int r) {
        const Seen s = play.seen(r, lag_);
        const Vec2 p = frame.to_play(s.pos);
        const Vec2 ahead = p + frame.dir_to_play(s.vel) * t.threat_lookahead;
        if (!zone_.contains(p) && !zone_.contains(ahead)) return;
        // The nearer, and the one the QB is looking at, the bigger the threat.
        const double eye = 1.0 + (r == eyes ? t.eyes_weight * skill_ / 100.0 : 0.0);
        threats.push_back({r, p, eye / (1.0 + (p - me).length() / 5.0)});
    };
    if (play.offense) {
        for (const auto& side : play.offense->receivers) {
            for (const int r : side) consider(r);
        }
        for (const int r : play.offense->backs) consider(r);
    }

    Vec2 spot = zone_.drop;
    if (zone_.deep) {
        // Carry the deepest threat: deeper than him and on his inside.
        const Threat* deepest = nullptr;
        for (const Threat& th : threats) {
            if (!deepest || th.pos.y > deepest->pos.y) deepest = &th;
        }
        if (deepest) {
            const double center_x = play.offense ? play.offense->center_x : 0.0;
            const double inside = deepest->pos.x >= center_x ? -1.0 : 1.0;
            spot = {deepest->pos.x + inside, std::max(zone_.drop.y, deepest->pos.y + t.deep_cushion)};
        }
    } else if (!threats.empty()) {
        // Play one man: the biggest threat, switching only when another is
        // clearly bigger. A defender can't sit on two receivers at once,
        // which is what high-low concepts attack.
        const Threat* best = &threats.front();
        const Threat* current = nullptr;
        for (const Threat& th : threats) {
            if (th.score > best->score) best = &th;
            if (th.player == threat_) current = &th;
        }
        if (current && best->score < current->score * t.switch_margin) best = current;
        threat_ = best->player;
        const Vec2 qb = play.qb >= 0 ? play.play_pos(play.qb) : Vec2{0.0, -5.0};
        spot = best->pos + (qb - best->pos).normalized() * t.under_short;
    } else {
        threat_ = -1;
        if (eyes >= 0) {
            // Nobody here: lean toward where the QB is looking.
            const Vec2 d = play.play_pos(eyes) - zone_.drop;
            spot = zone_.drop + d.normalized() * std::min(t.drift, d.length());
        }
    }
    const double m = t.zone_margin;
    spot.x = std::clamp(spot.x, zone_.drop.x - zone_.width / 2.0 - m,
                        zone_.drop.x + zone_.width / 2.0 + m);
    spot.y = std::clamp(spot.y, zone_.depth_min - m, zone_.depth_max + m);
    return spot;
}

Intent ZoneCoverage::decide(const PlayState& play, int self, Actions& /*actions*/) {
    const Agent& me = play.agent(self);
    Intent in;
    const Vec2 qb = toward_qb(play, me);
    in.face = qb;
    if (!play.started(self)) return in;
    if (play.since_snap() < play.get_off[static_cast<std::size_t>(self)] + late_) return in;

    spot_ = spot(play, self);
    in.velocity = steer::arrive(me, play.frame.to_field(spot_), me.move.top_speed, me.move.decel);
    // Open toward the QB, but not so far he slows to a backpedal.
    if (in.velocity.length() > 1.5) {
        in.face = face_toward(in.velocity, qb, play.tuning().defense.face_max_deg);
    }
    return in;
}

void ZoneCoverage::draw(const PlayState& play, int self, Json& shapes) const {
    const PlayFrame& f = play.frame;
    // The area (deep zones drawn only 15 yd deep, to keep the view readable),
    // his spot, and the man he's playing.
    const double l = zone_.drop.x - zone_.width / 2.0, r = zone_.drop.x + zone_.width / 2.0;
    const double top = std::min(zone_.depth_max, zone_.depth_min + 15.0);
    shapes.push_back(shapes::line("coverage",
                                  {f.to_field({l, zone_.depth_min}), f.to_field({r, zone_.depth_min}),
                                   f.to_field({r, top}), f.to_field({l, top}),
                                   f.to_field({l, zone_.depth_min})},
                                  "#8a7fff", true));
    shapes.push_back(shapes::text("coverage", f.to_field(spot_), "x", "#8a7fff"));
    if (threat_ >= 0) {
        Json link = {{"kind", "link"}, {"layer", "coverage"}, {"a", self}, {"b", threat_},
                     {"color", "#8a7fff"}, {"dash", true}};
        shapes.push_back(std::move(link));
    }
}

// ---------------------------------------------------------------------------
// Pass rush
// ---------------------------------------------------------------------------

Intent Rusher::decide(const PlayState& play, int self, Actions& actions) {
    const Agent& me = play.agent(self);
    const BlockingTuning& t = play.tuning().blocking;
    const PlayFrame& f = play.frame;
    Intent in;
    in.face = play.downfield_for(self);
    if (!play.started(self)) return in;

    // Where the QB will be in a moment.
    Vec2 qb = f.to_field({0.0, -5.0});
    if (play.qb >= 0) qb = play.agent(play.qb).pos + play.agent(play.qb).vel * t.rush_lead;
    const double now = play.since_snap();

    if (const Engagement* e = play.engagement_of(self)) {
        // Engaged: his goal is the QB, and he works his moves.
        in.velocity = (qb - me.pos).normalized() * me.move.top_speed;
        const Vec2 to_blocker = play.agent(e->blocker).pos - me.pos;
        if (to_blocker.length() > 0.05) in.face = to_blocker.normalized();
        if (engaged_tick_ != e->start_tick) {
            engaged_tick_ = e->start_tick;
            next_move_ = now + rng_.uniform(t.move_first_min, t.move_first_max);
        }
        if (e->last_move > answered_) {
            // His last move failed: chain the other kind sooner.
            answered_ = e->last_move;
            counter_ = !e->last_move_won;
            if (counter_) next_move_ = std::min(next_move_, e->last_move + t.counter_seconds);
        }
        if (now >= next_move_) {
            const bool power =
                counter_ ? !e->last_move_power
                        : s_.power + rng_.normal(0.0, t.move_choice_sd) >=
                              s_.finesse + rng_.normal(0.0, t.move_choice_sd);
            actions.rush_moves.push_back({self, power});
            counter_ = false;
            next_move_ = now + std::max(0.3, rng_.normal(t.move_interval, t.move_interval_sd));
        }
        return in;
    }
    engaged_tick_ = -1;

    const Vec2 p = f.to_play(me.pos);
    const Vec2 q = f.to_play(qb);
    Vec2 aim = q;
    if (p.y > 0.0) {
        // Through his lane at the line first.
        aim = {s_.lane_x, -0.5};
    } else if (s_.contain != 0.0 && (p - q).length() > 2.5) {
        // Contain: at a spot just outside the QB, so he can't escape that way.
        aim = q + Vec2{s_.contain * 1.0, 0.0};
    }
    in.velocity = steer::seek(me, f.to_field(aim), me.move.top_speed);
    return in;
}

// ---------------------------------------------------------------------------
// Hold a spot
// ---------------------------------------------------------------------------

Intent HoldSpot::decide(const PlayState& play, int self, Actions& /*actions*/) {
    const Agent& me = play.agent(self);
    Intent in;
    in.face = play.downfield_for(self);
    if (!play.started(self)) return in;
    in.velocity = steer::arrive(me, spot_, me.move.top_speed * speed_share_, me.move.decel);
    return in;
}

}  // namespace cfbsim
