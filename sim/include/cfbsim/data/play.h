// Plays and playbooks (docs/design/plays-and-data.md).
#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "cfbsim/data/enums.h"
#include "cfbsim/data/formation.h"

namespace cfbsim {

// ---------------------------------------------------------------------------
// Jobs: one thing a slot does on a play. An assignment is a sequence of jobs
// ("block, then run a flat route") or, for defenders, a run job + a pass job.
// ---------------------------------------------------------------------------
enum class JobKind : std::uint8_t {
    // Offense
    Route,
    Block,
    // Defense
    Gap,
    Support,
    Contain,
    Pursue,
    Rush,
    Zone,
    Man,
    Spy,
    Blitz,
    Stunt,
    // Special teams
    Kick,
    Punt,
    LongSnap,
    Hold,
    Protect,
    CoverLane,
    Safety,
    Gunner,
    Jam,
    HoldUp,
    ReturnBlock,
    Return,
    Recover,
};
inline constexpr std::array<EnumName<JobKind>, 25> kJobKindNames{{
    {JobKind::Route, "route"},
    {JobKind::Block, "block"},
    {JobKind::Gap, "gap"},
    {JobKind::Support, "support"},
    {JobKind::Contain, "contain"},
    {JobKind::Pursue, "pursue"},
    {JobKind::Rush, "rush"},
    {JobKind::Zone, "zone"},
    {JobKind::Man, "man"},
    {JobKind::Spy, "spy"},
    {JobKind::Blitz, "blitz"},
    {JobKind::Stunt, "stunt"},
    {JobKind::Kick, "kick"},
    {JobKind::Punt, "punt"},
    {JobKind::LongSnap, "long_snap"},
    {JobKind::Hold, "hold"},
    {JobKind::Protect, "protect"},
    {JobKind::CoverLane, "cover_lane"},
    {JobKind::Safety, "safety"},
    {JobKind::Gunner, "gunner"},
    {JobKind::Jam, "jam"},
    {JobKind::HoldUp, "hold_up"},
    {JobKind::ReturnBlock, "return_block"},
    {JobKind::Return, "return"},
    {JobKind::Recover, "recover"},
}};
constexpr std::string_view to_string(JobKind v) { return to_name(kJobKindNames, v); }

struct RouteRef {
    std::string route;            // id in the route library
    std::optional<double> depth;  // depth override
    Side side = Side::None;       // which way is "outside" for a back
};

struct RouteOption {
    std::string when;  // "vs_man", "vs_zone"
    RouteRef route;
};

// One struct holds every job kind; each kind uses only the fields noted.
// (Phase 4 turns these into behaviors; this is just the data.)
struct Job {
    JobKind kind = JobKind::Route;
    RouteRef route;                    // Route
    std::vector<RouteOption> options;  // Route: coverage-dependent branches
    std::string action;                // Block: "scheme", "protect", "stalk", "check_release"...
    std::string target;                // Block, ReturnBlock, Man: who to block / cover
    std::string ref;                   // Zone id, Support role, Rush lane, ReturnBlock rule
    std::string gap;                   // Gap, Blitz, Rush: "A".."D"
    Side side = Side::None;            // Gap, Blitz, Rush
    std::optional<double> lane_x;      // CoverLane
};

struct Assignment {
    std::vector<Job> sequence;  // offense / special teams: jobs in order
    std::optional<Job> run;     // defense: run responsibility
    std::optional<Job> pass;    // defense: pass responsibility
};

// ---------------------------------------------------------------------------
// Play-wide settings
// ---------------------------------------------------------------------------
struct PassSpec {
    std::string drop;                      // "gun_3", "5_step", ...
    std::vector<std::string> progression;  // slot names, in read order
    std::string read_type = "progression";
    std::string hot;         // slot name; empty = none
    std::string protection;  // protection scheme id
    Side protection_direction = Side::None;
};

struct PathPoint {
    Anchor x;
    Anchor y;
    std::string type;  // "mesh", "aim", or empty
};

struct RunSpec {
    std::string scheme;
    Side direction = Side::Right;
    std::string carrier;    // slot name
    std::string qb_action;  // "handoff", ...
    std::string qb_then;    // "boot_fake_left", ...
    std::vector<PathPoint> path;
    std::string free_after;
    std::vector<std::string> reads;
};

struct MotionSpec {
    std::string slot;
    std::vector<PathPoint> path;
    std::string type;  // "jet", "orbit", "shift", ...
};

struct KickSpec {
    std::string kind;                 // "deep", "squib", "onside", "normal", "directional"
    std::optional<PathPoint> target;  // nullopt = "auto"
    std::string hang;
};

struct ReturnSpec {
    std::string scheme;
    PathPoint aim;
    std::string free_after;
};

struct Play {
    std::string id;
    std::string name;
    Unit unit = Unit::Offense;
    PlayType type = PlayType::Pass;
    std::string formation;
    bool flip = false;
    std::string coverage;  // defense: "cover_3", ...
    std::optional<PassSpec> pass;
    std::optional<RunSpec> run;
    std::optional<MotionSpec> motion;
    std::optional<KickSpec> kick;
    std::optional<ReturnSpec> return_spec;
    std::map<std::string, Assignment, std::less<>> assignments;  // slot -> assignment
    std::vector<std::string> tags;
};

struct Playbook {
    std::string id;
    std::string name;
    PlaybookSide side = PlaybookSide::Offense;
    std::string scheme;                      // key for per-scheme Playbook Knowledge
    std::vector<std::string> strength_call;  // defense: "te", "passing_strength", "field"
    std::vector<std::string> formations;
    std::vector<Play> plays;

    const Play* find(std::string_view play_id) const;
};

}  // namespace cfbsim
