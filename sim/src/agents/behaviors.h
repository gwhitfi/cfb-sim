// The player behaviors used in phase 4 (internal). Each is a subclass of
// Behavior (play/play_state.h). `final` means nothing can subclass it further,
// which also lets the compiler skip the virtual lookup when it knows the type.
#pragma once

#include <string>
#include <vector>

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

  private:
    struct Point {
        Vec2 pos;  // field frame
        bool is_break = false;
        bool decel = false;
    };

    Vec2 last_direction() const;
    Intent run_route(const PlayState& play, const Agent& me, std::size_t& next,
                     double since_snap) const;
    // Where and when (seconds from now) the ball in the air comes down to his chest.
    std::optional<std::pair<Vec2, double>> catch_spot(const PlayState& play) const;

    std::string route_;
    Vec2 origin_;
    std::vector<Point> points_;
    RouteEnd end_;
    double start_;
    bool busted_;
    std::size_t next_ = 0;  // the waypoint he's heading for
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
    // `drop`: field frame. `reads`: agent indices in progression order.
    PassingQb(std::vector<Vec2> drop, double set_seconds, std::vector<int> reads,
              double look_seconds, double delivery_seconds)
        : drop_(std::move(drop)),
          set_seconds_(set_seconds),
          reads_(std::move(reads)),
          look_seconds_(look_seconds),
          delivery_seconds_(delivery_seconds) {}

    std::string_view name() const override { return "qb_pass"; }
    Intent decide(const PlayState& play, int self, Actions& actions) override;
    void draw(const PlayState& play, int self, Json& shapes) const override;

  private:
    enum class Stage : std::uint8_t { WaitForBall, Drop, Set, Read, Deliver, Done };

    std::vector<Vec2> drop_;
    double set_seconds_;
    std::vector<int> reads_;
    double look_seconds_;
    double delivery_seconds_;

    Stage stage_ = Stage::WaitForBall;
    std::size_t drop_next_ = 0;
    std::size_t read_ = 0;      // index into reads_
    double stage_start_ = 0.0;  // seconds after the snap
    double look_start_ = 0.0;   // when he started looking at the current read
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

// Straight downfield, easing off the sideline (field frame).
Vec2 run_downfield(const PlayState& play, const Agent& me);

}  // namespace cfbsim
