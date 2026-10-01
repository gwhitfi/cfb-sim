// Kinematic movement: turning a behavior's *desired* velocity into the velocity
// a player can actually reach this tick (movement-and-interaction.md#movement-physics).
#pragma once

#include <optional>
#include <span>

#include "cfbsim/core/vec.h"
#include "cfbsim/data/tuning.h"
#include "cfbsim/world/agent.h"

namespace cfbsim {

// What a player's behavior asks for this tick.
struct Intent {
    Vec2 velocity;             // desired velocity, yd/s (physics caps the speed)
    std::optional<Vec2> face;  // direction to face; default: the way he's moving
    bool plant = false;        // allow a planted cut (sharp change of direction)
};

// Advances one agent by `dt` seconds: facing, speed cap for the movement
// direction (run / shuffle / backpedal), turning and planted cuts, then
// acceleration or braking, then position.
void move_agent(Agent& agent, const Intent& intent, const MovementTuning& tuning, double dt);

// Fraction of speed kept through a planted cut of `angle_rad` (0..pi).
double cut_speed_kept(double angle_rad, double agility, const MovementTuning& tuning);

// Top speed multiplier for moving in `move_dir` while facing `facing`
// (both unit vectors): 1 running forward, shuffle sideways, backpedal backward.
double direction_factor(Vec2 facing, Vec2 move_dir, const MovementTuning& tuning);

// Soft collisions: pushes apart overlapping players and cancels the speed at
// which they're still moving into each other; heavier players give less.
// All pushes are computed first and then applied, so the result doesn't depend
// on the order of the players.
void separate_agents(std::span<Agent> agents, const MovementTuning& tuning);

}  // namespace cfbsim
