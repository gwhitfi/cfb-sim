// The state of one play that every player behavior can read, the Behavior
// base class, and the actions behaviors can ask for (internal).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cfbsim/data/load.h"
#include "cfbsim/field/geometry.h"
#include "cfbsim/physics/movement.h"
#include "cfbsim/world/world.h"

namespace cfbsim {

class PlayState;

// Things a behavior can ask for during Decide. They are carried out after
// every player has decided, so no player sees another's action early (the
// tick loop's "read the previous tick only" rule).
struct Actions {
    struct Throw {
        int from = -1;
        int target = -1;
        bool away = false;  // throw it away (out of bounds past `target`)
    };
    struct Handoff {
        int from = -1;
        int to = -1;
    };
    // A rusher's move against the blocker he's engaged with.
    struct RushMove {
        int from = -1;
        bool power = true;  // power (bull rush, long arm) or finesse (swim, rip, spin)
    };
    // A ball carrier's move against a tackler about to reach him.
    enum class Move : std::uint8_t { Juke, Spin, StiffArm, Shoulder };
    struct Evasion {
        int from = -1;
        int tackler = -1;
        Move move = Move::Juke;
    };
    std::optional<Throw> throw_ball;
    std::optional<Handoff> handoff;
    std::vector<RushMove> rush_moves;  // any number of rushers can try one on the same tick
    std::optional<Evasion> evasion;
    // Events a behavior wants written to the tracking file (a run read).
    std::vector<std::pair<std::string, Json>> notes;
};

std::string_view to_string(Actions::Move move);

// The result of an evasion move, kept while it applies to that tackler's try
// (movement-and-interaction.md#evasion-moves).
struct EvasionResult {
    int carrier = -1;
    int tackler = -1;
    Actions::Move move = Actions::Move::Juke;
    bool won = false;
    double bonus = 0.0;   // + to the carrier's resistance (won stiff arm, shoulder)
    double penalty = 0.0; // + to the tackler's score (a failed juke or spin)
    double fall = 0.0;    // yd of extra forward fall (shoulder)
    double until = 0.0;   // s after the snap
};

// A drag-down in progress: the tackler is wrapped up on the carrier.
// A foul that happened on the field (flagged or not, the rules layer decides).
struct CommittedFoul {
    std::string type;  // a penalty type in the rules table ("offensive_holding")
    int player = -1;
    Vec2 at;           // field frame
    double time = 0.0; // s after the snap
};

struct DragDown {
    int carrier = -1;
    int tackler = -1;
    Vec2 start;           // field frame
    double distance = 0.0;  // yd he'll drag him
    double best_y = 0.0;  // play frame: forward progress so far
};

// How a blocker is blocking, which picks the skill blend for his engagements.
enum class BlockStyle : std::uint8_t { None, Pass, Run, Space };

// A blocker and a defender locked together
// (movement-and-interaction.md#blocking--pass-rush-phase-6).
struct Engagement {
    int blocker = -1;
    int defender = -1;
    int helper = -1;  // a second blocker (double team), or -1
    BlockStyle style = BlockStyle::Run;
    double control = 0.0;   // -1..1, positive = the blocker is winning
    double shed = 0.0;      // 0..1, the defender is free at 1
    double leverage = 0.0;  // -1..1, positive = the blocker is on the side the defender wants
    double turn = 1.0;      // +1 / -1: which way the defender works around the blocker
    Vec2 axis;              // field frame: blocker -> defender at contact
    std::int64_t start_tick = 0;
    std::int64_t helper_tick = 0;  // when the helper joined
    bool hold_checked = false;
    // The rusher's latest move: when, and whether it worked.
    double last_move = -1.0;
    bool last_move_won = false;
    bool last_move_power = true;
};

// ---------------------------------------------------------------------------
// A player behavior: what one player is doing right now (running a route,
// pass setting, carrying the ball, ...). Each tick it reads the play and
// returns the Intent the physics layer turns into movement.
//
// This is classic C++ runtime polymorphism. `Behavior` is an abstract base
// class: `virtual ... = 0` declares a function without a body ("pure
// virtual"), so you can't create a plain Behavior, only a subclass that
// provides one. Code that holds a `Behavior&` or `std::unique_ptr<Behavior>`
// calls `decide()` and the call goes to whichever subclass the object really
// is, looked up at run time through the object's vtable.
// ---------------------------------------------------------------------------
// What a player shows the defense at the snap (movement-and-interaction.md#play-recognition).
enum class Key : std::uint8_t { None, PassSet, RunBlock, Drop, Handoff };

class Behavior {
  public:
    // A base class with virtual functions needs a virtual destructor, so
    // deleting a RouteRunner through a Behavior pointer runs RouteRunner's
    // destructor too. `= default` asks for the normal one.
    virtual ~Behavior() = default;

