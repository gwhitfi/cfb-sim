// Field geometry: frames, landmarks, anchors, gaps, and mirroring.

#include "cfbsim/field/geometry.h"

#include <gtest/gtest.h>

#include <vector>

#include "cfbsim/data/load.h"

using namespace cfbsim;

namespace {

constexpr double kEps = 1e-9;

const DataSet& data() {
    static const LoadReport report = load_data_dir(CFBSIM_DATA_DIR);
    return report.data;
}

Anchor landmark(AnchorKind kind, Side side, double offset) { return {kind, side, offset, {}}; }

}  // namespace

TEST(PlayFrame, BallSpotFromYardLineHashAndDirection) {
    const PlayFrame right(25, Hash::Middle, true, false);
    EXPECT_NEAR(right.ball().x, 35.0, kEps);  // own 25 going right: 10 + 25
    EXPECT_NEAR(right.ball().y, kFieldWidth / 2.0, kEps);

    const PlayFrame left(25, Hash::Middle, false, false);
    EXPECT_NEAR(left.ball().x, 85.0, kEps);  // own 25 going left: 110 - 25

    // The left hash is on the offense's left: +y going right, -y going left.
    EXPECT_NEAR(PlayFrame(25, Hash::Left, true, false).ball().y, kFieldWidth - 20.0, kEps);
    EXPECT_NEAR(PlayFrame(25, Hash::Left, false, false).ball().y, 20.0, kEps);
}

TEST(PlayFrame, PlayToFieldAndBack) {
    for (const bool offense_right : {true, false}) {
        const PlayFrame f(40, Hash::Right, offense_right, false);
        const double dir = offense_right ? 1.0 : -1.0;
        // +y is downfield, +x is the offense's right.
        EXPECT_NEAR(f.to_field({0, 10}).x, f.ball().x + 10 * dir, kEps);
        EXPECT_NEAR(f.to_field({3, 0}).y, f.ball().y - 3 * dir, kEps);
        const Vec2 p{-7.5, 12.25};
        const Vec2 back = f.to_play(f.to_field(p));
        EXPECT_NEAR(back.x, p.x, kEps);
        EXPECT_NEAR(back.y, p.y, kEps);
        EXPECT_NEAR(f.goal_line_y(), 60.0, kEps);  // own 40 -> 60 yd to go
    }
}

TEST(PlayFrame, Landmarks) {
    const PlayFrame middle(25, Hash::Middle, true, false);
    EXPECT_NEAR(middle.sideline_x(Side::Left), -kFieldWidth / 2.0, kEps);
    EXPECT_NEAR(middle.sideline_x(Side::Right), kFieldWidth / 2.0, kEps);
    EXPECT_NEAR(middle.hash_x(Side::Right), kFieldWidth / 2.0 - 20.0, kEps);
    EXPECT_NEAR(middle.numbers_x(Side::Left), -kFieldWidth / 2.0 + 8.0, kEps);

    // From the left hash the ball is 20 yd from the left sideline, and the
    // field side is the right.
    const PlayFrame left_hash(25, Hash::Left, false, false);
    EXPECT_NEAR(left_hash.sideline_x(Side::Left), -20.0, kEps);
    EXPECT_NEAR(left_hash.hash_x(Side::Left), 0.0, kEps);
    EXPECT_NEAR(left_hash.sideline_x(Side::Right), kFieldWidth - 20.0, kEps);
    EXPECT_EQ(left_hash.field_side(), Side::Right);
    EXPECT_EQ(left_hash.boundary_side(), Side::Left);
}

TEST(Anchors, LandmarksAndOffsets) {
    const PlayFrame f(25, Hash::Middle, true, false);
    AnchorContext ctx;
    ctx.frame = &f;
    EXPECT_NEAR(resolve_x(Anchor::at(2.5), ctx), 2.5, kEps);
    EXPECT_NEAR(resolve_x(landmark(AnchorKind::Numbers, Side::Left, 0), ctx), -18.6667, 1e-4);
    // Left/right offsets are signed in play x: -5 from the right sideline is 5 yd in.
    EXPECT_NEAR(resolve_x(landmark(AnchorKind::Sideline, Side::Right, -5), ctx),
                kFieldWidth / 2.0 - 5.0, kEps);
    EXPECT_NEAR(resolve_x(landmark(AnchorKind::Hash, Side::Right, 1), ctx), 7.6667, 1e-4);

    // Field/boundary offsets are signed toward their own sideline (+ = outside).
    const PlayFrame on_left(25, Hash::Left, true, false);  // field side = right
    ctx.frame = &on_left;
    EXPECT_NEAR(resolve_x(landmark(AnchorKind::Numbers, Side::Field, 1), ctx),
                on_left.numbers_x(Side::Right) + 1.0, kEps);
    EXPECT_NEAR(resolve_x(landmark(AnchorKind::Numbers, Side::Boundary, 1), ctx),
                on_left.numbers_x(Side::Left) - 1.0, kEps);

    // Strong/weak need the defense; gaps need the line.
    EXPECT_THROW(resolve_x(landmark(AnchorKind::Hash, Side::Strong, 0), ctx), GeometryError);
    EXPECT_THROW(resolve_x(Anchor{AnchorKind::Gap, Side::Right, 0, "B"}, ctx), GeometryError);
}

