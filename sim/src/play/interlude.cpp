// Between plays (see cfbsim/play/interlude.h): kinematic walks, stepped every
// 4 ticks. Each walk has a pace (walk, jog, hurry) and nobody ever goes
// faster than his; the stops and the times come from the game, and where the
// next play lines everyone up comes from the play's own setup (play_start,
// kick_start).

#include "cfbsim/play/interlude.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace cfbsim {

namespace {

constexpr std::int64_t kStepTicks = 4;
constexpr double kStep = static_cast<double>(kStepTicks) * kTickSeconds;

std::size_t side_index(TeamSide s) { return s == TeamSide::Home ? 0 : 1; }
Vec2 toward_field(TeamSide side) { return {0.0, side == TeamSide::Home ? 1.0 : -1.0}; }
double secs(std::int64_t tick) { return static_cast<double>(tick) * kTickSeconds; }
Pace faster(Pace a, Pace b) { return a > b ? a : b; }

}  // namespace

// ---------------------------------------------------------------------------
// The pace
// ---------------------------------------------------------------------------

Pace drawn_pace(double u, double work_rate, double clock_left, const GameTuning& t) {
    const GameTuning::PaceBand* band = &t.pace_bands.back();
    for (const GameTuning::PaceBand& b : t.pace_bands) {
        if (clock_left > b.above) {
            band = &b;
            break;
        }
    }
    // A hard worker's draw sits higher, toward the faster paces.
    const double x = std::clamp(u + t.work_rate_tilt * (work_rate - 50.0) / 50.0, 0.0, 1.0 - 1e-12);
    double sum = 0.0;
    for (std::size_t k = 0; k < 3; ++k) {
        sum += band->odds[k];
        if (x < sum) return static_cast<Pace>(k);
    }
    return Pace::Hurry;
}

Pace needed_pace(double distance, double seconds, double top_speed, const GameTuning& t) {
    const double need = distance * t.need_margin / std::max(seconds, 0.2);
    for (const Pace p : {Pace::Walk, Pace::Jog}) {
        if (pace_speed(p, top_speed, t) >= need) return p;
    }
    return Pace::Hurry;
}

double pace_speed(Pace p, double top_speed, const GameTuning& t) {
    switch (p) {
        case Pace::Walk:
            return t.walk_pace;
        case Pace::Jog:
            return t.jog_pace;
        case Pace::Hurry:
            break;
    }
    return std::max(t.jog_pace, t.hurry_share * top_speed);
}

// ---------------------------------------------------------------------------
// The walks
// ---------------------------------------------------------------------------

struct Between::Man {
    std::string id;
    TeamSide team = TeamSide::Home;
    std::size_t agent = 0;  // index in the world (and in each frame's rows)
    double top_speed = 9.0;
    double work_rate = 50.0;

    Vec2 pos, facing{1.0, 0.0};
    double speed = 0.0;
    bool on = true;      // in his side's lineup (off: walking to the sideline)
    bool coming = false; // coming on, not yet inside the sideline

    // Where he's going.
    bool walking = false;
    Vec2 to, face;
    std::optional<double> due;  // s
    Pace pace = Pace::Walk;
    double u = 0.0;             // this walk's draw
    std::size_t band = 0;       // the clock band the pace was drawn in

    // His stops: the huddle, then (once his side is in) his spot.
    bool in_ring = false;  // the huddle is where he's going (or standing)
    std::optional<Spot> spot;
};

