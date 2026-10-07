// Play art for the play-call menu: formations and plays laid out as pictures
// (docs/PROPOSALS.md#the-play-menu).

#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "cfbsim/data/load.h"
#include "cfbsim/play/play_art.h"
#include "cfbsim/play/play_sim.h"
#include "cfbsim/sim_api.h"

using namespace cfbsim;

namespace {

constexpr double kEps = 1e-9;

const DataSet& data() {
    static const LoadReport report = load_data_dir(CFBSIM_DATA_DIR);
    return report.data;
}

// A trips formation with numbers for x, so the expected points are easy to
// work out: Y and H inside Z on the right, the back offset right of the QB.
const char* kFormation = R"({
  "format_version": 1, "id": "art_trips", "name": "Art Trips", "side": "offense", "personnel": "11",
  "slots": {
    "LT": {"pos": "OT", "x": -3, "y": -1, "stance": "three_point", "on_line": true},
    "LG": {"pos": "OG", "x": -1.5, "y": -1, "stance": "three_point", "on_line": true},
    "C":  {"pos": "C", "x": 0, "y": -1, "stance": "snap", "on_line": true},
    "RG": {"pos": "OG", "x": 1.5, "y": -1, "stance": "three_point", "on_line": true},
    "RT": {"pos": "OT", "x": 3, "y": -1, "stance": "three_point", "on_line": true},
    "X":  {"pos": "WR", "x": -15, "y": -1, "stance": "two_point", "on_line": true},
    "Z":  {"pos": "WR", "x": 15, "y": -1, "stance": "two_point", "on_line": true},
    "H":  {"pos": "WR", "x": 10, "y": -2, "stance": "two_point"},
    "Y":  {"pos": "TE", "x": 6, "y": -2, "stance": "two_point"},
    "QB": {"pos": "QB", "x": 0, "y": -5, "stance": "gun"},
    "RB": {"pos": "RB", "x": 2, "y": -5, "stance": "two_point"}
  }
})";

const char* kPlaybook = R"({
  "format_version": 1, "id": "art_book", "name": "Art Book", "side": "offense", "scheme": "spread",
  "formations": ["art_trips"],
  "plays": [
    {
      "id": "art_pass", "name": "Art Pass", "side": "offense", "type": "pass", "formation": "art_trips", "flip": true,
      "pass": {"drop": "gun_3", "progression": ["H", "Z", "RB"], "read_type": "progression",
               "protection": {"scheme": "slide", "direction": "left"}},
      "assignments": {
        "X": {"route": "curl"},
        "Z": {"route": "curl"},
        "H": {"route": "slant", "depth": 5},
        "Y": {"route": "seam", "option": {"vs_man": {"route": "out", "depth": 6}}},
        "RB": {"block": {"do": "check_release"}, "then": {"route": "flat", "side": "left"}},
        "LT": {"block": {"do": "protect"}}, "LG": {"block": {"do": "protect"}}, "C": {"block": {"do": "protect"}},
        "RG": {"block": {"do": "protect"}}, "RT": {"block": {"do": "protect"}}
      },
      "tags": []
    },
    {
      "id": "art_power", "name": "Art Power", "side": "offense", "type": "run", "formation": "art_trips", "flip": true,
      "run": {
        "scheme": "inside_zone", "direction": "right", "carrier": "RB",
        "qb_action": {"do": "handoff", "then": "boot_fake_left"},
        "path": [{"x": 0.5, "y": -4.3, "type": "mesh"}, {"x": {"from": "gap_B", "side": "right"}, "y": -1, "type": "aim"}],
        "free_after": "aim"
      },
      "assignments": {
        "LT": {"block": {"do": "scheme"}}, "LG": {"block": {"do": "pull_kick", "target": "kick_out_man"}},
        "C": {"block": {"do": "scheme"}}, "RG": {"block": {"do": "scheme"}}, "RT": {"block": {"do": "scheme"}},
        "X": {"block": {"do": "stalk", "target": "man_over"}}, "Z": {"block": {"do": "stalk", "target": "man_over"}},
        "H": {"block": {"do": "stalk", "target": "man_over"}}, "Y": {"block": {"do": "stalk", "target": "man_over"}}
      },
      "tags": []
    }
  ]
})";

