// The offense between plays (docs/design/game-flow.md, step 7): before the
// call it gathers loosely behind the ball, the ring forms when the call comes
// in (a sugar huddle's near the ball), the break is staggered (the line
// first), and with no huddle it waits in its base formation and shifts.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "cfbsim/play/interlude.h"

using namespace cfbsim;

namespace {

const DataSet& data() {
    static const LoadReport report = load_data_dir(CFBSIM_DATA_DIR);
    return report.data;
}

const GameTuning& tuning() { return data().tuning.game; }

const PlayFrame kFrame(25.0, Hash::Middle, true, false);

// clemson_2018's players for a formation's slots (by each slot's position),
// and where the formation lines them up (field frame).
struct Lineup {
    std::vector<BetweenMan> men;
    std::vector<Spot> spots;
};

Lineup unit(const std::string& formation) {
    const Formation& f = data().formations.at(formation);
    const Roster& r = data().rosters.at("clemson_2018");
    const std::vector<Vec2> at = align_formation(f, kFrame);
    std::set<std::string> used;
    Lineup u;
    for (std::size_t k = 0; k < f.slots.size(); ++k) {
        const FormationSlot& slot = f.slots[k];
        const auto p = std::find_if(r.players.begin(), r.players.end(), [&](const Player& x) {
            return x.position == *slot.pos && !used.contains(x.id);
        });
        if (p == r.players.end()) {
            ADD_FAILURE() << "no " << slot.name;
            continue;
        }
        used.insert(p->id);
        u.men.push_back({&*p, TeamSide::Home, slot.name, std::nullopt});
        u.spots.push_back({p->id, slot.name, TeamSide::Home, kFrame.to_field(at[k]), kFrame.dir_to_field({0.0, 1.0}), {}});
    }
    return u;
}

// The formation set at the ball, by slot only (a look, as the game gives it).
std::vector<Spot> look(const std::string& formation) {
    std::vector<Spot> out = unit(formation).spots;
    for (Spot& s : out) s.id.clear();
    return out;
}

std::unique_ptr<Between> stretch(Tempo tempo, Rng& rng) {
    return std::make_unique<Between>(nullptr, TrackingInfo{}, kFrame, TeamSide::Home, tempo, data().tuning.movement,
                                     data().tuning.play, tuning(), &rng);
}

// Yards behind the ball (the play frame's -y).
double behind(Vec2 field) { return -kFrame.to_play(field).y; }

bool is_lineman(const Player& p) {
    return p.position == Position::OT || p.position == Position::OG || p.position == Position::C;
}

std::map<std::string, Spot> by_id(const Between& b) {
    std::map<std::string, Spot> out;
    for (const Spot& s : b.where()) out[s.id] = s;
    return out;
}

// Steps until `done`, at most a minute; returns the tick.
template <typename Done>
std::int64_t step_until(Between& b, std::int64_t from, Done done) {
    for (std::int64_t t = from; t < from + 60 * kTicksPerSecond; t = Between::next_step(t)) {
        b.run_to(t);
        if (done()) return t;
    }
    ADD_FAILURE() << "never";
    return -1;
}

}  // namespace

TEST(Huddle, TheBreakIsStaggered) {
    // The line (on it, inside the ends) leaves first, then the receivers,
    // then the QB and backs.
    const std::map<std::string, double> gun = break_stagger(data().formations.at("gun_2x2"), tuning());
    for (const char* s : {"LT", "LG", "C", "RG", "RT"}) EXPECT_EQ(gun.at(s), 0.0) << s;
    for (const char* s : {"X", "H", "Y", "Z"}) EXPECT_EQ(gun.at(s), tuning().stagger_receivers) << s;
    for (const char* s : {"QB", "RB"}) EXPECT_EQ(gun.at(s), tuning().stagger_backs) << s;
    // An inline tight end is an end: he goes with the receivers.
    const std::map<std::string, double> sb = break_stagger(data().formations.at("singleback_2x2"), tuning());
    EXPECT_EQ(sb.at("Y"), tuning().stagger_receivers);
    EXPECT_EQ(sb.at("RT"), 0.0);
    EXPECT_EQ(sb.at("QB"), tuning().stagger_backs);
    EXPECT_LT(tuning().stagger_receivers, tuning().stagger_backs);
}