    virtual std::string_view name() const = 0;

    // The Decide phase. `self` is this player's agent index. Behaviors may
    // change their own state (which waypoint they're on) but only read the
    // play: `const PlayState&` makes the compiler enforce that.
    virtual Intent decide(const PlayState& play, int self, Actions& actions) = 0;

    // True when this job is over and the next one in his assignment starts
    // (a back's pass protection check before he releases into a route).
    virtual bool done(const PlayState& /*play*/, int /*self*/) const { return false; }

    // Where he expects to be `seconds` from now (field frame), if he knows
    // his path. The QB uses it to lead receivers.
    virtual std::optional<Vec2> predict(const PlayState& /*play*/, int /*self*/,
                                        double /*seconds*/) const {
        return std::nullopt;
    }

    // Debug overlay shapes for this tick (see TrackingWriter).
    virtual void draw(const PlayState& /*play*/, int /*self*/, Json& /*shapes*/) const {}

    // The key this player shows the defense (OL: pass set or run block; QB:
    // drop or handoff).
    virtual Key key() const { return Key::None; }
    // QB: the receiver he's looking at (agent index), or -1.
    virtual int eyes() const { return -1; }
    // Man coverage: the receiver he's covering, or -1.
    virtual int man_target() const { return -1; }
    // Any coverage: the receiver he's playing right now (his man, or the
    // threat in his zone), or -1.
    virtual int covering() const { return -1; }
    // For the record only (the coverage event at the throw), never for
    // decisions: his pass job ("man", "zone:deep_half_weak", "rush") and the
    // receiver he's responsible for right now, including the one a deep zone
    // is carrying (which covering() leaves out, so openness isn't changed).
    virtual std::string job() const { return std::string(name()); }
    virtual int responsible() const { return covering(); }
    // Receivers: seconds after the snap of his latest break.
    virtual double last_break() const { return -std::numeric_limits<double>::infinity(); }
    // Blockers: the defender he's going for (agent index), or -1, and how
    // he's blocking (None: he isn't a blocker right now).
    virtual int block_target() const { return -1; }
    virtual BlockStyle block_style() const { return BlockStyle::None; }
    // QB: the pressure he feels (0..1).
    virtual double pressure() const { return 0.0; }
    // Running with the ball (a ball carrier, or a QB who has tucked it), and
    // whether he has picked his lane (past his read: fits end).
    virtual bool carrying() const { return false; }
    virtual bool committed() const { return false; }
};

// One player's place on this play.
struct SlotInfo {
    std::string slot;  // "X", "LT"
    const FormationSlot* formation_slot = nullptr;
    const Assignment* assignment = nullptr;  // nullptr: the QB, or a run play's carrier
    Vec2 aligned;                            // play frame
    double scheme_pk = 50.0;                 // Playbook Knowledge in this playbook's scheme
    double familiarity = 100.0;              // Position Familiarity at the slot's position
    bool late = false;                       // a defender who started the play away from `aligned`
};

enum class PlayPhase : std::uint8_t { PreSnap, Live, Dead };

// The pass in the air (or caught, or on the ground).
struct PassInfo {
    int thrower = -1;
    int target = -1;
    std::int64_t tick = 0;  // release tick
    Vec3 release;
    Vec3 aim;                // where the QB meant it to go, before placement error
    bool away = false;       // thrown away: nobody plays it
};

// Where a player was `lag` seconds ago, as he looked then.
struct Seen {
    Vec2 pos;  // extended along `vel` to now (dead reckoning)
    Vec2 vel;
};

class PlayState {
  public:
    // (The parameters are named differently from the members they set, so
    // the compiler's -Wshadow check doesn't flag them.)
    PlayState(const DataSet& data_set, const Play& the_play, const Formation& the_formation,
              const Playbook& playbook, PlayFrame play_frame, World& world)
        : data(data_set),
          play(the_play),
          formation(the_formation),
          book(playbook),
          frame(play_frame),
          // The defense is never flipped: its sides come from the strength call.
          defense_frame(play_frame.unflipped()),
          world_(world) {}

    const DataSet& data;
    const Play& play;
    const Formation& formation;
    const Playbook& book;
    const PlayFrame frame;
    const PlayFrame defense_frame;

