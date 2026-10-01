#include "cfbsim/field/geometry.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace cfbsim {

PlayFrame::PlayFrame(double yard_line, Hash hash, bool offense_right, bool flip)
    : dir_(offense_right ? 1.0 : -1.0), flip_(flip) {
    ball_.x = offense_right ? kLeftGoalLine + yard_line : kRightGoalLine - yard_line;
    // Facing +x, the offense's left is +y; facing -x, it is -y.
    const double left_hash_y = offense_right ? kFieldWidth - kHashFromSideline : kHashFromSideline;
    const double right_hash_y = kFieldWidth - left_hash_y;
    switch (hash) {
        case Hash::Left:
            ball_.y = left_hash_y;
            break;
        case Hash::Right:
            ball_.y = right_hash_y;
            break;
        case Hash::Middle:
            ball_.y = kFieldWidth / 2.0;
            break;
    }
}

double PlayFrame::sideline_x(Side side) const {
    // The play-frame x of the two sidelines (field y = 0 and y = 53.33).
    const double a = to_play({ball_.x, 0.0}).x;
    const double b = to_play({ball_.x, kFieldWidth}).x;
    if (side == Side::Left) return std::min(a, b);
    if (side == Side::Right) return std::max(a, b);
    throw GeometryError("sideline_x: side must be left or right");
}

double PlayFrame::hash_x(Side side) const {
    return sideline_x(side) + (side == Side::Left ? kHashFromSideline : -kHashFromSideline);
}

double PlayFrame::numbers_x(Side side) const {
    return sideline_x(side) + (side == Side::Left ? kNumbersFromSideline : -kNumbersFromSideline);
}

Side PlayFrame::field_side() const {
    return sideline_x(Side::Right) >= -sideline_x(Side::Left) ? Side::Right : Side::Left;
}

Side PlayFrame::boundary_side() const { return mirrored(field_side()); }

double PlayFrame::goal_line_y() const {
    const double goal_x = dir_ > 0.0 ? kRightGoalLine : kLeftGoalLine;
    return to_play({goal_x, ball_.y}).y;
}

Side mirrored(Side side) {
    if (side == Side::Left) return Side::Right;
    if (side == Side::Right) return Side::Left;
    return side;
}

// ---------------------------------------------------------------------------
// Gaps
// ---------------------------------------------------------------------------

double LineGaps::x(Side side, char letter) const {
    if (letter < 'A' || letter > 'D') throw GeometryError("gap must be A..D");
    const auto i = static_cast<std::size_t>(letter - 'A');
    if (side == Side::Left) return left[i];
    if (side == Side::Right) return right[i];
    throw GeometryError("gap side must be left or right");
}

LineGaps find_gaps(double center_x, std::span<const double> line_x, double max_split) {
    std::vector<double> xs(line_x.begin(), line_x.end());
    std::sort(xs.begin(), xs.end());

    // The linemen on one side: walk outward from the center while the split
    // to the next man is small enough. `step` is -1 (left) or +1 (right).
    const auto walk = [&](double step) {
        std::vector<double> men;
        double last = center_x;
        while (true) {
            // The nearest man beyond `last` on this side.
            std::optional<double> next;
            for (double x : xs) {
                if ((x - last) * step <= 1e-6) continue;
                if (!next || std::abs(x - last) < std::abs(*next - last)) next = x;
            }
            if (!next || std::abs(*next - last) > max_split) break;
            men.push_back(*next);
            last = *next;
            if (men.size() == 3) break;  // G, T, and an attached TE
        }
        if (men.size() < 2) {
            throw GeometryError(std::string("the line needs a guard and a tackle on the ") +
                                (step < 0 ? "left" : "right"));
        }
        // [A, B, C, D], from the inside out.
        const double g = men[0], t = men[1];
        std::array<double, 4> gaps{(center_x + g) / 2.0, (g + t) / 2.0, t + step * 1.0,
                                   t + step * 2.0};
        if (men.size() == 3) {
            const double te = men[2];
            gaps[2] = (t + te) / 2.0;
            gaps[3] = te + step * 1.0;
        }
        return gaps;
    };

    LineGaps gaps;
    gaps.left = walk(-1.0);
    gaps.right = walk(1.0);
    return gaps;
}

// ---------------------------------------------------------------------------
// Anchors
// ---------------------------------------------------------------------------

