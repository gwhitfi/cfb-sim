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
// Before the call nobody has a role yet: the linemen by position.
bool lineman(const Player& p) { return p.position == Position::OT || p.position == Position::OG || p.position == Position::C; }
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

std::map<std::string, double> break_stagger(const Formation& formation, const GameTuning& t) {
    // Where the formation lines each slot up (play frame: x across, y downfield).
    const std::vector<Vec2> at = align_formation(formation, PlayFrame(25.0, Hash::Middle, true, false));
    // The ends: the widest slots on the line, one each side.
    double left = 0.0, right = 0.0;
    for (std::size_t k = 0; k < at.size(); ++k) {
        if (!formation.slots[k].on_line) continue;
        left = std::min(left, at[k].x);
        right = std::max(right, at[k].x);
    }
    // The line: on it, inside the ends; the backfield: off it, no wider than the line.
    double line_width = 0.0;
    std::vector<bool> line(at.size(), false);
    for (std::size_t k = 0; k < at.size(); ++k) {
        line[k] = formation.slots[k].on_line && at[k].x > left + 1e-9 && at[k].x < right - 1e-9;
        if (line[k]) line_width = std::max(line_width, std::abs(at[k].x));
    }
    std::map<std::string, double> out;
    for (std::size_t k = 0; k < at.size(); ++k) {
        const std::string& slot = formation.slots[k].name;
        if (line[k]) {
            out[slot] = 0.0;
        } else if (!formation.slots[k].on_line && std::abs(at[k].x) <= line_width + 1.0) {
            out[slot] = t.stagger_backs;
        } else {
            out[slot] = t.stagger_receivers;
        }
    }
    return out;
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
    double anticipation = 50.0;

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
    bool in_ring = false;  // the huddle is where he's going (or standing): his stop before his spot
    bool ringed = false;   // ... and it's the offense's ring, not a stop before the call
    std::optional<Spot> spot;
    std::int64_t wait_until = 0;     // he doesn't set off before this tick (the read, the break's stagger)
    std::int64_t signal_until = -1;  // until this tick, standing, he looks to his sideline
};

Between::Between(std::unique_ptr<std::ostream> out, TrackingInfo info, const PlayFrame& frame, TeamSide offense,
                 Tempo tempo, const MovementTuning& movement, const PlayTuning& play, const GameTuning& game,
                 Rng* rng)
    : stream_(std::move(out)),
      out_(stream_ ? std::make_unique<TrackingWriter>(*stream_) : nullptr),
      info_(std::move(info)),
      frame_(frame),
      offense_(offense),
      tempo_(tempo),
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
    man->anticipation = a.ratings[Attr::Anticipation];
    man->pos = pos;
    man->facing = facing;
    a.pos = pos;
    a.facing = facing;
    if (announce && out_) out_->player(a);
    men_.push_back(std::move(man));
    return *men_.back();
}

Vec2 Between::ring(TeamSide side, std::size_t k, std::size_t n, Vec2* face) const {
    // The offense huddles behind the ball (a sugar huddle near it; with no
    // huddle, a player with no spot in its look stands just behind the line);
    // the defense across from it.
    const bool off = side == offense_;
    const double back = tempo_ == Tempo::Huddle ? t_.huddle_depth : tempo_ == Tempo::Sugar ? g_.sugar_depth : 3.0;
    const double depth = off ? -back : t_.huddle_depth + 1.0;
    const Vec2 spot = frame_.to_field(huddle_spot(k, std::max<std::size_t>(n, 1), depth, t_));
    if (face) *face = (frame_.to_field({0.0, depth}) - spot).normalized();
    return spot;
}

Vec2 Between::gather(TeamSide side, bool lineman, std::size_t k, std::size_t n, Vec2* face) const {
    // A fixed pattern (no draw: the walk stream doesn't change): the linemen
    // in a loose row, the rest spread wider and deeper, staggered.
    const double across = static_cast<double>(k) - 0.5 * static_cast<double>(n > 0 ? n - 1 : 0);
    Vec2 at;
    if (lineman) {
        at = {across * 1.6, -(g_.gather_line + (k % 2 == 0 ? 0.0 : 0.6))};
    } else {
        static constexpr double kDeep[] = {0.0, 1.0, 0.65, 0.3};
        const auto& [near, deep] = g_.gather_others;
        at = {across * 2.4, -(near + (deep - near) * kDeep[k % 4])};
    }
    // Facing his sideline for the signal.
    if (face) *face = toward_field(side) * -1.0;
    return frame_.to_field(at);
}

