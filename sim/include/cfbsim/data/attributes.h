// The universal attribute set (docs/cfbsim_design.md#ratings-and-attributes). Every player has every
// attribute. Playbook Knowledge is per scheme, so it lives on Player instead.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace cfbsim {

enum class Attr : std::uint8_t {
    // Physical
    Speed,
    Acceleration,
    Agility,
    Strength,
    Balance,
    Jumping,
    Stamina,
    NaturalFitness,
    // Mental (Playbook Knowledge is stored per scheme on Player)
    Anticipation,
    Decisions,
    Vision,
    Composure,
    Concentration,
    Discipline,
    Aggression,
    Bravery,
    WorkRate,
    Leadership,
    Determination,
    // Passing
    ThrowPower,
    ShortAccuracy,
    MediumAccuracy,
    DeepAccuracy,
    ThrowOnTheRun,
    Delivery,
    Touch,
    PocketPresence,
    CoverageReading,
    // Ball carrying
    BallSecurity,
    Elusiveness,
    BreakTackle,
    // Receiving
    Catching,
    RouteRunning,
    Release,
    CatchInTraffic,
    ContestedCatch,
    // Blocking
    RunBlocking,
    PassBlocking,
    BlockingInSpace,
    HandTechnique,
    Footwork,
    Snapping,
    // Pass rush & run defense
    BlockShedding,
    PowerMoves,
    FinesseMoves,
    // Coverage
    ManCoverage,
    ZoneCoverage,
    Press,
    BallSkills,
    // Tackling
    Tackling,
    HitPower,
    Pursuit,
    // Kicking
    KickPower,
    KickAccuracy,
    PuntPower,
    PuntAccuracy,
};
inline constexpr std::size_t kAttrCount = 56;

enum class AttrGroup : std::uint8_t {
    Physical,
    Mental,
    Passing,
    BallCarrying,
    Receiving,
    Blocking,
    PassRush,
    Coverage,
    Tackling,
    Kicking
};

struct AttrInfo {
    Attr attr;
    std::string_view key;    // JSON key (snake_case)
    std::string_view label;  // display name
    AttrGroup group;
};

