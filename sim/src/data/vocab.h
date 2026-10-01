// Fixed vocabularies that data files are checked against. Adding a new tag
// or blocking rule word means adding it here (and, for the sim, teaching
// the code what it means).
#pragma once

#include <algorithm>
#include <array>
#include <string_view>

namespace cfbsim::detail {

template <std::size_t N>
constexpr bool contains(const std::array<std::string_view, N>& list, std::string_view word) {
    return std::find(list.begin(), list.end(), word) != list.end();
}

// Play tags (plays-and-data.md#play-tags).
inline constexpr std::array<std::string_view, 30> kOffenseTags{
    "quick_game", "dropback",  "play_action", "screen",     "option",     "short",
    "medium",     "deep",      "inside",      "outside",    "base",       "1st_down",
    "2nd_long",   "2nd_short", "3rd_long",    "3rd_medium", "3rd_short",  "short_yardage",
    "goal_line",  "red_zone",  "two_minute",  "backed_up",  "clock_kill", "vs_man",
    "vs_zone",    "vs_cover2", "vs_cover3",   "vs_cover4",  "vs_blitz",   "vs_cover1",
};
inline constexpr std::array<std::string_view, 16> kDefenseTags{
    "base",      "pressure", "man",        "zone",       "vs_run",    "vs_pass",
    "1st_down",  "2nd_long", "3rd_long",   "3rd_medium", "3rd_short", "short_yardage",
    "goal_line", "red_zone", "two_minute", "prevent",
};

inline constexpr std::array<std::string_view, 3> kReadTypes{"progression", "half_field", "key"};
inline constexpr std::array<std::string_view, 3> kRunReads{"bang", "bend", "bounce"};
inline constexpr std::array<std::string_view, 5> kStrengthCalls{"te", "passing_strength", "field",
                                                                "boundary", "back"};

// Blocking rules (plays-and-data.md#blocking-schemes).
inline constexpr std::array<std::string_view, 4> kBlockConditions{"covered", "covered_playside",
                                                                  "uncovered", "has_inside_threat"};
inline constexpr std::array<std::string_view, 10> kBlockTargets{
    "man_on",       "man_on_or_inside",  "playside_gap",
    "backside_gap", "first_lb_playside", "emol_playside",
    "man_over",     "first_free_rusher", "first_coverage_inside",
    "kick_out_man"};
inline constexpr std::array<std::string_view, 10> kBlockActions{
    "drive",     "reach",     "cutoff",   "down",          "combo_climb",
    "pull_kick", "pull_lead", "pass_set", "check_release", "stalk"};
// Extra actions allowed in a play's assignment (they defer to the play's scheme).
inline constexpr std::array<std::string_view, 2> kAssignmentOnlyBlockActions{"scheme", "protect"};
inline constexpr std::array<std::string_view, 3> kProtectionGroups{"slide_side", "man_side",
                                                                   "backs"};

// Defensive technique numbers.
inline constexpr std::array<std::string_view, 11> kTechniques{"0", "1", "2i", "2", "3", "4i",
                                                              "4", "5", "7",  "6", "9"};

}  // namespace cfbsim::detail