Between::Between(std::unique_ptr<std::ostream> out, TrackingInfo info, const PlayFrame& frame, TeamSide offense,
                 bool huddle, const MovementTuning& movement, const PlayTuning& play, const GameTuning& game,
                 Rng* rng)
    : stream_(std::move(out)),
      out_(stream_ ? std::make_unique<TrackingWriter>(*stream_) : nullptr),
      info_(std::move(info)),
      frame_(frame),
      offense_(offense),
      huddle_(huddle),
      movement_(movement),
      t_(play),
      g_(game),
      rng_(rng),
      world_(movement, 0) {
    info_.frame_ticks = static_cast<int>(kStepTicks);
    // Where the ball will be spotted: the camera's aim before it is.
    info_.details["spot"] = {frame_.ball().x, frame_.ball().y};
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
    Agent& a = world_.agents()[man->agent];
    man->top_speed = a.move.top_speed;
    man->work_rate = a.ratings[Attr::WorkRate];
    man->pos = pos;
    man->facing = facing;
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

double Between::clock_left(std::int64_t tick) const {
    if (!clock_start_ || tick < *clock_start_) return play_clock_;
    return play_clock_ - secs(tick - *clock_start_);
}

void Between::walk(Man& m, std::int64_t tick, Vec2 to, Vec2 face, std::optional<double> due) {
    m.to = to;
    m.face = face;
    m.due = due;
    m.walking = true;
    m.u = rng_ ? rng_->uniform() : 0.0;
    const double left = clock_left(tick);
    m.band = 0;
    while (m.band + 1 < g_.pace_bands.size() && left <= g_.pace_bands[m.band].above) ++m.band;
    m.pace = rng_ ? drawn_pace(m.u, m.work_rate, left, g_) : Pace::Walk;
}

void Between::start(const std::vector<BetweenMan>& men, std::int64_t ball_tick, std::optional<std::int64_t> due) {
    ball_tick_ = ball_tick;
    std::size_t n[2] = {0, 0}, k[2] = {0, 0};
    for (const BetweenMan& m : men) ++n[side_index(m.team)];
    for (const BetweenMan& m : men) {
        const std::size_t s = side_index(m.team);
        Vec2 face;
        const Vec2 r = ring(m.team, k[s]++, n[s], &face);
        Man& man = add(m, m.at ? m.at->pos : r, m.at ? m.at->facing : face, false);
        man.in_ring = true;
        const std::optional<double> by = due && m.team == offense_ ? std::optional<double>(secs(*due)) : std::nullopt;
        walk(man, 0, r, face, by);
    }
    if (out_) {
        out_->header(info_, world_);
        frame(0);
    }
    started_ = true;
}

void Between::clock(double play_clock, std::optional<std::int64_t> start) {
    play_clock_ = play_clock;
    clock_start_ = start;
}

void Between::floor(std::int64_t tick, TeamSide side, Pace p) {
    run_to(tick);
    floor_[side_index(side)] = p;
}

void Between::lineup(std::int64_t tick, TeamSide side, const std::vector<BetweenMan>& men,
                     const std::vector<Spot>& spots, bool hold, std::optional<std::int64_t> ring_due) {
    run_to(tick);
    std::set<std::string> in;
    for (const BetweenMan& m : men) in.insert(m.player->id);
    for (auto& m : men_) {
        if (m->team != side || !m->on || in.contains(m->id)) continue;
        m->on = false;  // going off
        m->in_ring = false;
        m->spot.reset();
        walk(*m, tick, bench(side), toward_field(side), std::nullopt);
    }
    spots_in_[side_index(side)] = !spots.empty();
    std::size_t k = 0;
    for (const BetweenMan& bm : men) {
        Man* m = find(bm.player->id);
        if (!m) {
            m = &add(bm, bench(side), toward_field(side), true);
            m->coming = true;
        } else if (!m->on) {
            m->coming = m->pos.y < 0.0 || m->pos.y > kFieldWidth;
        }
        m->on = true;
        const auto s = std::find_if(spots.begin(), spots.end(), [&](const Spot& x) { return x.id == m->id; });
        m->spot = s != spots.end() ? std::optional<Spot>(*s) : std::nullopt;
        Vec2 face;
        const Vec2 r = ring(side, k++, men.size(), &face);
        if (hold || !m->spot) {
            m->in_ring = true;
            walk(*m, tick, r, face, ring_due ? std::optional<double>(secs(*ring_due)) : std::nullopt);
        } else {
            m->in_ring = false;
            walk(*m, tick, m->spot->pos, m->spot->facing, std::nullopt);
        }
    }
}

void Between::release(std::int64_t tick, TeamSide side) {
    run_to(tick);
    for (auto& m : men_) {
        if (m->team != side || !m->on || !m->in_ring || !m->spot) continue;
        m->in_ring = false;
        walk(*m, tick, m->spot->pos, m->spot->facing, std::nullopt);
    }
}

void Between::respot(std::int64_t tick, const std::vector<Spot>& spots) {
    run_to(tick);
    for (const Spot& s : spots) {
        Man* m = find(s.id);
        if (!m || !m->on) continue;
        m->spot = s;
        if (!m->in_ring) walk(*m, tick, s.pos, s.facing, std::nullopt);
    }
}

void Between::event(std::int64_t tick, const std::string& name) { events_.emplace_back(tick, name); }

void Between::step(std::int64_t tick) {
    const double now = secs(tick);
    const double left = clock_left(tick);
    // The line's due time: the play clock at :08 (none until it runs).
    std::optional<double> line_due;
    if (clock_start_) line_due = secs(*clock_start_) + play_clock_ - g_.line_due;
    for (auto& mp : men_) {
        Man& m = *mp;
        if (!m.walking) continue;
        const Vec2 d = m.to - m.pos;
        const double dist = d.length();
        // His pace: drawn again as the clock crosses into a new band; raised
        // to what he needs to get there in time, and to his side's floor. It
        // only goes up during a walk.
        std::size_t band = 0;
        while (band + 1 < g_.pace_bands.size() && left <= g_.pace_bands[band].above) ++band;
        if (band != m.band) {
            m.band = band;
            if (rng_) m.pace = faster(m.pace, drawn_pace(m.u, m.work_rate, left, g_));
        }
        std::optional<double> due = m.due;
        if (!due && m.on && !m.in_ring && m.spot) due = line_due;
        if (due) m.pace = faster(m.pace, needed_pace(dist, *due - now, m.top_speed, g_));
        m.pace = faster(m.pace, floor_[side_index(m.team)]);
        // Substitutes run on and off.
        if (m.coming || !m.on) m.pace = faster(m.pace, Pace::Jog);

        const double v = pace_speed(m.pace, m.top_speed, g_);
        m.speed = std::min({v, m.speed + g_.walk_accel * kStep, std::sqrt(2.0 * t_.arrive_decel * dist) + 0.3});
        const double go = m.speed * kStep;
        if (go >= dist) {
            m.pos = m.to;
            m.facing = m.face;
            m.speed = 0.0;
            m.walking = false;
        } else {
            const Vec2 dir = d * (1.0 / dist);
            m.pos = m.pos + dir * go;
            m.facing = dir;
        }
        if (m.coming && m.pos.y >= 0.0 && m.pos.y <= kFieldWidth) m.coming = false;
    }
}

void Between::frame(std::int64_t tick, const std::map<std::string, Spot>* exact) {
    if (!out_) return;
    std::stable_sort(events_.begin(), events_.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    while (!events_.empty() && events_.front().first <= tick) {
        world_.set_tick(std::max<std::int64_t>(0, events_.front().first));
        out_->event(world_, events_.front().second);
        events_.erase(events_.begin());
    }
    for (const auto& m : men_) {
        Agent& a = world_.agents()[m->agent];
        a.pos = m->pos;
        a.facing = m->facing;
        a.vel = m->walking ? m->facing * m->speed : Vec2{};
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
    for (std::int64_t s = next_step(stepped_); s <= tick; s += kStepTicks) {
        step(s);
        stepped_ = s;
        frame(s);
    }
}

std::int64_t Between::next_step(std::int64_t tick) { return (tick / kStepTicks + 1) * kStepTicks; }

bool Between::gathered(TeamSide side) const {
    bool any = false;
    for (const auto& m : men_) {
        if (m->team != side || !m->on) continue;
        if (!m->in_ring || m->walking) return false;
        any = true;
    }
    return any;
}

bool Between::arrived(TeamSide side) const {
    if (!spots_in_[side_index(side)]) return false;
    for (const auto& m : men_) {
        if (m->team != side || !m->on) continue;
        if (m->in_ring || !m->spot || m->walking) return false;
    }
    return true;
}

bool Between::on_field() const {
    return std::none_of(men_.begin(), men_.end(), [](const auto& m) { return m->on && m->coming; });
}

bool Between::settled() const {
    return std::none_of(men_.begin(), men_.end(), [](const auto& m) { return m->walking; });
}

std::vector<Spot> Between::where() const {
    std::vector<Spot> out;
    for (const auto& m : men_) {
        if (!m->on) continue;
        const Agent& a = world_.agents()[m->agent];
        out.push_back({m->id, a.label, m->team, m->pos, m->facing});
    }
    return out;
}

void Between::finish(std::int64_t tick, const std::vector<Spot>& exact, const std::string& last) {
    if (!started_) return;
    // The steps before the last tick, then the last frame with the play's own spots.
    for (std::int64_t s = next_step(stepped_); s <= tick; s += kStepTicks) {
        step(s);
        stepped_ = s;
        if (s < tick) frame(s);
    }
    if (out_) {
        tick = std::max(tick, written_ + 1);
        std::map<std::string, Spot> spots;
        for (const Spot& s : exact) spots[s.id] = s;
        frame(tick, &spots);
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
