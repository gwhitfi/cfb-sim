// Play art for the play-call menu: formations and plays laid out as pictures
// (docs/PROPOSALS.md#the-play-menu).

#include <gtest/gtest.h>

#include <string>

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