// The real data plus the test formation and playbook.
struct ArtData {
    DataSet set = data();
    Playbook book;
    ArtData() {
        set.formations["art_trips"] = parse_formation(Json::parse(kFormation), "art_trips");
        book = parse_playbook(Json::parse(kPlaybook), "art_book");
    }
    const Play& play(const char* id) const { return *book.find(id); }
};

const ArtData& art_data() {
    static const ArtData d;
    return d;
}

ArtRoute route_of(const PlayArt& art, const std::string& slot) {
    for (const ArtRoute& r : art.routes) {
        if (r.slot == slot) return r;
    }
    throw std::runtime_error("no route for " + slot);
}

void expect_point(Vec2 got, double x, double y) {
    EXPECT_NEAR(got.x, x, kEps);
    EXPECT_NEAR(got.y, y, kEps);
}

void expect_mirror(const std::vector<Vec2>& a, const std::vector<Vec2>& b, const std::string& what) {
    ASSERT_EQ(a.size(), b.size()) << what;
    for (std::size_t k = 0; k < a.size(); ++k) {
        EXPECT_NEAR(a[k].x, -b[k].x, kEps) << what << " point " << k;
        EXPECT_NEAR(a[k].y, b[k].y, kEps) << what << " point " << k;
    }
}

// Every position in the flipped layout is the mirror of the unflipped one.
void expect_flip_is_mirror(const DataSet& d, const Play& p) {
    const PlayArt art = play_art(d, p);
    const PlayArt flip = play_art(d, p, true);
    ASSERT_EQ(art.players.size(), flip.players.size());
    for (std::size_t k = 0; k < art.players.size(); ++k) {
        expect_mirror({art.players[k].pos}, {flip.players[k].pos}, p.id + " " + art.players[k].slot);
    }
    ASSERT_EQ(art.routes.size(), flip.routes.size());
    for (std::size_t k = 0; k < art.routes.size(); ++k) {
        expect_mirror(art.routes[k].points, flip.routes[k].points, p.id + " " + art.routes[k].slot + "'s route");
    }
    expect_mirror(art.qb, flip.qb, p.id + " QB");
    expect_mirror(art.path, flip.path, p.id + " path");
    ASSERT_EQ(art.pulls.size(), flip.pulls.size());
    for (std::size_t k = 0; k < art.pulls.size(); ++k) {
        expect_mirror(art.pulls[k].points, flip.pulls[k].points, p.id + " pull");
    }
}

}  // namespace

TEST(PlayArt, FormationsArePlacedWithTheBallInTheMiddle) {
    const FormationArt f = formation_art(art_data().set.formations.at("art_trips"));
    EXPECT_EQ(f.personnel, "11");
    ASSERT_EQ(f.players.size(), 11u);
    EXPECT_EQ(f.players[0].slot, "LT");
    EXPECT_EQ(f.players[0].role, "ol");
    expect_point(f.players[0].pos, -3, -1);
    for (const ArtPlayer& p : f.players) {
        if (p.slot == "Y") {
            EXPECT_EQ(p.role, "te");
        } else if (p.slot == "RB") {
            EXPECT_EQ(p.role, "back");
        } else if (p.slot == "QB") {
            EXPECT_EQ(p.role, "qb");
        }
    }
}

TEST(PlayArt, RoutesEndWhereTheirTemplatesPutThem) {
    const PlayArt art = play_art(art_data().set, art_data().play("art_pass"));
    // curl: break at (0, 12), back to (-1, 10). Z on the right comes back inside to the left.
    const ArtRoute z = route_of(art, "Z");
    expect_point(z.points.front(), 15, -1);
    expect_point(z.points.back(), 14, 9);
    EXPECT_EQ(z.end, RouteEnd::Stop);
    // X on the left comes back inside to the right.
    expect_point(route_of(art, "X").points.back(), -14, 9);
    // slant at depth 5: the break (0, 2) moves to 5, so (-6, 8) moves to (-6, 11).
    const ArtRoute h = route_of(art, "H");
    expect_point(h.points[1], 10, 3);
    expect_point(h.points.back(), 4, 9);
    // An option route shows its default branch.
    expect_point(route_of(art, "Y").points.back(), 6 - 1, -2 + 40);
}

