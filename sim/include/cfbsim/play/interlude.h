// The between-plays animation (docs/design/game-flow.md): from the whistle,
// players walk back to the huddles; when a side's call comes in, players
// who aren't in it jog off and the new ones come on; the offense breaks,
// and everyone walks to where the next play lines him up.
//
// It's written as the game goes (each decision adds walks from where the
// players are at that moment), so a live game can stream it. Visual only:
// nothing in the game reads it, so a game with tracking and one without are
// the same game.
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include "cfbsim/data/load.h"
#include "cfbsim/field/geometry.h"
#include "cfbsim/log/tracking.h"
#include "cfbsim/play/play_sim.h"
#include "cfbsim/world/world.h"

namespace cfbsim {

struct BetweenMan {
    const Player* player = nullptr;
    TeamSide team = TeamSide::Home;
    std::string label;      // his slot
    std::optional<Spot> at; // where he is (none: start him in his huddle spot)
};

// One between-plays file. Ticks are the file's own, from 0 at its start.
class Between {
  public:
    // `frame`: the snap's, the offense's view (the huddles, the ball);
    // `offense`: the team with the ball; `huddle`: the offense huddles.
    Between(std::unique_ptr<std::ostream> out, TrackingInfo info, const PlayFrame& frame, TeamSide offense,
            bool huddle, const MovementTuning& movement, const PlayTuning& t);
    ~Between();
    Between(const Between&) = delete;
    Between& operator=(const Between&) = delete;

    // Who's on the field as the file starts (the header). They walk to their
    // side's huddle (men with no `at` start in it). The ball is spotted at
    // `ball_tick`.
    void start(const std::vector<BetweenMan>& men, std::int64_t ball_tick);
    // A side's players for the snap are in (its call), with where the play
    // lines them up (`spots`, by id). Players of that side who aren't in it
    // jog off; new ones come on from the sideline. With `break_tick` they
    // huddle until then; they're set by `set_tick` (none: no hurry).
    void lineup(std::int64_t tick, TeamSide side, const std::vector<BetweenMan>& men, const std::vector<Spot>& spots,
                std::optional<std::int64_t> break_tick, std::optional<std::int64_t> set_tick);
    // New spots for players already in (a check): there by `by`.
    void respot(std::int64_t tick, const std::vector<Spot>& spots, std::int64_t by);
    void event(std::int64_t tick, const std::string& name);
    // Writes the frames (every 4 ticks) up to and including `tick`.
    void run_to(std::int64_t tick);
    // When the last walk planned so far ends.
    std::int64_t arrived() const;
    // The last frame at `tick`, with `exact` (the play's own lineup) as the
    // spots, then an optional last event; closes the file.
    void finish(std::int64_t tick, const std::vector<Spot>& exact, const std::string& last = "");

  private:
    struct Path;
    struct Man;
    Man* find(const std::string& id);
    Man& add(const BetweenMan& m, Vec2 pos, Vec2 facing, bool announce);
    Vec2 ring(TeamSide side, std::size_t k, std::size_t n, Vec2* face) const;
    Vec2 bench(TeamSide side);
    void frame(std::int64_t tick, const std::map<std::string, Spot>* exact = nullptr);

    std::unique_ptr<std::ostream> stream_;
    std::unique_ptr<TrackingWriter> out_;
    TrackingInfo info_;
    PlayFrame frame_;
    TeamSide offense_;
    bool huddle_;
    const MovementTuning& movement_;
    const PlayTuning& t_;
    World world_;
    std::vector<std::unique_ptr<Man>> men_;
    std::int64_t written_ = -1;  // the last frame written
    std::int64_t ball_tick_ = 0;
    int benched_[2] = {0, 0};
    bool started_ = false;
    std::vector<std::pair<std::int64_t, std::string>> events_;  // waiting for their frame
};

}  // namespace cfbsim