// One row per attribute, in the same order as the enum.
inline constexpr std::array<AttrInfo, kAttrCount> kAttrInfo{{
    {Attr::Speed, "speed", "Speed", AttrGroup::Physical},
    {Attr::Acceleration, "acceleration", "Acceleration", AttrGroup::Physical},
    {Attr::Agility, "agility", "Agility", AttrGroup::Physical},
    {Attr::Strength, "strength", "Strength", AttrGroup::Physical},
    {Attr::Balance, "balance", "Balance", AttrGroup::Physical},
    {Attr::Jumping, "jumping", "Jumping", AttrGroup::Physical},
    {Attr::Stamina, "stamina", "Stamina", AttrGroup::Physical},
    {Attr::NaturalFitness, "natural_fitness", "Natural Fitness", AttrGroup::Physical},

    {Attr::Anticipation, "anticipation", "Anticipation", AttrGroup::Mental},
    {Attr::Decisions, "decisions", "Decisions", AttrGroup::Mental},
    {Attr::Vision, "vision", "Vision", AttrGroup::Mental},
    {Attr::Composure, "composure", "Composure", AttrGroup::Mental},
    {Attr::Concentration, "concentration", "Concentration", AttrGroup::Mental},
    {Attr::Discipline, "discipline", "Discipline", AttrGroup::Mental},
    {Attr::Aggression, "aggression", "Aggression", AttrGroup::Mental},
    {Attr::Bravery, "bravery", "Bravery", AttrGroup::Mental},
    {Attr::WorkRate, "work_rate", "Work Rate", AttrGroup::Mental},
    {Attr::Leadership, "leadership", "Leadership", AttrGroup::Mental},
    {Attr::Determination, "determination", "Determination", AttrGroup::Mental},

    {Attr::ThrowPower, "throw_power", "Throw Power", AttrGroup::Passing},
    {Attr::ShortAccuracy, "short_accuracy", "Short Accuracy", AttrGroup::Passing},
    {Attr::MediumAccuracy, "medium_accuracy", "Medium Accuracy", AttrGroup::Passing},
    {Attr::DeepAccuracy, "deep_accuracy", "Deep Accuracy", AttrGroup::Passing},
    {Attr::ThrowOnTheRun, "throw_on_the_run", "Throw on the Run", AttrGroup::Passing},
    {Attr::Delivery, "delivery", "Delivery", AttrGroup::Passing},
    {Attr::Touch, "touch", "Touch", AttrGroup::Passing},
    {Attr::PocketPresence, "pocket_presence", "Pocket Presence", AttrGroup::Passing},
    {Attr::CoverageReading, "coverage_reading", "Coverage Reading", AttrGroup::Passing},

    {Attr::BallSecurity, "ball_security", "Ball Security", AttrGroup::BallCarrying},
    {Attr::Elusiveness, "elusiveness", "Elusiveness", AttrGroup::BallCarrying},
    {Attr::BreakTackle, "break_tackle", "Break Tackle", AttrGroup::BallCarrying},

    {Attr::Catching, "catching", "Catching", AttrGroup::Receiving},
    {Attr::RouteRunning, "route_running", "Route Running", AttrGroup::Receiving},
    {Attr::Release, "release", "Release", AttrGroup::Receiving},
    {Attr::CatchInTraffic, "catch_in_traffic", "Catch in Traffic", AttrGroup::Receiving},
    {Attr::ContestedCatch, "contested_catch", "Contested Catch", AttrGroup::Receiving},

    {Attr::RunBlocking, "run_blocking", "Run Blocking", AttrGroup::Blocking},
    {Attr::PassBlocking, "pass_blocking", "Pass Blocking", AttrGroup::Blocking},
    {Attr::BlockingInSpace, "blocking_in_space", "Blocking in Space", AttrGroup::Blocking},
    {Attr::HandTechnique, "hand_technique", "Hand Technique", AttrGroup::Blocking},
    {Attr::Footwork, "footwork", "Footwork", AttrGroup::Blocking},
    {Attr::Snapping, "snapping", "Snapping", AttrGroup::Blocking},

    {Attr::BlockShedding, "block_shedding", "Block Shedding", AttrGroup::PassRush},
    {Attr::PowerMoves, "power_moves", "Power Moves", AttrGroup::PassRush},
    {Attr::FinesseMoves, "finesse_moves", "Finesse Moves", AttrGroup::PassRush},

    {Attr::ManCoverage, "man_coverage", "Man Coverage", AttrGroup::Coverage},
    {Attr::ZoneCoverage, "zone_coverage", "Zone Coverage", AttrGroup::Coverage},
    {Attr::Press, "press", "Press", AttrGroup::Coverage},
    {Attr::BallSkills, "ball_skills", "Ball Skills", AttrGroup::Coverage},

    {Attr::Tackling, "tackling", "Tackling", AttrGroup::Tackling},
    {Attr::HitPower, "hit_power", "Hit Power", AttrGroup::Tackling},
    {Attr::Pursuit, "pursuit", "Pursuit", AttrGroup::Tackling},

    {Attr::KickPower, "kick_power", "Kick Power", AttrGroup::Kicking},
    {Attr::KickAccuracy, "kick_accuracy", "Kick Accuracy", AttrGroup::Kicking},
    {Attr::PuntPower, "punt_power", "Punt Power", AttrGroup::Kicking},
    {Attr::PuntAccuracy, "punt_accuracy", "Punt Accuracy", AttrGroup::Kicking},
}};

// A compile-time check: if someone adds an attribute to the enum but puts its
// table row in the wrong place, the build fails instead of the sim silently
// reading the wrong rating.
constexpr bool attr_table_in_enum_order() {
    for (std::size_t i = 0; i < kAttrInfo.size(); ++i) {
        if (static_cast<std::size_t>(kAttrInfo[i].attr) != i) return false;
    }
    return static_cast<std::size_t>(Attr::PuntAccuracy) + 1 == kAttrCount;
}
static_assert(attr_table_in_enum_order(), "kAttrInfo must list every Attr in enum order");

constexpr const AttrInfo& info(Attr a) { return kAttrInfo[static_cast<std::size_t>(a)]; }

constexpr std::optional<Attr> attr_from_key(std::string_view key) {
    for (const auto& row : kAttrInfo) {
        if (row.key == key) return row.attr;
    }
    return std::nullopt;
}

// A player's ratings: one number (1-100, stored as double) per attribute.
class Attributes {
  public:
    double& operator[](Attr a) { return values_[index(a)]; }
    double operator[](Attr a) const { return values_[index(a)]; }

  private:
    static constexpr std::size_t index(Attr a) { return static_cast<std::size_t>(a); }
    std::array<double, kAttrCount> values_{};  // {} zero-fills the array
};

}  // namespace cfbsim
