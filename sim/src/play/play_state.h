// The state of one play that every player behavior can read, the Behavior
// base class, and the actions behaviors can ask for (internal).
#pragma once

#include <cstdint>
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
    bool attempted = false;  // the target has tried to catch it
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
          world_(world) {}

    const DataSet& data;
    const Play& play;
    const Formation& formation;
    const Playbook& book;
    const PlayFrame frame;

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

    // Agent index of a slot, or -1.
    int index_of(std::string_view slot) const {
        for (int i = 0; i < count(); ++i) {
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
    bool has_ball(int i) const {
        const Ball& b = world_.ball();
        return b.state == Ball::State::Held && b.carrier == i;
    }

    // Field-frame unit vector pointing downfield.
    Vec2 downfield() const { return frame.dir_to_field({0.0, 1.0}); }
    Vec2 play_pos(int i) const { return frame.to_play(agent(i).pos); }

  private:
    World& world_;
};

// How open a receiver is: the chance a good throw right now is completed
// (movement-and-interaction.md#openness). With no defenders yet every
// receiver is wide open. Phase 5 replaces this with the real model.
inline double openness(const PlayState& /*play*/, int /*receiver*/) { return 1.0; }

}  // namespace cfbsim
