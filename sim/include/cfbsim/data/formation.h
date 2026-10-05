// Position anchors and formations (docs/cfbsim_design.md#part-iii-play-data).
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cfbsim/data/enums.h"

namespace cfbsim {

// Where a coordinate is measured from. A plain number in JSON is kind Ball.
enum class AnchorKind : std::uint8_t {
    Ball,             // offset from the ball
    Numbers,          // the field numbers on `side`
    Hash,             // the hash mark on `side`
    Sideline,         // the sideline on `side`
    GoalLine,         // y only: the receiving team's goal line (special teams)
    RestrainingLine,  // y only: the receiving team's kickoff restraining line
    Slot,             // another slot's aligned position (ref = slot name)
    Gap,              // an offensive line gap (ref = "A".."D")
    Tech,             // defense: technique alignment on an OL/TE (ref = "3", "2i", ...)
    Receiver,         // defense: a receiver counted from the sideline (ref = "#1".."#3")
};

struct Anchor {
    AnchorKind kind = AnchorKind::Ball;
    Side side = Side::None;
    double offset = 0.0;  // yards; for x, signed in the play frame (+ = offense's right)
    std::string ref;

    static Anchor at(double offset) { return Anchor{AnchorKind::Ball, Side::None, offset, {}}; }
};

struct FormationSlot {
    std::string name;             // "LT", "X", "MIKE", "L5"
    std::optional<Position> pos;  // expected position; nullopt = "any" (special teams)
    Anchor x;
    Anchor y;
    Stance stance = Stance::TwoPoint;
    bool on_line = false;
};

struct Formation {
    std::string id;
    std::string name;
    Unit unit = Unit::Offense;
    std::string personnel;             // offense: "11", "12", ...
    std::string package;               // defense: "base", "nickel", ...
    std::string special_unit;          // special teams: "kickoff", "punt", ...
    std::vector<FormationSlot> slots;  // in file order; this is the sim's update order

    const FormationSlot* slot(std::string_view slot_name) const;
};

}  // namespace cfbsim
