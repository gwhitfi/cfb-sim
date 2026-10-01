// Parser for formations/{offense,defense,special}/*.json.

#include <functional>
#include <set>

#include "data/parse_common.h"

namespace cfbsim {

using detail::Reader;

namespace {

// Slot anchors can chain ("H" is 6 yds from "X"). Reject chains that loop
// back on themselves, since they could never be resolved.
void check_slot_anchor_cycles(const Formation& f, const Reader& slots) {
    const std::function<void(const FormationSlot&, std::set<std::string>&)> visit =
        [&](const FormationSlot& slot, std::set<std::string>& seen) {
            if (!seen.insert(slot.name).second) {
                slots.at(slot.name).fail("slot anchors form a loop");
            }
            for (const Anchor* a : {&slot.x, &slot.y}) {
                if (a->kind != AnchorKind::Slot) continue;
                if (const FormationSlot* next = f.slot(a->ref)) visit(*next, seen);
            }
            seen.erase(slot.name);
        };
    for (const FormationSlot& slot : f.slots) {
        std::set<std::string> seen;
        visit(slot, seen);
    }
}

}  // namespace

Formation parse_formation(const Json& json, const std::string& source) {
    const Reader root(json, source);
    root.allow_only(
        {"format_version", "id", "name", "side", "personnel", "package", "unit", "slots"});
    detail::require_format_version(root);

    Formation f;
    f.id = root.string("id");
    f.name = root.string("name");
    f.unit = root.at("side").as_enum(kUnitNames, "side");

    const bool special = f.unit == Unit::Kicking || f.unit == Unit::Receiving;
    if (f.unit == Unit::Offense) f.personnel = root.string("personnel");
    if (f.unit == Unit::Defense) f.package = root.string("package");
    if (special) f.special_unit = root.string("unit");

    const Reader slots = root.at("slots");
    slots.for_each_member([&](const std::string& name, const Reader& r) {
        r.allow_only({"pos", "x", "y", "stance", "on_line"});
        FormationSlot slot;
        slot.name = name;
        const std::string pos = r.string("pos");
        if (pos == "any") {
            if (!special) r.at("pos").fail("pos 'any' is only allowed in special teams");
        } else {
            slot.pos = r.at("pos").as_enum(kPositionNames, "position");
        }
        slot.x = detail::parse_anchor(r.at("x"), f.unit, detail::Axis::X);
        slot.y = detail::parse_anchor(r.at("y"), f.unit, detail::Axis::Y);
        if (auto stance = r.find("stance")) slot.stance = stance->as_enum(kStanceNames, "stance");
        slot.on_line = r.boolean_or("on_line", false);
        f.slots.push_back(std::move(slot));
    });

    // --- legality and consistency checks ---
    if (f.slots.size() != 11) {
        slots.fail("a formation needs exactly 11 slots, found " + std::to_string(f.slots.size()));
    }
    for (const FormationSlot& slot : f.slots) {
        for (const Anchor* a : {&slot.x, &slot.y}) {
            if (a->kind != AnchorKind::Slot) continue;
            if (a->ref == slot.name) slots.at(slot.name).fail("a slot can't anchor to itself");
            if (f.slot(a->ref) == nullptr) {
                slots.at(slot.name).fail("anchors to unknown slot '" + a->ref + "'");
            }
        }
    }
    check_slot_anchor_cycles(f, slots);

    if (f.unit == Unit::Offense) {
        int on_line = 0;
        int snappers = 0;
        for (const FormationSlot& slot : f.slots) {
            on_line += slot.on_line ? 1 : 0;
            snappers += slot.stance == Stance::Snap ? 1 : 0;
        }
        if (on_line < 7) {
            slots.fail("offense needs at least 7 players on the line, found " +
                       std::to_string(on_line));
        }
        if (snappers != 1) slots.fail("offense needs exactly one slot with stance 'snap'");
    }
    return f;
}

}  // namespace cfbsim
