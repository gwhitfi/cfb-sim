// Running one play: huddle, alignment, snap, the live ball, and the whistle.
// The offense plays against a defense (phase 5) or an empty field.
// The tick loop follows movement-and-interaction.md#tick-loop: Decide (each
// behavior), actions, Move + Interact (World::step), Ball, then the rules.

#include "cfbsim/play/play_sim.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>
#include <sstream>

#include "agents/behaviors.h"
#include "agents/steer.h"
#include "cfbsim/interaction/catch.h"
#include "cfbsim/physics/ball_flight.h"
#include "cfbsim/play/routes.h"
#include "play/engagements.h"
#include "play/openness.h"
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

// A bust, rolled at the snap and applied when his job starts. Receivers:
// wrong route, wrong depth, late release. Coverage: wrong man, wrong zone,
// late (movement-and-interaction.md#busts-defense).
struct Bust {
    enum class Kind : std::uint8_t {
        WrongRoute,
        WrongDepth,
        LateRelease,
        WrongMan,
        WrongZone,
        Late
    };
    Kind kind = Kind::LateRelease;
    std::string route;         // WrongRoute: the route he runs instead
    double depth_delta = 0.0;  // WrongDepth
    double delay = 0.0;        // LateRelease, Late (a zone defender's drop)
    std::string zone;          // WrongZone
};

// Where a throw to a receiver would go: the projected catch point and the
// throw that gets it there.
struct Projection {
    Vec2 aim;  // field frame
    Vec3 release;
    Vec3 velocity;
    double flight = 0.0;
};

// The defense's side of the matchup.
struct DefenseSide {
    const Team* team = nullptr;
    const Roster* roster = nullptr;
    const Playbook* book = nullptr;
    const Play* play = nullptr;
    const Formation* formation = nullptr;
};

class PlaySim {
  public:
    PlaySim(const DataSet& data, const PlayOptions& opt, const Team& team, const Roster& roster,
            const Playbook& book, const Play& play, const Formation& formation, DefenseSide defense,
            TrackingWriter* out)
        : opt_(opt),
          team_(team),
          roster_(roster),
          defense_(defense),
          out_(out),
          world_(data.tuning.movement, opt.seed),
          state_(data, play, formation, book,
                 PlayFrame(opt.yard_line, opt.hash, opt.offense_right, opt.flip), world_),
          blocking_(state_, [this](std::string_view name, Json fields) {
              event(name, std::move(fields));
          }) {
        state_.defense_play = defense.play;
        state_.defense_formation = defense.formation;
        state_.defense_book = defense.book;
    }

    PlayResult run();

  private:
    const PlayTuning& tuning() const { return state_.tuning().play; }
    const PassingTuning& passing() const { return state_.tuning().passing; }
    const DefenseTuning& defense() const { return state_.tuning().defense; }
    const BlockingTuning& blocking() const { return state_.tuning().blocking; }
    std::size_t n() const { return state_.slots.size(); }
    Agent& agent(int i) { return world_.agents()[static_cast<std::size_t>(i)]; }
    void event(std::string_view name, Json fields = Json::object()) {
        if (out_) out_->event(world_, name, std::move(fields));
    }

    void setup();
    const Player& fill_slot(const Team& team, const Roster& roster, const SlotMap& map,
                            const std::string& slot) const;
    void setup_defense();
    OffenseRead read_offense_set(bool actual) const;
    void write_header();
    void write_frame();

    void presnap_intents(std::vector<Intent>& intents);
    void check_set();
    void snap();
    void record_history();
    void roll_bust(int i);

    std::unique_ptr<Behavior> first_behavior(int i);
    std::unique_ptr<Behavior> job_behavior(int i, const Job& job);
    std::unique_ptr<Behavior> route_behavior(int i, const Job& job);
    std::unique_ptr<Behavior> pass_blocker(int i, const Job& job);
    std::unique_ptr<Behavior> run_blocker(int i, const Job& job);
    double playside() const;
    BlockRule scheme_rule(int i, double side) const;
    bool block_condition(int i, const std::string& condition, double side) const;
    int block_target_for(int i, const std::string& target, double side) const;
    std::vector<int> front(bool down_linemen) const;
    std::unique_ptr<Behavior> qb_behavior();
    std::unique_ptr<Behavior> carrier_behavior();
    Vec2 resolve_path_point(const PathPoint& p) const;
    const RouteRef& option_branch(int i, const Job& job);

    void roll_coverage_bust(int i);
    std::unique_ptr<Behavior> defense_behavior(int i);
    std::unique_ptr<Behavior> defense_job(int i, const Job& job, const Job* pass_job);
    std::unique_ptr<Behavior> zone_behavior(int i, const std::string& zone_id, double late);
    ZoneArea zone_area(const std::string& zone_id) const;
    Side own_side(int i) const;
    double outward(Side side) const { return side == Side::Left ? -1.0 : 1.0; }
    double gap_x(Side side, const std::string& letter) const;

    void live_intents(std::vector<Intent>& intents, Actions& actions);
    void apply(const Actions& actions);
    // `delay`: seconds until the ball leaves his hand.
    Projection project_catch(int qb, int target, int rounds, double delay = 0.0) const;
    void update_openness();
    void throw_ball(int qb, int target, bool away);
    void ball_phase();
    void play_the_ball(Vec3 before);
    void after_touch(int player, Vec3 at, bool defender, const char* what);
    void catch_ball(int receiver, Vec3 at);
    void intercept(int defender, Vec3 at);
    void check_rules();
    void track_pressure();
    void whistle(const std::string& outcome);

    const PlayOptions& opt_;
    const Team& team_;
    const Roster& roster_;
    DefenseSide defense_;
    TrackingWriter* out_;
    World world_;
    PlayState state_;
    PlayResult result_;
    Engagements blocking_;

    std::vector<std::size_t> job_index_;  // per agent: his current job in the sequence
    std::vector<std::optional<Bust>> busts_;
    std::int64_t set_ticks_ = 0;
    bool exchange_pending_ = false;  // under center: the ball goes C -> QB just after the snap
    bool snap_in_flight_ = false;    // shotgun: the snap is in the air
    std::int64_t dead_tick_ = 0;
    // Playing a pass: who has had his try at it since the last touch.
    std::vector<bool> tried_;
    bool tipped_ = false;
};

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

const Player& PlaySim::fill_slot(const Team& team, const Roster& roster, const SlotMap& map,
                                 const std::string& slot) const {
    const auto fill = map.find(slot);
    if (fill == map.end()) throw PlayError(team.id + "'s depth chart doesn't fill slot " + slot);
    std::string player_id = fill->second.player_id;
    if (fill->second.rank) {
        const DepthRank r = *fill->second.rank;
        const auto pos = team.depth_chart.positions.find(r.position);
        const std::size_t at = static_cast<std::size_t>(r.rank) - 1;
        if (pos == team.depth_chart.positions.end() || at >= pos->second.size()) {
            throw PlayError(team.id + "'s depth chart has no " +
                            std::string(to_string(r.position)) + std::to_string(r.rank) +
                            " for slot " + slot);
        }
        player_id = pos->second[at];
    }
    const Player* player = roster.find(player_id);
    if (!player) throw PlayError("player '" + player_id + "' isn't on the roster");
    return *player;
}

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
        const Player& player = fill_slot(team_, roster_, personnel->second, slot.name);

        SlotInfo info;
        info.slot = slot.name;
        info.formation_slot = &slot;
        if (const auto a = state_.play.assignments.find(slot.name);
            a != state_.play.assignments.end()) {
            info.assignment = &a->second;
        }
        info.aligned = aligned[k];
        info.scheme_pk = player.playbook_knowledge_for(state_.book.scheme);
        info.familiarity = slot.pos ? player.familiarity_at(*slot.pos) : 100.0;
        state_.slots.push_back(info);

        Agent a = make_agent(player, TeamSide::Home, slot.name, {}, state_.tuning().movement);
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
    state_.offense_count = static_cast<int>(f.slots.size());
    if (state_.center < 0) throw PlayError("formation " + f.id + " has no snapper");
    if (state_.qb < 0) throw PlayError("formation " + f.id + " has no QB (under center or gun)");
    if (state_.play.run) {
        state_.run_carrier = state_.index_of(state_.play.run->carrier);
        if (state_.run_carrier < 0) throw PlayError("no carrier slot " + state_.play.run->carrier);
    }

    if (defense_.play) setup_defense();

    state_.get_off.assign(n(), 0.0);
    state_.throw_reaction.assign(n(), std::numeric_limits<double>::infinity());
    state_.handoff_reaction.assign(n(), std::numeric_limits<double>::infinity());
    state_.open.assign(n(), 1.0);
    state_.behaviors.resize(n());
    job_index_.assign(n(), 0);
    busts_.assign(n(), std::nullopt);
    tried_.assign(n(), false);
    blocking_.setup();

    Ball& ball = world_.ball();
    ball.state = Ball::State::Spotted;
    ball.pos = {state_.frame.ball().x, state_.frame.ball().y, 0.15};
}

