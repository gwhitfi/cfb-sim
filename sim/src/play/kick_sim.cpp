// One kicking play (phase 9; cfbsim_design.md#special-teams-play).
//
// Kickoffs and punts run with the *receiving* team as the "offense" (agents
// [0, offense_count), the PlayState frame is theirs): after the kick they
// carry the ball and block, so the lane AI, pursuit, tackles, and the
// engagement contest (blockers on the offense, defenders on the defense)
// all work unchanged. Field goals run with the kicking team as the offense
// (it protects; the block team rushes).
//
// Special teams formations and plays are authored in the kicking team's
// frame (ball at the origin, +y toward the receiving goal line) for both
// units; positions are worked out in that frame and converted to the field.

#include "cfbsim/play/kick_sim.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <sstream>

#include "agents/behaviors.h"
#include "agents/special_teams.h"
#include "agents/steer.h"
#include "cfbsim/physics/ball_flight.h"
#include "play/engagements.h"
#include "play/lanes.h"
#include "play/play_state.h"
#include "play/shapes.h"
#include "play/tackles.h"

namespace cfbsim {

namespace {

constexpr double kDeg = std::numbers::pi / 180.0;
constexpr double kMaxSeconds = 30.0;

std::string fixed1(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f", v);
    return buf;
}

Hash mirrored_hash(Hash h) {
    return h == Hash::Left ? Hash::Right : h == Hash::Right ? Hash::Left : Hash::Middle;
}

// A launch velocity that lands the ball at `to` (ground) after `hang` s.
Vec3 lob(Vec3 from, Vec2 to, double hang, double gravity) {
    const Vec2 flat = (to - from.ground()) / hang;
    const double vz = (gravity * hang * hang / 2.0 - from.z) / hang;
    return {flat.x, flat.y, vz};
}

struct Sides {
    const Team* team = nullptr;
    const Roster* roster = nullptr;
    const Playbook* book = nullptr;
    const Play* play = nullptr;
    const Formation* formation = nullptr;
};

class KickSim {
  public:
    KickSim(const DataSet& data, const KickOptions& opt, Sides kick, Sides recv, TrackingWriter* out)
        : data_(data),
          opt_(opt),
          k_(kick),
          r_(recv),
          out_(out),
          returns_(kick.play->type != PlayType::FieldGoal),
          kframe_(opt.yard_line, opt.hash, opt.kicking_right, false),
          world_(data.tuning.movement, opt.seed),
          state_(data, returns_ ? *recv.play : *kick.play, returns_ ? *recv.formation : *kick.formation,
                 returns_ ? *recv.book : *kick.book,
                 returns_ ? PlayFrame(100.0 - opt.yard_line, mirrored_hash(opt.hash), !opt.kicking_right, false)
                          : kframe_,
                 world_),
          blocking_(state_, [this](std::string_view name, Json fields) { event(name, std::move(fields)); }),
          tackles_(state_, [this](std::string_view name, Json fields) { event(name, std::move(fields)); }) {
        state_.defense_play = returns_ ? kick.play : recv.play;
        state_.defense_formation = returns_ ? kick.formation : recv.formation;
        state_.defense_book = returns_ ? kick.book : recv.book;
    }

    KickResult run();
    // Lines everyone up and stops (kick_start).
    std::vector<Spot> start() {
        setup();
        return spots_of(world_);
    }

  private:
    const SpecialTuning& st() const { return data_.tuning.special; }
    double gravity() const { return data_.tuning.passing.gravity; }
    std::size_t n() const { return state_.slots.size(); }
    bool kicking(int i) const { return returns_ ? !state_.is_offense(i) : state_.is_offense(i); }
    Vec2 kick_dir() const { return kframe_.dir_to_field({0.0, 1.0}); }
    Vec2 across() const { return kframe_.dir_to_field({1.0, 0.0}); }
    double ky(Vec2 field) const { return kframe_.to_play(field).y; }  // kicking-frame y
    void event(std::string_view name, Json fields = Json::object()) {
        if (out_) out_->event(world_, name, std::move(fields));
    }

    void setup();
    void place(const Sides& side, bool kicking_side);
    Vec2 resolve(const PathPoint& p) const;  // kicking frame
    double resolve_x(const Anchor& a) const;
    double resolve_y(const Anchor& a) const;
    std::unique_ptr<Behavior> job_behavior(int i, const Job& job);
    Pursuer::Setup pursuit(int i, bool contain) const;
    std::unique_ptr<Behavior> free_carrier(int i, std::optional<Vec2> aim);

    void live_intents(std::vector<Intent>& intents, Actions& actions);
    void apply(const Actions& actions);
    void ball_phase();
    void launch_kick();
    void launch_punt();
    void launch_field_goal();
    // The holder has the snap: the kicker steps up to the ball, timed to the hold.
    void approach_field_goal();
    void plan_return();
    void read_fair_catch();
    void field_kick(Vec3 before);
    void kick_landed();
    void loose_ball();
    void field_goal_flight(Vec3 before);
    void advance(int i);  // his next job now
    void possess(int player);    // a receiver fields (or scoops) the ball and returns it
    void everyone_loose(bool receivers_too);
    void check_rules();
    void handle_tackle(const TackleResult& r);
    void fumble(int carrier);
    void whistle(const std::string& result, Vec2 spot);
    void record_history();
    void write_header();
    void write_frame();

    const DataSet& data_;
    const KickOptions& opt_;
    Sides k_, r_;
    TrackingWriter* out_;
    bool returns_;
    PlayFrame kframe_;
    World world_;
    PlayState state_;
    Engagements blocking_;
    Tackles tackles_;
    KickResult result_;

