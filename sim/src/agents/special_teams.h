// Special teams behaviors (phase 9; cfbsim_design.md#special-teams-play):
// coverage, returners, return blockers, field goal protection and rush.
// The kick sim (play/kick_sim.cpp) swaps these for LaneCoverage, Pursuer,
// BallCarrier, and LooseBall when the ball is fielded or comes loose.
#pragma once

#include <optional>
#include <string>

#include "agents/behaviors.h"
#include "play/play_state.h"

namespace cfbsim {

// Has the ball been kicked? (The kick sim records the kick in PlayState::pass.)
inline bool kicked(const PlayState& play) { return play.pass.has_value(); }

// Where the kicked ball will come down (field frame), if it's in the air.
std::optional<Vec2> kick_landing(const PlayState& play);

// Where a kickoff safety aims: across from `ball` at `across` x `lane`,
// `depth` yd behind the front of his coverage (the deepest teammate who
// isn't a safety, or the kicker before the kick, led by 0.75 s of his run
// so the safety keeps up), and never closer to the ball than `depth`.
Vec2 safety_spot(const PlayState& play, int self, Vec2 ball, Vec2 across, Vec2 kick_dir, double lane, double depth);

// Seconds to run `distance` yd straight ahead from `speed`, accelerating as
// the physics step does (dv/dt = accel_rate x (top_speed - v)).
double run_time(double distance, double speed, const MovementProfile& move);

// Coverage before the ball is fielded: run down in a lane toward where the
// ball is coming down (or the loose ball). `across`: field-frame unit vector
// for the kicking team's +x; `lane`: yd across from the ball in that frame.
// `trail`: stay this far short of the ball (a safety). Before the kick he
// times his run so he isn't past the ball when it's kicked.
class Cover final : public Behavior {
  public:
    // Before the kick: hold his spot (punt protection), time his run to the
    // ball (kickoff), or release downfield at the snap (punt gunners).
    enum class Before { Hold, Time, Release };
    Cover(std::string label, Vec2 across, Vec2 kick_dir, double lane, double trail, Before before)
        : label_(std::move(label)), across_(across), kick_dir_(kick_dir), lane_(lane), trail_(trail),
          before_(before) {}

    std::string_view name() const override { return label_; }
    Intent decide(const PlayState& play, int self, Actions& actions) override;
    double lane() const { return lane_; }
    double behind() const { return behind_; }

    // Kickoff: time the run off this kicker's approach (Before::Time).
    void set_kicker(int k) { kicker_ = k; }
    // Kickoff safety: stay this far behind the front of the coverage
    // instead of `trail` short of the ball.
    void set_behind(double yd) { behind_ = yd; }

  private:
    std::string label_;
    Vec2 across_;
    Vec2 kick_dir_;  // field frame: the way the ball is kicked
    double lane_;
    double trail_;
    Before before_;
    int kicker_ = -1;
    double behind_ = 0.0;
    bool go_ = false;  // kickoff: he has started his run
};

// Kickoff coverage after the catch: keeps his lane, sliding across with the
// returner and squeezing as he closes, then breaks to the ball as a Pursuer
// (cfbsim_design.md#coverage-and-the-return).
class LaneCoverage final : public Behavior {
  public:
    struct Setup {
        Vec2 across;              // field frame: the kicking team's +x
        Vec2 kick_dir;            // field frame: the way the ball was kicked
        double lane = 0.0;        // yd across from the returner (kicking frame x)
        bool contain = false;     // stays outside the returner
        bool safety = false;      // a second line: stays across from the returner, `depth` behind the coverage
        double depth = 0.0;       // safety: yd behind the front of the coverage
        bool disciplined = true;  // false: he breaks to the ball at once
        double squeeze_depth = 25.0;
        double break_range = 8.0;
        double contain_width = 4.0;
        double safety_range = 10.0;
        Pursuer::Setup pursuit;   // how he chases once he breaks
    };
    explicit LaneCoverage(Setup setup) : s_(setup), pursue_(setup.pursuit) {}

