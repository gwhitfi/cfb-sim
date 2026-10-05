// Lanes: how a runner with the ball picks his heading, from what he can see
// (internal; cfbsim_design.md#lanes). Every runner uses it: a
// ball carrier past his path, a receiver after the catch, a returner, a
// scrambling QB.
#pragma once

#include <optional>
#include <vector>

#include "cfbsim/core/rng.h"
#include "play/play_state.h"

namespace cfbsim {

// A quick estimate of the seconds a player at `pos` moving at `vel` needs
// to get within `reach` of `point`: turn (brake off the speed that's going
// the wrong way), then run at top speed. Cheaper than openness's
// time_to_reach (no acceleration curve), for the thousands of checks lanes
// and pursuit make each tick.
double quick_reach_time(Vec2 pos, Vec2 vel, double top_speed, double decel, double reach,
                        Vec2 point);

// What one runner sees, from his ratings.
struct LaneSkill {
    double cone = 1.5;      // radians: half-angle of his vision cone
    double range = 20.0;    // yd
    double lag = 0.2;       // s
    double noise_sd = 1.0;  // yd on each heading's score
};

LaneSkill lane_skill(const Attributes& ratings, const CarrierTuning& tuning);

class LaneRunner {
  public:
    LaneRunner() = default;
    explicit LaneRunner(LaneSkill skill) : skill_(skill) {}

    // Re-picks his heading every `decide_seconds` and returns the Intent
    // (full speed along it). `bias`: a heading (field frame) his choice must
    // stay within `bias_cone` radians of (a run read he just made).
    Intent run(const PlayState& play, int self, Rng& rng, std::optional<Vec2> bias = std::nullopt,
               double bias_cone = 0.0);

    // One heading's score (field-frame unit vector), without noise.
    double score(const PlayState& play, int self, Vec2 dir) const;

    // The best score of the headings he can see looking downfield, without
    // changing his heading (a QB deciding whether to run).
    double probe(const PlayState& play, int self) const;

    // Forces a heading until `until` (s after the snap): a juke's cut.
    void force(Vec2 dir, double until) {
        heading_ = dir;
        forced_until_ = until;
    }

    // From his latest decision: the best score, and the soonest a defender
    // he noticed can get to him (s).
    double best() const { return best_; }
    double contact() const { return contact_; }
    std::optional<Vec2> heading() const { return heading_; }
    bool decided() const { return heading_.has_value(); }

    // Does he notice defender d (in his vision cone and range, or close)?
    bool notices(const PlayState& play, int self, int d) const;

    void draw(const PlayState& play, int self, Json& shapes) const;

  private:
    struct Threat {
        Vec2 pos, vel;
        double top = 8.0, decel = 8.0, reach = 1.0;
        double delay = 0.0;  // s before he can start (blocked, down, screened)
    };
    std::vector<Threat> threats(const PlayState& play, int self) const;
    // Seconds he can run along `dir` before a threat gets there, and the
    // yards he covers by then.
    void ray(const PlayState& play, int self, const std::vector<Threat>& threats, Vec2 dir,
             double& free_t, double& dist) const;
    double score(const PlayState& play, int self, const std::vector<Threat>& threats,
                 Vec2 dir, double* free_dist) const;
    Vec2 look_dir(const PlayState& play, int self) const;

    LaneSkill skill_;
    std::optional<Vec2> heading_;  // field frame
    double next_decide_ = 0.0;     // s after the snap
    double forced_until_ = -1.0;
    double best_ = 0.0;
    double contact_ = 1e9;
    double free_dist_ = 0.0;       // yd free along his heading (drawing)
};

}  // namespace cfbsim