    // The defense, if there is one. Agents [0, offense_count) are the
    // offense and the rest the defense.
    const Play* defense_play = nullptr;
    const Formation* defense_formation = nullptr;
    const Playbook* defense_book = nullptr;
    int offense_count = 0;
    std::optional<OffenseRead> offense;  // the offense as the defense read it at the snap

    std::vector<SlotInfo> slots;  // one per agent, in agent order
    int qb = -1;
    int center = -1;
    int run_carrier = -1;  // run plays: the designed ball carrier

    PlayPhase phase = PlayPhase::PreSnap;
    std::int64_t snap_tick = 0;
    std::optional<PassInfo> pass;
    std::optional<LineGaps> gaps;  // set at the snap

    // Per agent: how long after the snap he starts, and after a throw how
    // long until he notices it (drawn when it happens).
    std::vector<double> get_off;
    std::vector<double> throw_reaction;
    // The handoff, and when each player notices it.
    std::optional<std::int64_t> handoff_tick;
    std::vector<double> handoff_reaction;

    // Every player's position and velocity on every tick since the snap, so
    // defenders can react to what they saw a moment ago.
    struct Snapshot {
        Vec2 pos;
        Vec2 vel;
    };
    std::vector<std::vector<Snapshot>> history;

    // Openness of each receiver this tick (1 when there's no defense, or for
    // players who aren't running routes).
    std::vector<double> open;

    // Blocking: the engagements in progress, and per agent the time (seconds
    // after the snap) until which he is knocked down or recovering from a
    // shed. `immune` pairs: a blocker who can't re-engage the man who shed him.
    std::vector<Engagement> engagements;
    std::vector<double> down_until;
    std::vector<double> recover_until;
    struct Immunity {
        int blocker = -1;
        int defender = -1;
        double until = 0.0;
    };
    std::vector<Immunity> immune;

    // Tackling (phase 7): per agent, stumbling until (s after the snap) at a
    // share of top speed; evasion results in force; a drag-down in progress.
    std::vector<double> stumble_until;
    std::vector<double> stumble_share;
    std::vector<EvasionResult> evasions;
    std::optional<DragDown> drag;
    // Fouls committed on this play (phase 8).
    std::vector<CommittedFoul> fouls;

    // Behaviors, one per agent. The vector owns them: std::unique_ptr deletes
    // each behavior when it is replaced or when the PlayState goes away.
    std::vector<std::unique_ptr<Behavior>> behaviors;

    // --- read access -----------------------------------------------------
    const World& world() const { return world_; }
    World& world() { return world_; }
    const Tuning& tuning() const { return data.tuning; }
    const Agent& agent(int i) const { return world_.agents()[static_cast<std::size_t>(i)]; }
    const Behavior* behavior(int i) const { return behaviors[static_cast<std::size_t>(i)].get(); }
    int count() const { return static_cast<int>(slots.size()); }

    bool is_offense(int i) const { return i < offense_count; }
    bool has_defense() const { return count() > offense_count; }
    bool teammates(int a, int b) const { return is_offense(a) == is_offense(b); }

    // Agent index of an offensive slot, or -1.
    int index_of(std::string_view slot) const {
        for (int i = 0; i < offense_count; ++i) {
            if (slots[static_cast<std::size_t>(i)].slot == slot) return i;
        }
        return -1;
    }
    const std::string& slot_name(int i) const { return slots[static_cast<std::size_t>(i)].slot; }

