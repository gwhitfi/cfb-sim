// Between plays (docs/design/game-flow.md, steps 5-7): from the whistle, the
// offense gathers behind the ball (with no huddle: in its base formation) and
// the defense stands in a shell; when a side's call comes in, players who
// aren't in it jog off and the new ones come on; the offense huddles and
// breaks, and everyone walks to where the next play lines him up.
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
// The break's stagger (s after the line leaves, by slot): the line (on it,
// inside the ends) first, then the receivers, then the QB and backs.
std::map<std::string, double> break_stagger(const Formation& formation, const GameTuning& t);

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
    // team with the ball; `tempo`: its tempo (a huddle, a sugar huddle, or
    // none). `rng`: the game's walk stream for the paces (none: nobody draws;
    // everyone goes at his side's floor).
    Between(std::unique_ptr<std::ostream> out, TrackingInfo info, const PlayFrame& frame, TeamSide offense,
            Tempo tempo, const MovementTuning& movement, const PlayTuning& play, const GameTuning& game, Rng* rng);
    ~Between();
    Between(const Between&) = delete;
    Between& operator=(const Between&) = delete;

    // Who's on the field as it starts (the header). They walk to their
    // side's stop before its call (men with no `at` start there), the
    // offense by `due` (the coach's call; none: no hurry). The ball is
    // spotted at `ball_tick`. `looks`: each side's stops before its call, by
    // team and slot: the defense's shell (game-flow.md, step 6), and with no
    // huddle the offense's base formation (step 7). A huddling offense
    // gathers behind the ball; anyone else whose slot isn't in a look goes to
    // his side's huddle.
    void start(const std::vector<BetweenMan>& men, std::int64_t ball_tick, std::optional<std::int64_t> due,
               const std::vector<Spot>& looks = {});
    // The play clock: `play_clock` s, running from `start` (none: not yet; it
    // reads full until then).
    void clock(double play_clock, std::optional<std::int64_t> start);
    // A side goes at least `p` from now (its tempo, or the Hurry button).
    void floor(std::int64_t tick, TeamSide side, Pace p);
    // A side's players for the snap, with where the play lines them up
    // (`spots`, by id; none: the call isn't in, they go to their stops before
    // it). Players of that side who aren't in it jog off; new ones come on
    // from the sideline. `hold`: they go to the huddle (the offense's ring,
    // there by `ring_due`; the defense's shell) and to their spots at
    // `release` (or `read`); else straight to them.
    void lineup(std::int64_t tick, TeamSide side, const std::vector<BetweenMan>& men, const std::vector<Spot>& spots,
                bool hold, std::optional<std::int64_t> ring_due);
    // The break: a side's players head from the huddle to their spots, each
    // `stagger` s (by slot; see `break_stagger`) after it.
    void release(std::int64_t tick, TeamSide side, const std::map<std::string, double>& stagger = {});
    // A side's players walking to their spots set off no sooner than
    // `seconds` from now (no huddle: the call comes by signal, then the shift).
    void wait(std::int64_t tick, TeamSide side, double seconds);
    // New spots for players already in (a check).
    void respot(std::int64_t tick, const std::vector<Spot>& spots);
    // The defense's shell changes (its call came in): defenders held in it
    // go to their new shell spots at their next lineup (its look before the
    // call, as `start` takes it).
    void reshell(const std::vector<Spot>& shell);
    // The call comes in by signal: a side's players look to their sideline
    // (`between_plays.shell.signal` s) wherever they stand.
    void signal(std::int64_t tick, TeamSide side);
    // The defense reads the formation: each defender held in the shell heads
    // for his spot `shell.read` s (by his Anticipation) after `from` (the
    // offense's break), and not before now. No draw: it's his rating.
    void read(std::int64_t tick, TeamSide side, std::int64_t from);
    // Players of `side` still going off hurry to be off by `due` (the
    // earliest snap: game-flow.md, step 6).
    void hurry_off(std::int64_t tick, TeamSide side, std::int64_t due);
    void event(std::int64_t tick, const std::string& name);

    // Steps the walks up to and including `tick` (writing a frame each step).
    void run_to(std::int64_t tick);
    // The next tick the walks step at, after `tick`.
    static std::int64_t next_step(std::int64_t tick);
    // Every player of `side` in its lineup is in his spot in the huddle ring.
    bool gathered(TeamSide side) const;
    // Every player of `side` is at his stop before the call (the offense's
    // gathering spot or base look, the defense's shell).
    bool assembled(TeamSide side) const;
    // Every player of `side` is at the spot the play lines him up.
    bool arrived(TeamSide side) const;
    // Nobody coming on is still off the field.
    bool on_field() const;
    // `side` brought anyone on in this stretch (it substituted).
    bool subbed(TeamSide side) const { return subbed_[side_index_of(side)]; }
    // The players of `side` going off who are still inside the field.
    std::vector<std::string> still_leaving(TeamSide side) const;
    // Every player of `side` who went off in this stretch (or is going).
    std::vector<std::string> gone_off(TeamSide side) const;
    // `side`'s substitutions are done: everyone going off is off the field,
    // everyone coming on is inside it (the umpire's release: step 6).
    bool matched(TeamSide side) const;
    // Every walk is over.
    bool settled() const;
    // Where everyone in a lineup is now.
    std::vector<Spot> where() const;
    // Where the players of `side` who aren't at their spots yet are now (a
    // defense still walking up at the snap starts the play there).
    std::vector<Spot> unset(TeamSide side) const;

    // Steps to `tick` and writes the last frame, with `exact` (the play's own
    // lineup) as the spots, then an optional last event; closes the file.
    void finish(std::int64_t tick, const std::vector<Spot>& exact, const std::string& last = "");

  private:
    struct Man;
    static std::size_t side_index_of(TeamSide s) { return s == TeamSide::Home ? 0 : 1; }
    Man* find(const std::string& id);
    Man& add(const BetweenMan& m, Vec2 pos, Vec2 facing, bool announce);
    Vec2 ring(TeamSide side, std::size_t k, std::size_t n, Vec2* face) const;
    // A huddling offense before its call: a loose cluster behind the ball,
    // the linemen nearer it (`k` of `n` in his group).
    Vec2 gather(TeamSide side, bool lineman, std::size_t k, std::size_t n, Vec2* face) const;
    // Where each of `men` (all of `side`) goes before his side's call: his
    // slot in its look, the offense's gathering spot, or the huddle ring.
    std::vector<std::pair<Vec2, Vec2>> first_stops(TeamSide side, const std::vector<BetweenMan>& men) const;
    bool huddles() const { return tempo_ == Tempo::Huddle || tempo_ == Tempo::Sugar; }
    Vec2 bench(TeamSide side);
    double clock_left(std::int64_t tick) const;
    // Sends a man to `to`, due at `due` (s; none: no hurry).
    void walk(Man& m, std::int64_t tick, Vec2 to, Vec2 face, std::optional<double> due);
    void step(std::int64_t tick);
    // Standing (or reading the formation before he sets off).
    static Vec2 standing_facing(const Man& m, std::int64_t tick);
    void frame(std::int64_t tick, const std::map<std::string, Spot>* exact = nullptr);

    std::unique_ptr<std::ostream> stream_;
    std::unique_ptr<TrackingWriter> out_;
    TrackingInfo info_;
    PlayFrame frame_;
    TeamSide offense_;
    Tempo tempo_;
    const MovementTuning& movement_;
    const PlayTuning& t_;
    const GameTuning& g_;
    Rng* rng_;
    World world_;
    std::vector<std::unique_ptr<Man>> men_;
    std::array<std::map<std::string, Spot>, 2> looks_;  // each side's stops before its call, by slot
    std::int64_t stepped_ = 0;   // the walks are as of this tick
    std::int64_t written_ = -1;  // the last frame written
    std::int64_t ball_tick_ = 0;
    double play_clock_ = 40.0;
    std::optional<std::int64_t> clock_start_;
    std::array<Pace, 2> floor_{Pace::Walk, Pace::Walk};
    std::array<bool, 2> spots_in_{false, false};  // a side's lineup is in (it has spots)
    std::array<bool, 2> subbed_{false, false};    // a side brought someone on
    int benched_[2] = {0, 0};
    bool started_ = false;
    std::vector<std::pair<std::int64_t, std::string>> events_;  // waiting for their frame
};

}  // namespace cfbsim
