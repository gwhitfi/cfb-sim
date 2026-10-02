// Fouls committed during a live play (internal; rules-and-flow.md#first-build-phase-8):
// defensive holding, pass interference both ways, roughing the passer,
// facemask, and intentional grounding. (Offensive holding is rolled in the
// engagement contest.) Rolls use their own RNG stream, so watching for
// fouls never changes how a play plays out.
#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <vector>

#include "cfbsim/core/rng.h"
#include "play/play_state.h"

namespace cfbsim {

class FoulWatch {
  public:
    FoulWatch(PlayState& play, std::uint64_t seed) : play_(play), rng_(seed) {}

    void setup();
    // Every live tick, after Interact: holding in coverage, late hits on the QB.
    void tick();
    // The pass reaches its target: who was on him, and was it contested?
    void arrival(int target, bool contested);
    // The QB threw it away.
    void throw_away(int qb);
    // After the whistle: interference (now that we know if it was caught)
    // and facemasks on the tackle tries.
    void finish(bool caught, std::span<const std::pair<int, int>> contacts);

  private:
    // (1.5 - Discipline/Concentration blend/100): how foul-prone he is.
    double careless(int i) const;
    // Personal fouls: x (0.5 + Aggression/Dirtiness blend/100).
    double nasty(int i) const;
    void commit(const char* type, int player, Vec2 at);

    struct Arrival {
        int target = -1;
        int defender = -1;  // within range of him, or -1
        bool contested = false;
        Vec2 at;
    };

    PlayState& play_;
    Rng rng_;
    std::set<std::pair<int, int>> hold_checked_;  // (receiver, defender)
    std::vector<bool> late_checked_;
    std::optional<Arrival> arrival_;
};

}  // namespace cfbsim
