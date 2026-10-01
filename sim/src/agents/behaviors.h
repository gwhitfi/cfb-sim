// The player behaviors (internal). Each is a subclass of
// Behavior (play/play_state.h). `final` means nothing can subclass it further,
// which also lets the compiler skip the virtual lookup when it knows the type.
#pragma once

#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cfbsim/core/rng.h"
#include "cfbsim/play/routes.h"
#include "play/play_state.h"

namespace cfbsim {

// ---------------------------------------------------------------------------
// Runs a route, then plays the ball when it's thrown to him.
// ---------------------------------------------------------------------------
class RouteRunner final : public Behavior {
  public:
    // `plan` is in the play frame; `start` is seconds after the snap.
    RouteRunner(const RoutePlan& plan, const PlayFrame& frame, double start, bool busted);

    std::string_view name() const override { return "route"; }
    Intent decide(const PlayState& play, int self, Actions& actions) override;
    std::optional<Vec2> predict(const PlayState& play, int self, double seconds) const override;
    void draw(const PlayState& play, int self, Json& shapes) const override;
    double last_break() const override { return last_break_; }

  private:
    struct Point {
        Vec2 pos;  // field frame
        bool is_break = false;
        bool decel = false;
    };

    Vec2 last_direction() const;
    Intent run_route(const PlayState& play, const Agent& me, std::size_t& next,
                     double since_snap) const;
    // Settle routes: the open spot near the end of the route (field frame).
    Vec2 settle_spot(const PlayState& play) const;

    std::string route_;
    Vec2 origin_;
    std::vector<Point> points_;
    RouteEnd end_;
    double start_;
    bool busted_;
    std::size_t next_ = 0;  // the waypoint he's heading for
    double last_break_ = -std::numeric_limits<double>::infinity();
    std::optional<Vec2> settle_;  // where he's settling (field frame)
    double settle_checked_ = -1.0;
};

// ---------------------------------------------------------------------------
// Stand-in blocking until phase 6: move to a spot and hold it.
// ---------------------------------------------------------------------------
class StandInBlocker final : public Behavior {
  public:
    enum class Kind : std::uint8_t { PassSet, RunBlock, Stalk, CheckRelease };

    // `spot`: field frame. CheckRelease ends `until` seconds after the snap.
    StandInBlocker(Kind kind, Vec2 spot, double until = 0.0)
        : kind_(kind), spot_(spot), until_(until) {}

    std::string_view name() const override;
    Intent decide(const PlayState& play, int self, Actions& actions) override;
    bool done(const PlayState& play, int self) const override;
    Key key() const override;

  private:
    Kind kind_;
    Vec2 spot_;
    double until_;
};

// ---------------------------------------------------------------------------
// Pass play QB: take the snap, drop, set, go through his reads, throw.
// ---------------------------------------------------------------------------
class PassingQb final : public Behavior {
  public:
    struct Setup {
        std::vector<Vec2> drop;  // field frame
        double set_seconds = 0.0;
        std::vector<int> reads;  // agent indices in progression order
        double look_seconds = 0.5;
        double delivery_seconds = 0.3;
        double perceive_sd = 0.1;   // sd of his bias on a read's openness
        double threshold = 0.6;     // perceived openness he needs, before it decays
        double peripheral_rate = 1.0;  // per second
    };

    // `seed`: his own random stream (perception noise, peripheral vision),
    // drawn from the match RNG when he's created.
    PassingQb(Setup setup, std::uint64_t seed);

    std::string_view name() const override { return "qb_pass"; }
    Intent decide(const PlayState& play, int self, Actions& actions) override;
    void draw(const PlayState& play, int self, Json& shapes) const override;
    Key key() const override { return Key::Drop; }
    int eyes() const override;

  private:
    enum class Stage : std::uint8_t { WaitForBall, Drop, Set, Read, Deliver, Done };

    void look_at(std::size_t read, double now);
    double threshold(const PlayState& play) const;

    Setup s_;
    Rng rng_;

    Stage stage_ = Stage::WaitForBall;
    std::size_t drop_next_ = 0;
    std::size_t read_ = 0;      // index into reads
    double stage_start_ = 0.0;  // seconds after the snap
    double look_start_ = 0.0;   // when he started looking at the current read
    double set_time_ = -1.0;    // when he finished his drop
    double bias_ = 0.0;         // his misjudgment of the current read
    std::vector<double> seen_;  // last perceived openness of each read (-1 = never looked)
    int target_ = -1;           // who he's throwing to
    bool away_ = false;         // throwing it away
};

// ---------------------------------------------------------------------------
// Run play QB: take the snap, meet the carrier at the mesh, hand off, fake.
// ---------------------------------------------------------------------------
class HandoffQb final : public Behavior {
  public:
    // Field frame: where he meets the carrier, and the fake after the handoff.
    HandoffQb(Vec2 mesh_spot, int carrier, std::vector<Vec2> fake)
        : mesh_spot_(mesh_spot), carrier_(carrier), fake_(std::move(fake)) {}

    std::string_view name() const override { return "qb_handoff"; }
    Intent decide(const PlayState& play, int self, Actions& actions) override;
    Key key() const override { return Key::Handoff; }

  private:
    Vec2 mesh_spot_;
    int carrier_;
    std::vector<Vec2> fake_;
    bool handed_off_ = false;
    std::size_t fake_next_ = 0;
};

// ---------------------------------------------------------------------------
// Ball carrier: follow the designed path, then run straight downfield (the
// real ball carrier AI comes in phase 7). With no path he is free at once
// (a receiver after the catch).
// ---------------------------------------------------------------------------
class BallCarrier final : public Behavior {
  public:
    struct Mark {
        Vec2 pos;  // field frame
        std::string label;
    };

