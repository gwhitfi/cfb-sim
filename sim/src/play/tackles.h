// Tackles: evasion moves, tackle tries and their outcomes (clean, drag-down,
// broken, missed), forward progress, stumbles, and the fumble roll
// (internal; cfbsim_design.md#tackles).
#pragma once

#include <array>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
    int tackler_index = -1;
    std::vector<std::pair<int, int>> contacts;  // every try: (carrier, tackler)
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

    // After the whistle: the tackled carrier and the pile on him stop within
    // `slide` yd instead of pulling up like everyone else. Call each dead-ball
    // tick before Move.
    void settle(std::span<Intent> intents);

    void draw(Json& shapes) const;
    const TackleStats& stats() const { return stats_; }

    // White-box: the four outcome probabilities (clean, drag, broken, missed)
    // for a tackle score and a resistance.
    std::array<double, 4> outcome_odds(double tackle, double resistance, int carrier) const;
    // White-box: how squarely tackler gets to the carrier, 0-1 (distance x
    // angle), for any positions and velocities (field frame), with `gap`
    // between the bodies.
    double contact_quality_at(int tackler, Vec2 k_pos, Vec2 k_vel, Vec2 c_pos, Vec2 c_vel, double gap,
                              int carrier) const;

  private:
    TackleResult attempt(int carrier, int tackler, Rng& rng, bool dive = false);
    // Extra reach a free defender gets lunging at a carrier he's closing on.
    double lunge(int tackler, int carrier) const;
    TackleResult finish_drag(Rng& rng);
    bool fumble_roll(int carrier, int tackler, bool big_hit, Rng& rng) const;
    // The carrier is down: he and the pile on him stop short (settle()).
    void go_down(int carrier);
    double forward_y(Vec2 field_pos, int carrier) const;  // along the carrier's way, play-frame yd
    Vec2 spot_ahead(int carrier, double yards) const;     // field frame
    // `extra`: yd past his reach (a dive).
    bool can_try(int tackler, int carrier, double extra = 0.0) const;
    // Closest the two came during the last tick (they may cross between ticks).
    double swept_distance(int a, int b) const;
    // How squarely he gets to the carrier, 0-1: distance x angle.
    double contact_quality(int tackler, int carrier) const;
    // In reach but still closing to a better contact: he holds his try.
    bool waits(int tackler, int carrier) const;
    const EvasionResult* evasion(int carrier, int tackler) const;

    PlayState& play_;
    EventFn event_;
    TackleStats stats_;
    std::vector<double> next_try_;  // per agent: s after the snap he can try again (arm tackles)
    std::vector<bool> was_engaged_;  // per agent: in a block last tick (for block_recover)
    std::vector<std::pair<int, double>> downed_;  // after a tackle: (agent, braking yd/s^2)
};

}  // namespace cfbsim
