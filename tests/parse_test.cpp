// Parser tests using small inline JSON documents.

#include <gtest/gtest.h>

#include <string>

#include "cfbsim/data/load.h"

using namespace cfbsim;

namespace {

// A legal 11-man offensive formation to start from; tests break one thing at a time.
Json valid_formation() {
    return Json::parse(R"({
      "format_version": 1, "id": "f", "name": "F", "side": "offense", "personnel": "11",
      "slots": {
        "LT": {"pos": "OT", "x": -2.8, "y": -1, "stance": "three_point", "on_line": true},
        "LG": {"pos": "OG", "x": -1.4, "y": -1, "stance": "three_point", "on_line": true},
        "C":  {"pos": "C",  "x": 0,    "y": -1, "stance": "snap",        "on_line": true},
        "RG": {"pos": "OG", "x": 1.4,  "y": -1, "stance": "three_point", "on_line": true},
        "RT": {"pos": "OT", "x": 2.8,  "y": -1, "stance": "three_point", "on_line": true},
        "X":  {"pos": "WR", "x": {"from": "numbers", "side": "left", "offset": 0}, "y": -1, "on_line": true},
        "H":  {"pos": "WR", "x": {"from": "X", "offset": 6}, "y": -2},
        "Z":  {"pos": "WR", "x": {"from": "numbers", "side": "right"}, "y": -1, "on_line": true},
        "Y":  {"pos": "TE", "x": {"from": "Z", "offset": -6}, "y": -2},
        "QB": {"pos": "QB", "x": 0, "y": -5, "stance": "gun"},
        "RB": {"pos": "RB", "x": -2, "y": -5}
      }
    })");
}

// Runs `parse` and returns the DataError message, or "" if it didn't throw.
template <typename F>
std::string error_of(F parse) {
    try {
        parse();
    } catch (const DataError& e) {
        return e.what();
    }
    return "";
}

}  // namespace

TEST(ParseFormation, ValidFormationParsesInFileOrder) {
    const Formation f = parse_formation(valid_formation(), "f.json");
    ASSERT_EQ(f.slots.size(), 11u);
    EXPECT_EQ(f.slots.front().name, "LT");  // file order, not alphabetical
    EXPECT_EQ(f.slots.back().name, "RB");

    const FormationSlot* x = f.slot("X");
    ASSERT_NE(x, nullptr);
    EXPECT_EQ(x->x.kind, AnchorKind::Numbers);
    EXPECT_EQ(x->x.side, Side::Left);
    EXPECT_TRUE(x->on_line);

    const FormationSlot* h = f.slot("H");
    EXPECT_EQ(h->x.kind, AnchorKind::Slot);
    EXPECT_EQ(h->x.ref, "X");
    EXPECT_EQ(h->x.offset, 6.0);
    EXPECT_EQ(h->stance, Stance::TwoPoint);  // default
}

TEST(ParseFormation, ErrorNamesFileAndPath) {
    Json j = valid_formation();
    j["slots"]["LT"]["x"] = Json{{"from", "nmubers"}, {"side", "left"}};
    const std::string msg = error_of([&] { parse_formation(j, "formations/offense/f.json"); });
    EXPECT_NE(msg.find("formations/offense/f.json: slots.LT.x.from: unknown landmark 'nmubers'"),
              std::string::npos)
        << msg;
}

TEST(ParseFormation, RejectsTypos) {
    Json j = valid_formation();
    j["slots"]["QB"]["stnce"] = "gun";
    const std::string msg = error_of([&] { parse_formation(j, "f.json"); });
    EXPECT_NE(msg.find("slots.QB.stnce: unknown field 'stnce'"), std::string::npos) << msg;
}

TEST(ParseFormation, NeedsElevenSlots) {
    Json j = valid_formation();
    j["slots"].erase("RB");
    EXPECT_NE(error_of([&] { parse_formation(j, "f.json"); }).find("exactly 11 slots"),
              std::string::npos);
}

TEST(ParseFormation, NeedsSevenOnTheLine) {
    Json j = valid_formation();
    j["slots"]["X"]["on_line"] = false;
    EXPECT_NE(
        error_of([&] { parse_formation(j, "f.json"); }).find("at least 7 players on the line"),
        std::string::npos);
}

TEST(ParseFormation, RejectsSlotAnchorLoops) {
    Json j = valid_formation();
    j["slots"]["X"]["x"] = Json{{"from", "H"}, {"offset", -6}};  // X -> H -> X
    EXPECT_NE(error_of([&] { parse_formation(j, "f.json"); }).find("loop"), std::string::npos);
}

