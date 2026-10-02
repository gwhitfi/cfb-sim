// A player on the field: who he is, his ratings for this play, his body, and
// his current position, velocity, and facing.
#pragma once

#include <cstdint>
#include <string>

#include "cfbsim/core/vec.h"
#include "cfbsim/data/attributes.h"
#include "cfbsim/data/enums.h"
#include "cfbsim/data/player.h"
#include "cfbsim/data/tuning.h"

namespace cfbsim {

enum class TeamSide : std::uint8_t { Home, Away };

inline constexpr std::array<EnumName<TeamSide>, 2> kTeamSideNames{{
    {TeamSide::Home, "home"},
    {TeamSide::Away, "away"},
}};

// The multipliers in ratings.md#effective-ratings. All 1.0 until fatigue,
// injuries, form, and pressure are built (phase 8 and later).
struct RatingModifiers {
    double fatigue = 1.0;
    double injury = 1.0;
    double form = 1.0;
    double situational = 1.0;

    double product() const { return fatigue * injury * form * situational; }
};

// base x modifiers, kept within 1-100. The sim reads only effective ratings.
Attributes effective_attributes(const Attributes& base, const RatingModifiers& mods);

// Body size, from measurables (movement-and-interaction.md#body).
struct Body {
    double radius = 0.4;  // yd, for collisions and spacing
    double reach = 1.2;   // yd, contact range for blocks, tackles, and catches
    double mass = 200.0;  // lb
    double height = 2.0;  // yd (6 ft), for release points and catch windows
};

Body make_body(const Measurables& m, const MovementTuning& tuning);

// Movement limits worked out once from effective ratings, so the physics step
// doesn't re-evaluate curves for every player every tick.
struct MovementProfile {
    double top_speed = 9.0;      // yd/s
    double accel_rate = 1.5;     // k in dv/dt = k * (top_speed - v), 1/s
    double decel = 8.0;          // max braking, yd/s^2
    double lateral_accel = 7.0;  // max sideways acceleration, yd/s^2
    double agility = 50.0;       // effective Agility, for planted cuts
};

MovementProfile make_movement_profile(const Attributes& effective, const MovementTuning& tuning);

struct Agent {
    // Identity (copied from the Player, so an Agent never points back into a
    // roster that might be moved or freed).
    std::string id;
    std::string name;   // "J. Smith"
    std::string label;  // formation slot ("X", "LT") or drill label
    TeamSide team = TeamSide::Home;
    int jersey = 0;
    Position position = Position::WR;

    Attributes ratings;  // effective ratings for this play
    double dirtiness = 50.0;  // hidden: late hits and personal fouls
    Body body;
    MovementProfile move;

    // Kinematic state, in the field frame (plays-and-data.md#field-frame-engine-and-viewer).
    Vec2 pos;
    Vec2 vel;
    Vec2 facing{1.0, 0.0};  // unit vector
    bool carrying = false;  // has the ball
};

// Builds an Agent from a roster player. `label` is his slot on this play.
Agent make_agent(const Player& player, TeamSide team, std::string label,
                 const RatingModifiers& mods, const MovementTuning& tuning);

}  // namespace cfbsim