TEST(Huddle, GathersThenHuddles) {
    // Before the call: a loose cluster behind the ball, the linemen nearer
    // it, everyone facing his sideline (home: -y). The ring forms only when
    // the call comes in: huddle_depth behind the ball, a sugar huddle's sugar_depth.
    for (const Tempo tempo : {Tempo::Huddle, Tempo::Sugar}) {
        const Lineup u = unit("gun_2x2");
        Rng rng(1);
        const auto b = stretch(tempo, rng);
        b->start(u.men, 0, std::nullopt);
        b->clock(40.0, 0);
        step_until(*b, 0, [&] { return b->assembled(TeamSide::Home); });
        EXPECT_FALSE(b->gathered(TeamSide::Home));
        double line_deepest = 0.0, others_nearest = 100.0;
        for (const BetweenMan& m : u.men) {
            const Spot s = by_id(*b).at(m.player->id);
            EXPECT_NEAR((s.facing - Vec2{0.0, -1.0}).length(), 0.0, 1e-9) << m.label;
            if (is_lineman(*m.player)) {
                line_deepest = std::max(line_deepest, behind(s.pos));
            } else {
                others_nearest = std::min(others_nearest, behind(s.pos));
            }
        }
        EXPECT_GT(line_deepest, 3.0);
        EXPECT_LT(line_deepest, others_nearest);

        // The call: the ring.
        b->lineup(200, TeamSide::Home, u.men, u.spots, true, 200 + 2 * kTicksPerSecond);
        EXPECT_FALSE(b->gathered(TeamSide::Home));
        step_until(*b, 200, [&] { return b->gathered(TeamSide::Home); });
        double center = 0.0;
        for (const Spot& s : b->where()) center += behind(s.pos) / static_cast<double>(u.men.size());
        const double depth = tempo == Tempo::Sugar ? tuning().sugar_depth : data().tuning.play.huddle_depth;
        EXPECT_NEAR(center, depth, 0.3) << (tempo == Tempo::Sugar ? "sugar" : "huddle");
    }
}

TEST(Huddle, TheLineBreaksFirst) {
    // Each sets off from the ring his stagger after the break (within a step).
    const Lineup u = unit("gun_2x2");
    Rng rng(2);
    const auto b = stretch(Tempo::Huddle, rng);
    b->start(u.men, 0, std::nullopt);
    b->clock(40.0, 0);
    b->lineup(0, TeamSide::Home, u.men, u.spots, true, std::nullopt);
    const std::int64_t broke = step_until(*b, 0, [&] { return b->gathered(TeamSide::Home); });

    const std::map<std::string, double> stagger = break_stagger(data().formations.at("gun_2x2"), tuning());
    b->release(broke, TeamSide::Home, stagger);
    const std::map<std::string, Spot> ring = by_id(*b);
    std::map<std::string, std::int64_t> left;
    for (std::int64_t t = Between::next_step(broke); t < broke + 4 * kTicksPerSecond; t = Between::next_step(t)) {
        b->run_to(t);
        for (const auto& [id, s] : by_id(*b)) {
            if (!left.contains(id) && (s.pos - ring.at(id).pos).length() > 1e-9) left[id] = t;
        }
    }
    ASSERT_EQ(left.size(), u.men.size());
    for (const BetweenMan& m : u.men) {
        const double after = static_cast<double>(left[m.player->id] - broke) * kTickSeconds;
        const double want = stagger.at(m.label);
        EXPECT_GE(after, want - 1e-9) << m.label;
        EXPECT_LE(after, want + 0.2 + 1e-9) << m.label;
    }
}

TEST(Huddle, NoHuddleShiftsFromItsBaseLook) {
    // With no huddle the offense waits for its call in its base formation
    // (singleback 2x2 here); the call comes by signal, and a second later
    // everyone shifts into the called one (gun 2x2).
    const Lineup u = unit("gun_2x2");
    Rng rng(3);
    const auto b = stretch(Tempo::NoHuddle, rng);
    const std::vector<Spot> base = look("singleback_2x2");
    b->start(u.men, 0, std::nullopt, base);
    b->clock(40.0, 0);
    step_until(*b, 0, [&] { return b->assembled(TeamSide::Home); });
    for (const BetweenMan& m : u.men) {
        const auto s = std::find_if(base.begin(), base.end(), [&](const Spot& x) { return x.label == m.label; });
        EXPECT_NEAR((by_id(*b).at(m.player->id).pos - s->pos).length(), 0.0, 1e-9) << m.label;
    }

    const std::int64_t call = 100;
    b->lineup(call, TeamSide::Home, u.men, u.spots, false, std::nullopt);
    b->signal(call, TeamSide::Home);
    b->wait(call, TeamSide::Home, tuning().no_huddle_signal);
    const std::map<std::string, Spot> before = by_id(*b);
    const std::int64_t shift = call + static_cast<std::int64_t>(std::lround(tuning().no_huddle_signal * kTicksPerSecond));
    b->run_to(shift - 1);
    for (const auto& [id, s] : by_id(*b)) {
        EXPECT_NEAR((s.pos - before.at(id).pos).length(), 0.0, 1e-9) << id;
        EXPECT_NEAR((s.facing - Vec2{0.0, -1.0}).length(), 0.0, 1e-9) << id;  // the signal
    }
    step_until(*b, shift, [&] { return b->arrived(TeamSide::Home); });
    for (const Spot& s : u.spots) EXPECT_NEAR((by_id(*b).at(s.id).pos - s.pos).length(), 0.0, 1e-9) << s.label;
}