TEST(ParseFormation, DefenseOnlyAnchorsRejectedOnOffense) {
    Json j = valid_formation();
    j["slots"]["Y"]["x"] = Json{{"tech", "9"}, {"side", "strong"}};
    EXPECT_NE(error_of([&] { parse_formation(j, "f.json"); }).find("defense-only"),
              std::string::npos);
}

TEST(ParsePlaybook, RejectsUnknownTag) {
    const Json j = Json::parse(R"({
      "format_version": 1, "id": "b", "name": "B", "side": "offense", "scheme": "s",
      "formations": ["f"],
      "plays": [{
        "id": "p", "name": "P", "side": "offense", "type": "pass", "formation": "f",
        "pass": {"drop": "gun_3", "progression": ["X"], "protection": {"scheme": "slide"}},
        "assignments": {"X": {"route": "go"}},
        "tags": ["deep", "hail_mary"]
      }]
    })");
    const std::string msg = error_of([&] { parse_playbook(j, "b.json"); });
    EXPECT_NE(msg.find("plays[0].tags: unknown tag 'hail_mary'"), std::string::npos) << msg;
}

TEST(ParsePlaybook, ParsesJobSequencesAndDefensiveJobs) {
    const Json j = Json::parse(R"({
      "format_version": 1, "id": "b", "name": "B", "side": "defense", "scheme": "s",
      "strength_call": ["te"], "formations": ["d"],
      "plays": [{
        "id": "p", "name": "P", "side": "defense", "formation": "d",
        "assignments": {
          "SAM": {"run": {"gap": "D", "side": "strong"}, "pass": {"zone": "curl_flat_strong"}},
          "FS":  {"run": {"support": "deep_fill"}, "pass": {"man": "#2_strong"}}
        }
      }]
    })");
    const Playbook book = parse_playbook(j, "b.json");
    const Assignment& sam = book.plays[0].assignments.at("SAM");
    ASSERT_TRUE(sam.run && sam.pass);
    EXPECT_EQ(sam.run->kind, JobKind::Gap);
    EXPECT_EQ(sam.run->gap, "D");
    EXPECT_EQ(sam.run->side, Side::Strong);
    EXPECT_EQ(sam.pass->kind, JobKind::Zone);
    EXPECT_EQ(sam.pass->ref, "curl_flat_strong");
}

TEST(ParsePlaybook, RejectsWrongJobForUnit) {
    const Json j = Json::parse(R"({
      "format_version": 1, "id": "b", "name": "B", "side": "offense", "scheme": "s",
      "formations": ["f"],
      "plays": [{
        "id": "p", "name": "P", "side": "offense", "type": "pass", "formation": "f",
        "pass": {"drop": "gun_3", "progression": ["X"], "protection": {"scheme": "slide"}},
        "assignments": {"X": {"zone": "hook_middle"}}
      }]
    })");
    EXPECT_NE(error_of([&] { parse_playbook(j, "b.json"); }).find("not a valid offense job"),
              std::string::npos);
}

TEST(ParseRoster, RequiresEveryAttribute) {
    Json attrs = Json::object();
    for (const AttrInfo& row : kAttrInfo) attrs[std::string(row.key)] = 50;
    attrs.erase("punt_accuracy");
    const Json j = {
        {"format_version", 1},
        {"team_id", "t"},
        {"players",
         {{{"id", "p1"},
           {"first_name", "A"},
           {"last_name", "B"},
           {"jersey", 1},
           {"position", "QB"},
           {"class", "JR"},
           {"measurables",
            {{"height_in", 74}, {"weight_lb", 215}, {"arm_in", 32}, {"hand_in", 9.5}}},
           {"attributes", attrs}}}},
    };
    const std::string msg = error_of([&] { parse_roster(j, "r.json"); });
    EXPECT_NE(msg.find("missing attributes: punt_accuracy"), std::string::npos) << msg;
}

TEST(ParseZones, BoxesAndCapsules) {
    const Json j = Json::parse(R"({"format_version": 1, "zones": {
      "deep":    {"drop": {"x": 0, "y": 15}, "width": 18, "depth": [10, 45]},
      "circle":  {"drop": {"x": 0, "y": 10}, "radius": 6},
      "capsule": {"drop": {"x": {"from": "numbers", "side": "strong", "offset": -4}, "y": 9},
                  "ends": [{"x": {"from": "hash", "side": "strong", "offset": 5.5}, "y": 4.5},
                           {"x": {"from": "numbers", "side": "strong", "offset": 2.5}, "y": 4.5}], "radius": 5.5}
    }})");
    const ZoneLibrary z = parse_zones(j, "zones.json");
    EXPECT_EQ(z.at("deep").radius, 0.0);
    EXPECT_EQ(z.at("circle").radius, 6.0);
    EXPECT_FALSE(z.at("circle").ends.has_value());
    ASSERT_TRUE(z.at("capsule").ends.has_value());
    EXPECT_EQ((*z.at("capsule").ends)[1].x.offset, 2.5);
}