TEST(PlayArt, ACheckReleaseBackReleasesOnHisSideOfTheCenter) {
    // The flat says "left", but the back stands right of the center: he
    // releases right (PlaySim::route_behavior). flat ends at (8, 3).
    const PlayArt art = play_art(art_data().set, art_data().play("art_pass"));
    const ArtRoute rb = route_of(art, "RB");
    expect_point(rb.points.front(), 2, -5);
    expect_point(rb.points.back(), 10, -2);
}

TEST(PlayArt, ThePrimaryIsTheFirstRead) {
    const PlayArt art = play_art(art_data().set, art_data().play("art_pass"));
    int primaries = 0;
    for (const ArtRoute& r : art.routes) {
        primaries += r.primary ? 1 : 0;
        EXPECT_EQ(r.primary, r.slot == "H") << r.slot;
    }
    EXPECT_EQ(primaries, 1);
}

TEST(PlayArt, TheQbDropsFromHisAlignment) {
    const PlayArt art = play_art(art_data().set, art_data().play("art_pass"));
    ASSERT_EQ(art.qb.size(), 2u);
    expect_point(art.qb[0], 0, -5);
    expect_point(art.qb[1], 0, -6.3);  // gun_3
    EXPECT_TRUE(art.path.empty());
    EXPECT_TRUE(art.pulls.empty());
}

TEST(PlayArt, ARunShowsThePathTheFakeAndThePullers) {
    const PlayArt art = play_art(art_data().set, art_data().play("art_power"));
    EXPECT_TRUE(art.routes.empty());
    // The path starts at the carrier, then the mesh, then the right B gap
    // (halfway between RG at 1.5 and RT at 3).
    ASSERT_EQ(art.path.size(), 3u);
    expect_point(art.path[0], 2, -5);
    expect_point(art.path[1], 0.5, -4.3);
    expect_point(art.path[2], 2.25, -1);
    // The QB's fake after the handoff, from his alignment.
    ASSERT_EQ(art.qb.size(), 3u);
    expect_point(art.qb[0], 0, -5);
    expect_point(art.qb[2], -9, -6.5);
    // The pulling guard goes flat behind the line to the right D gap, then up.
    ASSERT_EQ(art.pulls.size(), 1u);
    EXPECT_EQ(art.pulls[0].slot, "LG");
    expect_point(art.pulls[0].points.front(), -1.5, -1);
    EXPECT_GT(art.pulls[0].points.back().x, 3.0);
    EXPECT_GT(art.pulls[0].points.back().y, -1.0);
}

TEST(PlayArt, AFlippedPlayIsTheMirrorOfThePicture) {
    for (const Play& p : art_data().book.plays) expect_flip_is_mirror(art_data().set, p);
    int checked = 0;
    for (const auto& [id, book] : data().playbooks) {
        for (const Play& p : book.plays) {
            if (!p.flip || (p.type != PlayType::Pass && p.type != PlayType::Run)) continue;
            expect_flip_is_mirror(data(), p);
            ++checked;
        }
    }
    EXPECT_GT(checked, 0) << "no flippable plays in data/";
}

TEST(PlayArt, PlayersStandWhereTheSnapLinesThemUp) {
    // Every team's plays, flipped and not, against the sim's own setup.
    int checked = 0;
    for (const auto& [team_id, team] : data().teams) {
        const auto book = data().playbooks.find(team.offense_playbook);
        if (book == data().playbooks.end() || !data().rosters.contains(team_id)) continue;
        for (const Play& p : book->second.plays) {
            if (p.type != PlayType::Pass && p.type != PlayType::Run) continue;
            for (const bool flip : {false, true}) {
                if (flip && !p.flip) continue;
                PlayOptions o;
                o.team = team_id;
                o.play = p.id;
                o.flip = flip;
                o.huddle = false;  // lined up, not in the huddle
                const std::vector<Spot> start = play_start(data(), o);
                const PlayFrame frame(o.yard_line, Hash::Middle, o.offense_right, flip);
                const PlayArt art = play_art(data(), p);
                ++checked;
                for (const ArtPlayer& a : art.players) {
                    bool found = false;
                    for (const Spot& s : start) {
                        if (s.label != a.slot) continue;
                        found = true;
                        const Vec2 at = frame.to_play(s.pos);
                        EXPECT_NEAR(at.x, flip ? -a.pos.x : a.pos.x, 1e-6) << team_id << " " << p.id << " " << a.slot;
                        EXPECT_NEAR(at.y, a.pos.y, 1e-6) << team_id << " " << p.id << " " << a.slot;
                    }
                    EXPECT_TRUE(found) << team_id << " " << p.id << " " << a.slot;
                }
            }
        }
    }
    EXPECT_GT(checked, 0) << "no team plays in data/";
}

