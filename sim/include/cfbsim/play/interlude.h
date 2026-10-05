// Between plays (docs/design/game-flow.md, steps 5-7): from the whistle,
// players walk back to the huddles; when a side's call comes in, players
// who aren't in it jog off and the new ones come on; the offense breaks,
// and everyone walks to where the next play lines him up.
//
// The walks are real: nobody moves faster than his pace (walk, jog, or
// hurry), and the game reads when players get there (the huddle, the set).
// So every game runs them, with or without tracking; with tracking they're
// also written to a between-plays file, which the game writes as it goes
// (each decision adds walks from where the players are at that moment), so
// a live game can stream it.
//
// The walks step every 4 ticks from the file's tick 0. A decision at tick t
// takes effect at the next step, so a game stepping tick by tick (live) and
// one jumping from moment to moment (headless) walk the same.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include "cfbsim/core/rng.h"
#include "cfbsim/data/load.h"
#include "cfbsim/field/geometry.h"
#include "cfbsim/log/tracking.h"
#include "cfbsim/play/play_sim.h"
#include "cfbsim/world/world.h"

namespace cfbsim {

// How fast a player walks between plays. Ordered: a later one is faster.
enum class Pace : std::uint8_t { Walk, Jog, Hurry };

// The pace a walk's draw `u` (in [0, 1)) gives with `clock_left` s on the
// play clock, tilted by `work_rate` (game-flow.md: the bands).
Pace drawn_pace(double u, double work_rate, double clock_left, const GameTuning& t);
// The slowest pace that covers `distance` yd in `seconds` (with the margin);
// hurry when none does.
Pace needed_pace(double distance, double seconds, double top_speed, const GameTuning& t);
// yd/s.
double pace_speed(Pace p, double top_speed, const GameTuning& t);

struct BetweenMan {
    const Player* player = nullptr;
    TeamSide team = TeamSide::Home;
    std::string label;      // his slot
    std::optional<Spot> at; // where he is (none: start him in his huddle spot)
};

// One between-plays stretch (and its file, with tracking). Ticks are the
// file's own, from 0 at its start.
class Between {
  public:
    // `out`: the file (none: the walks run, nothing is written). `frame`:
    // the snap's, the offense's view (the huddles, the ball); `offense`: the
    // team with the ball; `huddle`: the offense huddles. `rng`: the game's
    // walk stream for the paces (none: nobody draws; everyone goes at his
    // side's floor).
    Between(std::unique_ptr<std::ostream> out, TrackingInfo info, const PlayFrame& frame, TeamSide offense,
            bool huddle, const MovementTuning& movement, const PlayTuning& play, const GameTuning& game, Rng* rng);
    ~Between();
    Between(const Between&) = delete;
    Between& operator=(const Between&) = delete;

    // Who's on the field as it starts (the header). They walk to their side's
    // huddle (men with no `at` start in it), the offense by `due` (the
    // coach's call; none: no hurry). The ball is spotted at `ball_tick`.
    void start(const std::vector<BetweenMan>& men, std::int64_t ball_tick, std::optional<std::int64_t> due);
    // The play clock: `play_clock` s, running from `start` (none: not yet; it
    // reads full until then).
    void clock(double play_clock, std::optional<std::int64_t> start);
    // A side goes at least `p` from now (its tempo, or the Hurry button).
    void floor(std::int64_t tick, TeamSide side, Pace p);
    // A side's players for the snap, with where the play lines them up
    // (`spots`, by id). Players of that side who aren't in it jog off; new
    // ones come on from the sideline. `hold`: they go to the huddle (there by
    // `ring_due`) and to their spots at `release`; else straight to them.
    void lineup(std::int64_t tick, TeamSide side, const std::vector<BetweenMan>& men, const std::vector<Spot>& spots,
                bool hold, std::optional<std::int64_t> ring_due);
    // The break: a side's players head from the huddle to their spots.
    void release(std::int64_t tick, TeamSide side);
    // New spots for players already in (a check).
    void respot(std::int64_t tick, const std::vector<Spot>& spots);
    void event(std::int64_t tick, const std::string& name);

    // Steps the walks up to and including `tick` (writing a frame each step).
    void run_to(std::int64_t tick);
    // The next tick the walks step at, after `tick`.
    static std::int64_t next_step(std::int64_t tick);
    // Every player of `side` in its lineup is in his huddle spot.
    bool gathered(TeamSide side) const;
    // Every player of `side` is at the spot the play lines him up.
    bool arrived(TeamSide side) const;
    // Nobody coming on is still off the field.
    bool on_field() const;
    // Every walk is over.
    bool settled() const;
    // Where everyone in a lineup is now.
    std::vector<Spot> where() const;

    // Steps to `tick` and writes the last frame, with `exact` (the play's own
    // lineup) as the spots, then an optional last event; closes the file.
    void finish(std::int64_t tick, const std::vector<Spot>& exact, const std::string& last = "");

  private:
    struct Man;
    Man* find(const std::string& id);
    Man& add(const BetweenMan& m, Vec2 pos, Vec2 facing, bool announce);
    Vec2 ring(TeamSide side, std::size_t k, std::size_t n, Vec2* face) const;
    Vec2 bench(TeamSide side);
    double clock_left(std::int64_t tick) const;
    // Sends a man to `to`, due at `due` (s; none: no hurry).
    void walk(Man& m, std::int64_t tick, Vec2 to, Vec2 face, std::optional<double> due);
    void step(std::int64_t tick);
    void frame(std::int64_t tick, const std::map<std::string, Spot>* exact = nullptr);

    std::unique_ptr<std::ostream> stream_;
    std::unique_ptr<TrackingWriter> out_;
    TrackingInfo info_;
    PlayFrame frame_;
    TeamSide offense_;
    bool huddle_;
    const MovementTuning& movement_;
    const PlayTuning& t_;
    const GameTuning& g_;
    Rng* rng_;
    World world_;
    std::vector<std::unique_ptr<Man>> men_;
    std::int64_t stepped_ = 0;   // the walks are as of this tick
    std::int64_t written_ = -1;  // the last frame written
    std::int64_t ball_tick_ = 0;
    double play_clock_ = 40.0;
    std::optional<std::int64_t> clock_start_;
    std::array<Pace, 2> floor_{Pace::Walk, Pace::Walk};
    std::array<bool, 2> spots_in_{false, false};  // a side's lineup is in (it has spots)
    int benched_[2] = {0, 0};
    bool started_ = false;
    std::vector<std::pair<std::int64_t, std::string>> events_;  // waiting for their frame
};

}  // namespace cfbsim
