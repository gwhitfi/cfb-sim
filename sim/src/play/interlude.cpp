// The between-plays animation (see cfbsim/play/interlude.h): kinematic walks
// between a few stops, eased in and out. No ratings, no randomness: the
// stops and the times come from the game, and where the next play lines
// everyone up comes from the play's own setup (play_start, kick_start).

#include "cfbsim/play/interlude.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace cfbsim {

namespace {

constexpr std::int64_t kFrameTicks = 4;

// One walk: from `a` at t0 to `b` at t1 (s), then standing, facing `face`.
struct Leg {
    double t0 = 0.0, t1 = 0.0;
    Vec2 a, b;
    Vec2 face{1.0, 0.0};
};

std::size_t side_index(TeamSide s) { return s == TeamSide::Home ? 0 : 1; }
Vec2 toward_field(TeamSide side) { return {0.0, side == TeamSide::Home ? 1.0 : -1.0}; }
double secs(std::int64_t tick) { return static_cast<double>(tick) * kTickSeconds; }

}  // namespace

struct Between::Path {
    Vec2 start;
    Vec2 start_face{1.0, 0.0};
    std::vector<Leg> legs;

    // Where he is at `t` (s), how fast he's going, and which way he faces.
    void at(double t, Vec2& pos, Vec2& vel, Vec2& facing) const {
        pos = start;
        vel = {};
        facing = start_face;
        for (const Leg& l : legs) {
            if (t <= l.t0) return;
            if (t >= l.t1) {
                pos = l.b;
                facing = l.face;
                continue;
            }
            // Smoothstep: 0 to 1 with no speed at either end.
            const double u = (t - l.t0) / (l.t1 - l.t0);
            const Vec2 d = l.b - l.a;
            pos = l.a + d * (u * u * (3.0 - 2.0 * u));
            vel = d * (6.0 * u * (1.0 - u) / (l.t1 - l.t0));
            if (d.length() > 0.2) facing = d.normalized();
            return;
        }
    }

    // Drops the plan after `t`: a walk under way stops where he is then.
    void cut(double t) {
        Vec2 pos, vel, facing;
        at(t, pos, vel, facing);
        std::erase_if(legs, [&](const Leg& l) { return l.t0 >= t; });
        if (!legs.empty() && legs.back().t1 > t) {
            legs.back().b = pos;
            legs.back().t1 = t;
        }
    }

    // From wherever he is at `at`, walks to `to` at `speed`, there by `latest` when he can be.
    void walk(double when, Vec2 to, Vec2 face, double speed, std::optional<double> latest) {
        cut(when);
        Vec2 from, vel, facing;
        at(when, from, vel, facing);
        double take = std::max(0.6, (to - from).length() / speed);
        if (latest && when + take > *latest) take = std::max(0.4, *latest - when);
        legs.push_back({when, when + take, from, to, face});
    }

    double done() const { return legs.empty() ? 0.0 : legs.back().t1; }
};

struct Between::Man {
    std::string id;
    TeamSide team = TeamSide::Home;
    std::size_t agent = 0;  // index in the world (and in each frame's rows)
    Path path;
    bool on = true;         // in his side's lineup (off: walking to the sideline)
};

Between::Between(std::unique_ptr<std::ostream> out, TrackingInfo info, const PlayFrame& frame, TeamSide offense,
                 bool huddle, const MovementTuning& movement, const PlayTuning& t)
    : stream_(std::move(out)),
      out_(stream_ ? std::make_unique<TrackingWriter>(*stream_) : nullptr),
      info_(std::move(info)),
      frame_(frame),
      offense_(offense),
      huddle_(huddle),
      movement_(movement),
      t_(t),
      world_(movement, 0) {
    info_.frame_ticks = static_cast<int>(kFrameTicks);
}

Between::~Between() = default;

Between::Man* Between::find(const std::string& id) {
    for (auto& m : men_) {
        if (m->id == id) return m.get();
    }
    return nullptr;
}

Between::Man& Between::add(const BetweenMan& m, Vec2 pos, Vec2 facing, bool announce) {
    auto man = std::make_unique<Man>();
    man->id = m.player->id;
    man->team = m.team;
    man->agent = static_cast<std::size_t>(world_.add_agent(make_agent(*m.player, m.team, m.label, {}, movement_)));
    man->path.start = pos;
    man->path.start_face = facing;
    Agent& a = world_.agents()[man->agent];
    a.pos = pos;
    a.facing = facing;
    if (announce && out_) out_->player(a);
    men_.push_back(std::move(man));
    return *men_.back();
}

Vec2 Between::ring(TeamSide side, std::size_t k, std::size_t n, Vec2* face) const {
    // The offense huddles behind the ball (closer with no huddle: it
    // gathers at the line); the defense gathers across from it.
    const bool off = side == offense_;
    const double depth = off ? -(huddle_ ? t_.huddle_depth : 3.0) : t_.huddle_depth + 1.0;
    const Vec2 spot = frame_.to_field(huddle_spot(k, std::max<std::size_t>(n, 1), depth, t_));
    if (face) *face = (frame_.to_field({0.0, depth}) - spot).normalized();
    return spot;
}

Vec2 Between::bench(TeamSide side) {
    // Off the field on a team's sideline (home: below y = 0), near the ball.
    const int k = benched_[side_index(side)]++;
    const double y = side == TeamSide::Home ? -3.0 : kFieldWidth + 3.0;
    return {frame_.ball().x + (static_cast<double>(k % 12) - 5.5) * 1.5, y};
}