TEST(PlayArt, EveryTeamsPlaybookLaysOut) {
    ASSERT_FALSE(data().teams.empty());
    for (const auto& [team_id, team] : data().teams) {
        const Json art = playbook_art_json(data(), team_id);
        EXPECT_EQ(art["playbook"], team.offense_playbook);
        for (const Json& p : art["plays"]) {
            const auto routes = p["routes"];
            int primaries = 0;
            for (const Json& r : routes) primaries += r["primary"].get<bool>() ? 1 : 0;
            EXPECT_LE(primaries, 1) << p["id"];
            // Its formation is in the list, with the players.
            bool listed = false;
            for (const Json& f : art["formations"]) listed = listed || f["id"] == p["formation"];
            EXPECT_TRUE(listed) << p["id"];
        }
    }
}

TEST(PlayArt, TheCApiReturnsJsonOrAnError) {
    const char* ok = cfbsim_playbook_art(CFBSIM_DATA_DIR, data().teams.begin()->first.c_str());
    ASSERT_NE(ok, nullptr) << cfbsim_last_error();
    const Json art = Json::parse(ok);
    EXPECT_TRUE(art.contains("formations"));
    EXPECT_TRUE(art.contains("plays"));

    EXPECT_EQ(cfbsim_playbook_art(CFBSIM_DATA_DIR, "no_such_team"), nullptr);
    EXPECT_NE(std::string(cfbsim_last_error()).find("no_such_team"), std::string::npos);
}

namespace {

const ArtPlayer& defender(const std::vector<ArtPlayer>& players, std::string_view slot) {
    for (const ArtPlayer& p : players) {
        if (p.slot == slot) return p;
    }
    throw std::runtime_error("no defender " + std::string(slot));
}

const DefensiveArtJob& defense_job(const DefensivePlayArt& art, std::string_view slot) {
    for (const DefensiveArtJob& j : art.jobs) {
        if (j.slot == slot) return j;
    }
    throw std::runtime_error("no job for " + std::string(slot));
}

Job pass_job(JobKind kind, const std::string& ref = {}, Side side = Side::None) {
    Job job;
    job.kind = kind;
    job.ref = ref;
    job.side = side;
    return job;
}

Play defensive_call() { return data().playbooks.at("43_base").plays.front(); }

// Structural check: no offensive targets, receiver reads, or reference
// lineup can slip into the exported tree, including a man/zone job.
void expect_defense_only(const Json& node) {
    if (node.is_object()) {
        for (const auto& [key, value] : node.items()) {
            EXPECT_NE(key, "offense");
            EXPECT_NE(key, "reference");
            EXPECT_NE(key, "target");
            EXPECT_NE(key, "read");
            if (key == "players") {
                for (const Json& p : value) {
                    const std::string role = p.at("role");
                    EXPECT_TRUE(role == "dl" || role == "lb" || role == "cb" || role == "s") << p;
                }
            }
            expect_defense_only(value);
        }
    } else if (node.is_array()) {
        for (const Json& value : node) expect_defense_only(value);
    }
}

}  // namespace