    std::string_view name() const override { return broken_ ? "pursue" : s_.safety ? "safety" : s_.contain ? "contain" : "cover"; }
    Intent decide(const PlayState& play, int self, Actions& actions) override;
    void draw(const PlayState& play, int self, Json& shapes) const override;

  private:
    Setup s_;
    Pursuer pursue_;
    bool broken_ = false;      // he has left his lane for good
    std::optional<Vec2> aim_;  // field frame (drawing)
};

// The returner before he has the ball.
class Returner final : public Behavior {
  public:
    enum class Plan { Field, FairCatch, Touchback, Avoid };
    explicit Returner(Vec2 kick_dir) : kick_dir_(kick_dir) {}

    std::string_view name() const override { return "returner"; }
    Intent decide(const PlayState& play, int self, Actions& actions) override;

    void set_plan(Plan p) { plan_ = p; }
    Plan plan() const { return plan_; }

  private:
    Vec2 kick_dir_;
    Plan plan_ = Plan::Field;
};

// A return blocker: sets up in front of the catch, then blocks his man
// (assigned by the kick sim at the catch) between him and the returner.
class ReturnBlocker final : public Behavior {
  public:
    // `offset`: his lateral spot (field frame) relative to the catch;
    // `set_depth`: yd in front of the catch, toward the kicker.
    ReturnBlocker(Vec2 offset, Vec2 kick_dir, double set_depth)
        : offset_(offset), kick_dir_(kick_dir), set_depth_(set_depth) {}

    std::string_view name() const override { return "return_block"; }
    Intent decide(const PlayState& play, int self, Actions& actions) override;
    int block_target() const override { return target_; }
    BlockStyle block_style() const override { return active_ ? BlockStyle::Space : BlockStyle::None; }

    void set_target(int d) { target_ = d; }
    int target() const { return target_; }

  private:
    Vec2 offset_;
    Vec2 kick_dir_;
    double set_depth_;
    int target_ = -1;
    bool active_ = false;
};

// Punt return jammer: blocks the nearest coverage player (a gunner) at the
// line for `seconds`, then done.
class Jammer final : public Behavior {
  public:
    explicit Jammer(double seconds) : seconds_(seconds) {}
    std::string_view name() const override { return "jam"; }
    Intent decide(const PlayState& play, int self, Actions& actions) override;
    bool done(const PlayState& play, int /*self*/) const override { return play.since_snap() >= seconds_; }
    int block_target() const override { return target_; }
    BlockStyle block_style() const override { return BlockStyle::Space; }

  private:
    double seconds_;
    int target_ = -1;
};

// Field goal protection: hold the spot and block the nearest rusher.
class Protector final : public Behavior {
  public:
    explicit Protector(Vec2 spot) : spot_(spot) {}
    std::string_view name() const override { return "protect"; }
    Intent decide(const PlayState& play, int self, Actions& actions) override;
    int block_target() const override { return target_; }
    BlockStyle block_style() const override { return BlockStyle::Pass; }

  private:
    Vec2 spot_;
    int target_ = -1;
};

// Field goal rush: straight at the kick point.
class KickRusher final : public Behavior {
  public:
    explicit KickRusher(Vec2 point) : point_(point) {}
    std::string_view name() const override { return "kick_rush"; }
    Intent decide(const PlayState& play, int self, Actions& actions) override;

  private:
    Vec2 point_;
};

// Stand at a spot (facing a direction) for `seconds`, then done.
class Wait final : public Behavior {
  public:
    // `speed`: how fast he goes to the spot (default half his top speed).
    Wait(std::string label, Vec2 spot, Vec2 face, double seconds, std::optional<double> speed = std::nullopt)
        : label_(std::move(label)), spot_(spot), face_(face), seconds_(seconds), speed_(speed) {}
    std::string_view name() const override { return label_; }
    Intent decide(const PlayState& play, int self, Actions& actions) override;
    bool done(const PlayState& play, int self) const override;

  private:
    std::string label_;
    Vec2 spot_;
    Vec2 face_;
    double seconds_;  // after the snap; infinity: until the kick sim replaces him
    std::optional<double> speed_;
};

}  // namespace cfbsim