void Between::start(const std::vector<BetweenMan>& men, std::int64_t ball_tick) {
    ball_tick_ = ball_tick;
    std::size_t n[2] = {0, 0}, k[2] = {0, 0};
    for (const BetweenMan& m : men) ++n[side_index(m.team)];
    std::vector<std::pair<Man*, Vec2>> walks;
    std::vector<Vec2> faces;
    for (const BetweenMan& m : men) {
        const std::size_t s = side_index(m.team);
        Vec2 face;
        const Vec2 r = ring(m.team, k[s]++, n[s], &face);
        Man& man = add(m, m.at ? m.at->pos : r, m.at ? m.at->facing : face, false);
        if (m.at) {
            walks.emplace_back(&man, r);
            faces.push_back(face);
        }
    }
    if (out_) out_->header(info_, world_);
    started_ = true;
    for (std::size_t i = 0; i < walks.size(); ++i) {
        walks[i].first->path.walk(0.0, walks[i].second, faces[i], t_.walk_back, std::nullopt);
    }
}

void Between::lineup(std::int64_t tick, TeamSide side, const std::vector<BetweenMan>& men,
                     const std::vector<Spot>& spots, std::optional<std::int64_t> break_tick,
                     std::optional<std::int64_t> set_tick) {
    const double t = secs(tick);
    std::set<std::string> in;
    for (const BetweenMan& m : men) in.insert(m.player->id);
    for (auto& m : men_) {
        if (m->team != side || !m->on || in.contains(m->id)) continue;
        m->on = false;  // going off
        m->path.walk(t, bench(side), toward_field(side), t_.sideline_jog, std::nullopt);
    }
    std::size_t k = 0;
    for (const BetweenMan& bm : men) {
        Man* m = find(bm.player->id);
        const bool coming_on = !m || !m->on;
        if (!m) m = &add(bm, bench(side), toward_field(side), true);
        m->on = true;
        const auto s = std::find_if(spots.begin(), spots.end(), [&](const Spot& x) { return x.id == m->id; });
        Vec2 face;
        const Vec2 r = ring(side, k++, men.size(), &face);
        const double speed = coming_on ? t_.sideline_jog : t_.walk_back;
        const auto by = [&](std::int64_t earliest) -> std::optional<double> {
            if (!set_tick) return std::nullopt;
            return secs(std::max(*set_tick, earliest));
        };
        if (break_tick && *break_tick > tick) {
            m->path.walk(t, r, face, speed, std::max(t + 0.5, secs(*break_tick) - 0.3));
            if (s != spots.end()) m->path.walk(secs(*break_tick), s->pos, s->facing, t_.jog_speed, by(*break_tick + 20));
        } else if (s != spots.end()) {
            m->path.walk(t, s->pos, s->facing, std::max(speed, t_.jog_speed), by(tick + 40));
        } else {
            m->path.walk(t, r, face, speed, std::nullopt);
        }
    }
}

void Between::respot(std::int64_t tick, const std::vector<Spot>& spots, std::int64_t by) {
    for (const Spot& s : spots) {
        if (Man* m = find(s.id)) {
            m->path.walk(secs(tick), s.pos, s.facing, t_.jog_speed, secs(std::max(by, tick + 20)));
        }
    }
}

void Between::event(std::int64_t tick, const std::string& name) { events_.emplace_back(tick, name); }

void Between::frame(std::int64_t tick, const std::map<std::string, Spot>* exact) {
    if (!out_) {
        written_ = tick;
        return;
    }
    std::stable_sort(events_.begin(), events_.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    while (!events_.empty() && events_.front().first <= tick) {
        world_.set_tick(std::max<std::int64_t>(0, events_.front().first));
        out_->event(world_, events_.front().second);
        events_.erase(events_.begin());
    }
    for (const auto& m : men_) {
        Agent& a = world_.agents()[m->agent];
        m->path.at(secs(tick), a.pos, a.vel, a.facing);
        if (exact) {
            if (const auto it = exact->find(m->id); it != exact->end()) {
                a.pos = it->second.pos;
                a.facing = it->second.facing;
                a.vel = {};
            }
        }
    }
    Ball& b = world_.ball();
    b.state = tick >= ball_tick_ ? Ball::State::Spotted : Ball::State::Dead;
    b.pos = {frame_.ball().x, frame_.ball().y, 0.12};
    world_.set_tick(tick);
    out_->frame(world_);
    written_ = tick;
}

void Between::run_to(std::int64_t tick) {
    if (!started_) return;
    for (std::int64_t f = written_ < 0 ? 0 : (written_ / kFrameTicks + 1) * kFrameTicks; f <= tick; f += kFrameTicks) {
        frame(f);
    }
}

std::int64_t Between::arrived() const {
    double end = 0.0;
    for (const auto& m : men_) {
        if (m->on) end = std::max(end, m->path.done());
    }
    return static_cast<std::int64_t>(std::ceil(end * kTicksPerSecond));
}

void Between::finish(std::int64_t tick, const std::vector<Spot>& exact, const std::string& last) {
    if (!started_) return;
    run_to(tick - 1);
    tick = std::max(tick, written_ + 1);
    std::map<std::string, Spot> spots;
    for (const Spot& s : exact) spots[s.id] = s;
    frame(tick, &spots);
    if (out_) {
        world_.set_tick(tick);
        for (const auto& e : events_) out_->event(world_, e.second);
        events_.clear();
        if (!last.empty()) out_->event(world_, last);
    }
    out_.reset();
    stream_.reset();  // closes the file
    started_ = false;
}

}  // namespace cfbsim
