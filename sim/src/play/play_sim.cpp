// Running one play: huddle, alignment, snap, the live ball, and the whistle.
// The tick loop follows movement-and-interaction.md#tick-loop: Decide (each
// behavior), actions, Move + Interact (World::step), Ball, then the rules.

#include "cfbsim/play/play_sim.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <sstream>

#include "agents/behaviors.h"
#include "agents/steer.h"
#include "cfbsim/interaction/catch.h"
#include "cfbsim/physics/ball_flight.h"
#include "cfbsim/play/routes.h"
#include "play/play_state.h"
#include "play/shapes.h"

namespace cfbsim {

namespace {

std::string fixed1(double v) {
    std::ostringstream s;
    s.setf(std::ios::fixed);
    s.precision(1);
    s << v + 0.0;  // + 0.0 turns -0.0 into 0.0
    return s.str();
}

bool is_qb_stance(Stance s) {
    return s == Stance::UnderCenter || s == Stance::Gun || s == Stance::Pistol;
}

// Closest approach of a ball moving from p0 to p1 this tick to a player at r.
struct Closest {
    double u = 0.0;  // 0..1 along the segment
    Vec3 at;
    double dist = 0.0;  // horizontal, yd
};

Closest closest_approach(Vec3 p0, Vec3 p1, Vec2 r) {
    const Vec3 d = p1 - p0;
    const Vec2 flat = d.ground();
    const double len2 = flat.length_squared();
    Closest c;
    c.u = len2 > 0.0 ? std::clamp((r - p0.ground()).dot(flat) / len2, 0.0, 1.0) : 0.0;
    c.at = p0 + d * c.u;
    c.dist = (c.at.ground() - r).length();
    return c;
}

// A receiver's bust, rolled at the snap and applied when his route starts.
struct Bust {
    enum class Kind : std::uint8_t { WrongRoute, WrongDepth, LateRelease };
    Kind kind = Kind::LateRelease;
    std::string route;         // WrongRoute: the route he runs instead
    double depth_delta = 0.0;  // WrongDepth
    double delay = 0.0;        // LateRelease
};

class PlaySim {
  public:
    PlaySim(const DataSet& data, const PlayOptions& opt, const Team& team, const Roster& roster,
            const Playbook& book, const Play& play, const Formation& formation, TrackingWriter* out)
        : opt_(opt),
          team_(team),
          roster_(roster),
          out_(out),
          world_(data.tuning.movement, opt.seed),
          state_(data, play, formation, book,
                 PlayFrame(opt.yard_line, opt.hash, opt.offense_right, opt.flip), world_) {}

    PlayResult run();

  private:
    const PlayTuning& tuning() const { return state_.tuning().play; }
    const PassingTuning& passing() const { return state_.tuning().passing; }
    std::size_t n() const { return state_.slots.size(); }
    Agent& agent(int i) { return world_.agents()[static_cast<std::size_t>(i)]; }
    void event(std::string_view name, Json fields = Json::object()) {
        if (out_) out_->event(world_, name, std::move(fields));
    }

    void setup();
    void write_header();
    void write_frame();

    void presnap_intents(std::vector<Intent>& intents);
    void check_set();
    void snap();
    void roll_bust(int i);

    std::unique_ptr<Behavior> first_behavior(int i);
    std::unique_ptr<Behavior> job_behavior(int i, const Job& job);
    std::unique_ptr<Behavior> route_behavior(int i, const Job& job);
    std::unique_ptr<Behavior> qb_behavior();
    std::unique_ptr<Behavior> carrier_behavior();
    Vec2 resolve_path_point(const PathPoint& p) const;

    void live_intents(std::vector<Intent>& intents, Actions& actions);
    void apply(const Actions& actions);
    void throw_ball(int qb, int target);
    void ball_phase();
    void catch_ball(int receiver, Vec3 at);
    void check_rules();
    void whistle(const std::string& outcome);

    const PlayOptions& opt_;
    const Team& team_;
    const Roster& roster_;
    TrackingWriter* out_;
    World world_;
    PlayState state_;
    PlayResult result_;