    bool snapped() const { return phase != PlayPhase::PreSnap; }
    double since_snap() const {
        return snapped() ? static_cast<double>(world_.tick() - snap_tick) * kTickSeconds : 0.0;
    }
    // He has reacted to the snap and is moving.
    bool started(int i) const {
        return snapped() && since_snap() >= get_off[static_cast<std::size_t>(i)];
    }
    // He has noticed the pass is in the air.
    bool sees_throw(int i) const {
        if (!pass) return false;
        const double since = static_cast<double>(world_.tick() - pass->tick) * kTickSeconds;
        return since >= throw_reaction[static_cast<std::size_t>(i)];
    }
    bool sees_handoff(int i) const {
        if (!handoff_tick) return false;
        const double since = static_cast<double>(world_.tick() - *handoff_tick) * kTickSeconds;
        return since >= handoff_reaction[static_cast<std::size_t>(i)];
    }
    bool has_ball(int i) const {
        const Ball& b = world_.ball();
        return b.state == Ball::State::Held && b.carrier == i;
    }
    // The player holding the ball, or -1.
    int carrier() const {
        const Ball& b = world_.ball();
        return b.state == Ball::State::Held ? b.carrier : -1;
    }
    // The runner settings for whoever has the ball: the designed runner's (the
    // handoff carrier, the QB) or everyone else's (after a catch, a return).
    RunnerTuning runner(int c) const {
        const Tuning& t = tuning();
        if (c >= 0 && (c == qb || c == run_carrier)) return t.carrier.designed_run;
        RunnerTuning r;
        r.move_edge = t.carrier.move_edge;
        r.move_min = t.carrier.move_min;
        r.juke_stumble = t.carrier.juke_stumble;
        r.spin_keep = t.carrier.spin_keep;
        r.broken_base = t.tackling.broken_base;
        r.broken_keep_min = t.tackling.broken_keep_min;
        r.broken_keep_max = t.tackling.broken_keep_max;
        r.drag_min = t.tackling.drag_min;
        r.drag_max = t.tackling.drag_max;
        r.fall_max = t.tackling.fall_max;
        r.lunge_max = t.tackling.lunge_max;
        return r;
    }

    // Field-frame unit vector pointing downfield (the offense's way).
    Vec2 downfield() const { return frame.dir_to_field({0.0, 1.0}); }
    // The way player i's team is going.
    Vec2 downfield_for(int i) const { return is_offense(i) ? downfield() : downfield() * -1.0; }
    Vec2 play_pos(int i) const { return frame.to_play(agent(i).pos); }
    // Where he lines up, as the play reads it at the snap: his slot's spot if
    // he was still on his way there (a late defender: a lineman walking up
    // is still a lineman), else where he stands.
    Vec2 set_pos(int i) const {
        const SlotInfo& s = slots[static_cast<std::size_t>(i)];
        return s.late ? s.aligned : play_pos(i);
    }
    bool late(int i) const { return slots[static_cast<std::size_t>(i)].late; }

    double openness(int i) const {
        return open.empty() ? 1.0 : open[static_cast<std::size_t>(i)];
    }
    // The receiver the QB is looking at, or -1.
    int qb_eyes() const {
        const Behavior* b = qb >= 0 && behaviors.size() > static_cast<std::size_t>(qb)
                                ? behavior(qb)
                                : nullptr;
        return b ? b->eyes() : -1;
    }

    // The engagement player i is in (as blocker, helper, or defender), or nullptr.
    const Engagement* engagement_of(int i) const {
        for (const Engagement& e : engagements) {
            if (e.blocker == i || e.defender == i || e.helper == i) return &e;
        }
        return nullptr;
    }
    bool engaged(int i) const { return engagement_of(i) != nullptr; }
    bool stumbling(int i) const {
        return !stumble_until.empty() && since_snap() < stumble_until[static_cast<std::size_t>(i)];
    }
    // Knocked down or stumbling: seconds until he's back to full speed.
    double down_for(int i) const {
        const auto idx = static_cast<std::size_t>(i);
        double t = 0.0;
        if (!down_until.empty()) t = std::max(t, down_until[idx] - since_snap());
        if (!stumble_until.empty()) t = std::max(t, stumble_until[idx] - since_snap());
        return t;
    }
    bool knocked_down(int i) const {
        return !down_until.empty() && since_snap() < down_until[static_cast<std::size_t>(i)];
    }
    // Some blocker is engaged with, or going for, defender d.
    bool blocked(int d) const {
        for (const Engagement& e : engagements) {
            if (e.defender == d) return true;
        }
        for (int i = 0; i < offense_count; ++i) {
            if (behaviors[static_cast<std::size_t>(i)] &&
                behaviors[static_cast<std::size_t>(i)]->block_target() == d) {
                return true;
            }
        }
        return false;
    }

    // Where player i was `lag` seconds ago, extended along his velocity then.
    Seen seen(int i, double lag) const {
        const auto idx = static_cast<std::size_t>(i);
        if (history.empty()) return {agent(i).pos, agent(i).vel};
        const auto back = static_cast<std::size_t>(std::max(0.0, std::round(lag / kTickSeconds)));
        const std::size_t k = history.size() - 1 - std::min(back, history.size() - 1);
        const Snapshot& s = history[k][idx];
        const double dt = static_cast<double>(history.size() - 1 - k) * kTickSeconds;
        return {s.pos + s.vel * dt, s.vel};
    }

  private:
    World& world_;
};

}  // namespace cfbsim