namespace {

// Left/right sides are mirrored with the play; field/boundary come from where
// the ball is; strong/weak need the defense's strength call.
Side landmark_side(Side side, const PlayFrame& frame) {
    switch (side) {
        case Side::Left:
        case Side::Right:
            return frame.flipped() ? mirrored(side) : side;
        case Side::Field:
            return frame.field_side();
        case Side::Boundary:
            return frame.boundary_side();
        case Side::Strong:
        case Side::Weak:
            throw GeometryError("strong/weak sides need the defense's strength call (phase 5)");
        case Side::None:
            break;
    }
    throw GeometryError("this anchor needs a side");
}

bool is_relative(Side side) { return side == Side::Field || side == Side::Boundary; }

}  // namespace

double resolve_x(const Anchor& a, const AnchorContext& ctx) {
    if (!ctx.frame) throw GeometryError("resolve_x: no play frame");
    const PlayFrame& frame = *ctx.frame;
    const double m = frame.mirror();

    switch (a.kind) {
        case AnchorKind::Ball:
            return m * a.offset;

        case AnchorKind::Numbers:
        case AnchorKind::Hash:
        case AnchorKind::Sideline: {
            const Side side = landmark_side(a.side, frame);
            const double base = a.kind == AnchorKind::Numbers ? frame.numbers_x(side)
                                : a.kind == AnchorKind::Hash  ? frame.hash_x(side)
                                                              : frame.sideline_x(side);
            // Relative sides sign the offset toward their own sideline
            // (+ = outside); left/right sign it in play-frame x (+ = right).
            const double sign = is_relative(a.side) ? (side == Side::Left ? -1.0 : 1.0) : m;
            return base + sign * a.offset;
        }

        case AnchorKind::Slot: {
            const auto pos = ctx.slot_pos ? ctx.slot_pos(a.ref) : std::nullopt;
            if (!pos) throw GeometryError("slot '" + a.ref + "' isn't placed");
            return pos->x + m * a.offset;
        }

        case AnchorKind::Gap: {
            if (!ctx.gaps) throw GeometryError("gap anchors need the line set at the snap");
            const Side side = landmark_side(a.side, frame);
            return ctx.gaps->x(side, a.ref.at(0)) + m * a.offset;
        }

        case AnchorKind::Tech:
        case AnchorKind::Receiver:
            throw GeometryError("defensive anchors come in phase 5");

        case AnchorKind::GoalLine:
        case AnchorKind::RestrainingLine:
            break;
    }
    throw GeometryError("this anchor is for y only");
}

double resolve_y(const Anchor& a, const AnchorContext& ctx) {
    switch (a.kind) {
        case AnchorKind::Ball:
            return a.offset;
        case AnchorKind::Slot: {
            const auto pos = ctx.slot_pos ? ctx.slot_pos(a.ref) : std::nullopt;
            if (!pos) throw GeometryError("slot '" + a.ref + "' isn't placed");
            return pos->y + a.offset;
        }
        case AnchorKind::GoalLine:
        case AnchorKind::RestrainingLine:
            throw GeometryError("special teams y anchors come in phase 9");
        default:
            break;
    }
    throw GeometryError("this anchor is for x only");
}

std::vector<Vec2> align_formation(const Formation& formation, const PlayFrame& frame) {
    std::map<std::string, Vec2, std::less<>> placed;
    std::set<std::string, std::less<>> in_progress;  // to catch A -> B -> A loops

    AnchorContext ctx;
    ctx.frame = &frame;
    // A slot anchored to another slot places that one first, on demand. The
    // lambda refers to `ctx` (by reference) to call itself recursively.
    ctx.slot_pos = [&](std::string_view name) -> std::optional<Vec2> {
        if (const auto it = placed.find(name); it != placed.end()) return it->second;
        const FormationSlot* slot = formation.slot(name);
        if (!slot) return std::nullopt;
        if (!in_progress.emplace(name).second) {
            throw GeometryError("formation " + formation.id + ": slot '" + std::string(name) +
                                "' is anchored to itself through other slots");
        }
        const Vec2 pos{resolve_x(slot->x, ctx), resolve_y(slot->y, ctx)};
        in_progress.erase(in_progress.find(name));
        placed.emplace(std::string(name), pos);
        return pos;
    };

    std::vector<Vec2> out;
    out.reserve(formation.slots.size());
    for (const FormationSlot& slot : formation.slots) out.push_back(*ctx.slot_pos(slot.name));
    return out;
}

}  // namespace cfbsim