OffenseRead PlaySim::read_offense_set(bool actual) const {
    // The offense as the defense sees it: where its players are standing at
    // the snap, or before it, where they're lining up.
    std::vector<Vec2> pos;
    std::vector<bool> on_line;
    for (int i = 0; i < state_.offense_count; ++i) {
        const SlotInfo& slot = state_.slots[static_cast<std::size_t>(i)];
        pos.push_back(actual ? state_.play_pos(i) : slot.aligned);
        on_line.push_back(slot.formation_slot->on_line);
    }
    try {
        OffenseRead read = read_offense(pos, on_line, state_.center, state_.qb);
        read.strong = strength_from_call(read, defense_.book->strength_call, state_.defense_frame);
        return read;
    } catch (const GeometryError& e) {
        throw PlayError(std::string("reading the offense: ") + e.what());
    }
}

void PlaySim::setup_defense() {
    const Formation& f = *defense_.formation;
    const Team& team = *defense_.team;
    const auto package = team.depth_chart.defense_packages.find(f.package);
    if (package == team.depth_chart.defense_packages.end()) {
        throw PlayError("team " + team.id + " has no '" + f.package + "' package");
    }
    // The defense lines up on the offense's set.
    const OffenseRead read = read_offense_set(false);
    std::vector<Vec2> aligned;
    try {
        aligned = align_formation(f, state_.defense_frame, &read,
                                  {defense().shade, defense().ghost_te_split});
    } catch (const GeometryError& e) {
        throw PlayError("formation " + f.id + ": " + e.what());
    }

    const double count = static_cast<double>(f.slots.size());
    const double radius = tuning().huddle_spacing * count / (2.0 * std::numbers::pi);
    const Vec2 huddle{0.0, tuning().huddle_depth + 1.0};
    for (std::size_t k = 0; k < f.slots.size(); ++k) {
        const FormationSlot& slot = f.slots[k];
        const Player& player = fill_slot(team, *defense_.roster, package->second, slot.name);

        SlotInfo info;
        info.slot = slot.name;
        info.formation_slot = &slot;
        if (const auto a = defense_.play->assignments.find(slot.name);
            a != defense_.play->assignments.end()) {
            info.assignment = &a->second;
        }
        info.aligned = aligned[k];
        info.scheme_pk = player.playbook_knowledge_for(defense_.book->scheme);
        info.familiarity = slot.pos ? player.familiarity_at(*slot.pos) : 100.0;
        state_.slots.push_back(info);

        Agent a = make_agent(player, TeamSide::Away, slot.name, {}, state_.tuning().movement);
        if (opt_.huddle) {
            const double angle = 2.0 * std::numbers::pi * static_cast<double>(k) / count;
            const Vec2 spot = huddle + Vec2{std::cos(angle), std::sin(angle)} * radius;
            a.pos = state_.frame.to_field(spot);
            a.facing = (state_.frame.to_field(huddle) - a.pos).normalized();
        } else {
            a.pos = state_.frame.to_field(aligned[k]);
            a.facing = state_.downfield() * -1.0;
        }
        world_.add_agent(std::move(a));
    }
}