TEST(DefensivePlayArt, BaseAnchorsNeedNoOpposingFormation) {
    Formation f = data().formations.at("43_over");
    // Exercise every technique using explicit anchors, independent of data tuning.
    const std::vector<std::pair<std::string, double>> techniques{
        {"0", 0}, {"1", 0.75}, {"2i", 1}, {"2", 1.5}, {"3", 2},
        {"4i", 2.5}, {"4", 3}, {"5", 3.5}, {"7", 3.9}, {"6", 4.4}, {"9", 4.9}};
    DataSet d = data();
    d.tuning.defense.shade = 0.5;
    d.tuning.defense.ghost_te_split = 1.4;
    ASSERT_EQ(f.slots.size(), techniques.size());
    for (std::size_t i = 0; i < f.slots.size(); ++i) {
        f.slots[i].x = {AnchorKind::Tech, Side::Strong, 0.25, techniques[i].first};
        f.slots[i].y = Anchor::at(2.0);
    }
    const DefensiveFormationArt right = defensive_formation_art(d, f);
    EXPECT_EQ(right.package, "base");
    for (std::size_t i = 0; i < f.slots.size(); ++i) {
        expect_point(right.players[i].pos, techniques[i].second + 0.25, 2.0);
        f.slots[i].x.side = Side::Weak;
    }
    const DefensiveFormationArt left = defensive_formation_art(d, f);
    for (std::size_t i = 0; i < f.slots.size(); ++i) {
        expect_point(left.players[i].pos, -techniques[i].second - 0.25, 2.0);
    }
}

TEST(DefensivePlayArt, WidthLandmarksAndAlignmentOverridesAreBaseCoordinates) {
    DataSet d = data();
    d.tuning.defense.ghost_te_split = 1.4;
    Play p = defensive_call();
    auto& f = d.formations.at(p.formation);
    f.slots[0].x = {AnchorKind::Receiver, Side::Strong, 0, "#1"};
    f.slots[1].x = {AnchorKind::Receiver, Side::Weak, 0, "#2"};
    f.slots[2].x = {AnchorKind::Receiver, Side::Strong, 0, "#3"};
    const PlayFrame frame(25, Hash::Middle, true, false);
    const auto original = defensive_formation_art(d, f);
    EXPECT_NEAR(original.players[0].pos.x, frame.numbers_x(Side::Right), kEps);
    EXPECT_NEAR(original.players[1].pos.x, frame.hash_x(Side::Left), kEps);
    EXPECT_NEAR(original.players[2].pos.x, 4.4, kEps);
    // Relative offsets are outward, absolute offsets are signed x.
    p.assignments[f.slots[0].name].align_x = Anchor{AnchorKind::Hash, Side::Weak, 1, {}};
    p.assignments[f.slots[0].name].align_y = Anchor::at(12);
    p.assignments[f.slots[1].name].align_x = Anchor{AnchorKind::Hash, Side::Left, 1, {}};
    p.assignments[f.slots[2].name].align_x = Anchor{AnchorKind::Slot, Side::None, 2, f.slots[0].name};
    const auto art = defensive_play_art(d, p);
    expect_point(art.players[0].pos, frame.hash_x(Side::Left) - 1, 12);
    EXPECT_NEAR(art.players[1].pos.x, frame.hash_x(Side::Left) + 1, kEps);
    EXPECT_NEAR(art.players[2].pos.x, art.players[0].pos.x + 2, kEps);
    EXPECT_EQ(defensive_formation_art(d, f).players[0].pos.x, original.players[0].pos.x);
}

