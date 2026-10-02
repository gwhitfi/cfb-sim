// The box score as JSON and as text.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

#include "cfbsim/rules/game.h"

namespace cfbsim {

namespace {

double r1(double x) { return std::round(x * 10.0) / 10.0; }
int yd(double x) { return static_cast<int>(std::lround(x)); }

std::string mmss(double seconds) {
    const int s = static_cast<int>(std::lround(seconds));
    char buf[16];
    std::snprintf(buf, sizeof buf, "%d:%02d", s / 60, s % 60);
    return buf;
}

}  // namespace

nlohmann::ordered_json box_score_json(const BoxScore& box) {
    nlohmann::ordered_json j;
    j["teams"] = nlohmann::ordered_json::array();
    for (std::size_t t = 0; t < 2; ++t) {
        const TeamStats& s = box.teams[t];
        j["teams"].push_back({{"team", box.team_ids[t]},
                              {"points", s.points},
                              {"plays", s.plays},
                              {"total_yards", r1(s.total_yds())},
                              {"first_downs", s.first_downs},
                              {"first_downs_rush", s.first_downs_rush},
                              {"first_downs_pass", s.first_downs_pass},
                              {"first_downs_penalty", s.first_downs_penalty},
                              {"rush_att", s.rush_att},
                              {"rush_yds", r1(s.rush_yds)},
                              {"pass_att", s.pass_att},
                              {"pass_cmp", s.pass_cmp},
                              {"pass_yds", r1(s.pass_yds)},
                              {"sacks_taken", s.sacks_taken},
                              {"interceptions", s.ints},
                              {"fumbles_lost", s.fumbles_lost},
                              {"third_down", {s.third_conv, s.third_att}},
                              {"fourth_down", {s.fourth_conv, s.fourth_att}},
                              {"penalties", s.penalties},
                              {"penalty_yds", r1(s.penalty_yds)},
                              {"punts", s.punts},
                              {"punt_yds", r1(s.punt_yds)},
                              {"kick_returns", {s.kr_att, r1(s.kr_yds)}},
                              {"punt_returns", {s.pr_att, r1(s.pr_yds)}},
                              {"field_goals", {s.fg_made, s.fg_att}},
                              {"timeouts_used", s.timeouts_used},
                              {"possession", mmss(s.possession_seconds)},
                              {"injuries", s.injuries}});
    }
    j["periods"] = nlohmann::ordered_json::array();
    for (const auto& p : box.periods) j["periods"].push_back({p[0], p[1]});
    j["players"] = nlohmann::ordered_json::array();
    for (const auto& [id, p] : box.players) {
        nlohmann::ordered_json pj = {{"id", id}, {"name", p.name}, {"team", box.team_ids[static_cast<std::size_t>(p.team)]}};
        if (p.pass_att) pj["passing"] = {{"cmp", p.pass_cmp}, {"att", p.pass_att}, {"yds", r1(p.pass_yds)}, {"td", p.pass_td}, {"int", p.ints}};
        if (p.rush_att) pj["rushing"] = {{"att", p.rush_att}, {"yds", r1(p.rush_yds)}, {"td", p.rush_td}, {"long", r1(p.rush_long)}};
        if (p.targets || p.rec) pj["receiving"] = {{"rec", p.rec}, {"targets", p.targets}, {"yds", r1(p.rec_yds)}, {"td", p.rec_td}, {"long", r1(p.rec_long)}};
        if (p.tackles || p.sacks || p.def_int || p.fum_rec) pj["defense"] = {{"tackles", p.tackles}, {"sacks", p.sacks}, {"int", p.def_int}, {"fum_rec", p.fum_rec}};
        if (p.fumbles) pj["fumbles"] = {{"fumbles", p.fumbles}, {"lost", p.fumbles_lost}};
        if (p.fg_att || p.xp_att) pj["kicking"] = {{"fg", {p.fg_made, p.fg_att}}, {"long", r1(p.fg_long)}, {"xp", {p.xp_made, p.xp_att}}};
        if (p.punts) pj["punting"] = {{"punts", p.punts}, {"yds", r1(p.punt_yds)}};
        if (p.kickoffs) pj["kickoffs"] = {{"kickoffs", p.kickoffs}, {"touchbacks", p.touchbacks}};
        if (p.kr_att) pj["kick_returns"] = {{"att", p.kr_att}, {"yds", r1(p.kr_yds)}, {"td", p.kr_td}};
        if (p.pr_att) pj["punt_returns"] = {{"att", p.pr_att}, {"yds", r1(p.pr_yds)}, {"td", p.pr_td}};
        j["players"].push_back(pj);
    }
    return j;
}

std::string box_score_text(const BoxScore& box, const DataSet& data) {
    std::ostringstream o;
    const auto abbr = [&](std::size_t t) {
        const auto it = data.teams.find(box.team_ids[t]);
        return it == data.teams.end() ? box.team_ids[t] : it->second.abbr;
    };
    const std::array<std::string, 2> name{abbr(0), abbr(1)};
    char buf[256];

    // Score by period.
    o << "       ";
    for (std::size_t k = 0; k < box.periods.size(); ++k) {
        std::snprintf(buf, sizeof buf, "%4s", k < 4 ? std::to_string(k + 1).c_str() : ("OT" + std::to_string(k - 3)).c_str());
        o << buf;
    }
    o << "     F\n";
    for (std::size_t t = 0; t < 2; ++t) {
        std::snprintf(buf, sizeof buf, "%-7s", name[t].c_str());
        o << buf;
        for (const auto& p : box.periods) {
            std::snprintf(buf, sizeof buf, "%4d", p[t]);
            o << buf;
        }
        std::snprintf(buf, sizeof buf, "  %4d\n", box.teams[t].points);
        o << buf;
    }

    // Team stats.
    o << "\n";
    std::snprintf(buf, sizeof buf, "%-22s %10s %10s\n", "", name[0].c_str(), name[1].c_str());
    o << buf;
    const auto row = [&](const char* label, const std::string& a, const std::string& b) {
        std::snprintf(buf, sizeof buf, "%-22s %10s %10s\n", label, a.c_str(), b.c_str());
        o << buf;
    };
    const auto both = [&](auto f) { return std::array<std::string, 2>{f(box.teams[0]), f(box.teams[1])}; };
    const auto show = [&](const char* label, auto f) {
        const auto v = both(f);
        row(label, v[0], v[1]);
    };
    show("First downs", [](const TeamStats& s) { return std::to_string(s.first_downs); });
    show("Plays", [](const TeamStats& s) { return std::to_string(s.plays); });
    show("Total yards", [](const TeamStats& s) { return std::to_string(yd(s.total_yds())); });
    show("Rushing (att-yds)", [](const TeamStats& s) { return std::to_string(s.rush_att) + "-" + std::to_string(yd(s.rush_yds)); });
    show("Passing (cmp-att-yds)", [](const TeamStats& s) {
        return std::to_string(s.pass_cmp) + "-" + std::to_string(s.pass_att) + "-" + std::to_string(yd(s.pass_yds));
    });
    show("Sacks taken", [](const TeamStats& s) { return std::to_string(s.sacks_taken); });
    show("Turnovers (int-fum)", [](const TeamStats& s) { return std::to_string(s.ints) + "-" + std::to_string(s.fumbles_lost); });
    show("3rd down", [](const TeamStats& s) { return std::to_string(s.third_conv) + "/" + std::to_string(s.third_att); });
    show("4th down", [](const TeamStats& s) { return std::to_string(s.fourth_conv) + "/" + std::to_string(s.fourth_att); });
    show("Penalties (no-yds)", [](const TeamStats& s) { return std::to_string(s.penalties) + "-" + std::to_string(yd(s.penalty_yds)); });
    show("Punts (no-avg)", [](const TeamStats& s) {
        char b[32];
        std::snprintf(b, sizeof b, "%d-%.1f", s.punts, s.punts ? s.punt_yds / s.punts : 0.0);
        return std::string(b);
    });
    show("Kick returns (no-yds)", [](const TeamStats& s) { return std::to_string(s.kr_att) + "-" + std::to_string(yd(s.kr_yds)); });
    show("Punt returns (no-yds)", [](const TeamStats& s) { return std::to_string(s.pr_att) + "-" + std::to_string(yd(s.pr_yds)); });
    show("Field goals", [](const TeamStats& s) { return std::to_string(s.fg_made) + "/" + std::to_string(s.fg_att); });
    show("Possession", [](const TeamStats& s) { return mmss(s.possession_seconds); });
    show("Injuries", [](const TeamStats& s) { return std::to_string(s.injuries); });

    // Leaders.
    for (std::size_t t = 0; t < 2; ++t) {
        std::vector<const PlayerStats*> ps;
        for (const auto& [id, p] : box.players) {
            if (static_cast<std::size_t>(p.team) == t) ps.push_back(&p);
        }
        o << "\n" << name[t] << "\n";
        std::vector<const PlayerStats*> v = ps;
        std::stable_sort(v.begin(), v.end(), [](auto a, auto b) { return a->pass_att > b->pass_att; });
        for (const PlayerStats* p : v) {
            if (!p->pass_att) break;
            std::snprintf(buf, sizeof buf, "  Pass  %-18s %2d/%-2d %4d yd  %d TD  %d INT\n", p->name.c_str(), p->pass_cmp,
                          p->pass_att, yd(p->pass_yds), p->pass_td, p->ints);
            o << buf;
        }
        std::stable_sort(v.begin(), v.end(), [](auto a, auto b) { return a->rush_yds > b->rush_yds; });
        int n = 0;
        for (const PlayerStats* p : v) {
            if (!p->rush_att || ++n > 3) continue;
            std::snprintf(buf, sizeof buf, "  Rush  %-18s %2d car %4d yd  %d TD  long %d\n", p->name.c_str(), p->rush_att,
                          yd(p->rush_yds), p->rush_td, yd(p->rush_long));
            o << buf;
        }
        std::stable_sort(v.begin(), v.end(), [](auto a, auto b) { return a->rec_yds > b->rec_yds; });
        n = 0;
        for (const PlayerStats* p : v) {
            if (!p->rec || ++n > 4) continue;
            std::snprintf(buf, sizeof buf, "  Rec   %-18s %2d rec %4d yd  %d TD  long %d\n", p->name.c_str(), p->rec,
                          yd(p->rec_yds), p->rec_td, yd(p->rec_long));
            o << buf;
        }
        std::stable_sort(v.begin(), v.end(), [](auto a, auto b) { return a->tackles > b->tackles; });
        n = 0;
        for (const PlayerStats* p : v) {
            if (!p->tackles || ++n > 3) continue;
            std::snprintf(buf, sizeof buf, "  Tkl   %-18s %2d tkl  %d sack  %d INT\n", p->name.c_str(), p->tackles, p->sacks, p->def_int);
            o << buf;
        }
        for (const PlayerStats* p : v) {
            if (p->fg_att || p->xp_att) {
                std::snprintf(buf, sizeof buf, "  Kick  %-18s FG %d/%d (long %d)  XP %d/%d\n", p->name.c_str(), p->fg_made,
                              p->fg_att, yd(p->fg_long), p->xp_made, p->xp_att);
                o << buf;
            }
        }
    }
    return o.str();
}

}  // namespace cfbsim
