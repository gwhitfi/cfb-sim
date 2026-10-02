#include "play/lanes.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "play/shapes.h"

namespace cfbsim {

namespace {

constexpr double kDeg = std::numbers::pi / 180.0;

// Unit vector for a direction, or `fallback` if it's (nearly) zero.
Vec2 unit_or(Vec2 v, Vec2 fallback) { return v.length() > 1e-6 ? v.normalized() : fallback; }

}  // namespace

double quick_reach_time(Vec2 pos, Vec2 vel, double top_speed, double decel, double reach,
                        Vec2 point) {
    const Vec2 to = point - pos;
    const double dist = to.length();
    if (dist <= reach) return 0.0;
    double turn = 0.0;
    const double speed = vel.length();
    if (speed > 0.1) {
        const double c = vel.dot(to) / (speed * dist);  // cos of the angle off his path
        turn = (1.0 - c) * speed / decel;
    }
    return turn + (dist - reach) / top_speed;
}

LaneSkill lane_skill(const Attributes& ratings, const CarrierTuning& t) {
    const double vision = t.vision_skill(ratings);
    LaneSkill s;
    s.cone = t.cone_deg(vision) * kDeg;
    s.range = t.vision_range(vision);
    s.lag = t.vision_lag(vision);
    s.noise_sd = t.decision_sd(ratings[Attr::Decisions]);
    return s;
}

Vec2 LaneRunner::look_dir(const PlayState& play, int self) const {
    if (heading_) return *heading_;
    return unit_or(play.agent(self).vel, play.downfield_for(self));
}

bool LaneRunner::notices(const PlayState& play, int self, int d) const {
    const CarrierTuning& t = play.tuning().carrier;
    const Vec2 to = play.seen(d, skill_.lag).pos - play.agent(self).pos;
    const double dist = to.length();
    if (dist <= t.feel_range) return true;
    if (dist > skill_.range) return false;
    return std::abs(signed_angle(look_dir(play, self), to)) <= skill_.cone;
}

std::vector<LaneRunner::Threat> LaneRunner::threats(const PlayState& play, int self) const {
    const CarrierTuning& t = play.tuning().carrier;
    const Vec2 me = play.agent(self).pos;
    std::vector<Threat> out;
    for (int d = 0; d < play.count(); ++d) {
        if (play.teammates(d, self) || !notices(play, self, d)) continue;
        const Agent& a = play.agent(d);
        const Seen s = play.seen(d, skill_.lag);
        Threat th{s.pos, s.vel, a.move.top_speed, a.move.decel, a.body.reach, t.react};
        th.delay += play.down_for(d);
        if (const Engagement* e = play.engagement_of(d)) {
            th.delay += t.engaged_delay * (e->defender == d ? 1.0 - e->shed : 1.0);
        } else {
            // A teammate standing between him and me screens him.
            const Vec2 seg = me - s.pos;
            const double len2 = seg.length_squared();
            for (int k = 0; k < play.count() && len2 > 1e-6; ++k) {
                if (k == self || !play.teammates(k, self) || play.knocked_down(k) || play.engaged(k)) {
                    continue;
                }
                const Vec2 pk = play.agent(k).pos;
                const double u = (pk - s.pos).dot(seg) / len2;
                if (u < 0.15 || u > 0.9) continue;
                if ((s.pos + seg * u - pk).length() <= t.screen_width) {
                    th.delay += t.screen_delay;
                    break;
                }
            }
        }
        out.push_back(th);
    }
    return out;
}

void LaneRunner::ray(const PlayState& play, int self, const std::vector<Threat>& threats, Vec2 dir,
                     double& free_t, double& dist) const {
    const CarrierTuning& t = play.tuning().carrier;
    const Agent& me = play.agent(self);
    const PlayFrame& f = play.frame;

    // How far he can go before the ray ends: the sideline, or the goal line
    // (past it nothing can catch him).
    const Vec2 pp = f.to_play(me.pos);
    const Vec2 dp = f.dir_to_play(dir);
    double max_dist = 1e9;
    bool goal = false;
    if (dp.x > 1e-6) max_dist = (f.sideline_x(Side::Right) - t.sideline_margin - pp.x) / dp.x;
    if (dp.x < -1e-6) max_dist = (f.sideline_x(Side::Left) + t.sideline_margin - pp.x) / dp.x;
    max_dist = std::max(0.0, max_dist);
    const double goal_y = play.is_offense(self) ? f.goal_line_y() : f.own_goal_line_y();
    if (std::abs(dp.y) > 1e-6) {
        const double to_goal = (goal_y - pp.y) / dp.y;
        if (to_goal >= 0.0 && to_goal < max_dist) {
            max_dist = to_goal;
            goal = true;
        }
    }

    // Distance run in t seconds, accelerating from his speed along the ray:
    // dv/dt = k (top - v).
    const double top = me.move.top_speed;
    const double k = me.move.accel_rate;
    const double v0 = std::clamp(me.vel.dot(dir), 0.0, top);
    const auto run = [&](double s) { return top * s - (top - v0) * (1.0 - std::exp(-k * s)) / k; };

    double prev_t = 0.0, prev_margin = 1e9;
    for (double s = 0.0; s <= t.horizon + 1e-9; s += t.sample) {
        const double d = std::min(run(s), max_dist);
        const Vec2 at = me.pos + dir * d;
        double margin = 1e9;
        for (const Threat& th : threats) {
            const double tt = th.delay + quick_reach_time(th.pos, th.vel, th.top, th.decel, th.reach, at);
            margin = std::min(margin, tt - s);
        }
        if (margin <= 0.0) {
            // Caught here: interpolate between this sample and the last.
            free_t = s <= 0.0 ? 0.0 : prev_t + (s - prev_t) * prev_margin / (prev_margin - margin);
            dist = std::min(run(free_t), max_dist);
            return;
        }
        if (d >= max_dist) {
            // The sideline ends the ray; the goal line means he's in.
            free_t = goal ? t.horizon : s;
            dist = d;
            return;
        }
        prev_t = s;
        prev_margin = margin;
    }
    free_t = t.horizon;
    dist = std::min(run(t.horizon), max_dist);
}

double LaneRunner::score(const PlayState& play, int self, const std::vector<Threat>& threats,
                         Vec2 dir, double* free_dist) const {
    const CarrierTuning& t = play.tuning().carrier;
    const Agent& me = play.agent(self);
    double free_t = 0.0, dist = 0.0;
    ray(play, self, threats, dir, free_t, dist);
    if (free_dist) *free_dist = dist;
    const double fwd = dir.dot(play.downfield_for(self));
    double s = fwd * dist + t.fall_weight * fwd;
    if (free_t >= t.horizon - 1e-9) s += t.open_bonus * fwd;
    // Speed lost cutting onto it.
    const double speed = me.vel.length();
    if (speed > 1.0) {
        const double angle = std::abs(signed_angle(me.vel, dir));
        const double kept = cut_speed_kept(angle, me.move.agility, play.world().tuning());
        s -= (1.0 - kept) * speed * t.cut_cost_seconds;
    }
    return s;
}

double LaneRunner::score(const PlayState& play, int self, Vec2 dir) const {
    return score(play, self, threats(play, self), dir, nullptr);
}

double LaneRunner::probe(const PlayState& play, int self) const {
    const CarrierTuning& t = play.tuning().carrier;
    const std::vector<Threat> th = threats(play, self);
    const Vec2 down = play.downfield_for(self);
    double best = -1e9;
    for (int k = 0; k < t.headings; ++k) {
        const double a = (-t.span_deg + 2.0 * t.span_deg * k / (t.headings - 1)) * kDeg;
        if (std::abs(a) > skill_.cone) continue;
        best = std::max(best, score(play, self, th, down.rotated(a), nullptr));
    }
    return best;
}

Intent LaneRunner::run(const PlayState& play, int self, Rng& rng, std::optional<Vec2> bias,
                       double bias_cone) {
    const CarrierTuning& t = play.tuning().carrier;
    const Agent& me = play.agent(self);
    const double now = play.since_snap();
    Intent in;

    if (now < forced_until_ && heading_) {
        in.velocity = *heading_ * me.move.top_speed;
        return in;
    }
    if (!heading_ || now >= next_decide_ - 1e-9) {
        next_decide_ = now + t.decide_seconds;
        const std::vector<Threat> th = threats(play, self);
        const Vec2 ref = look_dir(play, self);
        const Vec2 down = play.downfield_for(self);

        std::vector<Vec2> candidates;
        for (int k = 0; k < t.headings; ++k) {
            const double a = (-t.span_deg + 2.0 * t.span_deg * k / (t.headings - 1)) * kDeg;
            const Vec2 c = down.rotated(a);
            if (std::abs(signed_angle(ref, c)) > skill_.cone) continue;  // can't see that way
            if (bias && std::abs(signed_angle(*bias, c)) > bias_cone) continue;
            candidates.push_back(c);
        }
        if (bias) candidates.push_back(*bias);
        if (candidates.empty()) candidates.push_back(ref);

        best_ = -1e9;
        Vec2 pick = candidates.front();
        double pick_score = -1e9;
        for (const Vec2 c : candidates) {
            const double raw = score(play, self, th, c, nullptr);
            best_ = std::max(best_, raw);
            const double noisy = raw + rng.normal(0.0, skill_.noise_sd);
            if (noisy > pick_score) {
                pick = c;
                pick_score = noisy;
            }
        }
        // He keeps his heading unless the new one is clearly better.
        bool keep = false;
        if (heading_ && (!bias || std::abs(signed_angle(*bias, *heading_)) <= bias_cone)) {
            const double current = score(play, self, th, *heading_, nullptr) + rng.normal(0.0, skill_.noise_sd);
            keep = pick_score < current + t.switch_margin;
        }
        if (!keep) {
            const Vec2 from = heading_.value_or(unit_or(me.vel, pick));
            in.plant = std::abs(signed_angle(from, pick)) > t.plant_deg * kDeg && me.vel.length() > 1.0;
            heading_ = pick;
        }
        score(play, self, th, *heading_, &free_dist_);

        contact_ = 1e9;
        for (const Threat& x : th) {
            contact_ = std::min(contact_, x.delay + quick_reach_time(x.pos, x.vel, x.top, x.decel,
                                                                     x.reach, me.pos));
        }
    }
    in.velocity = *heading_ * me.move.top_speed;
    return in;
}

void LaneRunner::draw(const PlayState& play, int self, Json& shapes) const {
    if (!heading_) return;
    const Vec2 me = play.agent(self).pos;
    shapes.push_back(shapes::line("lanes", {me, me + *heading_ * std::max(free_dist_, 0.5)}, "#66bb6a"));
    for (const double side : {-1.0, 1.0}) {
        const Vec2 edge = heading_->rotated(side * skill_.cone);
        shapes.push_back(shapes::line("lanes", {me, me + edge * 4.0}, "#78909c", true));
    }
}

}  // namespace cfbsim
