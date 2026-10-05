// Field geometry: the play frame <-> field frame conversion, landmarks
// (hashes, numbers, sidelines), anchor resolution, line gaps, and mirroring
// (docs/cfbsim_design.md#coordinates-and-anchors).
#pragma once

#include <array>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "cfbsim/core/vec.h"
#include "cfbsim/data/formation.h"

namespace cfbsim {

// Field frame constants, in yards.
inline constexpr double kFieldLength = 120.0;
inline constexpr double kFieldWidth = 160.0 / 3.0;  // 53.33
inline constexpr double kLeftGoalLine = 10.0;
inline constexpr double kRightGoalLine = 110.0;
inline constexpr double kHashFromSideline = 20.0;    // NCAA: 60 ft
inline constexpr double kNumbersFromSideline = 8.0;  // the middle of the numbers

// Where the ball is spotted across the field, from the offense's point of view.
enum class Hash : std::uint8_t { Left, Middle, Right };
inline constexpr std::array<EnumName<Hash>, 3> kHashNames{{
    {Hash::Left, "left"},
    {Hash::Middle, "middle"},
    {Hash::Right, "right"},
}};

// Thrown when a formation or play can't be laid out (an anchor that can't be
// resolved yet, a missing line, ...). Data validation catches most problems
// at load time; these are the ones that depend on the actual snap.
class GeometryError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;  // reuse the base class constructors
};

// One snap's coordinate system. The play frame has its origin at the ball,
// +x to the offense's right and +y downfield; the field frame is the
// engine's (x along the field 0..120, y across it 0..53.33).
class PlayFrame {
  public:
    // `yard_line`: the offense's own yard line (25 = its own 25, 60 = the
    // opponent's 40). `offense_right`: the offense moves toward +x.
    // `flip`: the play runs mirrored.
    PlayFrame(double yard_line, Hash hash, bool offense_right, bool flip);

    Vec2 ball() const { return ball_; }  // field frame
    double dir() const { return dir_; }  // +1 or -1: field x per yard downfield
    bool flipped() const { return flip_; }
    // Multiply an authored x (or x offset) by this to mirror it: -1 when flipped.
    double mirror() const { return flip_ ? -1.0 : 1.0; }
    // The same frame, not mirrored.
    PlayFrame unflipped() const {
        PlayFrame f = *this;
        f.flip_ = false;
        return f;
    }

    // Positions and directions between the frames.
    Vec2 to_field(Vec2 play) const { return {ball_.x + dir_ * play.y, ball_.y - dir_ * play.x}; }
    Vec2 to_play(Vec2 field) const {
        const Vec2 d = field - ball_;
        return {-dir_ * d.y, dir_ * d.x};
    }
    Vec2 dir_to_field(Vec2 play) const { return {dir_ * play.y, -dir_ * play.x}; }
    Vec2 dir_to_play(Vec2 field) const { return {-dir_ * field.y, dir_ * field.x}; }

    // Play-frame x of the landmarks on each side (Left/Right only).
    double sideline_x(Side side) const;
    double hash_x(Side side) const;
    double numbers_x(Side side) const;

    // The side with more room (ties go right), and the other one.
    Side field_side() const;
    Side boundary_side() const;

    // Play-frame y of the goal line the offense is attacking (and its own).
    double goal_line_y() const;
    double own_goal_line_y() const { return goal_line_y() - 100.0; }

  private:
    Vec2 ball_;
    double dir_ = 1.0;
    bool flip_ = false;
};

// Swaps Left and Right; leaves every other side alone.
Side mirrored(Side side);

// Line gaps, worked out from the offensive line's actual positions.
// A: between C and G. B: between G and T. C: outside the T (between the T and
// an attached TE). D: outside an attached TE.
struct LineGaps {
    std::array<double, 4> left{};   // play-frame x of the A..D gaps on the left
    std::array<double, 4> right{};  // ... and on the right

