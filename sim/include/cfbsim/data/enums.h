// Enumerations used across the data files, with their JSON spellings.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace cfbsim {

// Pairs an enum value with the string used for it in JSON files.
template <typename E>
struct EnumName {
    E value;
    std::string_view name;
};

// Look up an enum value by its JSON name. A template works for every enum
// table: the compiler generates one version per enum type it's used with.
template <typename E, std::size_t N>
constexpr std::optional<E> from_name(const std::array<EnumName<E>, N>& table,
                                     std::string_view name) {
    for (const auto& entry : table) {
        if (entry.name == name) return entry.value;
    }
    return std::nullopt;
}

template <typename E, std::size_t N>
constexpr std::string_view to_name(const std::array<EnumName<E>, N>& table, E value) {
    for (const auto& entry : table) {
        if (entry.value == value) return entry.name;
    }
    return "?";
}

// `enum class` (a "scoped enum") doesn't convert to int silently and its names
// don't leak into the surrounding namespace: you must write Position::QB.
// `: std::uint8_t` makes each value one byte.

// ---------------------------------------------------------------------------
// Positions (roster, depth chart, position familiarity)
// ---------------------------------------------------------------------------
enum class Position : std::uint8_t {
    QB,
    RB,
    FB,
    WR,
    TE,
    OT,
    OG,
    C,
    DE,
    DT,
    OLB,
    MLB,
    CB,
    FS,
    SS,
    K,
    P,
    LS
};
inline constexpr std::array<EnumName<Position>, 18> kPositionNames{{
    {Position::QB, "QB"},
    {Position::RB, "RB"},
    {Position::FB, "FB"},
    {Position::WR, "WR"},
    {Position::TE, "TE"},
    {Position::OT, "OT"},
    {Position::OG, "OG"},
    {Position::C, "C"},
    {Position::DE, "DE"},
    {Position::DT, "DT"},
    {Position::OLB, "OLB"},
    {Position::MLB, "MLB"},
    {Position::CB, "CB"},
    {Position::FS, "FS"},
    {Position::SS, "SS"},
    {Position::K, "K"},
    {Position::P, "P"},
    {Position::LS, "LS"},
}};

// ---------------------------------------------------------------------------
// Class year
// ---------------------------------------------------------------------------
enum class ClassYear : std::uint8_t { FR, SO, JR, SR, GR };
inline constexpr std::array<EnumName<ClassYear>, 5> kClassYearNames{{
    {ClassYear::FR, "FR"},
    {ClassYear::SO, "SO"},
    {ClassYear::JR, "JR"},
    {ClassYear::SR, "SR"},
    {ClassYear::GR, "GR"},
}};

// ---------------------------------------------------------------------------
// Which unit a formation or play belongs to
// ---------------------------------------------------------------------------
enum class Unit : std::uint8_t { Offense, Defense, Kicking, Receiving };
inline constexpr std::array<EnumName<Unit>, 4> kUnitNames{{
    {Unit::Offense, "offense"},
    {Unit::Defense, "defense"},
    {Unit::Kicking, "kicking"},
    {Unit::Receiving, "receiving"},
}};

// Which units a playbook holds. "special" holds both kicking and receiving plays.
enum class PlaybookSide : std::uint8_t { Offense, Defense, Special };
inline constexpr std::array<EnumName<PlaybookSide>, 3> kPlaybookSideNames{{
    {PlaybookSide::Offense, "offense"},
    {PlaybookSide::Defense, "defense"},
    {PlaybookSide::Special, "special"},
}};

// ---------------------------------------------------------------------------
// Sides: absolute (left/right in the play frame) or relative to the defense's
// strength call and the field.
// ---------------------------------------------------------------------------
enum class Side : std::uint8_t { None, Left, Right, Strong, Weak, Field, Boundary };
inline constexpr std::array<EnumName<Side>, 7> kSideNames{{
    {Side::None, "none"},
    {Side::Left, "left"},
    {Side::Right, "right"},
    {Side::Strong, "strong"},
    {Side::Weak, "weak"},
    {Side::Field, "field"},
    {Side::Boundary, "boundary"},
}};

// ---------------------------------------------------------------------------
// Stances
// ---------------------------------------------------------------------------
enum class Stance : std::uint8_t { TwoPoint, ThreePoint, Snap, UnderCenter, Gun, Pistol };
inline constexpr std::array<EnumName<Stance>, 6> kStanceNames{{
    {Stance::TwoPoint, "two_point"},
    {Stance::ThreePoint, "three_point"},
    {Stance::Snap, "snap"},
    {Stance::UnderCenter, "under_center"},
    {Stance::Gun, "gun"},
    {Stance::Pistol, "pistol"},
}};

// ---------------------------------------------------------------------------
// Play types
// ---------------------------------------------------------------------------
enum class PlayType : std::uint8_t {
    Pass,
    Run,
    Defense,
    Kickoff,
    KickoffReturn,
    Punt,
    PuntReturn,
    FieldGoal,
    FieldGoalBlock
};
inline constexpr std::array<EnumName<PlayType>, 9> kPlayTypeNames{{
    {PlayType::Pass, "pass"},
    {PlayType::Run, "run"},
    {PlayType::Defense, "defense"},
    {PlayType::Kickoff, "kickoff"},
    {PlayType::KickoffReturn, "kickoff_return"},
    {PlayType::Punt, "punt"},
    {PlayType::PuntReturn, "punt_return"},
    {PlayType::FieldGoal, "field_goal"},
    {PlayType::FieldGoalBlock, "field_goal_block"},
}};

// Convenience overloads so any of these enums can be printed with to_string().
constexpr std::string_view to_string(Position v) { return to_name(kPositionNames, v); }
constexpr std::string_view to_string(ClassYear v) { return to_name(kClassYearNames, v); }
constexpr std::string_view to_string(Unit v) { return to_name(kUnitNames, v); }
constexpr std::string_view to_string(PlaybookSide v) { return to_name(kPlaybookSideNames, v); }
constexpr std::string_view to_string(Side v) { return to_name(kSideNames, v); }
constexpr std::string_view to_string(Stance v) { return to_name(kStanceNames, v); }
constexpr std::string_view to_string(PlayType v) { return to_name(kPlayTypeNames, v); }

}  // namespace cfbsim
