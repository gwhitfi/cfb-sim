#include "cfbsim/world/world.h"

#include <stdexcept>

namespace cfbsim {

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
    for (std::size_t i = 0; i < agents_.size(); ++i) {
        move_agent(agents_[i], intents[i], tuning_, kTickSeconds);
    }

    // Phase 4: Interact. Only soft collisions so far; engagements and tackles
    // come in phases 6-7.
    separate_agents(agents_, tuning_);

    // Phase 5: Ball. A held ball rides in front of the carrier at waist height.
    if (ball_.state == Ball::State::Held && ball_.carrier >= 0) {
        const Agent& carrier = agents_[static_cast<std::size_t>(ball_.carrier)];
        const Vec2 at = carrier.pos + carrier.facing * (carrier.body.radius * 0.8);
        ball_.pos = {at.x, at.y, 1.0};
        ball_.vel = {carrier.vel.x, carrier.vel.y, 0.0};
    }

    // Phase 6: Events and rules: phase 8.

    ++tick_;
}

}  // namespace cfbsim