    // He is free once he reaches path[free_after] (or the end of the path).
    BallCarrier(std::vector<Mark> path, std::size_t free_after, Vec2 start)
        : path_(std::move(path)), free_after_(free_after), start_(start) {}

    std::string_view name() const override { return "ball_carrier"; }
    Intent decide(const PlayState& play, int self, Actions& actions) override;
    std::optional<Vec2> predict(const PlayState& play, int self, double seconds) const override;
    void draw(const PlayState& play, int self, Json& shapes) const override;

    bool is_free() const { return next_ > free_after_ || next_ >= path_.size(); }

  private:
    std::vector<Mark> path_;
    std::size_t free_after_;
    Vec2 start_;
    std::size_t next_ = 0;
};

// Straight downfield for his team, easing off the sideline (field frame).
Vec2 run_downfield(const PlayState& play, int self);

// Chases the ball carrier, aiming a little ahead of him (a stand-in until
// pursuit angles in phase 7).
class Pursuer final : public Behavior {
  public:
    std::string_view name() const override { return "pursue"; }
    Intent decide(const PlayState& play, int self, Actions& actions) override;
};

// Where and when (seconds from now) the ball in the air comes down through
// chest height (or failing that, the ground).
std::optional<std::pair<Vec2, double>> ball_drop_spot(const PlayState& play);

// ===========================================================================
// Defense (movement-and-interaction.md#defense--coverage-phase-5)
// ===========================================================================

// A zone's area in the play frame.
struct ZoneArea {
    std::string id;
    Vec2 drop;  // the landmark
    double width = 10.0;
    double depth_min = 0.0;
    double depth_max = 15.0;
    bool deep = false;  // plays the deepest threat

    bool contains(Vec2 p, double margin = 0.0) const {
        return std::abs(p.x - drop.x) <= width / 2.0 + margin && p.y >= depth_min - margin &&
               p.y <= depth_max + margin;
    }
};

// ---------------------------------------------------------------------------
// One defender: his run job, his pass job, and his belief about which this
// play is. The jobs are behaviors themselves; this one owns them (through
// std::unique_ptr) and passes each tick to whichever job his belief says.
// After a throw he plays the ball, after a catch he pursues.
// ---------------------------------------------------------------------------
class Defender final : public Behavior {
  public:
    Defender(std::unique_ptr<Behavior> run_job, std::unique_ptr<Behavior> pass_job,
             double read_delay, double read_rate, double prior);

    std::string_view name() const override { return "defender"; }
    Intent decide(const PlayState& play, int self, Actions& actions) override;
    void draw(const PlayState& play, int self, Json& shapes) const override;
    int man_target() const override { return pass_job_->man_target(); }
    int covering() const override { return playing_pass() ? pass_job_->covering() : -1; }

    double belief() const { return p_pass_; }
    bool playing_pass() const { return p_pass_ >= 0.5; }

  private:
    void update_belief(const PlayState& play, int self);

    std::unique_ptr<Behavior> run_job_;
    std::unique_ptr<Behavior> pass_job_;
    Pursuer pursue_;
    double read_delay_;
    double read_rate_;
    double p_pass_;
};

// ---------------------------------------------------------------------------
// Man coverage on one receiver.
// ---------------------------------------------------------------------------
class ManCoverage final : public Behavior {
  public:
    // `leverage`: +1 outside, -1 inside. `cushion`: yd off the receiver at the snap.
    ManCoverage(int target, double leverage, double cushion, double lag)
        : target_(target), leverage_(leverage), cushion_(cushion), lag_(lag) {}

    std::string_view name() const override { return "man"; }
    Intent decide(const PlayState& play, int self, Actions& actions) override;
    void draw(const PlayState& play, int self, Json& shapes) const override;
    int man_target() const override { return target_; }
    int covering() const override { return target_; }

  private:
    int target_;
    double leverage_;
    double cushion_;
    double lag_;
    std::optional<double> start_y_;  // the receiver's play-frame y at the snap
};

// ---------------------------------------------------------------------------
// Zone coverage: drop to the landmark, then play the threats in the area.
// ---------------------------------------------------------------------------
class ZoneCoverage final : public Behavior {
  public:
    // `late`: seconds he stands before dropping (a bust).
    ZoneCoverage(ZoneArea zone, double lag, double skill, double late)
        : zone_(std::move(zone)), lag_(lag), skill_(skill), late_(late) {}

    std::string_view name() const override { return "zone"; }
    Intent decide(const PlayState& play, int self, Actions& actions) override;
    void draw(const PlayState& play, int self, Json& shapes) const override;
    int covering() const override { return threat_; }

  private:
    Vec2 spot(const PlayState& play, int self);  // play frame; picks his threat

    ZoneArea zone_;
    double lag_;
    double skill_;
    double late_;
    Vec2 spot_;        // play frame, for drawing
    int threat_ = -1;  // the receiver he's playing (underneath zones)
};

// ---------------------------------------------------------------------------
// Go to a spot and hold it: the pass rush (until phase 6), run fits (until
// phase 7), and spies.
// ---------------------------------------------------------------------------
class HoldSpot final : public Behavior {
  public:
    // `spot`: field frame. `speed_share`: of his top speed.
    HoldSpot(std::string label, Vec2 spot, double speed_share)
        : label_(std::move(label)), spot_(spot), speed_share_(speed_share) {}

    std::string_view name() const override { return label_; }
    Intent decide(const PlayState& play, int self, Actions& actions) override;

  private:
    std::string label_;
    Vec2 spot_;
    double speed_share_;
};

}  // namespace cfbsim