    // `letter` is 'A'..'D'; `side` Left or Right.
    double x(Side side, char letter) const;
};

// `line_x`: play-frame x of every player on the line of scrimmage;
// `center_x`: the snapper's. Linemen are counted outward from the center while
// the split to the next man is at most `max_split` yd; the first two on each
// side are the G and T and a third is an attached TE.
// Throws GeometryError if either side has fewer than a G and a T.
LineGaps find_gaps(double center_x, std::span<const double> line_x, double max_split = 2.5);

// What a defense sees when it lines up: the offense's line, its receivers
// counted from the sideline in, its backs, and the strength call
// (cfbsim_design.md#reading-the-offense). Positions are play-frame.
struct OffenseRead {
    double center_x = 0.0;
    // Per side (index 0 = left, 1 = right), from the inside out: G, T, and an
    // attached TE if there is one.
    std::array<std::vector<double>, 2> line;
    // Per side: agent indices of the receivers, #1 (widest) first.
    std::array<std::vector<int>, 2> receivers;
    std::vector<int> backs;  // agent indices of backs in the middle (back_1 first)
    std::vector<Vec2> pos;   // every offensive player's position, by agent index
    Side strong = Side::Right;

    static constexpr std::size_t index(Side side) { return side == Side::Left ? 0 : 1; }
    bool has_te(Side side) const { return line[index(side)].size() == 3; }
    // play-frame x of the outermost lineman (the TE if attached) on `side`.
    double end_of_line(Side side) const { return line[index(side)].back(); }
    // A man coverage target ("#2_strong", "#1_weak", "back_1"): the agent
    // index, or nullopt if the offense has no such player.
    std::optional<int> player(std::string_view target) const;
    // Left/Right for a strong/weak side; any other side unchanged.
    Side resolve(Side side) const;
};

// Reads the offense's set. `pos`: each player's play-frame position;
// `on_line`: whether his formation slot is on the line; `center`, `qb`: agent
// indices. Throws GeometryError if the line has no G and T on a side.
// (std::vector<bool> is a packed special case that a std::span can't view,
// so it is passed by const reference.)
OffenseRead read_offense(std::span<const Vec2> pos, const std::vector<bool>& on_line, int center,
                         int qb);

// Applies a defensive playbook's strength call ("te", "passing_strength",
// "field", "boundary", "back"): the first call that picks a side wins; the
// field side if none does.
Side strength_from_call(const OffenseRead& offense, std::span<const std::string> calls,
                        const PlayFrame& frame);

// Defensive alignment constants (data/tuning/defense.json "alignment").
struct AlignmentTuning {
    double shade = 0.5;           // yd: a shade technique is this far off the man
    double ghost_te_split = 1.4;  // yd outside the T where a missing TE would be
};

// What anchors can refer to while one formation or play is being laid out.
struct AnchorContext {
    const PlayFrame* frame = nullptr;
    const OffenseRead* offense = nullptr;  // defense only: technique, receiver, strong/weak
    AlignmentTuning alignment;
    // Aligned play-frame position of a slot, or nullopt if it isn't placed (yet).
    std::function<std::optional<Vec2>(std::string_view slot)> slot_pos;
    const LineGaps* gaps = nullptr;  // only once the line is set
};

// Play-frame x / y of an authored coordinate, with mirroring applied. Throws
// GeometryError for anchors this context can't resolve (defensive anchors
// without an offense, special teams y anchors until phase 9, gaps before the snap).
double resolve_x(const Anchor& a, const AnchorContext& ctx);
double resolve_y(const Anchor& a, const AnchorContext& ctx);

// Every slot's aligned play-frame position, in formation order. Slots
// anchored to other slots are placed after them, wherever they are listed.
// `offense`: for a defensive formation, the offense it lines up against.
std::vector<Vec2> align_formation(const Formation& formation, const PlayFrame& frame,
                                  const OffenseRead* offense = nullptr,
                                  AlignmentTuning alignment = {});

}  // namespace cfbsim
