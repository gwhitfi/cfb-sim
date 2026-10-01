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
    std::optional<Throw> throw_ball;
    std::optional<Handoff> handoff;
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
    // Receivers: seconds after the snap of his latest break.
    virtual double last_break() const { return -std::numeric_limits<double>::infinity(); }
};

// One player's place on this play.
struct SlotInfo {
    std::string slot;  // "X", "LT"
    const FormationSlot* formation_slot = nullptr;
    const Assignment* assignment = nullptr;  // nullptr: the QB, or a run play's carrier
    Vec2 aligned;                            // play frame
    double scheme_pk = 50.0;                 // Playbook Knowledge in this playbook's scheme
    double familiarity = 100.0;              // Position Familiarity at the slot's position
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

    // Field-frame unit vector pointing downfield (the offense's way).
    Vec2 downfield() const { return frame.dir_to_field({0.0, 1.0}); }
    // The way player i's team is going.
    Vec2 downfield_for(int i) const { return is_offense(i) ? downfield() : downfield() * -1.0; }
    Vec2 play_pos(int i) const { return frame.to_play(agent(i).pos); }

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
