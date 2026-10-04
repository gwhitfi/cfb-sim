// The between-plays animation (see cfbsim/play/interlude.h): kinematic walks
// between a few stops, eased in and out. No ratings, no randomness: the
// stops and the times come from the game, and where the next play lines
// everyone up comes from the play's own setup (play_start, kick_start).

#include "cfbsim/play/interlude.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace cfbsim {

namespace {

constexpr std::int64_t kFrameTicks = 4;
constexpr double kMaxWalk = 10.0;  // s: a walk with no schedule (a kickoff, a try) ends by then

// One walk: from `a` at t0 to `b` at t1, then standing, facing `face`.
struct Leg {
    double t0 = 0.0, t1 = 0.0;
    Vec2 a, b;
    Vec2 face{1.0, 0.0};
};

struct Path {
    Vec2 start;
    Vec2 start_face{1.0, 0.0};
    std::vector<Leg> legs;

    // Walks on to `to` at `speed`, starting at `at` (or when the last walk
    // ends), done by `latest` when it can be.
    void walk(double at, Vec2 to, Vec2 face, double speed, std::optional<double> latest) {
        const Vec2 from = legs.empty() ? start : legs.back().b;
        at = std::max(at, legs.empty() ? 0.0 : legs.back().t1);
        double take = std::max(0.6, (to - from).length() / speed);
        if (latest && at + take > *latest) take = std::max(0.4, *latest - at);
        legs.push_back({at, at + take, from, to, face});
    }
    double done() const { return legs.empty() ? 0.0 : legs.back().t1; }

    // Where he is at `t`, how fast he's going, and which way he faces.
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
};

std::size_t side_index(TeamSide s) { return s == TeamSide::Home ? 0 : 1; }

// A spot off the field on a team's sideline (home: below y = 0), near the ball.
Vec2 sideline(TeamSide side, int k, double ball_x) {
    const double y = side == TeamSide::Home ? -3.0 : kFieldWidth + 3.0;
    return {ball_x + (static_cast<double>(k % 12) - 5.5) * 1.5, y};
}

Vec2 toward_field(TeamSide side) { return {0.0, side == TeamSide::Home ? 1.0 : -1.0}; }

}  // namespace

void write_interlude(const Interlude& in, const MovementTuning& movement, const PlayTuning& t, TrackingWriter& out) {
    const PlayFrame& f = in.frame;
    const Vec2 ball{f.ball().x, f.ball().y};
    // The huddles: a ring behind the ball for the offense, one across from it for the defense.
    const Vec2 off_center{0.0, -t.huddle_depth};
    const Vec2 def_center{0.0, t.huddle_depth + 1.0};
    std::size_t n_off = 0, n_def = 0;
    for (const InterludeMan& m : in.men) {
        if (m.to) ++(m.team == in.offense ? n_off : n_def);
    }

    std::vector<Path> paths;
    std::size_t k_off = 0, k_def = 0;
    std::array<int, 2> on_sideline{};
    double walked = 0.0;
    for (const InterludeMan& m : in.men) {
        Path p;
        const bool off = m.team == in.offense;
        std::optional<Vec2> ring;
        Vec2 ring_face{1.0, 0.0};
        if (m.to && in.times && in.huddle) {
            const Vec2 c = off ? off_center : def_center;
            ring = f.to_field(huddle_spot(off ? k_off++ : k_def++, off ? n_off : n_def, c.y, t));
            ring_face = (f.to_field(c) - *ring).normalized();
        }
        const auto bench = [&] { return sideline(m.team, on_sideline[side_index(m.team)]++, ball.x); };

        // Where he starts.
        if (m.from) {
            p.start = m.from->pos;
            p.start_face = m.from->facing;
        } else if (in.fresh && m.to) {
            p.start = ring ? *ring : m.to->pos;
            p.start_face = ring ? ring_face : m.to->facing;
        } else {
            p.start = bench();
            p.start_face = toward_field(m.team);
        }

        // Where he goes.
        const double speed = m.from ? t.walk_back : t.sideline_jog;
        if (!m.to) {
            if (m.from) p.walk(0.0, bench(), toward_field(m.team), t.sideline_jog, std::nullopt);
        } else if (in.times) {
            const InterludeTimes& w = *in.times;
            if (ring) {
                p.walk(0.0, *ring, ring_face, speed, std::max(0.5, w.broke - 0.3));
                p.walk(w.broke, m.to->pos, m.to->facing, t.jog_speed, w.set);
            } else {
                p.walk(0.0, m.to->pos, m.to->facing, speed, w.set);
            }
        } else {
            p.walk(0.0, m.to->pos, m.to->facing, std::max(speed, t.jog_speed), kMaxWalk);
            walked = std::max(walked, p.done());
        }
        paths.push_back(std::move(p));
    }
    const double end = in.times ? in.times->end : std::max(1.0, walked + t.set_seconds);

    // The steps as events, then any others, in time order.
    std::vector<std::pair<double, std::string>> events = in.events;
    if (in.times) {
        const InterludeTimes& w = *in.times;
        events.emplace_back(w.call, "play_called");
        events.emplace_back(w.defense_call, "defense_called");
        if (in.huddle) events.emplace_back(w.broke, "break");
        events.emplace_back(w.set, "set");
    }
    std::stable_sort(events.begin(), events.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    World world(movement, 0);
    for (const InterludeMan& m : in.men) world.add_agent(make_agent(*m.player, m.team, m.label, {}, movement));
    TrackingInfo info = in.info;
    info.frame_ticks = static_cast<int>(kFrameTicks);
    out.header(info, world);

    // The ball: spotted for the next snap a few seconds after the whistle
    // (at once with no schedule: the kicking tee, the try).
    const double spotted = in.times ? std::min(3.0, end) : 0.0;
    const std::int64_t end_tick = std::llround(end * kTicksPerSecond);
    std::size_t next_event = 0;
    for (std::int64_t tick = 0;; tick = std::min(tick + kFrameTicks, end_tick)) {
        const double time = static_cast<double>(tick) * kTickSeconds;
        while (next_event < events.size() && events[next_event].first <= time + 1e-9) {
            const double at = std::clamp(events[next_event].first, 0.0, end);
            world.set_tick(std::llround(at * kTicksPerSecond));
            out.event(world, events[next_event].second);
            ++next_event;
        }
        for (std::size_t i = 0; i < paths.size(); ++i) {
            Agent& a = world.agents()[i];
            paths[i].at(time, a.pos, a.vel, a.facing);
        }
        Ball& b = world.ball();
        b.state = time >= spotted ? Ball::State::Spotted : Ball::State::Dead;
        b.pos = {ball.x, ball.y, 0.12};
        world.set_tick(tick);
        out.frame(world);
        if (tick == end_tick) break;
    }
    // Events at the very end (a timeout, a flag).
    for (; next_event < events.size(); ++next_event) {
        world.set_tick(end_tick);
        out.event(world, events[next_event].second);
    }
}

}  // namespace cfbsim
