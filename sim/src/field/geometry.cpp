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
// the ball is; strong/weak from the defense's strength call.
Side landmark_side(Side side, const AnchorContext& ctx) {
    const PlayFrame& frame = *ctx.frame;
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
            if (!ctx.offense) throw GeometryError("strong/weak sides need the offense's set");
            return ctx.offense->resolve(side);
        case Side::None:
            break;
    }
    throw GeometryError("this anchor needs a side");
}

bool is_relative(Side side) { return side != Side::Left && side != Side::Right; }

// +1 toward the right sideline, -1 toward the left.
double outward(Side resolved) { return resolved == Side::Left ? -1.0 : 1.0; }

// A technique's x: head up, inside shade, or outside shade of a lineman.
double technique_x(std::string_view tech, Side side, const OffenseRead& o,
                   const AlignmentTuning& t) {
    const double out = outward(side);
    const auto& line = o.line[OffenseRead::index(side)];
    const double g = line[0];
    const double tackle = line[1];
    const double te = line.size() == 3 ? line[2] : tackle + out * t.ghost_te_split;
    if (tech == "0") return o.center_x;
    if (tech == "1") return (o.center_x + g) / 2.0;
    if (tech == "2i") return g - out * t.shade;
    if (tech == "2") return g;
    if (tech == "3") return g + out * t.shade;
    if (tech == "4i") return tackle - out * t.shade;
    if (tech == "4") return tackle;
    if (tech == "5") return tackle + out * t.shade;
    if (tech == "7") return te - out * t.shade;
    if (tech == "6") return te;
    if (tech == "9") return te + out * t.shade;
    throw GeometryError("unknown technique '" + std::string(tech) + "'");
}

// Receiver #n's x on `side`. A missing receiver is placed halfway between
// the last one there and the end of the line (or on the numbers if the side
// has no receivers at all).
double receiver_x(int n, Side side, const OffenseRead& o, const PlayFrame& frame) {
    const auto& recs = o.receivers[OffenseRead::index(side)];
    const auto k = static_cast<std::size_t>(n - 1);
    if (k < recs.size()) return o.pos[static_cast<std::size_t>(recs[k])].x;
    if (recs.empty()) return frame.numbers_x(side);
    const double last = o.pos[static_cast<std::size_t>(recs.back())].x;
    return (last + o.end_of_line(side)) / 2.0;
}

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
            const Side side = landmark_side(a.side, ctx);
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
            const Side side = landmark_side(a.side, ctx);
            return ctx.gaps->x(side, a.ref.at(0)) + m * a.offset;
        }

        case AnchorKind::Tech:
        case AnchorKind::Receiver: {
            if (!ctx.offense) throw GeometryError("defensive anchors need the offense's set");
            const Side side = landmark_side(a.side, ctx);
            const double base =
                a.kind == AnchorKind::Tech
                    ? technique_x(a.ref, side, *ctx.offense, ctx.alignment)
                    : receiver_x(a.ref.at(1) - '0', side, *ctx.offense, frame);
            // The offset is signed toward that side's sideline (+ = outside).
            return base + outward(side) * a.offset;
        }

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