void PlaySim::write_header() {
    if (!out_) return;
    const Play& play = state_.play;
    TrackingInfo info;
    info.kind = "play";
    info.name = play.name + (opt_.flip ? " (flip)" : "");
    info.seed = opt_.seed;
    info.home = {team_.name, team_.abbr, team_.color_primary};
    if (defense_.team) {
        const Team& d = *defense_.team;
        // The same team on both sides (a scrimmage) wears its second color.
        info.away = {d.name, d.abbr, d.id == team_.id ? d.color_secondary : d.color_primary};
        info.name += " vs " + defense_.play->name;
    }
    info.details = {{"team", team_.id},
                    {"play", play.id},
                    {"defense", defense_.team ? defense_.team->id : ""},
                    {"defense_play", defense_.play ? defense_.play->id : ""},
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
        blocking_.draw(dbg);
        if (state_.gaps) {
            for (const Side side : {Side::Left, Side::Right}) {
                for (const char letter : {'A', 'B', 'C', 'D'}) {
                    const Vec2 at = state_.frame.to_field({state_.gaps->x(side, letter), 0.8});
                    dbg.push_back(shapes::text("gaps", at, std::string(1, letter), "#9fb3c8"));
                }
            }
        }
        // Each receiver's openness while the QB still has the ball.
        if (state_.has_defense() && state_.play.pass && state_.has_ball(state_.qb)) {
            for (int i = 0; i < state_.offense_count; ++i) {
                if (!dynamic_cast<const RouteRunner*>(state_.behavior(i))) continue;
                const double o = state_.openness(i);
                char text[8];
                std::snprintf(text, sizeof text, "%.2f", o);
                const char* color = o >= 0.6 ? "#4caf50" : o >= 0.3 ? "#ffc107" : "#f44336";
                dbg.push_back(shapes::tag("openness", i, text, color));
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
    const Vec2 defense_huddle = state_.frame.to_field({0.0, tuning().huddle_depth + 1.0});
    for (int i = 0; i < state_.count(); ++i) {
        const Agent& a = state_.agent(i);
        Intent& in = intents[static_cast<std::size_t>(i)];
        if (world_.time() < tuning().huddle_break) {
            if (!state_.is_offense(i)) {
                in.face = (defense_huddle - a.pos).normalized();
                continue;
            }
            in.face = (huddle - a.pos).normalized();
            continue;
        }
        const Vec2 spot = state_.frame.to_field(state_.slots[static_cast<std::size_t>(i)].aligned);
        in.velocity = steer::arrive(a, spot, std::min(tuning().jog_speed, a.move.top_speed),
                                    tuning().arrive_decel);
        if (a.vel.length() < 1.0) in.face = state_.downfield_for(i);
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
    // then behaviors (defense first, then offense: their draws include
    // route noise and the option-route reads of the coverage).
    for (int i = 0; i < state_.count(); ++i) {
        const Attributes& r = state_.agent(i).ratings;
        // The offense knows the count; the defense reacts to the ball moving.
        const double delay =
            state_.is_offense(i)
                ? t.get_off_delay(t.get_off_skill(r)) + rng.normal(0.0, t.get_off_noise_sd)
                : t.reaction_delay(t.reaction_skill(r)) + rng.normal(0.0, t.reaction_noise_sd);
        state_.get_off[static_cast<std::size_t>(i)] = std::max(0.0, delay);
    }

    // The gaps, from where the linemen actually are.
    std::vector<double> line_x;
    for (int i = 0; i < state_.offense_count; ++i) {
        if (state_.slots[static_cast<std::size_t>(i)].formation_slot->on_line) {
            line_x.push_back(state_.play_pos(i).x);
        }
    }
    try {
        state_.gaps = find_gaps(state_.play_pos(state_.center).x, line_x);
    } catch (const GeometryError&) {
        state_.gaps.reset();  // only a problem if something anchors to a gap (reported then)
    }

    // The defense reads the offense as it stands at the snap.
    if (state_.has_defense()) state_.offense = read_offense_set(true);

    event("snap");
    record_history();
    for (int i = 0; i < state_.count(); ++i) {
        if (state_.is_offense(i)) {
            roll_bust(i);
        } else {
            roll_coverage_bust(i);
        }
    }
    for (int i = state_.offense_count; i < state_.count(); ++i) {
        state_.behaviors[static_cast<std::size_t>(i)] = defense_behavior(i);
    }
    for (int i = 0; i < state_.offense_count; ++i) {
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

void PlaySim::record_history() {
    std::vector<PlayState::Snapshot> snap;
    snap.reserve(n());
    for (const Agent& a : world_.agents()) snap.push_back({a.pos, a.vel});
    state_.history.push_back(std::move(snap));
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
        // No job: stay put and block whoever comes.
        RunBlocker::Setup setup;
        setup.fire_spot = state_.agent(i).pos;
        return std::make_unique<RunBlocker>(std::move(setup));
    }
    return job_behavior(i, slot.assignment->sequence.front());
}

std::unique_ptr<Behavior> PlaySim::job_behavior(int i, const Job& job) {
    if (job.kind == JobKind::Route) return route_behavior(i, job);
    if (job.kind != JobKind::Block) {
        throw PlayError(state_.slot_name(i) + ": '" + std::string(to_string(job.kind)) +
                        "' jobs aren't supported on offense");
    }

    if (job.action == "protect" || job.action == "pass_set" || job.action == "check_release") {
        return pass_blocker(i, job);
    }
    return run_blocker(i, job);
}

double PlaySim::playside() const {
    if (!state_.play.run) return 1.0;
    const Side dir = state_.frame.flipped() ? mirrored(state_.play.run->direction)
                                            : state_.play.run->direction;
    return dir == Side::Left ? -1.0 : 1.0;
}

std::unique_ptr<Behavior> PlaySim::pass_blocker(int i, const Job& job) {
    // (movement-and-interaction.md#pass-protection)
    const BlockingTuning& t = blocking();
    const SlotInfo& slot = state_.slots[static_cast<std::size_t>(i)];
    const Attributes& r = state_.agent(i).ratings;
    const double pk = effective_playbook_knowledge(slot.scheme_pk, slot.familiarity, tuning());
    const Vec2 at = state_.play_pos(i);

    PassBlocker::Setup s;
    s.start_y = at.y;
    s.max_depth = t.set_depth_min + r[Attr::Footwork] / 100.0 * t.set_depth_range;
    if (job.action == "check_release") {
        s.back = true;
        s.release_at = tuning().check_release_seconds;
        s.home = state_.frame.to_field(at + Vec2{0.0, -0.5});
        s.lag = t.pickup_lag((1.0 - t.pickup_pk_weight) * t.pickup_skill(r) + t.pickup_pk_weight * pk);
        return std::make_unique<PassBlocker>(s);
    }
    s.home = state_.frame.to_field(at + Vec2{0.0, -tuning().pass_set_depth});
    s.lag = t.pro_lag((1.0 - t.pro_pk_weight) * t.pro_skill(r) + t.pro_pk_weight * pk);

    // His area: with a slide, the snapper and the slide side own the gap
    // toward the slide; the man side owns their own spot and inside gap.
    std::vector<double> xs;  // every protector's x
    for (int j = 0; j < state_.offense_count; ++j) {
        const Assignment* a = state_.slots[static_cast<std::size_t>(j)].assignment;
        if (!a || a->sequence.empty()) continue;
        const Job& first = a->sequence.front();
        if (first.kind == JobKind::Block && (first.action == "protect" || first.action == "pass_set")) {
            xs.push_back(state_.play_pos(j).x);
        }
    }
    const double cx = state_.play_pos(state_.center).x;
    const double side = std::abs(at.x - cx) < 0.3 ? 0.0 : (at.x > cx ? 1.0 : -1.0);
    double slide = 0.0;
    if (state_.play.pass) {
        Side dir = state_.play.pass->protection_direction;
        if (state_.frame.flipped()) dir = mirrored(dir);
        slide = dir == Side::Left ? -1.0 : dir == Side::Right ? 1.0 : 0.0;
    }
    s.area_x = at.x;
    s.area_half = t.area_half;
    if (slide != 0.0 && (side == 0.0 || side == slide)) {
        double gap = 0.7;
        for (const double x : xs) {
            const double d = (x - at.x) * slide;
            if (d > 0.3) gap = std::min(gap, d / 2.0);
        }
        s.area_x = at.x + slide * gap;
    } else {
        s.area_x = at.x - side * 0.35;
    }
    // The end man on each side also owns the edge.
    bool outermost = side != 0.0;
    for (const double x : xs) {
        if ((x - at.x) * side > 0.3) outermost = false;
    }
    if (outermost) {
        s.area_x += side * 0.5;
        s.area_half += 0.5;
    }
    return std::make_unique<PassBlocker>(s);
}

std::vector<int> PlaySim::front(bool down_linemen) const {
    // The defense as the blocking rules see it at the snap: down linemen
    // (on the line) or linebackers (off it, in the box).
    const BlockingTuning& t = blocking();
    const double cx = state_.play_pos(state_.center).x;
    std::vector<int> out;
    for (int d = state_.offense_count; d < state_.count(); ++d) {
        const Vec2 p = state_.play_pos(d);
        if (std::abs(p.x - cx) > t.box_width) continue;
        const bool down = p.y <= t.down_lineman_depth;
        if (down_linemen ? down : (!down && p.y <= t.lb_depth_max)) out.push_back(d);
    }
    return out;
}

bool PlaySim::block_condition(int i, const std::string& condition, double side) const {
    const double x = state_.play_pos(i).x;
    for (const int d : front(true)) {
        const double dx = state_.play_pos(d).x - x;
        const double ps = dx * side;  // + toward the play side
        if (condition == "covered" && std::abs(dx) <= 0.9) return true;
        if (condition == "covered_playside" && ps >= -0.5 && ps <= 1.0) return true;
        if (condition == "uncovered" && std::abs(dx) <= 0.9) return false;
        if (condition == "has_inside_threat" && ps >= -1.6 && ps < -0.5) return true;
    }
    return condition == "uncovered";
}

int PlaySim::block_target_for(int i, const std::string& target, double side) const {
    const Vec2 me = state_.play_pos(i);
    const auto nearest = [&](const std::vector<int>& from, auto&& ok) {
        int best = -1;
        double best_d = 1e9;
        for (const int d : from) {
            const double dx = state_.play_pos(d).x - me.x;
            if (!ok(dx * side, dx)) continue;
            // Ties go to the play side.
            const double score = std::abs(dx) - 0.1 * dx * side;
            if (score < best_d) {
                best = d;
                best_d = score;
            }
        }
        return best;
    };
    const std::vector<int> down = front(true);
    if (target == "man_on" || target == "man_on_or_inside") {
        return nearest(down, [](double, double dx) { return std::abs(dx) <= 1.2; });
    }
    if (target == "playside_gap") {
        const int d = nearest(down, [](double ps, double) { return ps > 0.3 && ps <= 1.8; });
        if (d >= 0) return d;
        // Else the man over his playside neighbor (the combo).
        return nearest(down, [](double ps, double) { return ps > 0.3 && ps <= 2.8; });
    }
    if (target == "backside_gap") {
        return nearest(down, [](double ps, double) { return ps >= -2.0 && ps <= -0.3; });
    }
    if (target == "first_lb_playside") {
        // The nearest linebacker on his play side; failing that, the nearest.
        const std::vector<int> lbs = front(false);
        int best = -1;
        for (const int d : lbs) {
            const double ps = (state_.play_pos(d).x - me.x) * side;
            if (ps >= -0.3 && (best < 0 || ps < (state_.play_pos(best).x - me.x) * side)) best = d;
        }
        return best >= 0 ? best : nearest(lbs, [](double, double) { return true; });
    }
    if (target == "kick_out_man") {
        // The first defender in the box outside the end of the offense's
        // line on the play side (outside the down blocks).
        double end = state_.play_pos(state_.center).x;
        for (int j = 0; j < state_.offense_count; ++j) {
            const SlotInfo& sl = state_.slots[static_cast<std::size_t>(j)];
            const double x = state_.play_pos(j).x;
            if (sl.formation_slot->on_line && std::abs(x - end) < 6.0 && x * side > end * side &&
                std::abs(x - state_.play_pos(state_.center).x) <= 5.0) {
                end = x;
            }
        }
        int best = -1;
        for (const int d : front(true)) {
            const double ps = (state_.play_pos(d).x - end) * side;
            if (ps >= 0.0 && (best < 0 || ps < (state_.play_pos(best).x - end) * side)) best = d;
        }
        for (const int d : front(false)) {
            const double ps = (state_.play_pos(d).x - end) * side;
            if (ps >= 0.0 && (best < 0 || ps < (state_.play_pos(best).x - end) * side)) best = d;
        }
        return best;
    }
    if (target == "emol_playside") {
        // The end man on the line on the play side; if he isn't outside this
        // blocker (a TE outside the end), the first defender in the box
        // outside him.
        int best = -1;
        for (const int d : down) {
            if (best < 0 || state_.play_pos(d).x * side > state_.play_pos(best).x * side) best = d;
        }
        if (best >= 0 && (state_.play_pos(best).x - me.x) * side >= 0.5) return best;
        int outside = -1;
        for (const int d : front(false)) {
            const double ps = (state_.play_pos(d).x - me.x) * side;
            if (ps >= 0.0 && (outside < 0 || ps < (state_.play_pos(outside).x - me.x) * side)) outside = d;
        }
        return outside >= 0 ? outside : best;
    }
    if (target == "man_over") {
        int best = -1;
        double best_d = 10.0;
        for (int d = state_.offense_count; d < state_.count(); ++d) {
            const Vec2 p = state_.play_pos(d);
            if (p.y <= 2.0) continue;
            const double dist = (p - me).length();
            if (dist < best_d) {
                best = d;
                best_d = dist;
            }
        }
        return best;
    }
    return -1;
}

BlockRule PlaySim::scheme_rule(int i, double side) const {
    const RunSpec& run = *state_.play.run;
    const auto scheme = state_.data.blocking.run_schemes.find(run.scheme);
    if (scheme == state_.data.blocking.run_schemes.end()) {
        throw PlayError("unknown run scheme '" + run.scheme + "'");
    }
    const SlotInfo& slot = state_.slots[static_cast<std::size_t>(i)];
    auto rules = scheme->second.rules.find(slot.slot);
    const std::optional<Position> pos = slot.formation_slot->pos;
    const bool lineman = pos && (*pos == Position::OT || *pos == Position::OG || *pos == Position::C);
    if (rules == scheme->second.rules.end() && lineman) rules = scheme->second.rules.find("OL");
    if (rules == scheme->second.rules.end()) return {};
    for (const BlockRule& rule : rules->second) {
        if (rule.condition.empty() || block_condition(i, rule.condition, side)) return rule;
    }
    return {};
}

std::unique_ptr<Behavior> PlaySim::run_blocker(int i, const Job& job) {
    // (movement-and-interaction.md#run-blocking)
    const BlockingTuning& t = blocking();
    const PlayTuning& pt = tuning();
    const SlotInfo& slot = state_.slots[static_cast<std::size_t>(i)];
    const Attributes& r = state_.agent(i).ratings;
    const double pk = effective_playbook_knowledge(slot.scheme_pk, slot.familiarity, pt);
    const Vec2 at = state_.play_pos(i);
    const auto field = [&](Vec2 p) { return state_.frame.to_field(p); };
    const double side = playside();

    using Action = RunBlocker::Action;
    RunBlocker::Setup setup;
    setup.playside = side;
    setup.lag = t.pro_lag((1.0 - t.pro_pk_weight) * t.pro_skill(r) + t.pro_pk_weight * pk);
    // With nobody to block: fire out toward the play side (stalk: downfield).
    setup.fire_spot = job.action == "stalk" ? field(at + Vec2{0.0, pt.stalk_depth})
                                            : field(at + Vec2{side * pt.run_lean, pt.run_fire_out});
    if (!state_.has_defense()) return std::make_unique<RunBlocker>(std::move(setup));

    BlockRule rule;
    rule.action = job.action;
    rule.target = job.target;
    if (job.action == "scheme") {
        if (!state_.play.run) throw PlayError(slot.slot + ": 'scheme' blocks need a run play");
        rule = scheme_rule(i, side);
    }
    const std::string& a = rule.action;
    setup.action = a == "drive"                     ? Action::Drive
                   : a == "reach" || a == "cutoff"  ? Action::Reach
                   : a == "down"                    ? Action::Down
                   : a == "combo_climb"             ? Action::Combo
                   : a == "pull_kick"               ? Action::PullKick
                   : a == "pull_lead"               ? Action::PullLead
                   : a == "stalk"                   ? Action::Stalk
                                                    : Action::Fire;
    if (setup.action == Action::Fire) return std::make_unique<RunBlocker>(std::move(setup));

    std::string target = rule.target;
    if (target.empty()) {
        target = setup.action == Action::PullKick   ? "kick_out_man"
                 : setup.action == Action::PullLead ? "first_lb_playside"
                 : setup.action == Action::Stalk    ? "man_over"
                                                    : "man_on";
    }
    setup.target = block_target_for(i, target, side);
    if (setup.action == Action::Combo) setup.climb_to = block_target_for(i, rule.climb_to, side);

    // A pull: step back, run behind the line to the play side, then up
    // (inside-out on the man he kicks out, or through the hole he leads).
    if (setup.action == Action::PullKick || setup.action == Action::PullLead) {
        const double y = -t.pull_depth;
        double hole = at.x + side * 3.0;
        if (state_.play.run) {
            for (const PathPoint& p : state_.play.run->path) {
                if (p.type == "aim") hole = resolve_path_point(p).x;
            }
        }
        if (setup.action == Action::PullKick && setup.target >= 0) {
            setup.path.push_back(field({state_.play_pos(setup.target).x - side * 1.5, y}));
        } else {
            setup.path.push_back(field({hole - side * 0.5, y}));
            setup.path.push_back(field({hole, 0.5}));
        }
    }
    return std::make_unique<RunBlocker>(std::move(setup));
}

std::unique_ptr<Behavior> PlaySim::route_behavior(int i, const Job& job) {
    const auto idx = static_cast<std::size_t>(i);
    const PlayTuning& t = tuning();
    const RouteLibrary& routes = state_.data.routes;

    const RouteRef& ref = option_branch(i, job);
    const Route* route = &routes.at(ref.route);
    std::optional<double> depth = ref.depth;
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
                // (An option branch may have no break to move.)
                if (route->break_depth()) {
                    depth = depth.value_or(*route->break_depth()) + b.depth_delta;
                }
                break;
            case Bust::Kind::LateRelease:
                delay = b.delay;
                break;
            case Bust::Kind::WrongMan:
            case Bust::Kind::WrongZone:
            case Bust::Kind::Late:
                break;  // coverage busts: applied in defense_job
        }
        busts_[idx].reset();
    }

    const Side side = state_.frame.flipped() ? mirrored(ref.side) : ref.side;
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
        const PassingTuning& t = passing();
        PassingQb::Setup setup;
        setup.drop = to_field(drop.points);
        setup.set_seconds = drop.set_seconds;
        setup.reads = std::move(reads);
        setup.look_seconds = t.look_seconds(t.look_skill(r));
        setup.delivery_seconds = t.delivery_seconds(r[Attr::Delivery]);
        setup.perceive_sd = t.perceive_sd(t.perceive_skill(r));
        setup.threshold = t.throw_threshold(r[Attr::Decisions]);
        setup.peripheral_rate = t.peripheral_rate(r[Attr::Vision]);
        const BlockingTuning& bt = blocking();
        if (!pass.hot.empty() && opt_.target.empty()) setup.hot = state_.index_of(pass.hot);
        setup.pocket_lag = bt.pocket_lag(bt.pocket_skill(r));
        setup.blind_lag = bt.blind_lag(r[Attr::PocketPresence]);
        setup.escape_chance = bt.escape_chance(r[Attr::PocketPresence]);
        setup.hold = opt_.hold_ball;
        setup.snapper = state_.play_pos(state_.center);
        // His own random stream, seeded from the match RNG.
        return std::make_unique<PassingQb>(std::move(setup), world_.rng().next_u64());
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

const RouteRef& PlaySim::option_branch(int i, const Job& job) {
    // An option route reads man or zone at the snap
    // (movement-and-interaction.md#option-and-settle-routes).
    if (job.options.empty() || !state_.has_defense()) return job.route;
    bool man = false;
    for (int d = state_.offense_count; d < state_.count(); ++d) {
        man = man || state_.behavior(d)->man_target() == i;
    }
    const DefenseTuning& t = defense();
    const SlotInfo& slot = state_.slots[static_cast<std::size_t>(i)];
    const double pk = effective_playbook_knowledge(slot.scheme_pk, slot.familiarity, tuning());
    const double skill = (1.0 - t.option_pk_weight) * t.option_skill(state_.agent(i).ratings) +
                         t.option_pk_weight * pk;
    const bool right = world_.rng().chance(t.option_correct(skill));
    const bool read_man = right ? man : !man;
    event("option_read", {{"player", slot.slot},
                          {"read", read_man ? "man" : "zone"},
                          {"correct", right}});
    for (const RouteOption& o : job.options) {
        if ((o.when == "vs_man") == read_man) return o.route;
    }
    return job.route;
}

// ---------------------------------------------------------------------------
// Defense
// ---------------------------------------------------------------------------

Side PlaySim::own_side(int i) const {
    const double center = state_.offense ? state_.offense->center_x : 0.0;
    return state_.play_pos(i).x < center ? Side::Left : Side::Right;
}

double PlaySim::gap_x(Side side, const std::string& letter) const {
    if (!state_.gaps) throw PlayError("the offense has no line to fit gaps against");
    return state_.gaps->x(side, letter.at(0));
}

ZoneArea PlaySim::zone_area(const std::string& zone_id) const {
    const auto it = state_.data.zones.find(zone_id);
    if (it == state_.data.zones.end()) throw PlayError("unknown zone '" + zone_id + "'");
    const Zone& z = it->second;
    AnchorContext ctx;
    ctx.frame = &state_.defense_frame;
    ctx.offense = &*state_.offense;
    ctx.alignment = {defense().shade, defense().ghost_te_split};
    ZoneArea area;
    area.id = z.id;
    try {
        area.drop = {resolve_x(z.drop_x, ctx), resolve_y(z.drop_y, ctx)};
    } catch (const GeometryError& e) {
        throw PlayError("zone " + z.id + ": " + e.what());
    }
    area.width = z.width;
    area.depth_min = z.depth_min;
    area.depth_max = z.depth_max;
    area.deep = z.depth_min >= defense().deep_min_depth;
    return area;
}

void PlaySim::roll_coverage_bust(int i) {
    const SlotInfo& slot = state_.slots[static_cast<std::size_t>(i)];
    if (!slot.assignment || !slot.assignment->pass) return;
    const Job& job = *slot.assignment->pass;
    if (job.kind != JobKind::Man && job.kind != JobKind::Zone) return;

    Rng& rng = world_.rng();
    const double pk = effective_playbook_knowledge(slot.scheme_pk, slot.familiarity, tuning());
    if (!rng.chance(bust_chance(pk, tuning()))) return;

    Bust bust;
    std::string text;
    if (job.kind == JobKind::Man) {
        bust.kind = rng.chance(0.5) ? Bust::Kind::WrongMan : Bust::Kind::Late;
        text = bust.kind == Bust::Kind::WrongMan ? "covered the wrong man" : "reacted late";
    } else {
        bust.kind = rng.chance(0.5) ? Bust::Kind::WrongZone : Bust::Kind::Late;
        if (bust.kind == Bust::Kind::WrongZone) {
            // The neighboring zone at the same level.
            const ZoneArea mine = zone_area(job.ref);
            double best = 1e9;
            for (const auto& [id, z] : state_.data.zones) {
                if (id == job.ref) continue;
                const ZoneArea other = zone_area(id);
                const double gap = std::abs(other.drop.x - mine.drop.x);
                if (other.deep == mine.deep && gap > 1.0 && gap < best) {
                    best = gap;
                    bust.zone = id;
                }
            }
            if (bust.zone.empty()) bust.kind = Bust::Kind::Late;
            text = "dropped to " + bust.zone + " instead of " + job.ref;
        }
        if (bust.kind == Bust::Kind::Late) {
            bust.delay = rng.uniform(defense().late_drop_min, defense().late_drop_max);
            text = "dropped " + fixed1(bust.delay) + " s late";
        }
    }
    result_.busts.push_back(slot.slot + ": " + text);
    event("bust", {{"player", slot.slot}, {"detail", text}});
    busts_[static_cast<std::size_t>(i)] = bust;
}

std::unique_ptr<Behavior> PlaySim::defense_behavior(int i) {
    const SlotInfo& slot = state_.slots[static_cast<std::size_t>(i)];
    if (!slot.assignment || !slot.assignment->run || !slot.assignment->pass) {
        throw PlayError("defensive slot " + slot.slot + " has no run and pass jobs");
    }
    const DefenseTuning& t = defense();
    const PlayTuning& pt = tuning();
    const Attributes& r = state_.agent(i).ratings;
    const double pk = effective_playbook_knowledge(slot.scheme_pk, slot.familiarity, pt);
    // How fast he reads run or pass.
    const double skill = (1.0 - t.recognition_pk_weight) * t.recognition_skill(r) +
                         t.recognition_pk_weight * pk;
    const double delay = std::max(
        0.05, pt.reaction_delay(skill) + world_.rng().normal(0.0, pt.reaction_noise_sd));

    std::unique_ptr<Behavior> pass = defense_job(i, *slot.assignment->pass, nullptr);
    std::unique_ptr<Behavior> run = defense_job(i, *slot.assignment->run, &*slot.assignment->pass);
    return std::make_unique<Defender>(std::move(run), std::move(pass), delay,
                                      t.recognition_rate(skill), t.pass_prior);
}

std::unique_ptr<Behavior> PlaySim::zone_behavior(int i, const std::string& zone_id, double late) {
    const DefenseTuning& t = defense();
    const double skill = t.zone_skill(state_.agent(i).ratings);
    return std::make_unique<ZoneCoverage>(zone_area(zone_id), t.zone_lag(skill), skill, late);
}

std::unique_ptr<Behavior> PlaySim::defense_job(int i, const Job& job, const Job* pass_job) {
    const auto idx = static_cast<std::size_t>(i);
    const DefenseTuning& t = defense();
    const OffenseRead& o = *state_.offense;
    const Vec2 at = state_.play_pos(i);
    const auto field = [&](Vec2 p) { return state_.frame.to_field(p); };
    const Side mine = own_side(i);
    const auto side_of = [&](Side s) { return s == Side::None ? mine : o.resolve(s); };
    // Busts change only the pass job.
    const std::optional<Bust> bust = pass_job ? std::nullopt : busts_[idx];

    switch (job.kind) {
        // --- pass jobs
        case JobKind::Zone: {
            if (bust && bust->kind == Bust::Kind::WrongZone) return zone_behavior(i, bust->zone, 0.0);
            return zone_behavior(i, job.ref, bust ? bust->delay : 0.0);
        }
        case JobKind::Man: {
            std::optional<int> target = o.player(job.target);
            if (target && bust && bust->kind == Bust::Kind::WrongMan) {
                // The next man inside on the same side (or the back).
                std::optional<int> other;
                for (const auto& side : o.receivers) {
                    for (std::size_t k = 0; k + 1 < side.size(); ++k) {
                        if (side[k] == *target) other = side[k + 1];
                    }
                }
                if (!other && !o.backs.empty() && o.backs.front() != *target) other = o.backs.front();
                if (other) target = other;
            }
            // Nobody there to cover: he plays the hole.
            if (!target) return zone_behavior(i, "hole", 0.0);
            const double skill = t.man_skill(state_.agent(i).ratings);
            double lag = t.man_lag(skill);
            if (bust && bust->kind == Bust::Kind::Late) lag += t.late_lag;
            const double leverage = job.ref == "outside" ? 1.0 : -1.0;
            const double cushion = std::max(t.min_cushion, at.y - state_.play_pos(*target).y);
            return std::make_unique<ManCoverage>(*target, leverage, cushion, lag);
        }
        case JobKind::Rush:
        case JobKind::Blitz:
        case JobKind::Stunt: {
            // Through his lane to the QB (stunts rush their lane for now).
            const BlockingTuning& bt = blocking();
            const Side side = side_of(job.side);
            const std::string lane = job.gap.empty() ? job.ref : job.gap;
            Rusher::Setup rs;
            if (lane == "contain" || lane.empty()) {
                rs.lane_x = o.end_of_line(side) + outward(side) * bt.contain_outside;
                rs.contain = outward(side);
            } else {
                rs.lane_x = gap_x(side, lane);
            }
            const Attributes& r = state_.agent(i).ratings;
            rs.power = bt.power_move(r);
            rs.finesse = bt.finesse_move(r);
            return std::make_unique<Rusher>(rs, world_.rng().next_u64());
        }
        case JobKind::Spy:
            return std::make_unique<HoldSpot>("spy", field({o.center_x, 5.0}), 0.5);

        // --- run jobs (stand-ins until phase 7)
        case JobKind::Gap: {
            // A down lineman attacks his gap (into the blockers); others fill
            // it off the ball (a stand-in until phase 7).
            const Side side = side_of(job.side);
            const double depth =
                at.y <= blocking().down_lineman_depth ? blocking().dl_gap_depth : t.gap_depth;
            return std::make_unique<HoldSpot>("gap", field({gap_x(side, job.gap), depth}), 1.0);
        }
        case JobKind::Support: {
            // "deep_third": keep playing his deep zone; "alley_strong",
            // "force_weak", "deep_fill": go to that spot.
            const std::string& role = job.ref;
            if (role.starts_with("deep_third") && pass_job && pass_job->kind == JobKind::Zone) {
                return zone_behavior(i, pass_job->ref, 0.0);
            }
            if (role == "deep_fill") {
                return std::make_unique<HoldSpot>("support", field({o.center_x, t.deep_fill}), 1.0);
            }
            Side side = mine;
            if (role.ends_with("_strong")) side = o.resolve(Side::Strong);
            if (role.ends_with("_weak")) side = o.resolve(Side::Weak);
            const Vec2 spot = role.starts_with("force") ? t.force : t.alley;
            return std::make_unique<HoldSpot>(
                "support", field({o.end_of_line(side) + outward(side) * spot.x, spot.y}), 1.0);
        }
        case JobKind::Contain: {
            const Side side = side_of(job.side);
            return std::make_unique<HoldSpot>(
                "contain", field({o.end_of_line(side) + outward(side) * t.contain_outside, 2.0}),
                1.0);
        }
        case JobKind::Pursue:
            return std::make_unique<Pursuer>();
        default:
            break;
    }
    throw PlayError(state_.slot_name(i) + ": '" + std::string(to_string(job.kind)) +
                    "' isn't a defensive job");
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
    update_openness();
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
            state_.handoff_tick = world_.tick();
            // Everyone notices the handoff after his own reaction time.
            const PlayTuning& pt = tuning();
            for (int i = 0; i < state_.count(); ++i) {
                const double skill = pt.reaction_skill(state_.agent(i).ratings);
                const double delay =
                    pt.reaction_delay(skill) + world_.rng().normal(0.0, pt.reaction_noise_sd);
                state_.handoff_reaction[static_cast<std::size_t>(i)] = std::max(0.05, delay);
            }
            event("handoff", {{"from", state_.slot_name(actions.handoff->from)},
                              {"to", state_.slot_name(actions.handoff->to)}});
        }
    }
    if (actions.throw_ball) {
        throw_ball(actions.throw_ball->from, actions.throw_ball->target, actions.throw_ball->away);
    }
    if (!actions.rush_moves.empty()) blocking_.resolve_moves(actions.rush_moves, world_.rng());
}

Projection PlaySim::project_catch(int qb_index, int target, int rounds, double delay) const {
    const Agent& qb = state_.agent(qb_index);
    const PassingTuning& t = passing();
    Projection p;
    const Vec2 hand = qb.pos + qb.facing * 0.3;
    p.release = {hand.x, hand.y, qb.body.height * t.release_height_factor};
    const double top_speed = t.throw_speed(qb.ratings[Attr::ThrowPower]);
    const auto velocity_to = [&](Vec3 to) {
        const double distance = (to.ground() - p.release.ground()).length();
        const double speed = top_speed * std::clamp(t.throw_effort(distance), 0.1, 1.0);
        return launch_velocity(p.release, to, speed, t.gravity)
            .value_or(max_range_velocity(p.release, to, speed));
    };

    // The projected catch point: where the receiver will be when the ball
    // gets there. The flight time depends on the distance and the distance
    // on the flight time, so iterate; it settles within a few rounds.
    const Behavior* receiver = state_.behavior(target);
    const Agent& rec = state_.agent(target);
    Vec2 aim = rec.pos;
    for (int round = 0; round < rounds; ++round) {
        const Vec3 to{aim.x, aim.y, t.target_height};
        const Vec3 v = velocity_to(to);
        const double flight =
            (to.ground() - p.release.ground()).length() / std::max(v.ground().length(), 1e-6);
        aim = receiver->predict(state_, target, delay + flight)
                  .value_or(rec.pos + rec.vel * (delay + flight));
    }
    // Never aim closer to a sideline than routes are allowed to go.
    Vec2 pp = state_.frame.to_play(aim);
    const double margin = tuning().sideline_margin;
    pp.x = std::clamp(pp.x, state_.frame.sideline_x(Side::Left) + margin,
                      state_.frame.sideline_x(Side::Right) - margin);
    p.aim = state_.frame.to_field(pp);
    const Vec3 to{p.aim.x, p.aim.y, t.target_height};
    p.velocity = velocity_to(to);
    p.flight = (to.ground() - p.release.ground()).length() / std::max(p.velocity.ground().length(), 1e-6);
    return p;
}

void PlaySim::update_openness() {
    std::fill(state_.open.begin(), state_.open.end(), 1.0);
    if (!state_.has_defense() || !state_.play.pass || !state_.has_ball(state_.qb)) return;
    // Judged for a ball that leaves after his delivery: the defense keeps
    // moving while he winds up.
    const double delivery = passing().delivery_seconds(state_.agent(state_.qb).ratings[Attr::Delivery]);
    for (int i = 0; i < state_.offense_count; ++i) {
        if (!dynamic_cast<const RouteRunner*>(state_.behavior(i))) continue;
        const Projection p = project_catch(state_.qb, i, passing().projection_rounds, delivery);
        state_.open[static_cast<std::size_t>(i)] =
            openness_against_defense(state_, i, p.release, p.velocity, p.aim, p.flight, delivery);
    }
}

void PlaySim::throw_ball(int qb_index, int target, bool away) {
    Ball& ball = world_.ball();
    if (!state_.has_ball(qb_index)) return;
    const Agent& qb = state_.agent(qb_index);
    const PassingTuning& t = passing();
    Rng& rng = world_.rng();

    Projection p = project_catch(qb_index, target, 8);
    if (away) {
        // Out of bounds past the receiver, with plenty of room.
        Vec2 pp = state_.frame.to_play(state_.agent(target).pos);
        const Side side = pp.x < 0.0 ? Side::Left : Side::Right;
        pp.x = state_.frame.sideline_x(side) + outward(side) * 3.0;
        pp.y += t.throw_away_past;
        p.aim = state_.frame.to_field(pp);
    }
    const Vec3 aim{p.aim.x, p.aim.y, t.target_height};
    const double distance = (aim.ground() - p.release.ground()).length();

    // Placement error: accuracy by air distance, worse on longer throws and
    // on the move.
    const Attr accuracy = distance <= t.short_max    ? Attr::ShortAccuracy
                          : distance <= t.medium_max ? Attr::MediumAccuracy
                                                     : Attr::DeepAccuracy;
    double sd = t.accuracy_sd(qb.ratings[accuracy]) * t.distance_scale(distance);
    if (qb.vel.length() > t.on_the_run_speed)
        sd *= t.on_the_run_factor(qb.ratings[Attr::ThrowOnTheRun]);
    // Hurried: worse with pressure, less so with Composure.
    const double pressure = state_.behavior(qb_index)->pressure();
    sd *= 1.0 + pressure * blocking().composure_penalty(qb.ratings[Attr::Composure]);
    result_.pressure_at_throw = pressure;
    const double ex = rng.normal(0.0, sd);
    const double ey = rng.normal(0.0, sd);
    const double ez = rng.normal(0.0, sd * t.height_sd_factor);
    const Vec3 actual{aim.x + ex, aim.y + ey, std::max(0.3, aim.z + ez)};

    const double top_speed = t.throw_speed(qb.ratings[Attr::ThrowPower]);
    const double speed = top_speed * std::clamp(t.throw_effort(distance), 0.1, 1.0);
    ball.state = Ball::State::InFlight;
    ball.carrier = -1;
    ball.pos = p.release;
    ball.vel = launch_velocity(p.release, actual, speed, t.gravity)
                   .value_or(max_range_velocity(p.release, actual, speed));

    PassInfo info;
    info.thrower = qb_index;
    info.target = target;
    info.tick = world_.tick();
    info.release = p.release;
    info.aim = aim;
    info.away = away;
    state_.pass = info;
    std::fill(tried_.begin(), tried_.end(), false);

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
    result_.thrown_away = away;
    result_.openness = state_.openness(target);
    if (away) {
        event("throw_away", {{"from", state_.slot_name(qb_index)}});
        return;
    }
    event("throw", {{"from", state_.slot_name(qb_index)},
                    {"to", result_.target},
                    {"air_yards", result_.air_yards},
                    {"distance", distance},
                    {"speed", ball.vel.length()},
                    {"error", std::hypot(ex, ey)},
                    {"openness", state_.openness(target)}});
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

    if (state_.pass && state_.phase == PlayPhase::Live && !state_.pass->away) {
        play_the_ball(before);
        if (ball.state != Ball::State::InFlight) return;
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

void PlaySim::play_the_ball(Vec3 before) {
    // Who can play the ball this tick: the target, any defender who has seen
    // the throw, and after a tip anyone who has seen it. Each plays it on
    // the tick it passes closest to him, once per touch
    // (movement-and-interaction.md#catches-against-defenders).
    Ball& ball = world_.ball();
    const PassingTuning& t = passing();
    Rng& rng = world_.rng();
    struct Try {
        int player;
        Closest c;
        bool diving;
    };
    std::vector<Try> tries;
    for (int i = 0; i < state_.count(); ++i) {
        if (tried_[static_cast<std::size_t>(i)] || i == state_.pass->thrower) continue;
        const bool can = state_.is_offense(i)
                             ? i == state_.pass->target || (tipped_ && state_.sees_throw(i))
                             : state_.sees_throw(i);
        if (!can) continue;
        const Agent& a = state_.agent(i);
        const Closest c = closest_approach(before, ball.pos, a.pos);
        const CatchWindow w = catch_window(a, t);
        if (c.u < 1.0 && c.dist <= w.dive_reach && c.at.z >= w.low && c.at.z <= w.high) {
            tries.push_back({i, c, c.dist > w.reach});
        }
    }
    if (tries.empty()) return;
    // In the order the ball reaches them (agent order breaks ties).
    std::stable_sort(tries.begin(), tries.end(),
                     [](const Try& a, const Try& b) { return a.c.u < b.c.u; });

    const Try* off = nullptr;
    const Try* def = nullptr;
    for (const Try& tr : tries) {
        if (state_.is_offense(tr.player) && !off) off = &tr;
        if (!state_.is_offense(tr.player) && !def) def = &tr;
    }
    if (off) tried_[static_cast<std::size_t>(off->player)] = true;
    if (def) tried_[static_cast<std::size_t>(def->player)] = true;

    const auto stretch_factor = [&](const Try& tr) {
        const Agent& a = state_.agent(tr.player);
        const double stretch = std::clamp(tr.c.dist / a.body.reach, 0.0, 1.0);
        double f = 1.0 - t.reach_penalty * stretch * stretch;
        if (tr.diving) f *= t.dive_factor;
        return f;
    };

    if (off && def) {
        // Contested: who wins the ball?
        const Agent& o = state_.agent(off->player);
        const Agent& d = state_.agent(def->player);
        const double ro = t.contest_receiver(o.ratings) +
                          (off->c.dist < def->c.dist ? t.contest_closer_bonus : 0.0);
        const double rd = t.contest_defender(d.ratings) +
                          (def->c.dist < off->c.dist ? t.contest_closer_bonus : 0.0);
        if (rng.chance(contest_probability(ro, rd, t.contest_skill_scale))) {
            const double p = catch_chance(o, off->c.dist, off->diving, ball.vel.length(), t) *
                             t.contested_catch_factor;
            if (rng.chance(p)) {
                catch_ball(off->player, off->c.at);
                return;
            }
            after_touch(off->player, off->c.at, false, "drop");
        } else {
            if (rng.chance(t.int_contested(d.ratings[Attr::BallSkills]))) {
                intercept(def->player, def->c.at);
                return;
            }
            after_touch(def->player, def->c.at, true, "breakup");
        }
        return;
    }
    if (off) {
        const Agent& o = state_.agent(off->player);
        double p = catch_chance(o, off->c.dist, off->diving, ball.vel.length(), t);
        // Traffic: a defender right on him as the ball arrives.
        for (int d = state_.offense_count; d < state_.count(); ++d) {
            if ((state_.agent(d).pos - o.pos).length() <= t.traffic_range) {
                const double nerve = (1.0 - t.traffic_bravery_share) * o.ratings[Attr::CatchInTraffic] +
                                     t.traffic_bravery_share * o.ratings[Attr::Bravery];
                p *= 1.0 - t.traffic_penalty(nerve);
                break;
            }
        }
        if (rng.chance(p)) {
            catch_ball(off->player, off->c.at);
            return;
        }
        after_touch(off->player, off->c.at, false, "drop");
        return;
    }
    // A defender alone at the ball: does he get a hand on it (a lineman at
    // the line far less often), and if so, does he hold on?
    const Agent& d = state_.agent(def->player);
    double touch = t.touch(d.ratings[Attr::BallSkills]) * stretch_factor(*def);
    if (state_.play_pos(def->player).y < t.line_depth) touch *= t.bat_factor;
    if (!rng.chance(touch)) return;  // it goes by him
    if (rng.chance(t.int_alone(d.ratings[Attr::BallSkills]))) {
        intercept(def->player, def->c.at);
        return;
    }
    after_touch(def->player, def->c.at, true, "breakup");
}

void PlaySim::after_touch(int player, Vec3 at, bool defender, const char* what) {
    // A drop or a breakup: tipped up and still live, or knocked down.
    Ball& ball = world_.ball();
    const PassingTuning& t = passing();
    Rng& rng = world_.rng();
    if (defender) ++result_.breakups;
    event(what, {{"player", state_.slot_name(player)}});
    if (rng.chance(t.tip_chance)) {
        ++result_.tips;
        ball.pos = at;
        ball.vel = ball.vel * t.tip_keep;
        ball.vel.z += rng.uniform(t.tip_up_min, t.tip_up_max);
        tipped_ = true;
        // A new touch: everyone else gets another try.
        std::fill(tried_.begin(), tried_.end(), false);
        tried_[static_cast<std::size_t>(player)] = true;
        event("tip", {{"player", state_.slot_name(player)}});
        return;
    }
    // Knocked to the ground: nobody else can play it.
    ball.pos = at;
    ball.vel = {ball.vel.x * 0.2, ball.vel.y * 0.2, -3.0};
    std::fill(tried_.begin(), tried_.end(), true);
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

void PlaySim::intercept(int defender, Vec3 at) {
    Ball& ball = world_.ball();
    const Vec2 feet = state_.agent(defender).pos;
    if (feet.y < 0.0 || feet.y > kFieldWidth) {
        event("out_of_bounds_catch", {{"player", state_.slot_name(defender)}});
        ball.state = Ball::State::Dead;
        ball.pos = at;
        ball.vel = {};
        whistle("incomplete");
        return;
    }
    ball.state = Ball::State::Held;
    ball.carrier = defender;
    ball.pos = at;
    result_.interception = true;
    result_.intercepted_by = state_.slot_name(defender);
    event("interception", {{"player", result_.intercepted_by},
                           {"air_yards", state_.frame.to_play(at.ground()).y}});
    // He runs it back; the offense turns into tacklers.
    state_.behaviors[static_cast<std::size_t>(defender)] = std::make_unique<BallCarrier>(
        std::vector<BallCarrier::Mark>{}, 0, state_.agent(defender).pos);
    for (int i = 0; i < state_.offense_count; ++i) {
        state_.behaviors[static_cast<std::size_t>(i)] = std::make_unique<Pursuer>();
        job_index_[static_cast<std::size_t>(i)] = std::numeric_limits<std::size_t>::max() / 2;
    }
}

void PlaySim::track_pressure() {
    // Stats: the first time a defender gets near the QB while he holds the
    // ball before throwing, and whether he went hot.
    if (!state_.play.pass || state_.pass || !state_.has_ball(state_.qb)) return;
    if (const auto* qb = dynamic_cast<const PassingQb*>(state_.behavior(state_.qb));
        qb && qb->went_hot() && !result_.hot) {
        result_.hot = true;
        event("hot", {{"player", state_.slot_name(state_.qb)}});
    }
    if (result_.time_to_pressure >= 0.0) return;
    const Vec2 qb = state_.agent(state_.qb).pos;
    for (int d = state_.offense_count; d < state_.count(); ++d) {
        if ((state_.agent(d).pos - qb).length() <= blocking().pressure_radius) {
            result_.time_to_pressure = state_.since_snap();
            return;
        }
    }
}

void PlaySim::check_rules() {
    if (state_.phase != PlayPhase::Live) return;
    const int c = state_.carrier();
    if (c >= 0) {
        const Vec2 p = state_.agent(c).pos;
        if (p.y < 0.0 || p.y > kFieldWidth || p.x < 0.0 || p.x > kFieldLength) {
            whistle("out_of_bounds");
            return;
        }
        const double y = state_.frame.to_play(p).y;
        if (state_.is_offense(c) ? y >= state_.frame.goal_line_y()
                                 : y <= state_.frame.own_goal_line_y()) {
            whistle("touchdown");
            return;
        }
        // Contact is down until tackling exists (phase 7).
        for (int j = 0; j < state_.count(); ++j) {
            if (state_.teammates(j, c)) continue;
            // A blocked or knocked-down player can't make the tackle.
            if (state_.engaged(j) || state_.knocked_down(j)) continue;
            const Agent& tackler = state_.agent(j);
            if ((tackler.pos - p).length() <= tackler.body.reach) {
                const bool sack = c == state_.qb && state_.play.pass && !state_.pass;
                event("contact", {{"by", state_.slot_name(j)}, {"on", state_.slot_name(c)}});
                whistle(sack ? "sack" : "tackled");
                return;
            }
        }
    }
    if (state_.since_snap() >= tuning().play_max_seconds) whistle("whistle");
}

void PlaySim::whistle(const std::string& outcome) {
    state_.phase = PlayPhase::Dead;
    state_.engagements.clear();
    dead_tick_ = world_.tick();
    result_.outcome = outcome;
    result_.seconds = state_.since_snap();

    const int c = state_.carrier();
    if (c >= 0) result_.carrier = state_.slot_name(c);
    if (result_.interception) {
        result_.defense_touchdown = outcome == "touchdown";
    } else if (outcome == "touchdown") {
        result_.yards = state_.frame.goal_line_y();
    } else if (outcome != "incomplete" && c >= 0) {
        result_.yards = state_.frame.to_play(state_.agent(c).pos).y;
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
        if (state_.phase == PlayPhase::Live) blocking_.steer(intents);
        world_.step(intents);
        if (state_.phase == PlayPhase::Live) {
            blocking_.interact(world_.rng());
            track_pressure();
            record_history();
        }
        ball_phase();
        check_rules();
        if (state_.phase == PlayPhase::PreSnap) check_set();
        write_frame();
        if (state_.phase == PlayPhase::Dead && world_.tick() - dead_tick_ >= post_ticks) break;
    }

    const BlockingStats& bs = blocking_.stats();
    result_.sheds = bs.sheds;
    result_.pancakes = bs.pancakes;
    result_.rush_moves_won = bs.rush_moves_won;
    result_.holds = bs.holds;

    // One readable line.
    std::ostringstream s;
    if (state_.play.type == PlayType::Pass) {
        if (result_.target.empty()) {
            s << "No throw";
        } else if (result_.thrown_away) {
            s << "Thrown away after " << fixed1(result_.time_to_throw) << " s";
        } else if (result_.interception) {
            s << "Intercepted by " << result_.intercepted_by << " (thrown to " << result_.target
              << ", " << fixed1(result_.air_yards) << " yd downfield)";
        } else {
            s << (result_.complete ? "Complete to " : "Incomplete to ") << result_.target << ", "
              << fixed1(result_.air_yards) << " yd downfield after "
              << fixed1(result_.time_to_throw) << " s";
            if (result_.breakups > 0) s << " (broken up)";
        }
    } else {
        s << (result_.handoff ? "Handoff to " + state_.play.run->carrier : "No handoff");
    }
    if (result_.interception) {
        if (result_.defense_touchdown) s << "; returned for a touchdown";
    } else if (result_.outcome != "incomplete") {
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

    DefenseSide defense;
    if (!options.defense.empty()) {
        const auto dteam = data.teams.find(options.defense);
        if (dteam == data.teams.end()) throw PlayError("unknown team '" + options.defense + "'");
        const auto droster = data.rosters.find(options.defense);
        if (droster == data.rosters.end()) {
            throw PlayError("no roster for team '" + options.defense + "'");
        }
        const auto dbook = data.playbooks.find(dteam->second.defense_playbook);
        if (dbook == data.playbooks.end()) {
            throw PlayError("unknown playbook '" + dteam->second.defense_playbook + "'");
        }
        const Play* dplay = dbook->second.find(options.defense_play);
        if (!dplay) {
            throw PlayError("playbook " + dbook->first + " has no play '" + options.defense_play +
                            "'");
        }
        const auto dform = data.formations.find(dplay->formation);
        if (dform == data.formations.end()) {
            throw PlayError("unknown formation '" + dplay->formation + "'");
        }
        defense = {&dteam->second, &droster->second, &dbook->second, dplay, &dform->second};
    }

    PlaySim sim(data, options, team->second, roster->second, book->second, *play, formation->second,
                defense, out);
    return sim.run();
}

}  // namespace cfbsim
