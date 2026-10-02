// Engagements: forming blocks, the per-tick contest (control, shed,
// leverage), moving engaged pairs together, rush moves, pancakes, and holds
// (internal; movement-and-interaction.md#engagements).
#pragma once

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cfbsim/core/rng.h"
#include "play/play_state.h"

namespace cfbsim {

struct BlockingStats {
    int sheds = 0;
    int pancakes = 0;
    int rush_moves_won = 0;
    std::vector<std::string> holds;  // slot names of blockers who held
};

class Engagements {
  public:
    using EventFn = std::function<void(std::string_view, Json)>;

    // The engagement list itself lives in `play` (behaviors read it); this
    // class changes it. `event` writes a tracking event.
    Engagements(PlayState& play, EventFn event) : play_(play), event_(std::move(event)) {}

    // Sizes the per-agent arrays (call once all agents exist).
    void setup();

    // Resolves the rush moves behaviors asked for this tick.
    void resolve_moves(std::span<const Actions::RushMove> moves, Rng& rng);

    // Turns engaged players' intents into the pair's shared movement, and
    // stops knocked-down players. Called after Decide, before Move.
    void steer(std::span<Intent> intents) const;

    // The Interact step, after Move: ends, fights, and forms engagements.
    void interact(Rng& rng);

    void draw(Json& shapes) const;
    const BlockingStats& stats() const { return stats_; }

  private:
    void end_broken();
    void fight(Rng& rng);
    void form();
    // Which way the defender is trying to go, across the field (+1 / -1 in
    // the play frame's x), and the leverage and turn that follow from it.
    double goal_side(int defender) const;
    void set_leverage(Engagement& e, double leverage) const;
    double blocker_skill(const Engagement& e) const;

    PlayState& play_;
    EventFn event_;
    BlockingStats stats_;
};

}  // namespace cfbsim
