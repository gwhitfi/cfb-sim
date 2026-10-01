#include "data/parse_common.h"

#include "data/vocab.h"

namespace cfbsim::detail {

namespace {

bool is_special(Unit unit) { return unit == Unit::Kicking || unit == Unit::Receiving; }

Side parse_side(const Reader& r, Unit unit) {
    const Side side = r.as_enum(kSideNames, "side");
    if (side == Side::None) r.fail("side must not be 'none' here");
    const bool relative = side != Side::Left && side != Side::Right;
    if (relative && unit != Unit::Defense) {
        r.fail("side '" + std::string(to_string(side)) +
               "' is only allowed in defensive files (use left or right)");
    }
    return side;
}

}  // namespace

Anchor parse_anchor(const Reader& r, Unit unit, Axis axis) {
    if (r.is_number()) return Anchor::at(r.as_number());
    if (!r.is_object()) r.fail("expected a number or an anchor object");

    r.allow_only({"from", "tech", "over", "side", "offset"});
    Anchor a;
    a.offset = r.number_or("offset", 0.0);
    const int kinds = int(r.has("from")) + int(r.has("tech")) + int(r.has("over"));
    if (kinds != 1) r.fail("an anchor needs exactly one of 'from', 'tech', or 'over'");

    if (r.has("tech")) {
        if (unit != Unit::Defense) r.at("tech").fail("'tech' anchors are defense-only");
        if (axis != Axis::X) r.at("tech").fail("'tech' anchors are for x only");
        a.kind = AnchorKind::Tech;
        a.ref = r.string("tech");
        if (!contains(kTechniques, a.ref)) r.at("tech").fail("unknown technique '" + a.ref + "'");
        a.side = parse_side(r.at("side"), unit);
        return a;
    }
    if (r.has("over")) {
        if (unit != Unit::Defense) r.at("over").fail("'over' anchors are defense-only");
        if (axis != Axis::X) r.at("over").fail("'over' anchors are for x only");
        a.kind = AnchorKind::Receiver;
        a.ref = r.string("over");
        if (a.ref != "#1" && a.ref != "#2" && a.ref != "#3" && a.ref != "#4") {
            r.at("over").fail("receiver must be #1, #2, #3, or #4");
        }
        a.side = parse_side(r.at("side"), unit);
        return a;
    }

    const Reader from = r.at("from");
    const std::string name = from.as_string();
    bool needs_side = true;
    bool x_only = true;
    if (name == "ball") {
        a.kind = AnchorKind::Ball;
        needs_side = false;
        x_only = false;
    } else if (name == "numbers") {
        a.kind = AnchorKind::Numbers;
    } else if (name == "hash") {
        a.kind = AnchorKind::Hash;
    } else if (name == "sideline") {
        a.kind = AnchorKind::Sideline;
    } else if (name == "goal_line" || name == "restraining_line") {
        if (!is_special(unit)) from.fail("'" + name + "' anchors are special-teams-only");
        if (axis != Axis::Y) from.fail("'" + name + "' anchors are for y only");
        a.kind = name == "goal_line" ? AnchorKind::GoalLine : AnchorKind::RestrainingLine;
        needs_side = false;
        x_only = false;
    } else if (name.size() == 5 && name.starts_with("gap_") && name[4] >= 'A' && name[4] <= 'D') {
        a.kind = AnchorKind::Gap;
        a.ref = name.substr(4);
    } else {
        // Anything else names another slot; validated against the formation later.
        // Slot anchors never take a side, so a side here means a mistyped landmark.
        if (r.has("side")) {
            from.fail("unknown landmark '" + name +
                      "' (expected ball, numbers, hash, sideline, gap_A..gap_D, or a slot name)");
        }
        a.kind = AnchorKind::Slot;
        a.ref = name;
        needs_side = false;
        x_only = false;
    }

    if (x_only && axis != Axis::X) from.fail("'" + name + "' anchors are for x only");
    if (needs_side) {
        a.side = parse_side(r.at("side"), unit);
    } else if (r.has("side")) {
        r.at("side").fail("'" + name + "' anchors don't take a side");
    }
    return a;
}

PathPoint parse_path_point(const Reader& r, Unit unit) {
    r.allow_only({"x", "y", "type"});
    PathPoint p;
    p.x = parse_anchor(r.at("x"), unit, Axis::X);
    p.y = parse_anchor(r.at("y"), unit, Axis::Y);
    p.type = r.string_or("type", "");
    return p;
}

}  // namespace cfbsim::detail
