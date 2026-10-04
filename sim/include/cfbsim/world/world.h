// The world state and the fixed-step tick loop
// (movement-and-interaction.md#tick-loop).
#pragma once

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "cfbsim/core/rng.h"
#include "cfbsim/core/vec.h"
#include "cfbsim/data/tuning.h"
#include "cfbsim/physics/movement.h"
#include "cfbsim/world/agent.h"

namespace cfbsim {

// `inline constexpr` at namespace scope: one shared compile-time constant,
// usable from any file that includes this header.
inline constexpr int kTicksPerSecond = 20;
inline constexpr double kTickSeconds = 1.0 / kTicksPerSecond;

struct Ball {
    // Spotted: on the ground at the line of scrimmage, waiting for the snap.
    enum class State : std::uint8_t { Dead, Spotted, Held, InFlight, Loose };

    State state = State::Dead;
    Vec3 pos;
    Vec3 vel;
    int carrier = -1;  // index into World::agents() while Held
    // Just caught: it eases from where it was caught to the carry spot.
    // `hold_offset` is (catch point - carry spot) at the catch; it shrinks to
    // nothing over `hold_total` s (`hold_left` to go).
    Vec3 hold_offset;
    double hold_left = 0.0;
    double hold_total = 0.0;
    int hold_carrier = -1;  // who caught it (anyone else holds it normally)
};

// Where a held ball rides: in front of him at waist height.
Vec3 carry_spot(const Agent& carrier);

class World {
  public:
    // The World keeps its own copy of the tuning, so it can never be left
    // pointing at tuning data that has gone away.
    World(MovementTuning tuning, std::uint64_t seed) : tuning_(std::move(tuning)), rng_(seed) {}

    // Adds a player and returns his index. Indices are stable: agents are only
    // added before the first step, never removed.
    int add_agent(Agent agent);

    // std::span is a non-owning view of a contiguous array: callers can read
    // and change agents but can't add or remove them.
    std::span<Agent> agents() { return agents_; }
    std::span<const Agent> agents() const { return agents_; }

    Ball& ball() { return ball_; }
    const Ball& ball() const { return ball_; }

    // The match RNG: every random draw in the sim goes through this one engine.
    Rng& rng() { return rng_; }

    const MovementTuning& tuning() const { return tuning_; }

    std::int64_t tick() const { return tick_; }
    // For writers that place frames themselves (the between-plays animation).
    void set_tick(std::int64_t tick) { tick_ = tick; }
    double time() const { return static_cast<double>(tick_) * kTickSeconds; }

    // Runs one tick. `intents` holds one Intent per agent, in agent order: the
    // Decide phase, made by the caller from the state at the end of the
    // previous tick.
    void step(std::span<const Intent> intents);

  private:
    MovementTuning tuning_;
    Rng rng_;
    std::vector<Agent> agents_;
    Ball ball_;
    std::int64_t tick_ = 0;
};

}  // namespace cfbsim