    std::vector<std::size_t> job_index_;  // per agent: his current job in the sequence
    std::vector<std::optional<Bust>> busts_;
    std::int64_t set_ticks_ = 0;
    bool exchange_pending_ = false;  // under center: the ball goes C -> QB just after the snap
    bool snap_in_flight_ = false;    // shotgun: the snap is in the air
    std::int64_t dead_tick_ = 0;
};

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

void PlaySim::setup() {
    const Formation& f = state_.formation;
    const auto personnel = team_.depth_chart.offense_personnel.find(f.personnel);
    if (personnel == team_.depth_chart.offense_personnel.end()) {
        throw PlayError("team " + team_.id + " has no '" + f.personnel + "' personnel");
    }

    std::vector<Vec2> aligned;
    try {
        aligned = align_formation(f, state_.frame);
    } catch (const GeometryError& e) {
        throw PlayError("formation " + f.id + ": " + e.what());
    }

    const auto& huddle_slots = f.slots;
    const double count = static_cast<double>(huddle_slots.size());
    const double radius = tuning().huddle_spacing * count / (2.0 * std::numbers::pi);
    const Vec2 huddle{0.0, -tuning().huddle_depth};

    for (std::size_t k = 0; k < f.slots.size(); ++k) {
        const FormationSlot& slot = f.slots[k];
        const auto fill = personnel->second.find(slot.name);
        if (fill == personnel->second.end()) {
            throw PlayError("personnel '" + f.personnel + "' doesn't fill slot " + slot.name);
        }
        std::string player_id = fill->second.player_id;
        if (fill->second.rank) {
            const DepthRank r = *fill->second.rank;
            const auto pos = team_.depth_chart.positions.find(r.position);
            const std::size_t at = static_cast<std::size_t>(r.rank) - 1;
            if (pos == team_.depth_chart.positions.end() || at >= pos->second.size()) {
                throw PlayError("depth chart has no " + std::string(to_string(r.position)) +
                                std::to_string(r.rank) + " for slot " + slot.name);
            }
            player_id = pos->second[at];
        }
        const Player* player = roster_.find(player_id);
        if (!player) throw PlayError("player '" + player_id + "' isn't on the roster");

        SlotInfo info;
        info.slot = slot.name;
        info.formation_slot = &slot;
        if (const auto a = state_.play.assignments.find(slot.name);
            a != state_.play.assignments.end()) {
            info.assignment = &a->second;
        }
        info.aligned = aligned[k];
        info.scheme_pk = player->playbook_knowledge_for(state_.book.scheme);
        info.familiarity = slot.pos ? player->familiarity_at(*slot.pos) : 100.0;
        state_.slots.push_back(info);

        Agent a = make_agent(*player, TeamSide::Home, slot.name, {}, state_.tuning().movement);
        const Vec2 down = state_.downfield();
        if (opt_.huddle) {
            // A ring around the huddle's center, everyone facing in.
            const double angle = 2.0 * std::numbers::pi * static_cast<double>(k) / count;
            const Vec2 spot = huddle + Vec2{std::cos(angle), std::sin(angle)} * radius;
            a.pos = state_.frame.to_field(spot);
            a.facing = (state_.frame.to_field(huddle) - a.pos).normalized();
        } else {
            a.pos = state_.frame.to_field(aligned[k]);
            a.facing = down;
        }
        world_.add_agent(std::move(a));

        const int i = static_cast<int>(k);
        if (slot.stance == Stance::Snap && state_.center < 0) state_.center = i;
        if (is_qb_stance(slot.stance) && state_.qb < 0) state_.qb = i;
    }
    if (state_.center < 0) throw PlayError("formation " + f.id + " has no snapper");
    if (state_.qb < 0) throw PlayError("formation " + f.id + " has no QB (under center or gun)");
    if (state_.play.run) {
        state_.run_carrier = state_.index_of(state_.play.run->carrier);
        if (state_.run_carrier < 0) throw PlayError("no carrier slot " + state_.play.run->carrier);
    }

    state_.get_off.assign(n(), 0.0);
    state_.throw_reaction.assign(n(), std::numeric_limits<double>::infinity());
    state_.behaviors.resize(n());
    job_index_.assign(n(), 0);
    busts_.assign(n(), std::nullopt);

    Ball& ball = world_.ball();
    ball.state = Ball::State::Spotted;
    ball.pos = {state_.frame.ball().x, state_.frame.ball().y, 0.15};
}

void PlaySim::write_header() {
    if (!out_) return;
    const Play& play = state_.play;
    TrackingInfo info;
    info.kind = "play";
    info.name = play.name + (opt_.flip ? " (flip)" : "");
    info.seed = opt_.seed;
    info.home = {team_.name, team_.abbr, team_.color_primary};
    info.details = {{"team", team_.id},
                    {"play", play.id},
                    {"formation", state_.formation.id},
                    {"flip", opt_.flip},
                    {"yard_line", opt_.yard_line},
                    {"hash", std::string(to_name(kHashNames, opt_.hash))},
                    {"direction", opt_.offense_right ? "right" : "left"}};
    // The line of scrimmage, across the whole field.
    const double x = state_.frame.ball().x;
    info.overlays.push_back(shapes::line("lines", {{x, 0.0}, {x, kFieldWidth}}, "#4fc3f7"));
    out_->header(info, world_);
}

void PlaySim::write_frame() {
    if (!out_) return;
    Json dbg = Json::array();
    if (state_.snapped()) {
        for (int i = 0; i < state_.count(); ++i) state_.behavior(i)->draw(state_, i, dbg);
        if (state_.gaps) {
            for (const Side side : {Side::Left, Side::Right}) {
                for (const char letter : {'A', 'B', 'C', 'D'}) {
                    const Vec2 at = state_.frame.to_field({state_.gaps->x(side, letter), 0.8});
                    dbg.push_back(shapes::text("gaps", at, std::string(1, letter), "#9fb3c8"));
                }
            }
        }
        if (state_.pass) {
            dbg.push_back(shapes::line("throw",
                                       {state_.pass->release.ground(), state_.pass->aim.ground()},
                                       "#ffd400", true));
        }
    }
    out_->frame(world_, std::move(dbg));
}

// ---------------------------------------------------------------------------
// Pre-snap
// ---------------------------------------------------------------------------

void PlaySim::presnap_intents(std::vector<Intent>& intents) {
    const Vec2 huddle = state_.frame.to_field({0.0, -tuning().huddle_depth});
    for (int i = 0; i < state_.count(); ++i) {
        const Agent& a = state_.agent(i);
        Intent& in = intents[static_cast<std::size_t>(i)];
        if (world_.time() < tuning().huddle_break) {
            in.face = (huddle - a.pos).normalized();
            continue;
        }
        const Vec2 spot = state_.frame.to_field(state_.slots[static_cast<std::size_t>(i)].aligned);
        in.velocity = steer::arrive(a, spot, std::min(tuning().jog_speed, a.move.top_speed),
                                    tuning().arrive_decel);
        if (a.vel.length() < 1.0) in.face = state_.downfield();
    }
}

void PlaySim::check_set() {
    bool all_set = true;
    for (int i = 0; i < state_.count(); ++i) {
        const Agent& a = state_.agent(i);
        const Vec2 spot = state_.frame.to_field(state_.slots[static_cast<std::size_t>(i)].aligned);
        all_set = all_set && (spot - a.pos).length() < 0.3 && a.vel.length() < 0.3;
    }
    if (all_set) {
        if (set_ticks_ == 0) event("set");
        ++set_ticks_;
    } else {
        set_ticks_ = 0;
    }
    const bool set_long_enough =
        static_cast<double>(set_ticks_) * kTickSeconds >= tuning().set_seconds;
    if (set_long_enough || world_.time() >= tuning().presnap_max_seconds) snap();
}

// ---------------------------------------------------------------------------
// The snap
// ---------------------------------------------------------------------------

void PlaySim::snap() {
    state_.phase = PlayPhase::Live;
    state_.snap_tick = world_.tick();
    Rng& rng = world_.rng();
    const PlayTuning& t = tuning();

    // Draw order is fixed: everyone's get-off (agent order), then busts,
    // then behaviors (which draw route noise).
    for (int i = 0; i < state_.count(); ++i) {
        const double skill = t.get_off_skill(state_.agent(i).ratings);
        const double delay = t.get_off_delay(skill) + rng.normal(0.0, t.get_off_noise_sd);
        state_.get_off[static_cast<std::size_t>(i)] = std::max(0.0, delay);
    }

    // The gaps, from where the linemen actually are.
    std::vector<double> line_x;
    for (int i = 0; i < state_.count(); ++i) {
        if (state_.slots[static_cast<std::size_t>(i)].formation_slot->on_line) {
            line_x.push_back(state_.play_pos(i).x);
        }
    }
    try {
        state_.gaps = find_gaps(state_.play_pos(state_.center).x, line_x);
    } catch (const GeometryError&) {
        state_.gaps.reset();  // only a problem if something anchors to a gap (reported then)
    }

    event("snap");
    for (int i = 0; i < state_.count(); ++i) roll_bust(i);
    for (int i = 0; i < state_.count(); ++i) {
        state_.behaviors[static_cast<std::size_t>(i)] = first_behavior(i);
    }

    Ball& ball = world_.ball();
    const FormationSlot& qb_slot =
        *state_.slots[static_cast<std::size_t>(state_.qb)].formation_slot;
    if (qb_slot.stance == Stance::UnderCenter) {
        ball.state = Ball::State::Held;
        ball.carrier = state_.center;
        exchange_pending_ = true;
    } else {
        // Shotgun / pistol: the snap flies back to the QB's hands.
        const Agent& qb = state_.agent(state_.qb);
        const Vec3 from{ball.pos.x, ball.pos.y, 0.3};
        const Vec3 to{qb.pos.x, qb.pos.y, 1.2};
        const double speed = t.gun_snap_speed;
        ball.pos = from;
        ball.vel = launch_velocity(from, to, speed, passing().gravity)
                       .value_or(max_range_velocity(from, to, speed));
        ball.state = Ball::State::InFlight;
        ball.carrier = -1;
        snap_in_flight_ = true;
    }
}

void PlaySim::roll_bust(int i) {
    const SlotInfo& slot = state_.slots[static_cast<std::size_t>(i)];
    if (!slot.assignment) return;
    // Phase 4 busts receivers only; blockers and defenders bust in phases 5-6.
    const Job* route_job = nullptr;
    for (const Job& job : slot.assignment->sequence) {
        if (job.kind == JobKind::Route) {
            route_job = &job;
            break;
        }
    }
    if (!route_job) return;

    Rng& rng = world_.rng();
    const PlayTuning& t = tuning();
    const double pk = effective_playbook_knowledge(slot.scheme_pk, slot.familiarity, t);
    if (!rng.chance(bust_chance(pk, t))) return;

    const RouteLibrary& routes = state_.data.routes;
    const Route& right = routes.at(route_job->route.route);
    Bust bust;
    bust.kind = static_cast<Bust::Kind>(rng.uniform_int(0, 2));
    std::string text;

    if (bust.kind == Bust::Kind::WrongRoute) {
        // A route that goes to about the same depth: the kind of mix-up a
        // receiver actually makes.
        std::vector<const Route*> similar;
        for (const auto& [id, r] : routes) {
            if (id != right.id &&
                std::abs(route_depth(r) - route_depth(right)) <= t.similar_route_depth) {
                similar.push_back(&r);
            }
        }
        if (similar.empty()) {
            bust.kind = Bust::Kind::WrongDepth;
        } else {
            const auto pick = rng.uniform_int(0, static_cast<int>(similar.size()) - 1);
            bust.route = similar[static_cast<std::size_t>(pick)]->id;
            text = "ran " + bust.route + " instead of " + right.id;
        }
    }
    if (bust.kind == Bust::Kind::WrongDepth) {
        if (!right.break_depth()) {
            bust.kind = Bust::Kind::LateRelease;
        } else {
            const double size = rng.uniform(t.wrong_depth_min, t.wrong_depth_max);
            const double sign = rng.chance(0.5) ? 1.0 : -1.0;
            const double planned = route_job->route.depth.value_or(*right.break_depth());
            // Never shallower than 1 yd.
            bust.depth_delta = std::max(sign * size, 1.0 - planned);
            text = "broke at " + fixed1(planned + bust.depth_delta) + " yd instead of " +
                   fixed1(planned);
        }
    }
    if (bust.kind == Bust::Kind::LateRelease) {
        bust.delay = rng.uniform(t.late_release_min, t.late_release_max);
        text = "released " + fixed1(bust.delay) + " s late";
    }

    result_.busts.push_back(slot.slot + ": " + text);
    event("bust", {{"player", slot.slot}, {"detail", text}});
    busts_[static_cast<std::size_t>(i)] = bust;
}

// ---------------------------------------------------------------------------
// Behaviors
// ---------------------------------------------------------------------------

std::unique_ptr<Behavior> PlaySim::first_behavior(int i) {
    if (i == state_.qb) return qb_behavior();
    if (i == state_.run_carrier) return carrier_behavior();
    const SlotInfo& slot = state_.slots[static_cast<std::size_t>(i)];
    if (!slot.assignment || slot.assignment->sequence.empty()) {
        // No job: stay put.
        return std::make_unique<StandInBlocker>(StandInBlocker::Kind::RunBlock,
                                                state_.agent(i).pos);
    }
    return job_behavior(i, slot.assignment->sequence.front());
}

std::unique_ptr<Behavior> PlaySim::job_behavior(int i, const Job& job) {
    const PlayTuning& t = tuning();
    const Vec2 at = state_.play_pos(i);
    const auto field = [&](Vec2 p) { return state_.frame.to_field(p); };

    if (job.kind == JobKind::Route) return route_behavior(i, job);
    if (job.kind != JobKind::Block) {
        throw PlayError(state_.slot_name(i) + ": '" + std::string(to_string(job.kind)) +
                        "' jobs aren't supported on offense");
    }

    using Kind = StandInBlocker::Kind;
    if (job.action == "protect" || job.action == "pass_set") {
        return std::make_unique<StandInBlocker>(Kind::PassSet,
                                                field(at + Vec2{0.0, -t.pass_set_depth}));
    }
    if (job.action == "check_release") {
        return std::make_unique<StandInBlocker>(Kind::CheckRelease, field(at + Vec2{0.0, -0.5}),
                                                t.check_release_seconds);
    }
    if (job.action == "stalk") {
        return std::make_unique<StandInBlocker>(Kind::Stalk, field(at + Vec2{0.0, t.stalk_depth}));
    }
    // Run blocks ("scheme", "drive", "reach", ...): fire out toward the play side.
    double play_side = 0.0;
    if (state_.play.run) {
        const Side dir = state_.frame.flipped() ? mirrored(state_.play.run->direction)
                                                : state_.play.run->direction;
        play_side = dir == Side::Left ? -1.0 : 1.0;
    }
    return std::make_unique<StandInBlocker>(
        Kind::RunBlock, field(at + Vec2{play_side * t.run_lean, t.run_fire_out}));
}

std::unique_ptr<Behavior> PlaySim::route_behavior(int i, const Job& job) {
    const auto idx = static_cast<std::size_t>(i);
    const PlayTuning& t = tuning();
    const RouteLibrary& routes = state_.data.routes;

    const Route* route = &routes.at(job.route.route);
    std::optional<double> depth = job.route.depth;
    double delay = 0.0;
    bool busted = false;
    // A bust applies to his first route (and only once).
    if (busts_[idx]) {
        const Bust& b = *busts_[idx];
        busted = true;
        switch (b.kind) {
            case Bust::Kind::WrongRoute:
                route = &routes.at(b.route);
                depth.reset();
                break;
            case Bust::Kind::WrongDepth:
                depth = depth.value_or(*route->break_depth()) + b.depth_delta;
                break;
            case Bust::Kind::LateRelease:
                delay = b.delay;
                break;
        }
        busts_[idx].reset();
    }

    const Side side = state_.frame.flipped() ? mirrored(job.route.side) : job.route.side;
    const double outside = outside_sign(state_.slots[idx].aligned.x, side);
    RoutePlan plan = plan_route(*route, depth, state_.play_pos(i), outside);
    add_route_noise(plan, t.route_precision_sd(state_.agent(i).ratings[Attr::RouteRunning]),
                    world_.rng());
    clamp_to_field(plan, state_.frame, t.sideline_margin);

    const double start = std::max(state_.get_off[idx], state_.since_snap()) + delay;
    return std::make_unique<RouteRunner>(plan, state_.frame, start, busted);
}

std::unique_ptr<Behavior> PlaySim::qb_behavior() {
    const Play& play = state_.play;
    const PlayFrame& frame = state_.frame;
    const QbLibrary& qb_lib = state_.data.qb;
    // Authored play-frame points -> field frame, mirrored with the play.
    const auto to_field = [&](const std::vector<Vec2>& pts) {
        std::vector<Vec2> out;
        for (Vec2 p : pts) out.push_back(frame.to_field({p.x * frame.mirror(), p.y}));
        return out;
    };

    if (play.type == PlayType::Pass) {
        const PassSpec& pass = *play.pass;
        std::vector<int> reads;
        if (!opt_.target.empty()) {
            const int target = state_.index_of(opt_.target);
            if (target < 0) throw PlayError("no slot '" + opt_.target + "' on this play");
            reads.push_back(target);
        } else {
            for (const std::string& slot : pass.progression) {
                if (const int k = state_.index_of(slot); k >= 0) reads.push_back(k);
            }
        }
        if (reads.empty()) throw PlayError("the play has no reads");

        const QbPath& drop = qb_lib.drops.at(pass.drop);
        const Attributes& r = state_.agent(state_.qb).ratings;
        return std::make_unique<PassingQb>(to_field(drop.points), drop.set_seconds,
                                           std::move(reads),
                                           passing().look_seconds(passing().look_skill(r)),
                                           passing().delivery_seconds(r[Attr::Delivery]));
    }

    const RunSpec& run = *play.run;
    if (run.qb_action != "handoff") {
        throw PlayError("QB action '" + run.qb_action + "' isn't supported yet (only handoff)");
    }
    // He meets the carrier at the path's "mesh" point (or its first point),
    // coming from his own side of it.
    if (run.path.empty()) throw PlayError("a handoff needs a run path");
    const PathPoint* mesh_point = &run.path.front();
    for (const PathPoint& p : run.path) {
        if (p.type == "mesh") mesh_point = &p;
    }
    const Vec2 mesh = resolve_path_point(*mesh_point);
    const Vec2 qb_at = state_.play_pos(state_.qb);
    const Vec2 spot = mesh + (qb_at - mesh).normalized() * tuning().qb_mesh_offset;

    std::vector<Vec2> fake{frame.to_field(spot)};
    if (!run.qb_then.empty()) fake = to_field(qb_lib.fakes.at(run.qb_then).points);
    return std::make_unique<HandoffQb>(frame.to_field(spot), state_.run_carrier, std::move(fake));
}

Vec2 PlaySim::resolve_path_point(const PathPoint& p) const {
    // At the snap, anchors resolve against where players actually are and
    // the gaps between the linemen as they actually lined up.
    AnchorContext ctx;
    ctx.frame = &state_.frame;
    ctx.gaps = state_.gaps ? &*state_.gaps : nullptr;
    ctx.slot_pos = [this](std::string_view slot) -> std::optional<Vec2> {
        const int k = state_.index_of(slot);
        if (k < 0) return std::nullopt;
        return state_.play_pos(k);
    };
    try {
        return {resolve_x(p.x, ctx), resolve_y(p.y, ctx)};
    } catch (const GeometryError& e) {
        throw PlayError(std::string("run path: ") + e.what());
    }
}

std::unique_ptr<Behavior> PlaySim::carrier_behavior() {
    const RunSpec& run = *state_.play.run;
    std::vector<BallCarrier::Mark> path;
    std::size_t free_after = run.path.empty() ? 0 : run.path.size() - 1;
    for (std::size_t k = 0; k < run.path.size(); ++k) {
        const PathPoint& p = run.path[k];
        path.push_back({state_.frame.to_field(resolve_path_point(p)), p.type});
        if (!run.free_after.empty() && p.type == run.free_after) free_after = k;
    }
    return std::make_unique<BallCarrier>(std::move(path), free_after,
                                         state_.agent(state_.run_carrier).pos);
}

// ---------------------------------------------------------------------------
// The live ball
// ---------------------------------------------------------------------------

void PlaySim::live_intents(std::vector<Intent>& intents, Actions& actions) {
    // A QB still holding the ball on a run play (the mesh never happened)
    // keeps it and runs.
    if (state_.play.run && state_.has_ball(state_.qb) && state_.since_snap() > 2.5 &&
        !dynamic_cast<const BallCarrier*>(state_.behavior(state_.qb))) {
        event("handoff_missed");
        state_.behaviors[static_cast<std::size_t>(state_.qb)] = std::make_unique<BallCarrier>(
            std::vector<BallCarrier::Mark>{}, 0, state_.agent(state_.qb).pos);
    }

    for (int i = 0; i < state_.count(); ++i) {
        const auto idx = static_cast<std::size_t>(i);
        // When a job is done, the next job in his assignment starts.
        if (state_.behaviors[idx]->done(state_, i)) {
            const Assignment* a = state_.slots[idx].assignment;
            ++job_index_[idx];
            if (a && job_index_[idx] < a->sequence.size()) {
                state_.behaviors[idx] = job_behavior(i, a->sequence[job_index_[idx]]);
            }
        }
    }
    // Decide: every behavior reads the same (previous tick's) state.
    for (int i = 0; i < state_.count(); ++i) {
        intents[static_cast<std::size_t>(i)] =
            state_.behaviors[static_cast<std::size_t>(i)]->decide(state_, i, actions);
    }
}

void PlaySim::apply(const Actions& actions) {
    if (actions.handoff) {
        Ball& ball = world_.ball();
        if (ball.state == Ball::State::Held && ball.carrier == actions.handoff->from) {
            ball.carrier = actions.handoff->to;
            result_.handoff = true;
            event("handoff", {{"from", state_.slot_name(actions.handoff->from)},
                              {"to", state_.slot_name(actions.handoff->to)}});
        }
    }
    if (actions.throw_ball) throw_ball(actions.throw_ball->from, actions.throw_ball->target);
}

void PlaySim::throw_ball(int qb_index, int target) {
    Ball& ball = world_.ball();
    if (!state_.has_ball(qb_index)) return;
    const Agent& qb = state_.agent(qb_index);
    const PassingTuning& t = passing();
    Rng& rng = world_.rng();

    const Vec2 hand = qb.pos + qb.facing * 0.3;
    const Vec3 release{hand.x, hand.y, qb.body.height * t.release_height_factor};
    const double top_speed = t.throw_speed(qb.ratings[Attr::ThrowPower]);
    const auto speed_for = [&](double distance) {
        return top_speed * std::clamp(t.throw_effort(distance), 0.1, 1.0);
    };
    const auto velocity_to = [&](Vec3 to) {
        const double speed = speed_for((to.ground() - release.ground()).length());
        return launch_velocity(release, to, speed, t.gravity)
            .value_or(max_range_velocity(release, to, speed));
    };

    // The projected catch point: where the receiver will be when the ball
    // gets there. The flight time depends on the distance and the distance
    // on the flight time, so iterate; it settles within a few rounds.
    const Behavior* receiver = state_.behavior(target);
    const Agent& rec = state_.agent(target);
    Vec2 aim2 = rec.pos;
    for (int round = 0; round < 8; ++round) {
        const Vec3 to{aim2.x, aim2.y, t.target_height};
        const Vec3 v = velocity_to(to);
        const double flight =
            (to.ground() - release.ground()).length() / std::max(v.ground().length(), 1e-6);
        aim2 = receiver->predict(state_, target, flight).value_or(rec.pos + rec.vel * flight);
    }
    // Never aim closer to a sideline than routes are allowed to go.
    {
        Vec2 p = state_.frame.to_play(aim2);
        const double margin = tuning().sideline_margin;
        p.x = std::clamp(p.x, state_.frame.sideline_x(Side::Left) + margin,
                         state_.frame.sideline_x(Side::Right) - margin);
        aim2 = state_.frame.to_field(p);
    }
    const Vec3 aim{aim2.x, aim2.y, t.target_height};
    const double distance = (aim.ground() - release.ground()).length();

    // Placement error: accuracy by air distance, worse on longer throws and
    // on the move.
    const Attr accuracy = distance <= t.short_max    ? Attr::ShortAccuracy
                          : distance <= t.medium_max ? Attr::MediumAccuracy
                                                     : Attr::DeepAccuracy;
    double sd = t.accuracy_sd(qb.ratings[accuracy]) * t.distance_scale(distance);
    if (qb.vel.length() > t.on_the_run_speed)
        sd *= t.on_the_run_factor(qb.ratings[Attr::ThrowOnTheRun]);
    const double ex = rng.normal(0.0, sd);
    const double ey = rng.normal(0.0, sd);
    const double ez = rng.normal(0.0, sd * t.height_sd_factor);
    const Vec3 actual{aim.x + ex, aim.y + ey, std::max(0.3, aim.z + ez)};

    ball.state = Ball::State::InFlight;
    ball.carrier = -1;
    ball.pos = release;
    ball.vel = velocity_to(actual);

    PassInfo info;
    info.thrower = qb_index;
    info.target = target;
    info.tick = world_.tick();
    info.release = release;
    info.aim = aim;
    state_.pass = info;

    // Everyone notices the throw after his own reaction time.
    const PlayTuning& pt = tuning();
    for (int i = 0; i < state_.count(); ++i) {
        const double skill = pt.reaction_skill(state_.agent(i).ratings);
        const double delay = pt.reaction_delay(skill) + rng.normal(0.0, pt.reaction_noise_sd);
        state_.throw_reaction[static_cast<std::size_t>(i)] = std::max(0.05, delay);
    }

    result_.target = state_.slot_name(target);
    result_.time_to_throw = state_.since_snap();
    result_.air_yards = state_.frame.to_play(aim.ground()).y;
    event("throw", {{"from", state_.slot_name(qb_index)},
                    {"to", result_.target},
                    {"air_yards", result_.air_yards},
                    {"distance", distance},
                    {"speed", ball.vel.length()},
                    {"error", std::hypot(ex, ey)}});
}

void PlaySim::ball_phase() {
    Ball& ball = world_.ball();
    const PlayTuning& t = tuning();

    if (exchange_pending_ && state_.since_snap() >= t.snap_exchange_seconds) {
        ball.carrier = state_.qb;
        exchange_pending_ = false;
    }
    if (ball.state != Ball::State::InFlight) return;

    const Vec3 before = ball.pos;
    step_ballistic(ball.pos, ball.vel, passing().gravity, kTickSeconds);

    if (snap_in_flight_) {
        // No bad snaps yet: the QB catches it as it reaches him.
        const Closest c = closest_approach(before, ball.pos, state_.agent(state_.qb).pos);
        if ((c.u < 1.0 && c.dist <= t.snap_catch_range) || ball.pos.z <= 0.3) {
            ball.state = Ball::State::Held;
            ball.carrier = state_.qb;
            snap_in_flight_ = false;
        }
        return;
    }

    if (state_.pass && state_.phase == PlayPhase::Live && !state_.pass->attempted) {
        const int target = state_.pass->target;
        const Agent& rec = state_.agent(target);
        const Closest c = closest_approach(before, ball.pos, rec.pos);
        const CatchWindow w = catch_window(rec, passing());
        // The ball is as close to him as it gets this tick, and within reach.
        if (c.u < 1.0 && c.dist <= w.dive_reach && c.at.z >= w.low && c.at.z <= w.high) {
            state_.pass->attempted = true;
            const bool diving = c.dist > w.reach;
            const double p = catch_chance(rec, c.dist, diving, ball.vel.length(), passing());
            if (world_.rng().chance(p)) {
                catch_ball(target, c.at);
                return;
            }
            event("drop", {{"player", state_.slot_name(target)}, {"diving", diving}});
        }
    }

    if (ball.pos.z <= 0.0) {
        // Where it hit the ground, between this tick's two points.
        const double f = before.z / (before.z - ball.pos.z);
        ball.pos = before + (ball.pos - before) * f;
        ball.pos.z = 0.0;
        ball.vel = {};
        ball.state = Ball::State::Dead;
        if (state_.phase == PlayPhase::Live) whistle("incomplete");
    }
}

void PlaySim::catch_ball(int receiver, Vec3 at) {
    Ball& ball = world_.ball();
    // A catch with a foot out of bounds is no catch. (The full rule, one foot
    // in bounds with control, comes with the rules layer in phase 8.)
    const Vec2 feet = state_.agent(receiver).pos;
    if (feet.y < 0.0 || feet.y > kFieldWidth) {
        event("out_of_bounds_catch", {{"player", state_.slot_name(receiver)}});
        ball.state = Ball::State::Dead;
        ball.pos = at;
        ball.vel = {};
        whistle("incomplete");
        return;
    }
    ball.state = Ball::State::Held;
    ball.carrier = receiver;
    ball.pos = at;
    result_.complete = true;
    result_.air_yards = state_.frame.to_play(at.ground()).y;
    event("catch", {{"player", state_.slot_name(receiver)}, {"air_yards", result_.air_yards}});
    // After the catch he is a ball carrier. Assigning a new unique_ptr
    // deletes the RouteRunner it held.
    state_.behaviors[static_cast<std::size_t>(receiver)] = std::make_unique<BallCarrier>(
        std::vector<BallCarrier::Mark>{}, 0, state_.agent(receiver).pos);
    job_index_[static_cast<std::size_t>(receiver)] = std::numeric_limits<std::size_t>::max() / 2;
}

void PlaySim::check_rules() {
    if (state_.phase != PlayPhase::Live) return;
    const Ball& ball = world_.ball();
    if (ball.state == Ball::State::Held && ball.carrier >= 0) {
        const Vec2 p = state_.agent(ball.carrier).pos;
        if (p.y < 0.0 || p.y > kFieldWidth || p.x < 0.0 || p.x > kFieldLength) {
            whistle("out_of_bounds");
            return;
        }
        if (state_.frame.to_play(p).y >= state_.frame.goal_line_y()) {
            whistle("touchdown");
            return;
        }
    }
    // Nobody can tackle him yet, so the play ends on time.
    if (state_.since_snap() >= tuning().play_max_seconds) whistle("whistle");
}

void PlaySim::whistle(const std::string& outcome) {
    state_.phase = PlayPhase::Dead;
    dead_tick_ = world_.tick();
    result_.outcome = outcome;
    result_.seconds = state_.since_snap();

    const Ball& ball = world_.ball();
    if (ball.state == Ball::State::Held && ball.carrier >= 0) {
        result_.carrier = state_.slot_name(ball.carrier);
    }
    if (outcome == "touchdown") {
        result_.yards = state_.frame.goal_line_y();
    } else if (outcome != "incomplete" && ball.state == Ball::State::Held) {
        result_.yards = state_.frame.to_play(state_.agent(ball.carrier).pos).y;
    }
    event(outcome, {{"yards", result_.yards}});
}

PlayResult PlaySim::run() {
    setup();
    write_header();
    if (!opt_.huddle) snap();
    write_frame();

    const auto post_ticks =
        static_cast<std::int64_t>(std::round(tuning().post_whistle_seconds * kTicksPerSecond));
    std::vector<Intent> intents(n());
    while (true) {
        Actions actions;
        std::fill(intents.begin(), intents.end(), Intent{});
        switch (state_.phase) {
            case PlayPhase::PreSnap:
                presnap_intents(intents);
                break;
            case PlayPhase::Live:
                live_intents(intents, actions);
                break;
            case PlayPhase::Dead:
                break;  // everyone pulls up (a zero desired velocity)
        }
        apply(actions);
        world_.step(intents);
        ball_phase();
        check_rules();
        if (state_.phase == PlayPhase::PreSnap) check_set();
        write_frame();
        if (state_.phase == PlayPhase::Dead && world_.tick() - dead_tick_ >= post_ticks) break;
    }

    // One readable line.
    std::ostringstream s;
    if (state_.play.type == PlayType::Pass) {
        if (result_.target.empty()) {
            s << "No throw";
        } else {
            s << (result_.complete ? "Complete to " : "Incomplete to ") << result_.target << ", "
              << fixed1(result_.air_yards) << " yd downfield after "
              << fixed1(result_.time_to_throw) << " s";
        }
    } else {
        s << (result_.handoff ? "Handoff to " + state_.play.run->carrier : "No handoff");
    }
    if (result_.outcome != "incomplete") {
        s << "; " << result_.outcome << ", " << fixed1(result_.yards) << " yd";
    }
    if (!result_.busts.empty()) s << " (" << result_.busts.size() << " bust)";
    result_.summary = s.str();
    return result_;
}

}  // namespace

PlayResult run_play(const DataSet& data, const PlayOptions& options, TrackingWriter* out) {
    const auto team = data.teams.find(options.team);
    if (team == data.teams.end()) throw PlayError("unknown team '" + options.team + "'");
    const auto roster = data.rosters.find(options.team);
    if (roster == data.rosters.end()) throw PlayError("no roster for team '" + options.team + "'");
    const auto book = data.playbooks.find(team->second.offense_playbook);
    if (book == data.playbooks.end()) {
        throw PlayError("unknown playbook '" + team->second.offense_playbook + "'");
    }
    const Play* play = book->second.find(options.play);
    if (!play) {
        throw PlayError("playbook " + book->first + " has no play '" + options.play + "'");
    }
    if (play->type != PlayType::Pass && play->type != PlayType::Run) {
        throw PlayError("only pass and run plays can be run so far");
    }
    if (options.flip && !play->flip) throw PlayError("play " + play->id + " can't be flipped");
    if (options.yard_line < 1.0 || options.yard_line > 99.0) {
        throw PlayError("yard line must be 1-99");
    }
    const auto formation = data.formations.find(play->formation);
    if (formation == data.formations.end()) {
        throw PlayError("unknown formation '" + play->formation + "'");
    }

    PlaySim sim(data, options, team->second, roster->second, book->second, *play, formation->second,
                out);
    return sim.run();
}

}  // namespace cfbsim