    std::vector<std::size_t> job_index_;
    std::vector<double> distance_, engaged_;
    int kicker_ = -1, snapper_ = -1, holder_ = -1, returner_ = -1;
    std::int64_t dead_tick_ = 0;
    // The snap and the kick.
    bool snap_in_flight_ = false;
    std::optional<double> caught_snap_;  // s after the snap the punter/holder had it
    bool kicked_ = false;
    std::optional<double> kick_time_;
    Vec2 kick_from_;        // field frame
    Vec2 landing_;          // field frame, the planned landing spot
    double hang_ = 0.0;
    bool fc_read_ = false;  // the kickoff fair catch read is done
    // The ball after the kick.
    bool touched_ = false;  // a receiver has touched it (muff, catch)
    std::optional<double> touch_time_;  // the game clock starts
    std::optional<Vec2> fielded_at_;    // field frame: where he caught or scooped it
    bool fumbled_ = false;
    bool landed_ = false;
    double loose_since_ = -1.0;
    bool fg_resolved_ = false;
};

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

double KickSim::resolve_x(const Anchor& a) const {
    switch (a.kind) {
        case AnchorKind::Ball:
            return a.offset;
        case AnchorKind::Numbers:
            return kframe_.numbers_x(a.side) + a.offset;
        case AnchorKind::Hash:
            return kframe_.hash_x(a.side) + a.offset;
        case AnchorKind::Sideline:
            return kframe_.sideline_x(a.side) + a.offset;
        default:
            throw PlayError("special teams x anchors are ball, numbers, hash, or sideline");
    }
}

double KickSim::resolve_y(const Anchor& a) const {
    switch (a.kind) {
        case AnchorKind::Ball:
            return a.offset;
        case AnchorKind::GoalLine:
            return kframe_.goal_line_y() + a.offset;
        case AnchorKind::RestrainingLine:
            return 10.0 + a.offset;
        default:
            throw PlayError("special teams y anchors are ball, goal_line, or restraining_line");
    }
}

Vec2 KickSim::resolve(const PathPoint& p) const { return {resolve_x(p.x), resolve_y(p.y)}; }

void KickSim::place(const Sides& side, bool kicking_side) {
    const std::string& unit = side.formation->special_unit;
    const auto map = side.team->depth_chart.special_units.find(unit);
    const auto& chosen = kicking_side ? opt_.kicking_players : opt_.receiving_players;
    if (map == side.team->depth_chart.special_units.end() && chosen.empty()) {
        throw PlayError(side.team->id + " has no '" + unit + "' special teams unit");
    }
    for (const FormationSlot& slot : side.formation->slots) {
        std::string id;
        if (const auto c = chosen.find(slot.name); c != chosen.end()) {
            id = c->second;
        } else {
            const auto fill = map->second.find(slot.name);
            if (fill == map->second.end()) {
                throw PlayError(side.team->id + "'s " + unit + " unit doesn't fill " + slot.name);
            }
            id = fill->second.player_id;
            if (fill->second.rank) {
                const DepthRank rk = *fill->second.rank;
                const auto pos = side.team->depth_chart.positions.find(rk.position);
                const auto at = static_cast<std::size_t>(rk.rank) - 1;
                if (pos == side.team->depth_chart.positions.end() || at >= pos->second.size()) {
                    throw PlayError(side.team->id + " has no " + std::string(to_string(rk.position)) +
                                    std::to_string(rk.rank) + " for " + slot.name);
                }
                id = pos->second[at];
            }
        }
        const Player* player = side.roster->find(id);
        if (!player) throw PlayError("player '" + id + "' isn't on the roster");

        const Vec2 kp{resolve_x(slot.x), resolve_y(slot.y)};
        const Vec2 field = kframe_.to_field(kp);
        SlotInfo info;
        info.slot = slot.name;
        info.formation_slot = &slot;
        if (const auto a = side.play->assignments.find(slot.name); a != side.play->assignments.end()) {
            info.assignment = &a->second;
        }
        info.aligned = state_.frame.to_play(field);
        info.scheme_pk = player->playbook_knowledge_for(side.book->scheme);
        state_.slots.push_back(info);

        RatingModifiers mods;
        if (const auto m = opt_.modifiers.find(id); m != opt_.modifiers.end()) mods = m->second;
        const bool home = kicking_side == opt_.kicking_home;
        Agent a = make_agent(*player, home ? TeamSide::Home : TeamSide::Away, slot.name, mods,
                             data_.tuning.movement);
        a.pos = field;
        a.facing = kicking_side ? kick_dir() : kick_dir() * -1.0;
        const int i = world_.add_agent(std::move(a));
        if (info.assignment && !info.assignment->sequence.empty()) {
            const JobKind first = info.assignment->sequence.front().kind;
            if (first == JobKind::Kick || first == JobKind::Punt) kicker_ = i;
            if (first == JobKind::LongSnap) snapper_ = i;
            if (first == JobKind::Hold) holder_ = i;
            if (first == JobKind::Return) returner_ = i;
        }
    }
}

void KickSim::setup() {
    if (returns_) {
        place(r_, false);
        state_.offense_count = static_cast<int>(n());
        place(k_, true);
    } else {
        place(k_, true);
        state_.offense_count = static_cast<int>(n());
        place(r_, false);
    }
    if (kicker_ < 0) throw PlayError("kick play " + k_.play->id + " has no kicker");
    if (k_.play->type != PlayType::Kickoff && snapper_ < 0) {
        throw PlayError("kick play " + k_.play->id + " has no long snapper");
    }
    if (k_.play->type == PlayType::FieldGoal && holder_ < 0) {
        throw PlayError("kick play " + k_.play->id + " has no holder");
    }

    state_.get_off.assign(n(), 0.0);
    state_.throw_reaction.assign(n(), 0.0);
    state_.handoff_reaction.assign(n(), std::numeric_limits<double>::infinity());
    state_.open.assign(n(), 1.0);
    state_.behaviors.resize(n());
    job_index_.assign(n(), 0);
    distance_.assign(n(), 0.0);
    engaged_.assign(n(), 0.0);
    blocking_.setup();
    tackles_.setup();

    Ball& ball = world_.ball();
    ball.state = Ball::State::Spotted;
    ball.pos = {kframe_.ball().x, kframe_.ball().y, 0.15};
    kick_from_ = kframe_.ball();
    result_.type = k_.play->type;
    result_.kind = k_.play->kick ? k_.play->kick->kind : "";
    result_.kicker_id = state_.agent(kicker_).id;
}

Pursuer::Setup KickSim::pursuit(int i, bool contain) const {
    const TacklingTuning& t = data_.tuning.tackling;
    const Attributes& r = state_.agent(i).ratings;
    const double skill = t.pursuit_skill(r);
    Pursuer::Setup s;
    s.quality = t.prediction(skill);
    s.lag = t.pursue_lag(skill);
    s.contain = contain;
    s.effort = t.effort(r[Attr::WorkRate]);
    s.breakdown = t.breakdown_speed(skill);
    if (k_.play->type == PlayType::Kickoff && kicking(i)) s.breakdown_extra = st().breakdown_extra;
    return s;
}

std::unique_ptr<Behavior> KickSim::free_carrier(int i, std::optional<Vec2> aim) {
    BallCarrier::Setup s;
    s.start = state_.agent(i).pos;
    s.lane = lane_skill(state_.agent(i).ratings, data_.tuning.carrier);
    if (aim) {
        s.path.push_back({*aim, "aim"});
        s.free_after = 0;
    }
    return std::make_unique<BallCarrier>(std::move(s), world_.rng().next_u64());
}

std::unique_ptr<Behavior> KickSim::job_behavior(int i, const Job& job) {
    const Agent& a = state_.agent(i);
    const double inf = std::numeric_limits<double>::infinity();
    const Vec2 kd = kick_dir();
    const Vec2 face = kicking(i) ? kd : kd * -1.0;
    const double side_x = kframe_.to_play(a.pos).x;
    // Kickoff coverage times its run to the kick; after a punt they just go.
    const Cover::Before timing = k_.play->type == PlayType::Kickoff ? Cover::Before::Time : Cover::Before::Release;
    switch (job.kind) {
        case JobKind::Kick:
        case JobKind::Punt:
            if (k_.play->type == PlayType::Kickoff) {
                return std::make_unique<Wait>("kick", kframe_.ball() - kd * 0.6, kd, inf);
            }
            return std::make_unique<Wait>(job.kind == JobKind::Punt ? "punt" : "kick", a.pos, kd, inf);
        case JobKind::LongSnap:
            return std::make_unique<Wait>("long_snap", a.pos, kd, 0.4);
        case JobKind::Hold:
            return std::make_unique<Wait>("hold", a.pos, kd, inf);
        case JobKind::Protect:
            if (!returns_) return std::make_unique<Protector>(a.pos);
            return std::make_unique<Cover>("protect", across(), kd, 0.0, 0.0, Cover::Before::Hold);
        case JobKind::CoverLane: {
            auto c = std::make_unique<Cover>("cover", across(), kd, job.lane_x.value_or(0.0), 0.0, timing);
            c->set_kicker(kicker_);
            return c;
        }
        case JobKind::Contain: {
            auto c = std::make_unique<Cover>("contain", across(), kd, side_x < 0.0 ? -18.0 : 18.0, 0.0, timing);
            c->set_kicker(kicker_);
            return c;
        }
        case JobKind::Gunner:
            return std::make_unique<Cover>("gunner", across(), kd, side_x < 0.0 ? -3.0 : 3.0, 0.0, Cover::Before::Release);
        case JobKind::Safety:
            if (!kicking(i)) return std::make_unique<Wait>("safety", a.pos, face, inf);
            if (k_.play->type != PlayType::Kickoff) return std::make_unique<Cover>("safety", across(), kd, 0.0, 18.0, timing);
            {
                // A kickoff safety: a second line behind the coverage.
                auto c = std::make_unique<Cover>("safety", across(), kd, job.lane_x.value_or(0.0), 0.0, timing);
                c->set_kicker(kicker_);
                c->set_behind(job.depth.value_or(st().safety_depth));
                return c;
            }
        case JobKind::Jam:
            return std::make_unique<Jammer>(st().jam);
        case JobKind::HoldUp:
            return std::make_unique<Wait>("hold_up", a.pos, face, st().hold_up);
        case JobKind::ReturnBlock: {
            // His lateral spot relative to the catch: a third of his alignment.
            const Vec2 offset = across() * (side_x * 0.35);
            return std::make_unique<ReturnBlocker>(offset, kd, st().set_depth);
        }
        case JobKind::Return:
            return std::make_unique<Returner>(kd);
        case JobKind::Recover:
            return std::make_unique<LooseBall>();
        case JobKind::Rush: {
            // At the kick point: a yard in front of the holder.
            const Vec2 point = holder_ >= 0 ? state_.agent(holder_).pos + kd * 1.0 : kframe_.ball();
            return std::make_unique<KickRusher>(point);
        }
        default:
            return std::make_unique<Wait>(std::string(to_name(kJobKindNames, job.kind)), a.pos, face, inf);
    }
}

// ---------------------------------------------------------------------------
// The tick
// ---------------------------------------------------------------------------

void KickSim::live_intents(std::vector<Intent>& intents, Actions& actions) {
    for (int i = 0; i < state_.count(); ++i) {
        const auto idx = static_cast<std::size_t>(i);
        if (state_.behaviors[idx]->done(state_, i)) {
            const Assignment* a = state_.slots[idx].assignment;
            ++job_index_[idx];
            if (a && job_index_[idx] < a->sequence.size()) {
                state_.behaviors[idx] = job_behavior(i, a->sequence[job_index_[idx]]);
            }
        }
    }
    for (int i = 0; i < state_.count(); ++i) {
        intents[static_cast<std::size_t>(i)] = state_.behaviors[static_cast<std::size_t>(i)]->decide(state_, i, actions);
    }
}

void KickSim::apply(const Actions& actions) {
    if (actions.evasion) tackles_.resolve_evasion(*actions.evasion, world_.rng());
    for (const auto& [name, fields] : actions.notes) event(name, fields);
}

void KickSim::record_history() {
    std::vector<PlayState::Snapshot> snap;
    snap.reserve(n());
    for (const Agent& a : world_.agents()) snap.push_back({a.pos, a.vel});
    state_.history.push_back(std::move(snap));
}

void KickSim::launch_kick() {
    // Kickoff from the tee (rules: the kick spot).
    const SpecialTuning& t = st();
    Rng& rng = world_.rng();
    const Agent& k = state_.agent(kicker_);
    const KickSpec& spec = *k_.play->kick;
    Vec2 target{0.0, kframe_.goal_line_y()};
    if (spec.target) target = resolve(*spec.target);
    double hang = t.ko_deep_hang(k.ratings[Attr::KickPower]);
    if (spec.kind == "deep") {
        target.y = t.ko_deep_distance(k.ratings[Attr::KickPower]) + rng.normal(0.0, t.ko_distance_sd);
        target.x += rng.normal(0.0, t.ko_lateral_sd(k.ratings[Attr::KickAccuracy]));
    } else {
        hang = spec.kind == "onside" ? t.ko_onside_hang : t.ko_squib_hang;
        target.y += rng.normal(0.0, t.ko_short_sd);
        target.x += rng.normal(0.0, t.ko_short_sd);
    }
    Ball& ball = world_.ball();
    landing_ = kframe_.to_field(target);
    hang_ = hang;
    ball.state = Ball::State::InFlight;
    ball.carrier = -1;
    ball.vel = lob(ball.pos, landing_, hang, gravity());
    kicked_ = true;
    kick_time_ = state_.since_snap();
    PassInfo kick;
    kick.thrower = kicker_;
    kick.tick = world_.tick();
    kick.release = ball.pos;
    kick.aim = {landing_.x, landing_.y, 0.0};
    kick.away = true;
    state_.pass = kick;
    event("kick", {{"kind", spec.kind}, {"distance", target.y}, {"hang", hang}});
    advance(kicker_);
    plan_return();
}

void KickSim::launch_punt() {
    const SpecialTuning& t = st();
    Rng& rng = world_.rng();
    const Agent& p = state_.agent(kicker_);
    const double power = p.ratings[Attr::PuntPower];
    double distance = t.punt_distance(power) + rng.normal(0.0, t.punt_distance_sd);
    double hang = t.punt_hang(power);
    const double goal = kframe_.goal_line_y();
    if (distance > goal - 3.0) {
        // He could reach the end zone: aim short of the goal line instead.
        const double aimed = std::max(5.0, goal - t.punt_coffin + rng.normal(0.0, 2.5));
        hang *= std::clamp(aimed / std::max(distance, 1.0), 0.6, 1.0);
        distance = aimed;
    }
    const Vec2 target{rng.normal(0.0, t.punt_lateral_sd(p.ratings[Attr::PuntAccuracy])), distance};
    Ball& ball = world_.ball();
    ball.state = Ball::State::InFlight;
    ball.carrier = -1;
    ball.pos = {p.pos.x, p.pos.y, 1.0};
    landing_ = kframe_.to_field(target);
    hang_ = hang;
    ball.vel = lob(ball.pos, landing_, hang, gravity());
    kicked_ = true;
    kick_time_ = state_.since_snap();
    kick_from_ = p.pos;
    PassInfo kick;
    kick.thrower = kicker_;
    kick.tick = world_.tick();
    kick.release = ball.pos;
    kick.aim = {landing_.x, landing_.y, 0.0};
    kick.away = true;
    state_.pass = kick;
    event("punt", {{"distance", distance}, {"hang", hang}});
    plan_return();
    // The punter and the protection go into coverage.
    advance(kicker_);
    for (int i = 0; i < state_.count(); ++i) {
        if (!kicking(i) || i == kicker_) continue;
        const auto idx = static_cast<std::size_t>(i);
        const Assignment* a = state_.slots[idx].assignment;
        if (a && job_index_[idx] < a->sequence.size() && a->sequence[job_index_[idx]].kind == JobKind::Protect) {
            advance(i);
        }
    }
}

void KickSim::approach_field_goal() {
    const SpecialTuning& t = st();
    const Agent& k = state_.agent(kicker_);
    const Agent& h = state_.agent(holder_);
    const Vec2 kd = kick_dir();
    // Plant beside and just behind the ball (the holder's spot), on his side.
    const Vec2 across = kd.rotated(std::numbers::pi / 2.0);
    const double side = (k.pos - h.pos).dot(across) >= 0.0 ? 1.0 : -1.0;
    const Vec2 plant = h.pos - kd * 0.9 + across * (0.5 * side);
    const double hold = std::max(t.fg_hold(t.fg_holder(h.ratings)), kTickSeconds);
    // A little quicker than even pace: he speeds up and eases in at the end.
    const double speed = std::min(k.move.top_speed, 1.4 * (plant - k.pos).length() / hold);
    state_.behaviors[static_cast<std::size_t>(kicker_)] =
        std::make_unique<Wait>("kick", plant, kd, std::numeric_limits<double>::infinity(), speed);
}

void KickSim::launch_field_goal() {
    const SpecialTuning& t = st();
    Rng& rng = world_.rng();
    const Agent& k = state_.agent(kicker_);
    const Agent& h = state_.agent(holder_);
    const double holder = t.fg_holder(h.ratings);
    // Under a rush (a defender close to the holder), Composure matters.
    bool rushed = false;
    for (int d = state_.offense_count; d < state_.count(); ++d) {
        if ((state_.agent(d).pos - h.pos).length() < 3.0) rushed = true;
    }
    double sd = t.fg_aim_sd(k.ratings[Attr::KickAccuracy]) + t.fg_hold_sd * (1.0 - holder / 100.0);
    if (rushed) sd += t.fg_composure_sd * (1.0 - k.ratings[Attr::Composure] / 100.0);
    const double speed = t.fg_speed(k.ratings[Attr::KickPower]) * (1.0 + rng.normal(0.0, t.fg_speed_sd));
    const Vec2 posts = kframe_.to_field({0.0, kframe_.goal_line_y() + 10.0});
    Ball& ball = world_.ball();
    ball.pos = {h.pos.x, h.pos.y, 0.1};
    const Vec2 dir = (posts - ball.pos.ground()).normalized().rotated(rng.normal(0.0, sd) * kDeg);
    const double up = t.fg_angle_deg * kDeg;
    ball.vel = {dir.x * speed * std::cos(up), dir.y * speed * std::cos(up), speed * std::sin(up)};
    ball.state = Ball::State::InFlight;
    ball.carrier = -1;
    kicked_ = true;
    kick_time_ = state_.since_snap();
    kick_from_ = h.pos;
    PassInfo kick;
    kick.thrower = kicker_;
    kick.tick = world_.tick();
    kick.release = ball.pos;
    kick.away = true;
    state_.pass = kick;
    result_.kick_distance = 100.0 - opt_.yard_line + 17.0;
    event("field_goal_try", {{"distance", result_.kick_distance}});
}

void KickSim::advance(int i) {
    const auto idx = static_cast<std::size_t>(i);
    const Assignment* a = state_.slots[idx].assignment;
    if (!a || job_index_[idx] + 1 >= a->sequence.size()) return;
    ++job_index_[idx];
    state_.behaviors[idx] = job_behavior(i, a->sequence[job_index_[idx]]);
}

void KickSim::plan_return() {
    // (cfbsim_design.md#returner-decisions) Field it, fair catch,
    // take the touchback, or let it go, from where it's coming down and how
    // close the coverage is.
    if (returner_ < 0) return;
    auto* ret = dynamic_cast<Returner*>(state_.behaviors[static_cast<std::size_t>(returner_)].get());
    if (!ret) return;
    const SpecialTuning& t = st();
    const double land_y = ky(landing_);
    const double depth = land_y - kframe_.goal_line_y();  // + = in the end zone
    const double own_yl = kframe_.goal_line_y() - land_y;  // the receivers' yard line at the landing
    double margin = 1e9;
    for (int i = 0; i < state_.count(); ++i) {
        if (!kicking(i)) continue;
        const Agent& a = state_.agent(i);
        margin = std::min(margin, quick_reach_time(a.pos, a.vel, a.move.top_speed, a.move.decel, 1.0, landing_) - hang_);
    }
    // The return_aggression tactic: -1 (conservative) .. +1 (aggressive).
    const double aggr = std::clamp((opt_.return_aggression - 50.0) / 50.0, -1.0, 1.0);
    margin += aggr * t.aggression_margin;
    Returner::Plan plan = Returner::Plan::Field;
    if (k_.play->type == PlayType::Kickoff) {
        // A deep kick he can reach he sets to field; the fair catch read
        // decides later.
        if (result_.kind != "deep") {
            plan = Returner::Plan::Avoid;  // squibs and onsides: whoever is near plays the bounce
        } else if (depth > 10.0) {
            plan = Returner::Plan::Avoid;  // out the back: a touchback
        }
    } else {
        if (own_yl < t.let_bounce) {
            plan = Returner::Plan::Avoid;
        } else if (margin < t.fair_catch_punt) {
            plan = Returner::Plan::FairCatch;
        }
    }
    ret->set_plan(plan);
    static constexpr const char* kPlan[] = {"field", "fair_catch", "touchback", "let_it_go"};
    event("return_plan", {{"player", state_.slot_name(returner_)}, {"plan", kPlan[static_cast<int>(plan)]}});
}

void KickSim::read_fair_catch() {
    // (cfbsim_design.md#returner-decisions) A moment before a deep
    // kickoff comes down, a returner set to field it may fair catch it (a
    // knee in the end zone) for the 25 rather than return it, at his landing
    // zone's chance.
    fc_read_ = true;
    if (returner_ < 0 || result_.kind != "deep") return;
    auto* ret = dynamic_cast<Returner*>(state_.behaviors[static_cast<std::size_t>(returner_)].get());
    if (!ret || ret->plan() != Returner::Plan::Field) return;
    const SpecialTuning& t = st();
    const double own_yl = std::max(0.0, kframe_.goal_line_y() - ky(landing_));
    if (own_yl >= data_.rules.kickoff_touchback) return;
    double chance = 0.0;
    for (const auto& z : t.fc_zones) {
        if (own_yl <= z.max_yl) {
            chance = z.chance;
            break;
        }
    }
    const bool take = chance > 0.0 && world_.rng().chance(chance);
    if (take) ret->set_plan(ky(landing_) > kframe_.goal_line_y() ? Returner::Plan::Touchback : Returner::Plan::FairCatch);
    event("fair_catch_read", {{"player", state_.slot_name(returner_)},
                              {"yard_line", std::round(own_yl * 10.0) / 10.0},
                              {"chance", std::round(chance * 100.0) / 100.0},
                              {"take", take}});
}

void KickSim::possess(int player) {
    // A receiver has the ball: he returns it, his teammates block, the
    // kicking team pursues.
    Ball& ball = world_.ball();
    ball.state = Ball::State::Held;
    ball.carrier = player;
    ball.vel = {};
    touched_ = true;
    if (!touch_time_) touch_time_ = state_.since_snap();
    if (!fielded_at_) fielded_at_ = state_.agent(player).pos;
    if (result_.returner_id.empty()) result_.returner_id = state_.agent(player).id;
    state_.engagements.clear();
    tackles_.reset_carrier();
    // Aim the return upfield from where he caught it.
    std::optional<Vec2> aim;
    if (r_.play->return_spec) {
        const Vec2 a = kframe_.to_field(resolve(r_.play->return_spec->aim));
        // The aim's depth is relative to the catch: only its lateral spot is kept.
        const Vec2 at = state_.agent(player).pos;
        aim = at + across() * (a - at).dot(across()) * 0.5 - kick_dir() * 8.0;
    }
    // Blockers take the nearest coverage players, closest pairs first.
    std::vector<int> blockers, cover;
    for (int i = 0; i < state_.count(); ++i) {
        const auto idx = static_cast<std::size_t>(i);
        job_index_[idx] = std::numeric_limits<std::size_t>::max() / 2;
        if (i == player) continue;
        if (kicking(i)) {
            cover.push_back(i);
        } else {
            blockers.push_back(i);
        }
    }
    // On a kickoff the lane and contain players keep their lanes until they
    // close in and the kicker plays safety; all punt coverage pursues at once.
    for (const int i : cover) {
        const std::string_view job = state_.behavior(i)->name();
        const bool contain = job == "contain";
        const auto* lane = dynamic_cast<const Cover*>(state_.behavior(i));
        auto& behavior = state_.behaviors[static_cast<std::size_t>(i)];
        if (k_.play->type == PlayType::Kickoff && lane && (contain || job == "cover" || job == "safety")) {
            const SpecialTuning& t = st();
            LaneCoverage::Setup s;
            s.across = across();
            s.kick_dir = kick_dir();
            s.lane = lane->lane();
            s.contain = contain;
            s.safety = job == "safety";
            s.depth = lane->behind();
            s.disciplined = world_.rng().chance(t.stay_in_lane(state_.agent(i).ratings[Attr::Discipline]));
            s.squeeze_depth = t.squeeze_depth;
            s.break_range = t.break_range;
            s.contain_width = t.contain_width;
            s.safety_range = t.safety_range;
            s.pursuit = pursuit(i, contain);
            // The returner on his inside shoulder: he stays on his own side of him.
            if (std::abs(s.lane) > 0.5) s.pursuit.side = across() * (s.lane < 0.0 ? -1.0 : 1.0);
            behavior = std::make_unique<LaneCoverage>(s);
        } else {
            behavior = std::make_unique<Pursuer>(pursuit(i, contain));
        }
    }
    std::vector<bool> taken(n(), false);
    const Vec2 carrier = state_.agent(player).pos;
    for (const int b : blockers) {
        auto rb = std::make_unique<ReturnBlocker>(Vec2{}, kick_dir(), st().set_depth);
        // The nearest coverage man still unblocked who is closer to the ball than he is to the end zone.
        int best = -1;
        double best_d = 1e9;
        for (const int c : cover) {
            if (taken[static_cast<std::size_t>(c)]) continue;
            const Vec2 cp = state_.agent(c).pos;
            if ((cp - carrier).dot(kick_dir() * -1.0) < -2.0) continue;  // already past the returner
            const double d = (cp - state_.agent(b).pos).length();
            if (d < best_d) {
                best_d = d;
                best = c;
            }
        }
        if (best >= 0) taken[static_cast<std::size_t>(best)] = true;
        rb->set_target(best);
        state_.behaviors[static_cast<std::size_t>(b)] = std::move(rb);
    }
    state_.behaviors[static_cast<std::size_t>(player)] = free_carrier(player, aim);
    // Caught standing still, he secures it and looks upfield before he runs.
    const Agent& r = state_.agent(player);
    const double still = std::clamp(1.0 - r.vel.length() / st().secure_speed, 0.0, 1.0);
    const double delay = st().secure(r.ratings[Attr::Vision]) * still;
    state_.get_off[static_cast<std::size_t>(player)] = state_.since_snap() + delay;
    event("fielded", {{"player", state_.slot_name(player)}, {"delay", std::round(delay * 100.0) / 100.0}});
}

void KickSim::everyone_loose(bool receivers_too) {
    for (int i = 0; i < state_.count(); ++i) {
        const auto idx = static_cast<std::size_t>(i);
        if (!receivers_too && !kicking(i)) continue;
        job_index_[idx] = std::numeric_limits<std::size_t>::max() / 2;
        state_.behaviors[idx] = std::make_unique<LooseBall>();
    }
}

void KickSim::field_kick(Vec3 before) {
    // The returner fields the kick as it comes down to him.
    const Ball& ball = world_.ball();
    if (returner_ < 0 || landed_ || ball.vel.z > 0.0) return;
    const auto* ret = dynamic_cast<const Returner*>(state_.behavior(returner_));
    if (!ret || ret->plan() == Returner::Plan::Avoid) return;
    if (ball.pos.z > 2.7 || before.z < 0.3) return;
    const Agent& r = state_.agent(returner_);
    if ((ball.pos.ground() - r.pos).length() > st().catch_range) return;
    const Vec2 at = r.pos;
    if (ret->plan() == Returner::Plan::FairCatch) {
        touched_ = true;
        fielded_at_ = at;
        result_.returner_id = r.id;
        event("fair_catch", {{"player", state_.slot_name(returner_)}});
        whistle("fair_catch", at);
        return;
    }
    if (ret->plan() == Returner::Plan::Touchback) {
        event("touchback", {{"player", state_.slot_name(returner_)}});
        whistle("touchback", at);
        return;
    }
    const Curve& muff = k_.play->type == PlayType::Punt ? st().muff_punt : st().muff_kickoff;
    if (world_.rng().chance(muff(r.ratings[Attr::Catching]))) {
        Ball& b = world_.ball();
        touched_ = true;
        if (!touch_time_) touch_time_ = state_.since_snap();
        b.state = Ball::State::Loose;
        b.pos = {at.x, at.y, 1.0};
        const double angle = world_.rng().uniform(0.0, 2.0 * std::numbers::pi);
        b.vel = {std::cos(angle) * 3.0, std::sin(angle) * 3.0, 1.5};
        loose_since_ = state_.since_snap();
        event("muff", {{"player", state_.slot_name(returner_)}});
        everyone_loose(true);
        return;
    }
    possess(returner_);
}

void KickSim::kick_landed() {
    // The kick comes down untouched.
    Ball& ball = world_.ball();
    landed_ = true;
    const Vec2 at = ball.pos.ground();
    const bool out = at.y < 0.0 || at.y > kFieldWidth || at.x < 0.0 || at.x > kFieldLength;
    if (ky(at) >= kframe_.goal_line_y() && !out) {
        whistle("touchback", at);
        return;
    }
    if (out) {
        whistle(ky(at) >= kframe_.goal_line_y() ? "touchback" : "out_of_bounds", at);
        return;
    }
    // It bounces: the coverage goes for it (a punt: to down it), and on a
    // kickoff the receivers too.
    ball.state = Ball::State::Loose;
    loose_since_ = state_.since_snap();
    const bool kickoff = k_.play->type == PlayType::Kickoff;
    everyone_loose(kickoff);
    if (!kickoff && returner_ >= 0) {
        // The returner stays clear of a bouncing punt.
        auto* ret = dynamic_cast<Returner*>(state_.behaviors[static_cast<std::size_t>(returner_)].get());
        if (ret) ret->set_plan(Returner::Plan::Avoid);
    }
}

void KickSim::loose_ball() {
    // A kick bouncing or rolling, a muff, or a fumble on the return.
    const TacklingTuning& t = data_.tuning.tackling;
    Rng& rng = world_.rng();
    Ball& ball = world_.ball();
    if (ball.pos.z <= 0.0) {
        ball.pos.z = 0.0;
        if (ball.vel.z < -0.8) {
            const double turn = rng.uniform(-t.bounce_turn_deg, t.bounce_turn_deg) * kDeg;
            const Vec2 flat = ball.vel.ground().rotated(turn) * t.bounce_keep_xy;
            ball.vel = {flat.x, flat.y, -ball.vel.z * t.bounce_keep_z};
        } else {
            ball.vel.z = 0.0;
            const Vec2 flat = ball.vel.ground();
            const double speed = flat.length();
            const double slower = std::max(0.0, speed - t.roll_friction * kTickSeconds);
            const Vec2 v = speed > 1e-6 ? flat * (slower / speed) : Vec2{};
            ball.vel = {v.x, v.y, 0.0};
        }
    }
    const Vec2 at = ball.pos.ground();
    const bool kickoff = k_.play->type == PlayType::Kickoff;
    if (at.y < 0.0 || at.y > kFieldWidth || at.x < 0.0 || at.x > kFieldLength) {
        if (ky(at) >= kframe_.goal_line_y()) {
            whistle("touchback", at);
        } else if (touched_ || fumbled_) {
            whistle("returned", at);  // the receivers keep it where it went out
        } else {
            whistle("out_of_bounds", at);
        }
        return;
    }
    if (ky(at) >= kframe_.goal_line_y() && !fumbled_) {
        whistle("touchback", at);
        return;
    }
    // A punt that stops rolling is dead there.
    if (!kickoff && !touched_ && !fumbled_ && ball.pos.z <= 0.0 && ball.vel.ground().length() < 0.3) {
        whistle("downed", at);
        return;
    }
    if (state_.since_snap() - loose_since_ > 6.0) {
        whistle(fumbled_ || touched_ ? "returned" : "recovered_receiving", at);
        return;
    }
    if (ball.pos.z > t.recover_height) return;

    std::vector<std::pair<double, int>> near;
    for (int i = 0; i < state_.count(); ++i) {
        if (state_.knocked_down(i)) continue;
        const Agent& a = state_.agent(i);
        const double d = (a.pos - at).length();
        if (d <= a.body.reach) near.emplace_back(d, i);
    }
    std::stable_sort(near.begin(), near.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [d, i] : near) {
        if (kicking(i)) {
            // Kicking team: an onside kick only once it has gone 10 yd (or a
            // receiver touched it); an untouched punt is downed where he
            // touches it.
            if (!kickoff && !touched_ && !fumbled_) {
                whistle("downed", at);
                return;
            }
            if (kickoff && !touched_ && ky(at) < 10.0) continue;
            if (!rng.chance(t.recover_rate * (0.5 + t.recover_skill(state_.agent(i).ratings) / 200.0))) continue;
            result_.recovered_by_id = state_.agent(i).id;
            event("recovery", {{"player", state_.slot_name(i)}});
            whistle("recovered_kicking", at);
            return;
        }
        if (!kickoff && !touched_ && !fumbled_) continue;  // the receivers leave a bouncing punt alone
        if (!rng.chance(t.recover_rate * (0.5 + t.recover_skill(state_.agent(i).ratings) / 200.0))) continue;
        result_.recovered_by_id = state_.agent(i).id;
        event("recovery", {{"player", state_.slot_name(i)}});
        if (fumbled_) {
            whistle("returned", at);  // he falls on it
        } else {
            possess(i);  // he scoops it up and returns it
        }
        return;
    }
}

void KickSim::field_goal_flight(Vec3 before) {
    const SpecialTuning& t = st();
    Ball& ball = world_.ball();
    // A block: a rusher's hands in the ball's path while it's still low.
    const double from_kick = (ball.pos.ground() - kick_from_).length();
    if (from_kick <= t.block_window) {
        for (int d = state_.offense_count; d < state_.count(); ++d) {
            if (state_.engaged(d) || state_.knocked_down(d)) continue;
            const Agent& a = state_.agent(d);
            const double reach_up = t.block_height + 0.5 * (a.ratings[Attr::Jumping] - 50.0) / 50.0;
            if ((a.pos - ball.pos.ground()).length() > a.body.reach || ball.pos.z > reach_up) continue;
            if (!world_.rng().chance(t.block_chance)) continue;
            result_.blocked_by_id = a.id;
            event("blocked_kick", {{"player", state_.slot_name(d)}});
            fg_resolved_ = true;
            whistle("blocked", a.pos);
            return;
        }
    }
    const double posts_y = kframe_.goal_line_y() + 10.0;
    const double y0 = ky(before.ground()), y1 = ky(ball.pos.ground());
    if (y1 >= posts_y && y0 < posts_y) {
        const double f = (posts_y - y0) / (y1 - y0);
        const Vec3 at = before + (ball.pos - before) * f;
        const double x = kframe_.to_play(at.ground()).x;
        const bool good = std::abs(x) <= t.post_half_width && at.z >= t.crossbar;
        fg_resolved_ = true;
        event(good ? "kick_good" : "kick_no_good", {{"x", x}, {"height", at.z}});
        whistle(good ? "made" : "missed", kframe_.ball());
        return;
    }
    if (ball.pos.z <= 0.0) {
        fg_resolved_ = true;
        event("kick_no_good", {{"short", true}});
        whistle("missed", kframe_.ball());
    }
}

void KickSim::ball_phase() {
    Ball& ball = world_.ball();
    const SpecialTuning& t = st();
    const PlayType type = k_.play->type;
    const double now = state_.since_snap();

    if (!kicked_) {
        if (type == PlayType::Kickoff) {
            const Agent& k = state_.agent(kicker_);
            if (now >= t.ko_approach && (k.pos - ball.pos.ground()).length() < 1.2) launch_kick();
            else if (now >= t.ko_approach + 0.6) launch_kick();
            return;
        }
        if (snap_in_flight_) {
            const Vec3 before = ball.pos;
            step_ballistic(ball.pos, ball.vel, gravity(), kTickSeconds);
            const int to = type == PlayType::Punt ? kicker_ : holder_;
            const Agent& r = state_.agent(to);
            if ((ball.pos.ground() - r.pos).length() < 0.8 || ball.pos.z <= 0.3 ||
                (before.ground() - r.pos).dot(ball.pos.ground() - r.pos) < 0.0) {
                ball.state = Ball::State::Held;
                ball.carrier = to;
                ball.vel = {};
                snap_in_flight_ = false;
                caught_snap_ = now;
                if (type == PlayType::FieldGoal) approach_field_goal();
            }
            return;
        }
        if (caught_snap_) {
            if (type == PlayType::Punt) {
                if (now - *caught_snap_ >= t.punt_hold(state_.agent(kicker_).ratings[Attr::Decisions])) launch_punt();
            } else if (now - *caught_snap_ >= t.fg_hold(t.fg_holder(state_.agent(holder_).ratings))) {
                launch_field_goal();
            }
        }
        return;
    }

    if (ball.state == Ball::State::Loose) {
        step_ballistic(ball.pos, ball.vel, gravity(), kTickSeconds);
        if (state_.phase == PlayPhase::Live) loose_ball();
        return;
    }
    if (ball.state != Ball::State::InFlight) return;
    const Vec3 before = ball.pos;
    step_ballistic(ball.pos, ball.vel, gravity(), kTickSeconds);
    if (state_.phase != PlayPhase::Live) return;
    if (type == PlayType::FieldGoal) {
        field_goal_flight(before);
        return;
    }
    if (type == PlayType::Kickoff && !fc_read_ && kick_time_ && now >= *kick_time_ + hang_ - t.fc_look) {
        read_fair_catch();
    }
    field_kick(before);
    if (ball.state != Ball::State::InFlight || state_.phase != PlayPhase::Live) return;
    if (ball.pos.z <= 0.0) {
        const double f = before.z / (before.z - ball.pos.z);
        ball.pos = before + (ball.pos - before) * f;
        ball.pos.z = 0.0;
        // A line drive skids and bounces short; a high kick takes a bigger hop.
        const double keep = result_.kind == "onside" ? 0.3 : result_.kind == "squib" ? 0.4 : 0.55;
        ball.vel = {ball.vel.x * keep, ball.vel.y * keep, -ball.vel.z * 0.5};
        kick_landed();
    }
}

void KickSim::check_rules() {
    if (state_.phase != PlayPhase::Live) return;
    const int c = state_.carrier();
    if (c >= 0 && !kicking(c) && kicked_) {
        const Vec2 p = state_.agent(c).pos;
        const double y = state_.frame.to_play(p).y;
        if (y >= state_.frame.goal_line_y() && p.y >= 0.0 && p.y <= kFieldWidth) {
            whistle("touchdown", p);
            return;
        }
        if (p.y < 0.0 || p.y > kFieldWidth || p.x < 0.0 || p.x > kFieldLength) {
            whistle(y <= state_.frame.own_goal_line_y() ? "touchback" : "returned", p);
            return;
        }
    }
    if (state_.since_snap() >= kMaxSeconds) whistle(c >= 0 ? "returned" : "downed", world_.ball().pos.ground());
}

void KickSim::handle_tackle(const TackleResult& r) {
    if (state_.phase != PlayPhase::Live) return;
    const int c = state_.carrier();
    if (c < 0) return;
    if (r.fumble) {
        fumble(c);
        return;
    }
    if (!r.down) return;
    // Down in his own end zone after fielding it there: a touchback.
    const bool end_zone = state_.frame.to_play(r.spot).y <= state_.frame.own_goal_line_y();
    whistle(end_zone ? "touchback" : "returned", r.spot);
}

void KickSim::fumble(int carrier) {
    const TacklingTuning& t = data_.tuning.tackling;
    Rng& rng = world_.rng();
    const Agent& a = state_.agent(carrier);
    Ball& ball = world_.ball();
    fumbled_ = true;
    ball.state = Ball::State::Loose;
    ball.carrier = -1;
    ball.pos = {a.pos.x, a.pos.y, 1.0};
    const double angle = rng.uniform(0.0, 2.0 * std::numbers::pi);
    const double speed = rng.uniform(t.out_speed_min, t.out_speed_max);
    const Vec2 out = a.vel * 0.5 + Vec2{std::cos(angle), std::sin(angle)} * speed;
    ball.vel = {out.x, out.y, rng.uniform(t.out_up_min, t.out_up_max)};
    loose_since_ = state_.since_snap();
    event("fumble", {{"player", state_.slot_name(carrier)}});
    state_.engagements.clear();
    tackles_.reset_carrier();
    everyone_loose(true);
}

void KickSim::whistle(const std::string& result, Vec2 spot) {
    state_.phase = PlayPhase::Dead;
    state_.engagements.clear();
    state_.drag.reset();
    dead_tick_ = world_.tick();
    result_.result = result;
    result_.seconds = state_.since_snap();
    const Vec2 kp = kframe_.to_play(spot);
    result_.end_x = kp.x;
    result_.end_y = kp.y;
    if (result == "touchdown") result_.end_y = 0.0;
    const PlayType type = k_.play->type;
    result_.kicking_ball = result == "recovered_kicking" || type == PlayType::FieldGoal;
    if (type == PlayType::Kickoff) {
        result_.clock_seconds = touch_time_ ? std::max(0.0, result_.seconds - *touch_time_) : 0.0;
    } else {
        result_.clock_seconds = result_.seconds;
    }
    if (type != PlayType::FieldGoal) {
        const double from = 0.0;  // the kick spot or the line of scrimmage
        double kicked_to = fielded_at_ ? ky(*fielded_at_) : kp.y;
        if (result == "touchback") kicked_to = std::min(kicked_to, kframe_.goal_line_y());
        result_.kick_distance = kicked_to - from;
        if (fielded_at_ && result != "fair_catch") result_.return_yards = ky(*fielded_at_) - result_.end_y;
        if (result == "touchdown") result_.return_yards = fielded_at_ ? ky(*fielded_at_) : 0.0;
        result_.hang = hang_;
    }
    if (const int tk = tackles_.stats().tackler_index; tk >= 0) result_.tackler_id = state_.agent(tk).id;
    event(result, {{"x", kp.x}, {"y", kp.y}});
}

// ---------------------------------------------------------------------------
// Tracking
// ---------------------------------------------------------------------------

void KickSim::write_header() {
    if (!out_) return;
    TrackingInfo info;
    info.kind = "play";
    info.name = k_.play->name + " vs " + r_.play->name;
    info.seed = opt_.seed;
    const Team& home = opt_.kicking_home ? *k_.team : *r_.team;
    const Team& away = opt_.kicking_home ? *r_.team : *k_.team;
    info.home = {home.name, home.abbr, home.color_primary};
    info.away = {away.name, away.abbr, away.id == home.id ? away.color_secondary : away.color_primary};
    info.details = {{"kicking", k_.team->id},
                    {"kick_play", k_.play->id},
                    {"receiving", r_.team->id},
                    {"return_play", r_.play->id},
                    {"yard_line", opt_.yard_line},
                    {"hash", std::string(to_name(kHashNames, opt_.hash))},
                    // Whether the home team attacks +x (the viewer's vertical view uses it).
                    {"home_right", opt_.kicking_right == opt_.kicking_home},
                    // The team with the ball at the snap (the viewer's "behind offense").
                    {"offense", opt_.kicking_home ? "home" : "away"}};
    const double x = kframe_.ball().x;
    info.overlays.push_back(shapes::line("lines", {{x, 0.0}, {x, kFieldWidth}}, "#4fc3f7"));
    out_->header(info, world_);
}

void KickSim::write_frame() {
    if (!out_) return;
    Json dbg = Json::array();
    for (int i = 0; i < state_.count(); ++i) state_.behavior(i)->draw(state_, i, dbg);
    blocking_.draw(dbg);
    tackles_.draw(dbg);
    if (kicked_ && world_.ball().state == Ball::State::InFlight) {
        dbg.push_back(shapes::line("throw", {kick_from_, landing_}, "#ffd400", true));
    }
    out_->frame(world_, std::move(dbg));
}

KickResult KickSim::run() {
    setup();
    result_.start = spots_of(world_);
    write_header();
    state_.phase = PlayPhase::Live;
    state_.snap_tick = world_.tick();
    for (int i = 0; i < state_.count(); ++i) {
        const auto idx = static_cast<std::size_t>(i);
        const Assignment* a = state_.slots[idx].assignment;
        state_.behaviors[idx] = a && !a->sequence.empty()
                                    ? job_behavior(i, a->sequence.front())
                                    : std::make_unique<Wait>("stand", state_.agent(i).pos, kick_dir(), 1e9);
    }
    record_history();
    if (k_.play->type != PlayType::Kickoff) {
        // The long snap.
        const SpecialTuning& t = st();
        Ball& ball = world_.ball();
        const int to = k_.play->type == PlayType::Punt ? kicker_ : holder_;
        const Agent& r = state_.agent(to);
        const Curve& speed = k_.play->type == PlayType::Punt ? t.punt_snap_speed : t.fg_snap_speed;
        const double v = speed(state_.agent(snapper_).ratings[Attr::Snapping]);
        const Vec3 from{ball.pos.x, ball.pos.y, 0.3};
        const Vec3 at{r.pos.x, r.pos.y, 0.9};
        ball.pos = from;
        ball.vel = launch_velocity(from, at, v, gravity()).value_or(max_range_velocity(from, at, v));
        ball.state = Ball::State::InFlight;
        snap_in_flight_ = true;
        event("snap");
    }
    write_frame();

    const auto post_ticks =
        static_cast<std::int64_t>(std::round(data_.tuning.play.post_whistle_seconds * kTicksPerSecond));
    std::vector<Intent> intents(n());
    while (true) {
        Actions actions;
        std::fill(intents.begin(), intents.end(), Intent{});
        if (state_.phase == PlayPhase::Live) live_intents(intents, actions);
        if (state_.phase == PlayPhase::Dead) tackles_.settle(intents);  // the rest pull up
        apply(actions);
        if (state_.phase == PlayPhase::Live) {
            blocking_.steer(intents);
            tackles_.steer(intents);
        }
        for (int i = 0; i < state_.count(); ++i) {
            world_.agents()[static_cast<std::size_t>(i)].grounded = state_.knocked_down(i);
        }
        world_.step(intents);
        if (state_.phase == PlayPhase::Live) {
            for (int i = 0; i < state_.count(); ++i) {
                const auto idx = static_cast<std::size_t>(i);
                distance_[idx] += state_.agent(i).vel.length() * kTickSeconds;
                if (state_.engaged(i)) engaged_[idx] += kTickSeconds;
            }
            blocking_.interact(world_.rng());
            if (kicked_) handle_tackle(tackles_.interact(world_.rng()));
            record_history();
        }
        ball_phase();
        check_rules();
        write_frame();
        if (state_.phase == PlayPhase::Dead && world_.tick() - dead_tick_ >= post_ticks) break;
    }
    result_.end = spots_of(world_);

    const TackleStats& ts = tackles_.stats();
    for (int i = 0; i < state_.count(); ++i) {
        const auto idx = static_cast<std::size_t>(i);
        PlayerUsage u;
        u.id = state_.agent(i).id;
        u.offense = kicking(i);  // here: true for the kicking team
        u.distance = distance_[idx];
        u.engaged = engaged_[idx];
        for (const auto& [carrier, tackler] : ts.contacts) {
            if (carrier == i || tackler == i) ++u.contacts;
        }
        result_.usage.push_back(u);
    }

    std::ostringstream s;
    const PlayType type = k_.play->type;
    if (type == PlayType::FieldGoal) {
        s << fixed1(result_.kick_distance) << " yd field goal: " << result_.result;
    } else {
        s << (type == PlayType::Kickoff ? "Kickoff (" + result_.kind + ")" : std::string("Punt")) << " "
          << fixed1(result_.kick_distance) << " yd: " << result_.result;
        if (result_.result == "returned" || result_.result == "touchdown") {
            s << ", " << fixed1(result_.return_yards) << " yd return";
        }
    }
    result_.summary = s.str();
    return result_;
}

const Team& find_team(const DataSet& data, const std::string& id) {
    const auto it = data.teams.find(id);
    if (it == data.teams.end()) throw PlayError("unknown team '" + id + "'");
    return it->second;
}

Sides side_for(const DataSet& data, const std::string& team_id, const std::string& play_id) {
    Sides s;
    s.team = &find_team(data, team_id);
    const auto roster = data.rosters.find(team_id);
    if (roster == data.rosters.end()) throw PlayError("no roster for team '" + team_id + "'");
    s.roster = &roster->second;
    const auto book = data.playbooks.find(s.team->special_playbook);
    if (book == data.playbooks.end()) throw PlayError(team_id + " has no special teams playbook");
    s.book = &book->second;
    s.play = s.book->find(play_id);
    if (!s.play) throw PlayError("playbook " + book->first + " has no play '" + play_id + "'");
    const auto f = data.formations.find(s.play->formation);
    if (f == data.formations.end()) throw PlayError("unknown formation '" + s.play->formation + "'");
    s.formation = &f->second;
    return s;
}

}  // namespace

namespace {

KickResult run_or_start(const DataSet& data, const KickOptions& options, TrackingWriter* out, bool start_only) {
    const Sides kick = side_for(data, options.kicking_team, options.kick_play);
    const Sides recv = side_for(data, options.receiving_team, options.return_play);
    const PlayType t = kick.play->type;
    if (t != PlayType::Kickoff && t != PlayType::Punt && t != PlayType::FieldGoal) {
        throw PlayError(kick.play->id + " isn't a kickoff, punt, or field goal");
    }
    const PlayType want = t == PlayType::Kickoff ? PlayType::KickoffReturn
                          : t == PlayType::Punt  ? PlayType::PuntReturn
                                                 : PlayType::FieldGoalBlock;
    if (recv.play->type != want) {
        throw PlayError(recv.play->id + " can't answer " + kick.play->id);
    }
    if (options.yard_line <= 0.0 || options.yard_line >= 100.0) throw PlayError("yard line must be between 0 and 100");
    KickSim sim(data, options, kick, recv, out);
    if (start_only) {
        KickResult r;
        r.start = sim.start();
        return r;
    }
    return sim.run();
}

}  // namespace

KickResult run_kick(const DataSet& data, const KickOptions& options, TrackingWriter* out) {
    return run_or_start(data, options, out, false);
}

std::vector<Spot> kick_start(const DataSet& data, const KickOptions& options) {
    return run_or_start(data, options, nullptr, true).start;
}

}  // namespace cfbsim