TEST(DefensivePlayArt, ZonesKeepTheirAuthoredBoxAndCapsuleGeometry) {
    DataSet d = data();
    Play p = defensive_call();
    Zone box;
    box.id = "art_box";
    box.drop_x = {AnchorKind::Numbers, Side::Weak, -2, {}};
    box.drop_y = Anchor::at(15);
    box.width = 18;
    box.depth_min = 10;
    box.depth_max = 45;
    d.zones[box.id] = box;
    Zone capsule;
    capsule.id = "art_capsule";
    capsule.drop_x = Anchor::at(4);
    capsule.drop_y = Anchor::at(6);
    capsule.radius = 3;
    capsule.ends = std::array<Zone::End, 2>{{{Anchor::at(2), Anchor::at(6)},
                                          {Anchor::at(8), Anchor::at(6)}}};
    d.zones[capsule.id] = capsule;
    Zone circle = capsule;
    circle.id = "art_circle";
    circle.ends.reset();
    d.zones[circle.id] = circle;
    p.assignments["SS"].pass = pass_job(JobKind::Zone, box.id);
    p.assignments["SAM"].pass = pass_job(JobKind::Zone, capsule.id);
    p.assignments["MIKE"].pass = pass_job(JobKind::Zone, circle.id);
    const auto art = defensive_play_art(d, p);
    const auto& deep = defense_job(art, "SS");
    ASSERT_TRUE(deep.area);
    const PlayFrame frame(25, Hash::Middle, true, false);
    expect_point(deep.area->drop, frame.numbers_x(Side::Left) + 2, 15);
    EXPECT_EQ(deep.area->width, 18);
    EXPECT_EQ(deep.area->depth_min, 10);
    EXPECT_EQ(deep.area->depth_max, 45);
    EXPECT_EQ(deep.area->radius, 0);
    ASSERT_EQ(deep.points.size(), 2u);
    expect_point(deep.points.front(), defender(art.players, "SS").pos.x, defender(art.players, "SS").pos.y);
    expect_point(deep.points.back(), deep.area->drop.x, 15);
    const auto& under = *defense_job(art, "SAM").area;
    expect_point(under.end_a, 2, 6);
    expect_point(under.end_b, 8, 6);
    EXPECT_EQ(under.radius, 3);
    const auto& round = *defense_job(art, "MIKE").area;
    expect_point(round.end_a, 4, 6);
    expect_point(round.end_b, 4, 6);
}

TEST(DefensivePlayArt, RushBlitzStuntManAndSpyHaveOnlyDefensiveGeometry) {
    DataSet d = data();
    d.tuning.blocking.contain_outside = 1.5;
    Play p = defensive_call();
    p.assignments["SDE"].pass = pass_job(JobKind::Rush, "contain", Side::Strong);
    p.assignments["DT"].pass = pass_job(JobKind::Stunt, "B", Side::Weak);
    p.assignments["SAM"].pass = pass_job(JobKind::Blitz, {}, Side::Strong);
    p.assignments["SAM"].pass->gap = "A";
    p.assignments["MIKE"].pass = pass_job(JobKind::Spy);
    p.assignments["SCB"].pass = pass_job(JobKind::Man, "outside");
    p.assignments["SCB"].pass->target = "#1_strong";
    p.assignments["WCB"].pass = pass_job(JobKind::Man);
    p.assignments["WCB"].pass->target = "back_99";
    const auto art = defensive_play_art(d, p);
    EXPECT_TRUE(defense_job(art, "SDE").contain);
    expect_point(defense_job(art, "SDE").points.back(), 4.5, -3);
    EXPECT_FALSE(defense_job(art, "DT").contain);
    expect_point(defense_job(art, "DT").points[1], -2.25, 0);
    expect_point(defense_job(art, "SAM").points.back(), 0.75, -3);
    expect_point(defense_job(art, "MIKE").at, 0, 5);
    for (const std::string slot : {"SCB", "WCB"}) {
        const auto& man = defense_job(art, slot);
        EXPECT_EQ(man.kind, JobKind::Man);  // no missing-target fallback in a base diagram
        EXPECT_TRUE(man.points.empty());
        EXPECT_FALSE(man.area);
        expect_point(man.at, defender(art.players, slot).pos.x, defender(art.players, slot).pos.y);
    }
    EXPECT_EQ(defense_job(art, "SCB").leverage, "outside");
    EXPECT_EQ(defense_job(art, "WCB").leverage, "inside");
}