std::vector<Vec2> align_formation(const Formation& formation, const PlayFrame& frame,
                                  const OffenseRead* offense, AlignmentTuning alignment) {
    std::map<std::string, Vec2, std::less<>> placed;
    std::set<std::string, std::less<>> in_progress;  // to catch A -> B -> A loops

    AnchorContext ctx;
    ctx.frame = &frame;
    ctx.offense = offense;
    ctx.alignment = alignment;
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

// ---------------------------------------------------------------------------
// Reading the offense
// ---------------------------------------------------------------------------

std::optional<int> OffenseRead::player(std::string_view target) const {
    if (target.starts_with("back_")) {
        const auto n = static_cast<std::size_t>(target.back() - '1');
        if (n < backs.size()) return backs[n];
        return std::nullopt;
    }
    // "#2_strong"
    if (target.size() < 4 || target[0] != '#') return std::nullopt;
    const auto n = static_cast<std::size_t>(target[1] - '1');
    const std::string_view side_name = target.substr(3);
    Side side = Side::None;
    if (side_name == "strong") side = Side::Strong;
    if (side_name == "weak") side = Side::Weak;
    if (side_name == "left") side = Side::Left;
    if (side_name == "right") side = Side::Right;
    if (side == Side::None) return std::nullopt;
    const auto& recs = receivers[index(resolve(side))];
    if (n < recs.size()) return recs[n];
    return std::nullopt;
}

Side OffenseRead::resolve(Side side) const {
    if (side == Side::Strong) return strong;
    if (side == Side::Weak) return mirrored(strong);
    return side;
}

OffenseRead read_offense(std::span<const Vec2> pos, const std::vector<bool>& on_line, int center,
                         int qb) {
    OffenseRead o;
    o.pos.assign(pos.begin(), pos.end());
    o.center_x = pos[static_cast<std::size_t>(center)].x;

    std::vector<double> line_x;
    for (std::size_t i = 0; i < pos.size(); ++i) {
        if (on_line[i]) line_x.push_back(pos[i].x);
    }
    // The same walk as the gaps: G, T, and an attached TE on each side.
    find_gaps(o.center_x, line_x);  // throws if a side has no G and T
    std::vector<int> interior{center};
    for (const Side side : {Side::Left, Side::Right}) {
        const double step = side == Side::Left ? -1.0 : 1.0;
        double last = o.center_x;
        auto& men = o.line[OffenseRead::index(side)];
        while (men.size() < 3) {
            int next = -1;
            for (std::size_t i = 0; i < pos.size(); ++i) {
                const double x = pos[i].x;
                if (!on_line[i] || (x - last) * step <= 1e-6) continue;
                if (next < 0 || std::abs(x - last) < std::abs(pos[static_cast<std::size_t>(next)].x - last)) {
                    next = static_cast<int>(i);
                }
            }
            if (next < 0 || std::abs(pos[static_cast<std::size_t>(next)].x - last) > 2.5) break;
            last = pos[static_cast<std::size_t>(next)].x;
            men.push_back(last);
            if (men.size() <= 2) interior.push_back(next);
        }
    }

    // Everyone else but the QB is a receiver: a back if he's in the
    // backfield near the middle, otherwise on the side he's on.
    std::array<std::vector<std::pair<double, int>>, 2> sides;
    std::vector<std::pair<double, int>> backs;
    for (std::size_t i = 0; i < pos.size(); ++i) {
        const int k = static_cast<int>(i);
        if (k == qb || std::find(interior.begin(), interior.end(), k) != interior.end()) continue;
        const Vec2 p = pos[i];
        const double dx = p.x - o.center_x;
        if (p.y < -3.0 && std::abs(dx) < 4.0) {
            backs.emplace_back(-p.y, k);  // sorted by depth: the shallowest is back_1
        } else {
            // Sort key: distance from the middle, widest first.
            sides[dx < 0.0 ? 0 : 1].emplace_back(-std::abs(dx), k);
        }
    }
    for (std::size_t s = 0; s < 2; ++s) {
        std::sort(sides[s].begin(), sides[s].end());
        for (const auto& [key, k] : sides[s]) o.receivers[s].push_back(k);
    }
    std::sort(backs.begin(), backs.end());
    for (const auto& [key, k] : backs) o.backs.push_back(k);
    return o;
}

Side strength_from_call(const OffenseRead& o, std::span<const std::string> calls,
                        const PlayFrame& frame) {
    for (const std::string& call : calls) {
        if (call == "te") {
            const bool l = o.has_te(Side::Left), r = o.has_te(Side::Right);
            if (l != r) return l ? Side::Left : Side::Right;
        } else if (call == "passing_strength") {
            const std::size_t l = o.receivers[0].size(), r = o.receivers[1].size();
            if (l != r) return l > r ? Side::Left : Side::Right;
        } else if (call == "field") {
            return frame.field_side();
        } else if (call == "boundary") {
            return frame.boundary_side();
        } else if (call == "back") {
            if (!o.backs.empty()) {
                const double dx = o.pos[static_cast<std::size_t>(o.backs.front())].x - o.center_x;
                if (std::abs(dx) > 0.5) return dx < 0.0 ? Side::Left : Side::Right;
            }
        }
    }
    return frame.field_side();
}

}  // namespace cfbsim
