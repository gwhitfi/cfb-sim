// Tackles: evasion moves, tackle tries and their outcomes (clean, drag-down,
// broken, missed), forward progress, stumbles, and the fumble roll
// (internal; movement-and-interaction.md#tackles).
#pragma once

#include <array>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "cfbsim/core/rng.h"
#include "play/play_state.h"

namespace cfbsim {

struct TackleStats {
    int tries = 0;
    int broken = 0;
    int missed = 0;
    int evasions_won = 0;
    std::optional<double> first_contact_y;  // play frame: where the first try was made
    std::string tackler;                    // slot of the man who brought him down
};

// What a tackle try did to the play.
struct TackleResult {
    bool down = false;
    Vec2 spot;             // field frame: forward progress, when down
    bool fumble = false;   // the ball came out (the carrier is not down)
};

class Tackles {
  public:
    using EventFn = std::function<void(std::string_view, Json)>;

    Tackles(PlayState& play, EventFn event) : play_(play), event_(std::move(event)) {}

    // Sizes the per-agent arrays (call once all agents exist).
    void setup();

    // Resolves a ball carrier's evasion move (after Decide).
    void resolve_evasion(const Actions::Evasion& move, Rng& rng);

    // Caps the speed of stumbling players and moves a drag-down pair
    // together. Called after Decide, before Move.
    void steer(std::span<Intent> intents) const;

    // The Interact step, after the engagements: tackle tries on the carrier.
    TackleResult interact(Rng& rng);

    // A new carrier (a catch, a handoff, a turnover): forget the old drag.
    void reset_carrier();

    void draw(Json& shapes) const;
    const TackleStats& stats() const { return stats_; }

    // White-box: the four outcome probabilities (clean, drag, broken, missed)
    // for a tackle score and a resistance.
    std::array<double, 4> outcome_odds(double tackle, double resistance) const;

  private:
    TackleResult attempt(int carrier, int tackler, Rng& rng, bool dive = false);
    TackleResult finish_drag(Rng& rng);
    bool fumble_roll(int carrier, int tackler, bool big_hit, Rng& rng) const;
    double forward_y(Vec2 field_pos, int carrier) const;  // along the carrier's way, play-frame yd
    Vec2 spot_ahead(int carrier, double yards) const;     // field frame
    // `extra`: yd past his reach (a dive).
    bool can_try(int tackler, int carrier, double extra = 0.0) const;
    // Closest the two came during the last tick (they may cross between ticks).
    double swept_distance(int a, int b) const;
    const EvasionResult* evasion(int carrier, int tackler) const;

    PlayState& play_;
    EventFn event_;
    TackleStats stats_;
    std::vector<double> next_try_;  // per agent: s after the snap he can try again (arm tackles)
};

}  // namespace cfbsim