TEST(Anchors, MirroringNegatesXAndSwapsSides) {
    const PlayFrame plain(25, Hash::Middle, true, false);
    const PlayFrame flipped(25, Hash::Middle, true, true);
    AnchorContext a, b;
    a.frame = &plain;
    b.frame = &flipped;
    const std::vector<Anchor> anchors{Anchor::at(2.5), landmark(AnchorKind::Numbers, Side::Left, 0),
                                      landmark(AnchorKind::Hash, Side::Right, 1),
                                      landmark(AnchorKind::Sideline, Side::Left, 5)};
    for (const Anchor& anchor : anchors) {
        EXPECT_NEAR(resolve_x(anchor, b), -resolve_x(anchor, a), kEps);
    }
    // Field/boundary come from the ball, not the author, so they don't mirror.
    const Anchor field = landmark(AnchorKind::Numbers, Side::Field, 0);
    EXPECT_NEAR(resolve_x(field, b), resolve_x(field, a), kEps);
}

TEST(Anchors, FormationAlignmentUsesSlotAnchors) {
    const Formation& f = data().formations.at("gun_2x2");
    const PlayFrame frame(25, Hash::Middle, true, false);
    const std::vector<Vec2> pos = align_formation(f, frame);
    const auto at = [&](std::string_view slot) {
        for (std::size_t i = 0; i < f.slots.size(); ++i) {
            if (f.slots[i].name == slot) return pos[i];
        }
        return Vec2{999, 999};
    };
    EXPECT_NEAR(at("X").x, frame.numbers_x(Side::Left), kEps);
    EXPECT_NEAR(at("H").x, at("X").x + 6.0, kEps);  // {"from": "X", "offset": 6}
    EXPECT_NEAR(at("Y").x, at("Z").x - 6.0, kEps);  // {"from": "Z", "offset": -6}
    EXPECT_NEAR(at("QB").y, -5.0, kEps);
}

TEST(Anchors, FlippedFromOneHashMirrorsTheOtherHash) {
    // A flipped play from the left hash is the exact mirror image of the
    // normal play from the right hash.
    for (const auto& [id, f] : data().formations) {
        if (f.unit != Unit::Offense) continue;
        const auto normal = align_formation(f, PlayFrame(25, Hash::Right, true, false));
        const auto flipped = align_formation(f, PlayFrame(25, Hash::Left, true, true));
        ASSERT_EQ(normal.size(), flipped.size());
        for (std::size_t i = 0; i < normal.size(); ++i) {
            EXPECT_NEAR(flipped[i].x, -normal[i].x, 1e-9) << id << " " << f.slots[i].name;
            EXPECT_NEAR(flipped[i].y, normal[i].y, 1e-9) << id << " " << f.slots[i].name;
        }
    }
}

TEST(Anchors, SlotLoopsAreReported) {
    Formation f;
    f.id = "loop";
    f.slots.push_back(
        {"A", std::nullopt, Anchor{AnchorKind::Slot, Side::None, 1, "B"}, Anchor::at(-1)});
    f.slots.push_back(
        {"B", std::nullopt, Anchor{AnchorKind::Slot, Side::None, 1, "A"}, Anchor::at(-1)});
    EXPECT_THROW(align_formation(f, PlayFrame(25, Hash::Middle, true, false)), GeometryError);
}

TEST(Gaps, FromTheLinemensPositions) {
    // LT LG C RG RT at 1.4 yd splits, plus a TE attached on the right.
    const std::vector<double> line{-2.8, -1.4, 0.0, 1.4, 2.8, 4.2, -15.0};  // and a split end
    const LineGaps g = find_gaps(0.0, line);
    EXPECT_NEAR(g.x(Side::Right, 'A'), 0.7, kEps);
    EXPECT_NEAR(g.x(Side::Right, 'B'), 2.1, kEps);
    EXPECT_NEAR(g.x(Side::Right, 'C'), 3.5, kEps);  // between the T and the TE
    EXPECT_NEAR(g.x(Side::Right, 'D'), 5.2, kEps);  // outside the TE
    EXPECT_NEAR(g.x(Side::Left, 'A'), -0.7, kEps);
    EXPECT_NEAR(g.x(Side::Left, 'C'), -3.8, kEps);  // no TE: outside the T
    EXPECT_NEAR(g.x(Side::Left, 'D'), -4.8, kEps);

    // Gaps move with the linemen: wider splits, wider gaps.
    const LineGaps wide = find_gaps(0.0, std::vector<double>{-4.0, -2.0, 0.0, 2.0, 4.0});
    EXPECT_NEAR(wide.x(Side::Right, 'B'), 3.0, kEps);

    EXPECT_THROW(find_gaps(0.0, std::vector<double>{-1.4, 0.0, 1.4}), GeometryError);
}
