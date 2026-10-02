#include "cfbsim/world/agent.h"

#include <algorithm>
#include <cmath>

namespace cfbsim {

Attributes effective_attributes(const Attributes& base, const RatingModifiers& mods) {
    const double factor = mods.product();
    Attributes out;
    for (const AttrInfo& row : kAttrInfo) {
        out[row.attr] = std::clamp(base[row.attr] * factor, 1.0, 100.0);
    }
    return out;
}

Body make_body(const Measurables& m, const MovementTuning& tuning) {
    Body b;
    b.radius = tuning.radius_by_weight(m.weight_lb);
    b.reach = b.radius + tuning.reach_arm_factor * (m.arm_in / 36.0);  // inches -> yards
    b.mass = m.weight_lb;
    b.height = m.height_in / 36.0;
    return b;
}

MovementProfile make_movement_profile(const Attributes& effective, const MovementTuning& tuning) {
    MovementProfile p;
    p.top_speed = tuning.top_speed(effective[Attr::Speed]);
    // Exponential approach to top speed reaches 90% after t90 seconds when
    // e^(-k * t90) = 0.1, so k = ln(10) / t90.
    p.accel_rate = std::log(10.0) / tuning.t90(effective[Attr::Acceleration]);
    p.decel = tuning.decel(effective[Attr::Agility]);
    p.lateral_accel = tuning.lateral_accel(effective[Attr::Agility]);
    p.agility = effective[Attr::Agility];
    return p;
}

Agent make_agent(const Player& player, TeamSide team, std::string label,
                 const RatingModifiers& mods, const MovementTuning& tuning) {
    Agent a;
    a.id = player.id;
    a.name = player.first_name.empty() ? player.last_name
                                       : player.first_name.substr(0, 1) + ". " + player.last_name;
    a.label = std::move(label);
    a.team = team;
    a.jersey = player.jersey;
    a.position = player.position;
    a.ratings = effective_attributes(player.attributes, mods);
    a.dirtiness = player.hidden.dirtiness;
    a.body = make_body(player.measurables, tuning);
    a.move = make_movement_profile(a.ratings, tuning);
    return a;
}

}  // namespace cfbsim