std::vector<std::pair<Vec2, Vec2>> Between::first_stops(TeamSide side, const std::vector<BetweenMan>& men) const {
    const auto& look = looks_[side_index(side)];
    const bool gathering = side == offense_ && huddles();
    std::size_t lines = 0, others = 0;
    for (const BetweenMan& m : men) ++(lineman(*m.player) ? lines : others);
    std::vector<std::pair<Vec2, Vec2>> out;
    std::size_t k = 0, kl = 0, ko = 0;
    for (const BetweenMan& m : men) {
        Vec2 face;
        Vec2 at;
        if (const auto s = look.find(m.label); s != look.end()) {
            at = s->second.pos;
            face = s->second.facing;
        } else if (gathering) {
            at = lineman(*m.player) ? gather(side, true, kl++, lines, &face) : gather(side, false, ko++, others, &face);
        } else {
            at = ring(side, k, men.size(), &face);
        }
        ++k;
        out.emplace_back(at, face);
    }
    return out;
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

void Between::start(const std::vector<BetweenMan>& men, std::int64_t ball_tick, std::optional<std::int64_t> due,
                    const std::vector<Spot>& looks) {
    ball_tick_ = ball_tick;
    for (const Spot& s : looks) looks_[side_index(s.team)][s.label] = s;
    std::vector<BetweenMan> of[2];
    for (const BetweenMan& m : men) of[side_index(m.team)].push_back(m);
    std::vector<std::pair<Vec2, Vec2>> stops[2] = {first_stops(TeamSide::Home, of[0]), first_stops(TeamSide::Away, of[1])};
    std::size_t k[2] = {0, 0};
    for (const BetweenMan& m : men) {
        const std::size_t s = side_index(m.team);
        const auto [r, face] = stops[s][k[s]++];
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
        m->ringed = false;
        m->spot.reset();
        walk(*m, tick, bench(side), toward_field(side), std::nullopt);
    }
    spots_in_[side_index(side)] = !spots.empty();
    // Held: the offense forms its ring once its call is in; before it (and
    // the defense always) goes to its stop before the call.
    const bool ring_now = side == offense_ && !spots.empty() && huddles();
    const std::vector<std::pair<Vec2, Vec2>> stops = first_stops(side, men);
    std::size_t k = 0;
    for (const BetweenMan& bm : men) {
        Man* m = find(bm.player->id);
        if (!m) {
            m = &add(bm, bench(side), toward_field(side), true);
            m->coming = true;
            subbed_[side_index(side)] = true;
        } else if (!m->on) {
            m->coming = m->pos.y < 0.0 || m->pos.y > kFieldWidth;
            subbed_[side_index(side)] = true;
        }
        m->on = true;
        const auto s = std::find_if(spots.begin(), spots.end(), [&](const Spot& x) { return x.id == m->id; });
        m->spot = s != spots.end() ? std::optional<Spot>(*s) : std::nullopt;
        Vec2 face = stops[k].second;
        const Vec2 r = ring_now ? ring(side, k, men.size(), &face) : stops[k].first;
        ++k;
        m->ringed = ring_now && hold;
        if (hold || !m->spot) {
            m->in_ring = true;
            walk(*m, tick, r, face, ring_due ? std::optional<double>(secs(*ring_due)) : std::nullopt);
        } else {
            m->in_ring = false;
            walk(*m, tick, m->spot->pos, m->spot->facing, std::nullopt);
        }
    }
}

void Between::release(std::int64_t tick, TeamSide side, const std::map<std::string, double>& stagger) {
    run_to(tick);
    for (auto& m : men_) {
        if (m->team != side || !m->on || !m->in_ring || !m->spot) continue;
        m->in_ring = false;
        m->ringed = false;
        const auto s = stagger.find(m->spot->label);
        const double after = s != stagger.end() ? s->second : 0.0;
        m->wait_until = tick + static_cast<std::int64_t>(std::lround(after / kTickSeconds));
        walk(*m, tick, m->spot->pos, m->spot->facing, std::nullopt);
    }
}

void Between::wait(std::int64_t tick, TeamSide side, double seconds) {
    run_to(tick);
    const std::int64_t until = tick + static_cast<std::int64_t>(std::lround(seconds / kTickSeconds));
    for (auto& m : men_) {
        if (m->team == side && m->on && !m->in_ring && m->spot) m->wait_until = std::max(m->wait_until, until);
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

void Between::reshell(const std::vector<Spot>& shell) {
    auto& look = looks_[side_index(offense_ == TeamSide::Home ? TeamSide::Away : TeamSide::Home)];
    look.clear();
    for (const Spot& s : shell) look[s.label] = s;
}

void Between::signal(std::int64_t tick, TeamSide side) {
    run_to(tick);
    const std::int64_t until = tick + static_cast<std::int64_t>(std::lround(g_.shell_signal / kTickSeconds));
    for (auto& m : men_) {
        if (m->team == side && m->on) m->signal_until = until;
    }
}

void Between::read(std::int64_t tick, TeamSide side, std::int64_t from) {
    run_to(tick);
    for (auto& m : men_) {
        if (m->team != side || !m->on || !m->in_ring || !m->spot) continue;
        m->in_ring = false;
        const auto delay = static_cast<std::int64_t>(std::lround(g_.shell_read(m->anticipation) / kTickSeconds));
        m->wait_until = std::max(tick, from + delay);
        walk(*m, tick, m->spot->pos, m->spot->facing, std::nullopt);
    }
}

void Between::hurry_off(std::int64_t tick, TeamSide side, std::int64_t due) {
    run_to(tick);
    for (auto& m : men_) {
        if (m->team == side && !m->on && m->walking) m->due = secs(due);
    }
}

void Between::event(std::int64_t tick, const std::string& name) { events_.emplace_back(tick, name); }

Vec2 Between::standing_facing(const Man& m, std::int64_t tick) {
    // Where his stop has him face, or his sideline for the signal.
    return tick < m.signal_until ? toward_field(m.team) * -1.0 : m.face;
}

void Between::step(std::int64_t tick) {
    const double now = secs(tick);
    const double left = clock_left(tick);
    // The line's due time: the play clock at :08 (none until it runs).
    std::optional<double> line_due;
    if (clock_start_) line_due = secs(*clock_start_) + play_clock_ - g_.line_due;
    for (auto& mp : men_) {
        Man& m = *mp;
        if (!m.walking || tick < m.wait_until) {
            m.facing = standing_facing(m, tick);
            continue;
        }
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
            m.speed = 0.0;
            m.walking = false;
            m.facing = standing_facing(m, tick);
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
                a.vel = it->second.vel;
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
        if (!m->in_ring || !m->ringed || m->walking) return false;
        any = true;
    }
    return any;
}

bool Between::assembled(TeamSide side) const {
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

std::vector<std::string> Between::still_leaving(TeamSide side) const {
    std::vector<std::string> out;
    for (const auto& m : men_) {
        if (m->team == side && !m->on && m->pos.y >= 0.0 && m->pos.y <= kFieldWidth) out.push_back(m->id);
    }
    return out;
}

std::vector<std::string> Between::gone_off(TeamSide side) const {
    std::vector<std::string> out;
    for (const auto& m : men_) {
        if (m->team == side && !m->on) out.push_back(m->id);
    }
    return out;
}

bool Between::matched(TeamSide side) const {
    if (!still_leaving(side).empty()) return false;
    return std::none_of(men_.begin(), men_.end(), [&](const auto& m) { return m->team == side && m->on && m->coming; });
}

bool Between::settled() const {
    return std::none_of(men_.begin(), men_.end(), [](const auto& m) { return m->walking; });
}

std::vector<Spot> Between::where() const {
    std::vector<Spot> out;
    for (const auto& m : men_) {
        if (!m->on) continue;
        const Agent& a = world_.agents()[m->agent];
        out.push_back({m->id, a.label, m->team, m->pos, m->facing, m->walking ? m->facing * m->speed : Vec2{}});
    }
    return out;
}

std::vector<Spot> Between::unset(TeamSide side) const {
    std::vector<Spot> out;
    for (const auto& m : men_) {
        if (m->team != side || !m->on || (!m->in_ring && m->spot && !m->walking)) continue;
        const Agent& a = world_.agents()[m->agent];
        out.push_back({m->id, a.label, m->team, m->pos, m->facing, m->walking ? m->facing * m->speed : Vec2{}});
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
