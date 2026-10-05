// The rules table loaded from data/rules/ncaa_fbs.json: game structure,
// spots, and how each foul is flagged and enforced
// (docs/cfbsim_design.md#part-v-the-game).
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>

#include "cfbsim/data/enums.h"

namespace cfbsim {

// Where a penalty is enforced from.
enum class EnforceSpot : std::uint8_t {
    Previous,              // the line of scrimmage
    DeadBall,              // pre-snap: the line of scrimmage, no play
    SpotOfFoul,            // where the foul happened (grounding)
    PreviousOrFoulBeyond,  // previous spot, or the spot of the foul if beyond the line (holding)
    SpotCapped,            // DPI: the spot of the foul, at most yards past the line
    EndOfPlay,             // where the play ended (roughing, facemask)
};
inline constexpr std::array<EnumName<EnforceSpot>, 6> kEnforceSpotNames{{
    {EnforceSpot::Previous, "previous"},
    {EnforceSpot::DeadBall, "dead_ball"},
    {EnforceSpot::SpotOfFoul, "spot_of_foul"},
    {EnforceSpot::PreviousOrFoulBeyond, "previous_or_foul_beyond"},
    {EnforceSpot::SpotCapped, "spot_capped"},
    {EnforceSpot::EndOfPlay, "end_of_play"},
}};

struct PenaltyRule {
    std::string type;  // "offensive_holding"
    std::string name;  // "Holding"
    double yards = 0.0;
    EnforceSpot spot = EnforceSpot::Previous;
    bool first_down = false;          // automatic first down (fouls by the defense)
    bool loss_of_down = false;
    bool safety_in_end_zone = false;  // committed by the offense in its own end zone
    double flag = 1.0;                // chance a committed foul is flagged
};

struct RulesTable {
    std::string id = "ncaa_fbs";
    int quarters = 4;
    double quarter_seconds = 900.0;
    int timeouts_per_half = 3;
    std::array<int, 3> overtime_timeouts{1, 1, 1};  // OT1, OT2, OT3 on (in total)
    double play_clock = 40.0, admin_play_clock = 25.0;
    double two_minute = 120.0, last_minute = 60.0, runoff = 10.0;
    double first_down = 10.0;
    double kickoff_touchback = 25.0, touchback = 20.0, try_line = 3.0, overtime_start = 25.0;
    double safety_kick_from = 20.0;
    double kickoff_from = 35.0;           // the kicking team's own yard line
    double kickoff_out_of_bounds = 30.0;  // yd from the kick spot the receivers get it
    std::map<std::string, PenaltyRule, std::less<>> penalties;

    // Throws std::out_of_range for an unknown foul type.
    const PenaltyRule& penalty(std::string_view type) const;
};

}  // namespace cfbsim
