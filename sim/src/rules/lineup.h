// One team's players through a game: energy, rest, injuries, and who fills
// each slot (internal; cfbsim_design.md#fatigue-substitutions-and-injuries).
#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "cfbsim/data/load.h"
#include "cfbsim/play/play_sim.h"

namespace cfbsim {

class Lineup {
  public:
    Lineup(const Team& team, const Roster& roster, const GameTuning& tuning);

    // Who plays each of `slots` (slot -> player id), from the slot map of a
    // personnel group or package. Throws GameError if a slot can't be filled.
    std::map<std::string, std::string> fill(const SlotMap& map, const std::vector<std::string>& slots) const;
    // The first available player at a position (kneel and spike plays).
    std::string starter(Position pos) const;

    RatingModifiers modifiers(const std::string& id) const;
    // Effective ratings (base x modifiers): pre-snap foul rolls, kickers.
    Attributes ratings(const std::string& id) const;
    const Player& player(const std::string& id) const;

    // A snap's work drains the players who played it.
    void play(const std::vector<PlayerUsage>& usage, bool offense);
    // Game time between snaps: `on_field` recover slowly, everyone else faster.
    void recover(double seconds, const std::set<std::string>& on_field);
    void halftime();
    void injure(const std::string& id);

    // Players whose rest state changed since the last call: (id, resting).
    std::vector<std::pair<std::string, bool>> rest_changes();

    double energy(const std::string& id) const;
    bool injured(const std::string& id) const;

  private:
    struct State {
        double energy = 1.0;
        bool resting = false;
        bool injured = false;
        bool reported_resting = false;
    };
    void update_rest(const std::string& id, State& s);
    // Who plays `pos`, best first: fresh players in depth chart order, then
    // tired ones (most energy first), then anyone else (familiarity at `pos`).
    std::vector<std::string> order(Position pos, const std::set<std::string>& used) const;

    const Team& team_;
    const Roster& roster_;
    const GameTuning& tuning_;
    std::map<std::string, State> players_;
};

}  // namespace cfbsim