TEST(DefensivePlayArt, EveryDefensiveBookExportsOnlyDefenseWithoutOffensiveData) {
    ASSERT_TRUE(load_data_dir(CFBSIM_DATA_DIR).ok());
    int checked = 0;
    for (const auto& [id, book] : data().playbooks) {
        if (book.side != PlaybookSide::Defense) continue;
        DataSet d = data();
        const std::string team_id = d.teams.begin()->first;
        d.teams.at(team_id).defense_playbook = id;
        const Json before = defensive_playbook_art_json(d, team_id);
        EXPECT_EQ(before.at("unit"), "defense");
        EXPECT_EQ(before.at("plays").size(), book.plays.size());
        expect_defense_only(before);
        for (const Json& play : before.at("plays")) {
            EXPECT_EQ(play.at("players").size(), 11u);
            EXPECT_EQ(play.at("jobs").size(), 11u);
        }
        std::erase_if(d.formations, [](const auto& entry) { return entry.second.unit != Unit::Defense; });
        std::erase_if(d.playbooks, [](const auto& entry) { return entry.second.side != PlaybookSide::Defense; });
        d.playbooks.at(id).strength_call = {"back", "boundary"};
        d.teams.at(team_id).offense_playbook = "does_not_exist";
        d.rosters.clear();
        d.routes.clear();
        d.qb = {};
        EXPECT_EQ(defensive_playbook_art_json(d, team_id), before);
        ++checked;
    }
    EXPECT_GT(checked, 0);
}

TEST(DefensivePlayArt, ErrorsIdentifyTheCall) {
    DataSet d = data();
    Play p = defensive_call();
    p.id = "bad_defensive_art";
    const auto expect_error = [&] {
        try {
            (void)defensive_play_art(d, p);
            FAIL() << "expected an error";
        } catch (const PlayError& e) {
            EXPECT_NE(std::string(e.what()).find(p.id), std::string::npos) << e.what();
        }
    };
    p.formation = "missing";
    expect_error();
    p.formation = defensive_call().formation;
    p.assignments["SS"].pass = pass_job(JobKind::Zone, "missing_zone");
    expect_error();
    p.assignments["SS"].pass = pass_job(JobKind::Route);
    expect_error();
    p.assignments["SS"].pass.reset();
    expect_error();
    EXPECT_THROW(defensive_formation_art(d, d.formations.at("gun_wide_trips")), PlayError);
}

TEST(DefensivePlayArt, TheCApiReturnsTheSchemaOrAnErrorAndKeepsOffenseArtValid) {
    const std::string team = data().teams.begin()->first;
    const char* offense = cfbsim_playbook_art(CFBSIM_DATA_DIR, team.c_str());
    ASSERT_NE(offense, nullptr);
    const std::string saved_offense = offense;
    const char* result = cfbsim_defensive_playbook_art(CFBSIM_DATA_DIR, team.c_str());
    ASSERT_NE(result, nullptr) << cfbsim_last_error();
    const Json art = Json::parse(result);
    EXPECT_EQ(art, defensive_playbook_art_json(data(), team));
    EXPECT_EQ(std::string(offense), saved_offense);
    expect_defense_only(art);
    bool box = false, capsule = false, man = false, rush = false;
    for (const Json& p : art["plays"]) {
        for (const Json& job : p["jobs"]) {
            const std::string kind = job.at("kind");
            if (kind == "zone") {
                const Json& a = job.at("area");
                if (a.at("shape") == "box") {
                    box = true;
                    EXPECT_TRUE(a.contains("width") && a.contains("depth"));
                    EXPECT_FALSE(a.contains("radius"));
                } else {
                    capsule = true;
                    EXPECT_EQ(a.at("ends").size(), 2u);
                    EXPECT_TRUE(a.contains("radius"));
                }
            } else if (kind == "man") {
                man = true;
                EXPECT_TRUE(job.contains("at") && job.contains("leverage"));
                EXPECT_FALSE(job.contains("points"));
            } else if (kind == "rush") {
                rush = true;
                EXPECT_EQ(job.at("points").size(), 3u);
                EXPECT_TRUE(job.at("contain").is_boolean());
            }
        }
    }
    EXPECT_TRUE(box && capsule && man && rush);
    EXPECT_EQ(cfbsim_defensive_playbook_art(CFBSIM_DATA_DIR, "no_such_team"), nullptr);
    EXPECT_NE(std::string(cfbsim_last_error()).find("no_such_team"), std::string::npos);
    EXPECT_EQ(cfbsim_defensive_playbook_art(CFBSIM_DATA_DIR, nullptr), nullptr);
    EXPECT_NE(cfbsim_defensive_playbook_art(CFBSIM_DATA_DIR, team.c_str()), nullptr);
    EXPECT_STREQ(cfbsim_last_error(), "");
}