TEST(ParseZones, RejectsMixedShapes) {
    const auto bad = [](const char* zone) {
        const Json j = Json::parse(std::string(R"({"format_version": 1, "zones": {"z": )") + zone + "}}");
        return error_of([&] { parse_zones(j, "zones.json"); });
    };
    EXPECT_NE(bad(R"({"drop": {"x": 0, "y": 10}, "radius": 6, "width": 10})").find("not both"), std::string::npos);
    EXPECT_NE(bad(R"({"drop": {"x": 0, "y": 10}, "radius": 0})").find("positive"), std::string::npos);
    EXPECT_NE(bad(R"({"drop": {"x": 0, "y": 10}, "ends": [{"x": 0, "y": 3}, {"x": 5, "y": 3}], "width": 10, "depth": [0, 8]})")
                  .find("need a radius"),
              std::string::npos);
    EXPECT_NE(bad(R"({"drop": {"x": 0, "y": 10}, "ends": [{"x": 0, "y": 3}], "radius": 5})").find("[point, point]"),
              std::string::npos);
}

TEST(ParseTargets, ParsesAndRejectsBadEntries) {
    const Json good = Json::parse(R"({"format_version": 1, "targets": [
        {"metric": "ypp", "kind": "both", "value": 5.8, "span": 3},
        {"metric": "ypp", "kind": "run", "value": 5.0, "span": 3, "note": "designed"}]})");
    const Targets t = parse_targets(good, "targets");
    ASSERT_EQ(t.list.size(), 2u);
    EXPECT_EQ(t.list[1].kind, "run");
    EXPECT_EQ(t.list[1].note, "designed");
    EXPECT_EQ(to_json(t).size(), 2u);

    const auto bad = [](const char* entry) {
        return error_of([&] {
            parse_targets(Json::parse(std::string(R"({"format_version": 1, "targets": [)") + entry + "]}"), "targets");
        });
    };
    EXPECT_NE(bad(R"({"metric": "ypp", "kind": "kicks", "value": 1, "span": 1})").find("kickoff, punt, or field_goal"), std::string::npos);
    EXPECT_NE(bad(R"({"metric": "ypp", "kind": "run", "value": 1, "span": 0})").find("span"), std::string::npos);
    EXPECT_NE(bad(R"({"metric": "ypp", "kind": "run", "value": 1, "span": 1, "spam": 2})").find("spam"), std::string::npos);
    EXPECT_NE(bad(R"({"metric": "a", "kind": "run", "value": 1, "span": 1}, {"metric": "a", "kind": "run", "value": 2, "span": 1})")
                   .find("second target"), std::string::npos);
}

TEST(ParseTargets, GameTargetsTakeAValueOrBorrowOneFromAPerPlayTarget) {
    const Json json = Json::parse(R"({"format_version": 1,
        "targets": [{"metric": "completion", "kind": "pass", "value": 0.61, "span": 0.2}],
        "game": [
          {"row": "points", "value": 28},
          {"row": "completion %", "from": {"metric": "completion", "kind": "pass", "scale": 100}},
          {"row": "sacks", "value": 0.065, "text": "~6-7"}]})");
    const Targets t = parse_targets(json, "targets");
    ASSERT_EQ(t.game.size(), 3u);
    EXPECT_EQ(t.find_game("points")->text, "~28");
    EXPECT_NEAR(t.find_game("completion %")->value, 61.0, 1e-9);
    EXPECT_EQ(t.find_game("completion %")->text, "~61");  // not ~61.00000000000001
    EXPECT_EQ(t.find_game("sacks")->text, "~6-7");        // the text wins over the number
    EXPECT_EQ(t.find_game("nothing"), nullptr);
    EXPECT_EQ(to_json(t).size(), 1u);                     // the Builder gets the per-play list only

    const auto bad = [](const char* game) {
        return error_of([&] {
            parse_targets(Json::parse(std::string(R"({"format_version": 1, "targets": [], "game": [)") + game + "]}"), "targets");
        });
    };
    EXPECT_NE(bad(R"({"row": "a", "from": {"metric": "x", "kind": "run"}})").find("no per-play target"), std::string::npos);
    EXPECT_NE(bad(R"({"row": "a"})").find("either"), std::string::npos);
    EXPECT_NE(bad(R"({"row": "a", "value": 1, "from": {"metric": "x", "kind": "run"}})").find("either"), std::string::npos);
    EXPECT_NE(bad(R"({"row": "a", "value": 1}, {"row": "a", "value": 2})").find("second target"), std::string::npos);
}
