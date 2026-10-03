#include "cfbsim/world/world.h"

#include <algorithm>
#include <stdexcept>

namespace cfbsim {

Vec3 carry_spot(const Agent& carrier) {
    const Vec2 at = carrier.pos + carrier.facing * (carrier.body.radius * 0.8);
    return {at.x, at.y, carrier.z + 1.0};
}

int World::add_agent(Agent agent) {
    if (tick_ != 0) throw std::logic_error("World::add_agent: agents must be added before step()");
    agents_.push_back(std::move(agent));
    return static_cast<int>(agents_.size()) - 1;
}

void World::step(std::span<const Intent> intents) {
    if (intents.size() != agents_.size()) {
        throw std::invalid_argument("World::step: need exactly one Intent per agent");
    }

    // Phases 1-2, Perceive and Decide, happen in the caller for now (it built
    // `intents`). Player behaviors move in here in phase 4.

    // Phase 3: Move.
    // A player in the air can't steer: he keeps the velocity he took off
    // with and comes down under gravity.
    for (std::size_t i = 0; i < agents_.size(); ++i) {
        Agent& a = agents_[i];
        if (a.airborne()) {
            a.pos += a.vel * kTickSeconds;
            a.vz -= tuning_.gravity * kTickSeconds;
            a.z += a.vz * kTickSeconds;
            if (a.z <= 0.0) {
                a.z = 0.0;
                a.vz = 0.0;
            }
            continue;
        }
        move_agent(a, intents[i], tuning_, kTickSeconds);
    }

    // Phase 4: Interact. Only soft collisions so far; engagements and tackles
    // come in phases 6-7.
    separate_agents(agents_, tuning_);

    // Phase 5: Ball. A held ball rides in front of the carrier at waist height
    // (just caught, it's still coming in from where he caught it).
    if (ball_.state == Ball::State::Held && ball_.carrier >= 0) {
        const Agent& carrier = agents_[static_cast<std::size_t>(ball_.carrier)];
        ball_.pos = carry_spot(carrier);
        if (ball_.hold_left > 0.0 && ball_.hold_carrier == ball_.carrier) {
            ball_.hold_left = std::max(0.0, ball_.hold_left - kTickSeconds);
            ball_.pos = ball_.pos + ball_.hold_offset * (ball_.hold_left / ball_.hold_total);
        }
        ball_.vel = {carrier.vel.x, carrier.vel.y, carrier.vz};
    }

    // Phase 6: Events and rules: phase 8.

    ++tick_;
}

}  // namespace cfbsim
