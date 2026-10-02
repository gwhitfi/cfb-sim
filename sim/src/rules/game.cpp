// A full game (rules-and-flow.md#first-build-phase-8): the game state
// machine around the play sim. Every decision goes through ask(), which
// takes it from the auto coach (and logs it as an input) or, in a replay,
// from the log.

#include "cfbsim/rules/game.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <istream>
#include <ostream>
#include <set>
#include <sstream>

#include "cfbsim/play/kick_sim.h"
#include "cfbsim/version.h"
#include "coach/auto_coach.h"
#include "rules/clock.h"
#include "rules/enforce.h"
#include "rules/game_state.h"
#include "rules/lineup.h"

namespace cfbsim {

namespace {

double r2(double x) { return std::round(x * 100.0) / 100.0; }

std::string clock_text(double seconds) {
    const int s = static_cast<int>(std::ceil(std::max(0.0, seconds) - 1e-9));
    char buf[16];
    std::snprintf(buf, sizeof buf, "%d:%02d", s / 60, s % 60);
    return buf;
}

const char* team_key(int team) { return team == 0 ? "home" : "away"; }

std::int64_t ticks(double seconds) {
    return static_cast<std::int64_t>(std::llround(seconds * kTicksPerSecond));
}

// The hash for the next snap from where the ball was dead (play-frame x).
Hash hash_from(double x) {
    const double hash = kFieldWidth / 2.0 - kHashFromSideline;
    if (x <= -hash / 2.0) return Hash::Left;
    if (x >= hash / 2.0) return Hash::Right;
    return Hash::Middle;
}

Hash mirrored(Hash h) {
    if (h == Hash::Left) return Hash::Right;
    if (h == Hash::Right) return Hash::Left;
    return h;
}

class ReplayError : public GameError {
  public:
    using GameError::GameError;
};

// How a snap ended, for the flow around it.
enum class SnapEnd { Continue, Touchdown, Safety, FieldGoal, MissedKick, Turnover, PeriodOver };

struct SnapResult {
    SnapEnd end = SnapEnd::Continue;
    int scorer = -1;  // team that scored (touchdown, safety, field goal)
};

class Game {
  public:
    Game(const DataSet& data, const GameOptions& opt, std::ostream* log, const std::vector<Json>* replay)
        : data_(data),
          rules_(data.rules),
          tuning_(data.tuning.game),
          opt_(opt),
          log_(log),
          replay_(replay),
          rng_(opt.seed),
          teams_{&team(opt.home), &team(opt.away)},
          lineups_{Lineup(*teams_[0], roster(opt.home), tuning_), Lineup(*teams_[1], roster(opt.away), tuning_)},
          coaches_{AutoCoach(data, *teams_[0], 0, opt.seed ^ 0xA5A5A5A5A5A5A5A5ULL),
                   AutoCoach(data, *teams_[1], 1, opt.seed ^ 0x5A5A5A5A5A5A5A5AULL)} {
        box_.team_ids = {opt.home, opt.away};
    }

    GameResult run();

  private:
    const Team& team(const std::string& id) const {
        const auto it = data_.teams.find(id);
        if (it == data_.teams.end()) throw GameError("unknown team '" + id + "'");
        return it->second;
    }
    const Roster& roster(const std::string& id) const {
        const auto it = data_.rosters.find(id);
        if (it == data_.rosters.end()) throw GameError("no roster for team '" + id + "'");
        return it->second;
    }
    Lineup& lineup(int t) { return lineups_[static_cast<std::size_t>(t)]; }
    AutoCoach& coach(int t) { return coaches_[static_cast<std::size_t>(t)]; }
    const Team& team_of(int t) const { return *teams_[static_cast<std::size_t>(t)]; }
    // A play in `t`'s offensive playbook (null for kneel, spike, or an unknown id).
    const Play* offense_play(int t, const std::string& id) const {
        const auto book = data_.playbooks.find(team_of(t).offense_playbook);
        return book == data_.playbooks.end() ? nullptr : book->second.find(id);
    }
    // `t`'s kicker and punter, for the coach's 4th-down decisions.
    Specialists specialists(int t) const {
        Specialists sp;
        const Lineup& l = lineups_[idx(t)];
        if (const std::string k = l.starter(Position::K); !k.empty()) {
            const Attributes a = l.ratings(k);
            sp.kick_power = a[Attr::KickPower];
            sp.kick_accuracy = a[Attr::KickAccuracy];
        }
        if (const std::string p = l.starter(Position::P); !p.empty()) sp.punt_power = l.ratings(p)[Attr::PuntPower];
        return sp;
    }
    std::size_t idx(int t) const { return static_cast<std::size_t>(t); }

    // --- the log and inputs ------------------------------------------------
    void line(const Json& j) {
        if (log_) *log_ << j.dump() << '\n';
    }
    void event(const std::string& name, Json fields = Json::object()) {
        Json j = {{"type", "event"}, {"tick", g_.tick}, {"event", name}};
        for (auto& [k, v] : fields.items()) j[k] = v;
        line(j);
    }
    Json ask(const std::string& input, int team, const std::function<Json()>& decide);
    std::optional<Json> ask_optional(const std::string& input, int team,
                                     const std::function<std::optional<Json>()>& decide);

    // --- the flow ----------------------------------------------------------
    void coin_toss();
    SnapResult kickoff(int kicking, bool free_kick);
    void end_quarter();
    void overtime();
    void series(int team, int ot);
    SnapResult snap();
    void attempt_try(int team, int ot);
    bool run_try_play(int team);
    // Runs the play, writing a tracking file when the options ask for one.
    PlayResult run_tracked(const PlayOptions& o, const std::string& label);

    // --- one snap's pieces -------------------------------------------------
    PlayOptions play_options(const PlayCall& call, const std::string& defense_play);
    PlayResult kneel_or_spike(const std::string& kind);
    void timeouts();
    bool between_plays(double used);  // false: the period ended before the snap
    std::optional<Foul> presnap_foul(const PlayOptions& o, double hike);
    void enforce_presnap(const Foul& f);
    SnapResult punt();
    SnapResult field_goal();
    // A play's work and injuries; `offense`: the team whose usage rows say offense.
    void injuries(const std::vector<PlayerUsage>& usage, int offense);
    void wear(const std::vector<PlayerUsage>& usage, int offense);
    // One kicking play (phase 9): `kicking` kicks from its own `yard_line`.
    KickResult kick_play(int kicking, double yard_line, PlayType type, const std::string& kind, Hash hash,
                         bool timed = true);
    // A kickoff or punt is over: who has the ball where, and the stats.
    SnapResult after_return(const KickResult& k, int kicking, double yard_line);
    void line_for_kick(const KickResult& k, int kicking, const std::string& before);
    void count_stats(const PlayResult& r, const Situation& before, const Next& standing);
    std::string spot_text(int team, double yl) const;
    std::string situation_text() const;
    // A player's box score line (created on first use).
    PlayerStats& stat(const std::string& id);

    const DataSet& data_;
    const RulesTable& rules_;
    const GameTuning& tuning_;
    GameOptions opt_;
    std::ostream* log_;
    const std::vector<Json>* replay_;
    std::size_t next_input_ = 0;
    Rng rng_;  // the game RNG: play seeds, flags, injuries, kicks, the coin toss
    std::array<const Team*, 2> teams_;
    std::array<Lineup, 2> lineups_;
    std::array<AutoCoach, 2> coaches_;

    GameState g_;
    BoxScore box_;
    int snaps_ = 0;
    int toss_winner_ = 0;
    int first_receiver_ = 0;
    std::optional<int> pending_kickoff_;
    bool pending_free_kick_ = false;
    std::array<int, 2> period_start_{};
    std::set<std::string> last_on_field_;  // both teams' players on the last snap
};

// ---------------------------------------------------------------------------
// Inputs
// ---------------------------------------------------------------------------

Json Game::ask(const std::string& input, int team, const std::function<Json()>& decide) {
    Json j;
    if (replay_) {
        if (next_input_ >= replay_->size()) {
            throw ReplayError("the log ran out of inputs at tick " + std::to_string(g_.tick) +
                              " (wanted " + input + ")");
        }
        j = (*replay_)[next_input_++];
        if (j.value("input", "") != input || j.value("team", "") != team_key(team) ||
            j.value("tick", std::int64_t{-1}) != g_.tick) {
            throw ReplayError("input " + std::to_string(next_input_) + ": the game wanted " + input +
                              " from " + team_key(team) + " at tick " + std::to_string(g_.tick) +
                              ", the log has " + j.dump());
        }
    } else {
        const Json answer = decide();
        j = {{"type", "input"}, {"tick", g_.tick}, {"input", input}, {"team", team_key(team)}};
        for (auto& [k, v] : answer.items()) j[k] = v;
    }
    line(j);
    return j;
}

std::optional<Json> Game::ask_optional(const std::string& input, int team,
                                       const std::function<std::optional<Json>()>& decide) {
    Json j;
    if (replay_) {
        if (next_input_ >= replay_->size()) return std::nullopt;
        const Json& next = (*replay_)[next_input_];
        if (next.value("input", "") != input || next.value("team", "") != team_key(team) ||
            next.value("tick", std::int64_t{-1}) != g_.tick) {
            return std::nullopt;
        }
        j = next;
        ++next_input_;
    } else {
        const std::optional<Json> answer = decide();
        if (!answer) return std::nullopt;
        j = {{"type", "input"}, {"tick", g_.tick}, {"input", input}, {"team", team_key(team)}};
        for (auto& [k, v] : answer->items()) j[k] = v;
    }
    line(j);
    return j;
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

std::string Game::spot_text(int team, double yl) const {
    const double rounded = std::round(yl);
    if (std::abs(rounded - 50.0) < 0.5) return "50";
    if (yl < 50.0) return team_of(team).abbr + " " + std::to_string(static_cast<int>(std::max(1.0, rounded)));
    return team_of(1 - team).abbr + " " + std::to_string(static_cast<int>(std::max(1.0, 100.0 - rounded)));
}

std::string Game::situation_text() const {
    static const char* kDown[] = {"1st", "2nd", "3rd", "4th"};
    std::ostringstream s;
    s << kDown[std::clamp(g_.s.down, 1, 4) - 1] << " & ";
    if (g_.s.line_to_gain() >= 100.0 - 1e-9) {
        s << "goal";
    } else {
        s << std::max(1, static_cast<int>(std::round(g_.s.distance)));
    }
    s << " at " << spot_text(g_.offense, g_.s.yard_line);
    return s.str();
}

PlayerStats& Game::stat(const std::string& id) {
    PlayerStats& p = box_.players[id];
    if (p.id.empty()) {
        p.id = id;
        p.team = roster(opt_.home).find(id) ? 0 : 1;
        const Player& pl = lineup(p.team).player(id);
        p.name = pl.first_name.empty() ? pl.last_name : pl.first_name.substr(0, 1) + ". " + pl.last_name;
    }
    return p;
}

// ---------------------------------------------------------------------------
// The flow
// ---------------------------------------------------------------------------

void Game::coin_toss() {
    toss_winner_ = rng_.chance(0.5) ? 0 : 1;
    const Json c = ask("coaching_decision", toss_winner_, [] {
        return Json{{"decision", "coin_toss"}, {"choice", "defer"}};
    });
    const bool defer = c.value("choice", "defer") == "defer";
    first_receiver_ = defer ? 1 - toss_winner_ : toss_winner_;
    event("coin_toss", {{"winner", team_key(toss_winner_)}, {"receives", team_key(first_receiver_)}});
}

SnapResult Game::kickoff(int kicking, bool free_kick) {
    // (rules-and-flow.md#kicks-verify-stubbed-in-phase-8) The kicking team
    // chooses deep, squib, or onside; the receivers answer with their return.
    const Json c = ask("coaching_decision", kicking, [&] {
        return Json{{"decision", "kickoff"}, {"choice", coach(kicking).kickoff_kind(g_)}};
    });
    const double from = free_kick ? rules_.safety_kick_from : rules_.kickoff_from;
    const std::string before = std::string(free_kick ? "Free kick" : "Kickoff") + " from " + spot_text(kicking, from);
    const KickResult k = kick_play(kicking, from, PlayType::Kickoff, c.value("choice", "deep"), Hash::Middle);
    PlayerStats& ks = stat(k.kicker_id);
    ++ks.kickoffs;
    if (k.result == "touchback") ++ks.touchbacks;
    const SnapResult out = after_return(k, kicking, from);
    line_for_kick(k, kicking, before);
    return out;
}

KickResult Game::kick_play(int kicking, double yard_line, PlayType type, const std::string& kind, Hash hash,
                           bool timed) {
    const int receiving = 1 - kicking;
    const Team& kt = team_of(kicking);
    const Team& rt = team_of(receiving);
    const auto find_play = [&](const Team& t, PlayType want, const std::string& k) -> const Play* {
        const auto book = data_.playbooks.find(t.special_playbook);
        if (book == data_.playbooks.end()) throw GameError(t.id + " has no special teams playbook");
        const Play* first = nullptr;
        for (const Play& p : book->second.plays) {
            if (p.type != want) continue;
            if (!first) first = &p;
            if (!k.empty() && p.kick && p.kick->kind == k) return &p;
        }
        if (!first) throw GameError(t.id + " has no " + std::string(to_string(want)) + " play");
        return first;
    };
    const PlayType answer = type == PlayType::Kickoff ? PlayType::KickoffReturn
                            : type == PlayType::Punt  ? PlayType::PuntReturn
                                                      : PlayType::FieldGoalBlock;
    const Play* kp = find_play(kt, type, kind);
    const Play* rp = find_play(rt, answer, "");
    KickOptions o;
    o.kicking_team = kt.id;
    o.receiving_team = rt.id;
    o.kick_play = kp->id;
    o.return_play = rp->id;
    o.yard_line = std::clamp(yard_line, 0.5, 99.5);
    o.hash = hash;
    o.kicking_right = (kicking == 0) == (g_.quarter % 2 == 1);
    o.seed = rng_.next_u64();
    o.kicking_home = kicking == 0;
    o.return_aggression = rt.tactics.special.return_aggression;
    const auto fill = [&](int team, const Play& play) {
        const Team& t = team_of(team);
        const Formation& f = data_.formations.at(play.formation);
        const auto unit = t.depth_chart.special_units.find(f.special_unit);
        if (unit == t.depth_chart.special_units.end()) {
            throw GameError(t.id + " has no '" + f.special_unit + "' special teams unit");
        }
        std::vector<std::string> slots;
        for (const FormationSlot& sl : f.slots) slots.push_back(sl.name);
        return lineup(team).fill(unit->second, slots);
    };
    o.kicking_players = fill(kicking, *kp);
    o.receiving_players = fill(receiving, *rp);
    for (const auto& [slot, id] : o.kicking_players) o.modifiers[id] = lineup(kicking).modifiers(id);
    for (const auto& [slot, id] : o.receiving_players) o.modifiers[id] = lineup(receiving).modifiers(id);

    ++snaps_;
    KickResult k;
    try {
        if (opt_.tracking) {
            char num[8];
            std::snprintf(num, sizeof num, "%03d", snaps_);
            std::string clock = clock_text(g_.clock);
            std::erase(clock, ':');
            const std::unique_ptr<std::ostream> out = opt_.tracking(
                std::string(num) + "_q" + std::to_string(g_.quarter) + "_" + clock + "_" + team_key(kicking) + "_" + kp->id);
            if (out) {
                TrackingWriter writer(*out);
                k = run_kick(data_, o, &writer);
            } else {
                k = run_kick(data_, o);
            }
        } else {
            k = run_kick(data_, o);
        }
    } catch (const PlayError& e) {
        throw GameError(std::string("kick ") + kp->id + ": " + e.what());
    }
    // The clock runs from the snap (punts, kicks) or the first touch (kickoffs).
    if (timed && !g_.untimed && !g_.overtime() && g_.clock > 0.0) {
        const double run = std::min(g_.clock, k.clock_seconds);
        g_.clock -= run;
        box_.teams[idx(type == PlayType::Kickoff ? receiving : kicking)].possession_seconds += run;
    }
    if (timed) g_.untimed = false;
    g_.tick += ticks(k.seconds);
    wear(k.usage, kicking);
    injuries(k.usage, kicking);
    return k;
}

SnapResult Game::after_return(const KickResult& k, int kicking, double yard_line) {
    const int receiving = 1 - kicking;
    const bool kickoff = k.type == PlayType::Kickoff;
    // The receivers' own yard line where the ball is dead.
    const double theirs = 100.0 - (yard_line + k.end_y);
    if (!k.returner_id.empty() && (k.result == "returned" || k.result == "touchdown")) {
        PlayerStats& r = stat(k.returner_id);
        TeamStats& t = box_.teams[idx(receiving)];
        if (kickoff) {
            ++r.kr_att;
            r.kr_yds += k.return_yards;
            ++t.kr_att;
            t.kr_yds += k.return_yards;
            if (k.result == "touchdown") ++r.kr_td;
        } else {
            ++r.pr_att;
            r.pr_yds += k.return_yards;
            ++t.pr_att;
            t.pr_yds += k.return_yards;
            if (k.result == "touchdown") ++r.pr_td;
        }
    }
    if (!k.tackler_id.empty()) ++stat(k.tackler_id).tackles;
    g_.clock_state = {ClockStart::OnSnap, true};
    if (k.result == "touchdown") {
        g_.score[idx(receiving)] += 6;
        g_.offense = receiving;
        return {SnapEnd::Touchdown, receiving};
    }
    if (k.kicking_ball) {
        // An onside kick or a muff the kicking team fell on: its ball, first down.
        g_.offense = kicking;
        g_.s = first_down_at(std::clamp(yard_line + k.end_y, 1.0, 99.0), rules_);
        g_.hash = hash_from(k.end_x);
        return {};
    }
    double yl = theirs;
    Hash hash = mirrored(hash_from(k.end_x));
    if (k.result == "touchback") {
        yl = kickoff ? rules_.kickoff_touchback : rules_.touchback;
        hash = Hash::Middle;
    } else if (k.result == "out_of_bounds" && kickoff) {
        yl = 100.0 - (yard_line + rules_.kickoff_out_of_bounds);
        hash = Hash::Middle;
    } else if (k.result == "fair_catch" && kickoff) {
        yl = std::max(theirs, rules_.kickoff_touchback);
    }
    if (yl <= 0.0) {
        yl = kickoff ? rules_.kickoff_touchback : rules_.touchback;
        hash = Hash::Middle;
    }
    g_.offense = receiving;
    g_.s = first_down_at(std::min(yl, 99.0), rules_);
    g_.hash = hash;
    return {SnapEnd::Turnover, -1};
}

void Game::line_for_kick(const KickResult& k, int kicking, const std::string& before) {
    Json pl = {{"type", "play"},  {"tick", g_.tick}, {"quarter", g_.quarter}, {"clock", clock_text(g_.clock)},
               {"kicking", team_key(kicking)}, {"situation", before}, {"call", k.kind},
               {"outcome", k.result}, {"summary", k.summary}};
    if (k.result == "touchdown" || k.result == "made") {
        pl["home"] = g_.score[0];
        pl["away"] = g_.score[1];
    } else {
        pl["next"] = std::string(team_key(g_.offense)) + " ball, " + situation_text();
    }
    line(pl);
}

void Game::end_quarter() {
    box_.periods.push_back({g_.score[0] - period_start_[0], g_.score[1] - period_start_[1]});
    period_start_ = g_.score;
    event("end_of_quarter", {{"quarter", g_.quarter}, {"home", g_.score[0]}, {"away", g_.score[1]}});
    g_.untimed = false;
    if (g_.quarter == 2) {
        // Halftime: the team that didn't get the ball first receives.
        g_.quarter = 3;
        g_.clock = rules_.quarter_seconds;
        g_.timeouts = {rules_.timeouts_per_half, rules_.timeouts_per_half};
        g_.two_minute_done = false;
        lineup(0).halftime();
        lineup(1).halftime();
        pending_kickoff_ = first_receiver_;
        pending_free_kick_ = false;
        event("halftime");
        return;
    }
    // After the 1st and 3rd quarters the teams switch ends; the ball and the
    // down stay.
    ++g_.quarter;
    g_.clock = rules_.quarter_seconds;
    g_.clock_state = {ClockStart::OnSnap, true};
    if (g_.quarter == 3) g_.two_minute_done = false;
}

GameResult Game::run() {
    line({{"type", "header"},
          {"sim_version", std::string(version())},
          {"rules", rules_.id},
          {"seed", opt_.seed},
          {"home", opt_.home},
          {"away", opt_.away},
          {"crowd", opt_.crowd},
          {"neutral", opt_.neutral}});
    g_.clock = rules_.quarter_seconds;
    g_.timeouts = {rules_.timeouts_per_half, rules_.timeouts_per_half};
    coin_toss();
    pending_kickoff_ = 1 - first_receiver_;

    while (g_.quarter <= rules_.quarters) {
        if (g_.clock <= 0.0 && !g_.untimed) {
            if (g_.quarter == rules_.quarters) {
                box_.periods.push_back({g_.score[0] - period_start_[0], g_.score[1] - period_start_[1]});
                period_start_ = g_.score;
                event("end_of_regulation", {{"home", g_.score[0]}, {"away", g_.score[1]}});
                break;
            }
            end_quarter();
            continue;
        }
        SnapResult r;
        if (pending_kickoff_) {
            const int kicking = *pending_kickoff_;
            const bool free_kick = pending_free_kick_;
            pending_kickoff_.reset();
            pending_free_kick_ = false;
            r = kickoff(kicking, free_kick);
        } else {
            r = snap();
        }
        if (r.end == SnapEnd::Touchdown) {
            attempt_try(r.scorer, 0);
            pending_kickoff_ = r.scorer;
        } else if (r.end == SnapEnd::FieldGoal) {
            pending_kickoff_ = r.scorer;
        } else if (r.end == SnapEnd::Safety) {
            pending_kickoff_ = 1 - r.scorer;  // the team that gave it up free kicks
            pending_free_kick_ = true;
        }
    }
    if (g_.score[0] == g_.score[1]) overtime();
    event("final", {{"home", g_.score[0]}, {"away", g_.score[1]}});

    GameResult out;
    out.score = g_.score;
    out.overtimes = std::max(0, g_.quarter - rules_.quarters);
    out.snaps = snaps_;
    box_.teams[0].points = g_.score[0];
    box_.teams[1].points = g_.score[1];
    out.box = box_;
    Json periods = Json::array();
    for (const auto& p : box_.periods) periods.push_back({p[0], p[1]});
    line({{"type", "final"}, {"home", g_.score[0]}, {"away", g_.score[1]}, {"periods", periods}});
    return out;
}

void Game::overtime() {
    // (rules-and-flow.md#overtime-checked) The toss winner takes defense
    // first; the order alternates.
    const int first = 1 - toss_winner_;
    for (int ot = 1;; ++ot) {
        g_.quarter = rules_.quarters + ot;
        g_.clock = 0.0;
        if (ot <= 3) {
            const int n = rules_.overtime_timeouts[static_cast<std::size_t>(ot - 1)];
            g_.timeouts = {n, n};
        }
        event("overtime", {{"period", ot}});
        const int lead = ot % 2 == 1 ? first : 1 - first;
        for (const int team : {lead, 1 - lead}) {
            if (ot <= 2) {
                series(team, ot);
            } else {
                attempt_try(team, ot);  // two-point plays only
            }
        }
        box_.periods.push_back({g_.score[0] - period_start_[0], g_.score[1] - period_start_[1]});
        period_start_ = g_.score;
        if (g_.score[0] != g_.score[1]) return;
        if (ot >= 20) throw GameError("overtime didn't end after 20 periods");
    }
}

void Game::series(int team, int ot) {
    g_.offense = team;
    g_.s = first_down_at(100.0 - rules_.overtime_start, rules_);
    g_.hash = Hash::Middle;
    g_.clock_state = {ClockStart::OnSnap, true};
    event("overtime_possession", {{"offense", team_key(team)}, {"spot", spot_text(team, g_.s.yard_line)}});
    for (int k = 0; k < 200; ++k) {
        const SnapResult r = snap();
        switch (r.end) {
            case SnapEnd::Continue:
            case SnapEnd::PeriodOver:
                if (g_.offense == team) continue;
                return;  // a change of possession
            case SnapEnd::Touchdown:
                if (r.scorer == team) attempt_try(team, ot);
                return;
            default:
                return;
        }
    }
    throw GameError("an overtime possession didn't end");
}

void Game::attempt_try(int team, int ot) {
    // After a touchdown (or, from the 3rd overtime, on its own): a kick or a
    // two-point play from the 3.
    bool two = ot >= 2;
    if (ot < 2) {
        const Json c = ask("coaching_decision", team, [&] {
            GameState view = g_;
            view.offense = team;
            return Json{{"decision", "try"}, {"choice", coach(team).go_for_two(view) ? "two" : "kick"}};
        });
        two = c.value("choice", "kick") == "two";
    }
    if (!two) {
        const GameState saved = g_;
        const KickResult kick = kick_play(team, 100.0 - rules_.try_line, PlayType::FieldGoal, "", Hash::Middle, false);
        const std::array<int, 2> score = g_.score;
        const std::int64_t tick = g_.tick;
        g_ = saved;  // the try is untimed and leaves the game state alone
        g_.score = score;
        g_.tick = tick;
        const bool good = kick.result == "made";
        PlayerStats& k = stat(kick.kicker_id);
        ++k.xp_att;
        if (good) {
            ++k.xp_made;
            g_.score[idx(team)] += 1;
        }
        event("extra_point", {{"team", team_key(team)}, {"good", good}, {"home", g_.score[0]}, {"away", g_.score[1]}});
        return;
    }
    const bool good = run_try_play(team);
    event("two_point_try", {{"team", team_key(team)}, {"good", good}, {"home", g_.score[0]}, {"away", g_.score[1]}});
}

bool Game::run_try_play(int team) {
    const GameState saved = g_;
    g_.offense = team;
    g_.s = first_down_at(100.0 - rules_.try_line, rules_);
    g_.hash = Hash::Middle;
    const Json call = ask("call_play", team, [&] {
        const PlayCall c = coach(team).offense_call(g_, true);
        return Json{{"play", c.play}, {"flip", c.flip}, {"try", true}};
    });
    const Json dcall = ask("call_play", 1 - team, [&] {
        return Json{{"play", coach(1 - team).defense_call(g_, offense_play(team, call.value("play", "")))}};
    });
    const PlayOptions o = play_options({call.value("play", ""), call.value("flip", false)}, dcall.value("play", ""));
    ++snaps_;
    const PlayResult r = run_tracked(o, o.play + "_try");
    wear(r.usage, g_.offense);
    bool good = false;
    if (r.outcome == "touchdown") {
        if (r.interception || r.fumble_lost) {
            g_ = saved;
            g_.score[idx(1 - team)] += 2;  // a defensive return of a try
            line({{"type", "play"}, {"tick", g_.tick}, {"try", true}, {"offense", team_key(team)},
                  {"call", o.play}, {"defense_call", o.defense_play}, {"summary", r.summary}});
            return false;
        }
        good = true;
    }
    line({{"type", "play"}, {"tick", g_.tick}, {"try", true}, {"offense", team_key(team)},
          {"call", o.play}, {"defense_call", o.defense_play}, {"outcome", r.outcome}, {"summary", r.summary}});
    const std::array<int, 2> score = g_.score;
    g_ = saved;
    g_.score = score;
    if (good) g_.score[idx(team)] += 2;
    return good;
}

// ---------------------------------------------------------------------------
// One snap
// ---------------------------------------------------------------------------

PlayResult Game::run_tracked(const PlayOptions& o, const std::string& label) {
    if (!opt_.tracking) return run_play(data_, o);
    const std::string clock = clock_text(g_.clock);
    std::string mmss;
    for (const char c : clock) {
        if (c != ':') mmss += c;
    }
    char num[8];
    std::snprintf(num, sizeof num, "%03d", snaps_);
    const std::string name = std::string(num) + "_q" + std::to_string(g_.quarter) + "_" + mmss + "_" +
                             team_key(g_.offense) + "_" + label;
    const std::unique_ptr<std::ostream> out = opt_.tracking(name);
    if (!out) return run_play(data_, o);
    TrackingWriter writer(*out);
    return run_play(data_, o, &writer);
}

PlayOptions Game::play_options(const PlayCall& call, const std::string& defense_play) {
    const int off = g_.offense, def = g_.defense();
    const Team& ot = team_of(off);
    const Team& dt = team_of(def);
    PlayOptions o;
    o.team = ot.id;
    o.play = call.play;
    o.flip = call.flip;
    o.hash = g_.hash;
    o.yard_line = std::clamp(g_.s.yard_line, 0.5, 99.5);
    // The home team attacks +x in odd quarters.
    o.offense_right = (off == 0) == (g_.quarter % 2 == 1);
    o.seed = rng_.next_u64();
    o.huddle = false;
    o.defense = dt.id;
    o.defense_play = defense_play;
    o.offense_home = off == 0;

    const Playbook& obook = data_.playbooks.at(ot.offense_playbook);
    const Play* play = obook.find(call.play);
    if (!play) throw GameError(ot.id + " called an unknown play '" + call.play + "'");
    const Formation& of = data_.formations.at(play->formation);
    std::vector<std::string> slots;
    for (const FormationSlot& s : of.slots) slots.push_back(s.name);
    const auto personnel = ot.depth_chart.offense_personnel.find(of.personnel);
    if (personnel == ot.depth_chart.offense_personnel.end()) {
        throw GameError(ot.id + " has no '" + of.personnel + "' personnel");
    }
    o.offense_players = lineup(off).fill(personnel->second, slots);

    const Playbook& dbook = data_.playbooks.at(dt.defense_playbook);
    const Play* dplay = dbook.find(defense_play);
    if (!dplay) throw GameError(dt.id + " called an unknown play '" + defense_play + "'");
    const Formation& df = data_.formations.at(dplay->formation);
    slots.clear();
    for (const FormationSlot& s : df.slots) slots.push_back(s.name);
    const auto package = dt.depth_chart.defense_packages.find(df.package);
    if (package == dt.depth_chart.defense_packages.end()) {
        throw GameError(dt.id + " has no '" + df.package + "' package");
    }
    o.defense_players = lineup(def).fill(package->second, slots);

    for (const auto& [slot, id] : o.offense_players) o.modifiers[id] = lineup(off).modifiers(id);
    for (const auto& [slot, id] : o.defense_players) o.modifiers[id] = lineup(def).modifiers(id);
    return o;
}

PlayResult Game::kneel_or_spike(const std::string& kind) {
    PlayResult r;
    const std::string qb = lineup(g_.offense).starter(Position::QB);
    if (kind == "kneel") {
        r.outcome = "tackled";
        r.yards = -1.0;
        r.spot_y = -1.0;
        r.seconds = 1.5;
        r.rusher_id = qb;
        r.carrier_id = qb;
        r.summary = "QB kneel";
    } else {
        r.outcome = "incomplete";
        r.seconds = 1.0;
        r.passer_id = qb;
        r.thrown_away = true;
        r.summary = "QB spike";
    }
    return r;
}

void Game::timeouts() {
    // Right after the play: the defense may stop the clock, then the offense.
    for (const int team : {g_.defense(), g_.offense}) {
        if (g_.timeouts[idx(team)] <= 0) continue;
        const bool on_offense = team == g_.offense;
        const std::optional<Json> t = ask_optional("timeout", team, [&]() -> std::optional<Json> {
            if (coach(team).timeout(g_, on_offense)) return Json::object();
            return std::nullopt;
        });
        if (!t) continue;
        --g_.timeouts[idx(team)];
        ++box_.teams[idx(team)].timeouts_used;
        g_.clock_state = {ClockStart::OnSnap, true};
        event("timeout", {{"team", team_key(team)}, {"left", g_.timeouts[idx(team)]},
                          {"clock", clock_text(g_.clock)}});
        return;  // one per dead-ball period
    }
}

bool Game::between_plays(double used) {
    // Time passes between the end of the last play and the snap.
    double run = g_.overtime() || g_.untimed ? 0.0 : runoff_before_snap(g_.clock_state, used, tuning_);
    const double before = g_.clock;
    const double two = rules_.two_minute;
    if (g_.second_or_fourth() && !g_.two_minute_done && before > two && before - run <= two) {
        // The two-minute timeout stops the clock at 2:00; it restarts on the
        // ready signal with a 25-second play clock.
        g_.two_minute_done = true;
        g_.clock = two;
        g_.clock_state = {ClockStart::OnReady, true};
        box_.teams[idx(g_.offense)].possession_seconds += before - two;
        event("two_minute_timeout");
        run = runoff_before_snap(g_.clock_state, used * rules_.admin_play_clock / rules_.play_clock, tuning_);
    }
    g_.tick += ticks(used);
    const double from = g_.clock;
    g_.clock = std::max(0.0, g_.clock - run);
    box_.teams[idx(g_.offense)].possession_seconds += from - g_.clock;
    lineup(0).recover(used, last_on_field_);
    lineup(1).recover(used, last_on_field_);
    if (!g_.overtime() && !g_.untimed && g_.clock <= 0.0 && run > 0.0) {
        event("clock_expired", {{"quarter", g_.quarter}});
        return false;
    }
    return true;
}

std::optional<Foul> Game::presnap_foul(const PlayOptions& o, double hike) {
    if (hike <= 0.0) {
        Foul f;
        f.type = "delay_of_game";
        f.by_offense = true;
        return f;
    }
    // False starts (the away offense hears the crowd), then offside.
    const double crowd = (!opt_.neutral && g_.offense == 1) ? 1.0 + opt_.crowd * tuning_.crowd : 1.0;
    for (const auto& [slot, id] : o.offense_players) {
        const double careless = std::max(0.0, 1.5 - tuning_.foul_skill(lineup(g_.offense).ratings(id)) / 100.0);
        if (rng_.chance(tuning_.false_start * careless * crowd)) {
            Foul f;
            f.type = "false_start";
            f.by_offense = true;
            f.slot = slot;
            f.player_id = id;
            return f;
        }
    }
    for (const auto& [slot, id] : o.defense_players) {
        const double careless = std::max(0.0, 1.5 - tuning_.foul_skill(lineup(g_.defense()).ratings(id)) / 100.0);
        if (rng_.chance(tuning_.offside * careless)) {
            Foul f;
            f.type = "offside";
            f.by_offense = false;
            f.slot = slot;
            f.player_id = id;
            return f;
        }
    }
    return std::nullopt;
}

void Game::enforce_presnap(const Foul& f) {
    // Dead-ball fouls: no play, the down is replayed, always enforced.
    const PenaltyRule& rule = rules_.penalty(f.type);
    const bool was_running = g_.clock_state.start == ClockStart::Running;
    const Next n = enforce(g_.s, PlayResult{}, f, rule, rules_);
    const int fouling = f.by_offense ? g_.offense : g_.defense();
    TeamStats& ts = box_.teams[idx(fouling)];
    ++ts.penalties;
    ts.penalty_yds += std::abs(n.yards);
    if (n.first_down) {
        ++box_.teams[idx(g_.offense)].first_downs;
        ++box_.teams[idx(g_.offense)].first_downs_penalty;
    }
    g_.s = n.next;
    Json fields = {{"foul", f.type}, {"team", team_key(fouling)}, {"yards", r2(n.yards)},
                   {"result", situation_text()}};
    if (!f.player_id.empty()) fields["player"] = f.player_id;
    // The offended team may take a 10-second runoff late in a half.
    g_.clock_state = clock_after({.penalty = true});
    if (!was_running) g_.clock_state.start = ClockStart::OnSnap;
    if (was_running && g_.second_or_fourth() && g_.clock <= rules_.last_minute && !g_.overtime()) {
        const int offended = 1 - fouling;
        const Json c = ask("runoff", offended, [&] { return Json{{"take", coach(offended).runoff(g_)}}; });
        if (c.value("take", false)) {
            g_.clock = std::max(0.0, g_.clock - rules_.runoff);
            fields["runoff"] = rules_.runoff;
        }
    }
    event("penalty", fields);
}

SnapResult Game::punt() {
    const int off = g_.offense;
    const double yl = g_.s.yard_line;
    const std::string before = situation_text();
    const KickResult k = kick_play(off, yl, PlayType::Punt, "", g_.hash);
    PlayerStats& p = stat(k.kicker_id);
    ++p.punts;
    p.punt_yds += k.kick_distance;
    ++box_.teams[idx(off)].punts;
    box_.teams[idx(off)].punt_yds += k.kick_distance;
    const SnapResult out = after_return(k, off, yl);
    line_for_kick(k, off, before);
    return out;
}

SnapResult Game::field_goal() {
    const int off = g_.offense;
    const double yl = g_.s.yard_line;
    const std::string before = situation_text();
    const KickResult k = kick_play(off, yl, PlayType::FieldGoal, "", g_.hash);
    const double distance = 100.0 - yl + 17.0;
    PlayerStats& ks = stat(k.kicker_id);
    ++ks.fg_att;
    ++box_.teams[idx(off)].fg_att;
    const bool good = k.result == "made";
    if (good) {
        ++ks.fg_made;
        ++box_.teams[idx(off)].fg_made;
        ks.fg_long = std::max(ks.fg_long, std::round(distance));
        g_.score[idx(off)] += 3;
    }
    g_.clock_state = {ClockStart::OnSnap, true};
    SnapResult out{SnapEnd::FieldGoal, off};
    if (!good) {
        // A miss: the defense takes over at the previous spot, or the 20; a
        // block, where the ball was blocked.
        g_.offense = 1 - off;
        const double at = k.result == "blocked" ? std::clamp(100.0 - (yl + k.end_y), 1.0, 99.0)
                                                : std::max(100.0 - yl, rules_.touchback);
        g_.s = first_down_at(at, rules_);
        g_.hash = mirrored(g_.hash);
        out = {SnapEnd::MissedKick, -1};
    }
    line_for_kick(k, off, before);
    return out;
}

void Game::wear(const std::vector<PlayerUsage>& usage, int offense) {
    lineup(offense).play(usage, true);
    lineup(1 - offense).play(usage, false);
    last_on_field_.clear();
    for (const PlayerUsage& u : usage) last_on_field_.insert(u.id);
    for (int t = 0; t < 2; ++t) {
        for (const auto& [id, resting] : lineup(t).rest_changes()) {
            event(resting ? "sub_out" : "sub_in", {{"team", team_key(t)}, {"player", id},
                                                  {"energy", r2(lineup(t).energy(id))}});
        }
    }
}

void Game::injuries(const std::vector<PlayerUsage>& usage, int offense) {
    for (const PlayerUsage& u : usage) {
        const int t = u.offense ? offense : 1 - offense;
        Lineup& l = lineup(t);
        const double prone = 0.5 + l.player(u.id).hidden.injury_proneness / 100.0;
        const double p = (tuning_.injury_snap + tuning_.injury_contact * u.contacts) * prone;
        if (!rng_.chance(p)) continue;
        l.injure(u.id);
        ++box_.teams[idx(t)].injuries;
        event("injury", {{"team", team_key(t)}, {"player", u.id}, {"out", "game"}});
    }
}

void Game::count_stats(const PlayResult& r, const Situation& before, const Next& standing) {
    const int off = g_.offense, def = g_.defense();
    TeamStats& to = box_.teams[idx(off)];
    const auto player = [&](const std::string& id, int) -> PlayerStats& { return stat(id); };
    ++to.plays;
    const bool turnover = r.interception || r.fumble_lost;
    const bool scored = standing.kind == Next::Kind::Touchdown;
    // Yards the offense gained: to the end of the play (or the fumble).
    const double gained = r.outcome == "incomplete" || r.interception ? 0.0 : r.yards;
    const bool pass = !r.passer_id.empty();
    if (pass) {
        PlayerStats& qb = player(r.passer_id, off);
        ++qb.pass_att;
        ++to.pass_att;
        if (!r.target_id.empty() && !r.thrown_away) ++player(r.target_id, off).targets;
        if (r.complete) {
            ++qb.pass_cmp;
            ++to.pass_cmp;
            qb.pass_yds += gained;
            to.pass_yds += gained;
            PlayerStats& rec = player(r.target_id, off);
            ++rec.rec;
            rec.rec_yds += gained;
            rec.rec_long = std::max(rec.rec_long, gained);
            if (scored) {
                ++qb.pass_td;
                ++rec.rec_td;
            }
        }
        if (r.interception) {
            ++qb.ints;
            ++to.ints;
            if (!r.interceptor_id.empty()) ++player(r.interceptor_id, def).def_int;
        }
    } else if (!r.rusher_id.empty()) {
        // Runs, scrambles, sacks, and kneels are all rushes (NCAA).
        PlayerStats& ru = player(r.rusher_id, off);
        ++ru.rush_att;
        ru.rush_yds += gained;
        ru.rush_long = std::max(ru.rush_long, gained);
        ++to.rush_att;
        to.rush_yds += gained;
        if (scored) ++ru.rush_td;
        if (r.outcome == "sack") ++to.sacks_taken;
    }
    if (!r.tackler_id.empty()) {
        const bool tackler_on_defense = !turnover;
        PlayerStats& tk = player(r.tackler_id, tackler_on_defense ? def : off);
        ++tk.tackles;
        if (r.outcome == "sack") ++tk.sacks;
    }
    if (r.fumble && !r.fumbled_by_id.empty()) {
        PlayerStats& f = stat(r.fumbled_by_id);
        ++f.fumbles;
        if (r.fumble_lost) ++f.fumbles_lost;
        if (r.fumble_lost) {
            ++to.fumbles_lost;
            if (!r.recovered_by_id.empty()) ++player(r.recovered_by_id, def).fum_rec;
        }
    }
    if (standing.first_down && standing.kind == Next::Kind::Continue) {
        ++to.first_downs;
        ++(pass ? to.first_downs_pass : to.first_downs_rush);
    }
    const bool converted = (standing.first_down && standing.kind == Next::Kind::Continue) || scored;
    if (before.down == 3) {
        ++to.third_att;
        if (converted) ++to.third_conv;
    } else if (before.down == 4) {
        ++to.fourth_att;
        if (converted) ++to.fourth_conv;
    }
}

SnapResult Game::snap() {
    timeouts();

    // A kick on 4th down (or a last-chance field goal at the end of a half).
    const double kick_distance = 100.0 - g_.s.yard_line + 17.0;
    const Specialists sp = specialists(g_.offense);
    const bool last_chance =
        g_.second_or_fourth() && g_.clock <= 8.0 && kick_distance <= coach(g_.offense).fg_range(sp);
    KickChoice kick = KickChoice::Go;
    if (g_.s.down == 4 || last_chance) {
        const Json c = ask("coaching_decision", g_.offense, [&] {
            const KickChoice k = coach(g_.offense).fourth_down(g_, sp);
            return Json{{"decision", "fourth_down"},
                        {"choice", k == KickChoice::Punt ? "punt" : k == KickChoice::FieldGoal ? "field_goal" : "go"}};
        });
        const std::string choice = c.value("choice", "go");
        kick = choice == "punt" ? KickChoice::Punt : choice == "field_goal" ? KickChoice::FieldGoal : KickChoice::Go;
    }

    PlayCall call;
    std::string defense_play;
    if (kick == KickChoice::Go) {
        const Json oc = ask("call_play", g_.offense, [&] {
            const PlayCall c = coach(g_.offense).offense_call(g_, false);
            return Json{{"play", c.play}, {"flip", c.flip}};
        });
        call = {oc.value("play", ""), oc.value("flip", false)};
        if (call.play != "kneel" && call.play != "spike") {
            const Json dc = ask("call_play", g_.defense(), [&] {
                return Json{{"play", coach(g_.defense()).defense_call(g_, offense_play(g_.offense, call.play))}};
            });
            defense_play = dc.value("play", "");
        }
    }

    // The hike: how much of the play clock the offense uses.
    const double play_clock = g_.clock_state.admin ? rules_.admin_play_clock : rules_.play_clock;
    const Json h = ask("hike", g_.offense, [&] { return Json{{"play_clock", r2(coach(g_.offense).hike(g_, play_clock))}}; });
    const double reading = h.value("play_clock", 10.0);
    const double used = std::min(play_clock, play_clock - reading);
    if (!between_plays(used)) return {SnapEnd::PeriodOver, -1};
    ++snaps_;

    if (kick == KickChoice::Punt || kick == KickChoice::FieldGoal) {
        if (reading <= 0.0) {
            enforce_presnap(Foul{"delay_of_game", true, "", "", 0.0, 0.0, 0.0});
            return {};
        }
        return kick == KickChoice::Punt ? punt() : field_goal();
    }

    // The play.
    const bool clock_play = call.play == "kneel" || call.play == "spike";
    PlayOptions o;
    if (!clock_play) {
        o = play_options(call, defense_play);
        if (const std::optional<Foul> f = presnap_foul(o, reading)) {
            enforce_presnap(*f);
            return {};
        }
    } else if (reading <= 0.0) {
        enforce_presnap(Foul{"delay_of_game", true, "", "", 0.0, 0.0, 0.0});
        return {};
    }
    PlayResult r;
    try {
        r = clock_play ? kneel_or_spike(call.play) : run_tracked(o, call.play);
    } catch (const PlayError& e) {
        throw GameError(std::string("play ") + call.play + " at " + situation_text() + ": " + e.what());
    }

    const int off = g_.offense, def = g_.defense();
    if (const Play* called = clock_play ? nullptr : offense_play(off, call.play)) {
        ++(called->type == PlayType::Run ? g_.runs : g_.passes)[idx(off)];
    }
    const Situation before = g_.s;
    const std::string before_text = situation_text();
    const double clock_before = g_.clock;
    const bool timed = !g_.untimed && !g_.overtime();
    if (timed) {
        g_.clock = std::max(0.0, g_.clock - r.seconds);
        box_.teams[idx(off)].possession_seconds += clock_before - g_.clock;
    }
    g_.tick += ticks(r.seconds);
    const bool expired = timed && clock_before > 0.0 && g_.clock <= 0.0;
    const bool was_untimed = g_.untimed;
    g_.untimed = false;
    wear(r.usage, off);
    const int injuries_before = box_.teams[0].injuries + box_.teams[1].injuries;
    injuries(r.usage, off);
    const bool injury = box_.teams[0].injuries + box_.teams[1].injuries > injuries_before;

    // Fouls: flagged, then the offended team accepts or declines.
    const Next standing = result_of_play(before, r, rules_);
    std::vector<Foul> flagged;
    Json fouls = Json::array();
    for (const Foul& f : r.fouls) {
        const bool flag = rng_.chance(rules_.penalty(f.type).flag);
        fouls.push_back({{"foul", f.type}, {"team", team_key(f.by_offense ? off : def)},
                         {"player", f.player_id}, {"flagged", flag}});
        if (flag) flagged.push_back(f);
    }
    Next result = standing;
    bool penalty = false;      // an accepted foul (or offsetting fouls)
    bool nullified = false;    // the play doesn't count
    std::optional<Foul> accepted;
    const bool off_fouled = std::any_of(flagged.begin(), flagged.end(), [](const Foul& f) { return f.by_offense; });
    const bool def_fouled = std::any_of(flagged.begin(), flagged.end(), [](const Foul& f) { return !f.by_offense; });
    if (off_fouled && def_fouled) {
        result = replay_down(before);
        penalty = nullified = true;
        event("offsetting_fouls");
    } else if (!flagged.empty()) {
        const int offended = off_fouled ? def : off;
        const double sign = offended == off ? 1.0 : -1.0;  // the offended team maximizes this
        const Json d = ask("penalty_decision", offended, [&] {
            std::vector<double> values;
            for (const Foul& f : flagged) {
                values.push_back(sign * value_for_offense(enforce(before, r, f, rules_.penalty(f.type), rules_), rules_));
            }
            const int best = coach(offended).penalty_choice(sign * value_for_offense(standing, rules_), values);
            Json j = {{"accept", best >= 0}};
            j["foul"] = best >= 0 ? flagged[static_cast<std::size_t>(best)].type : flagged.front().type;
            return j;
        });
        if (d.value("accept", false)) {
            const std::string type = d.value("foul", "");
            const auto it = std::find_if(flagged.begin(), flagged.end(), [&](const Foul& f) { return f.type == type; });
            if (it == flagged.end()) throw ReplayError("accepted a foul that wasn't flagged: " + type);
            accepted = *it;
            const PenaltyRule& rule = rules_.penalty(type);
            result = enforce(before, r, *it, rule, rules_);
            penalty = true;
            nullified = rule.spot != EnforceSpot::EndOfPlay ||
                        standing.kind != Next::Kind::Continue || r.outcome == "incomplete";
            // Fouls carried to the kickoff aren't built: a foul enforced at
            // the end of a scoring play is dropped.
            if (rule.spot == EnforceSpot::EndOfPlay && standing.kind != Next::Kind::Continue &&
                standing.kind != Next::Kind::ChangeOfPossession) {
                result = standing;
                penalty = nullified = false;
                accepted.reset();
            }
        }
        for (auto& f : fouls) {
            if (accepted && f["foul"] == accepted->type && f["player"] == accepted->player_id &&
                f["flagged"] == true) {
                f["accepted"] = true;
                break;
            }
        }
    }
    if (accepted) {
        const int fouling = accepted->by_offense ? off : def;
        ++box_.teams[idx(fouling)].penalties;
        // Yards walked off: from the line, or from the end of a play that stands.
        const double walked = nullified ? result.yards : result.yards - standing.yards;
        box_.teams[idx(fouling)].penalty_yds += std::abs(walked);
        if (result.first_down && !accepted->by_offense) {
            ++box_.teams[idx(off)].first_downs;
            ++box_.teams[idx(off)].first_downs_penalty;
        }
    }
    if (!nullified && !clock_play) count_stats(r, before, standing);
    if (!nullified && clock_play) {
        if (call.play == "kneel") {
            PlayerStats& qb = stat(r.rusher_id);
            ++qb.rush_att;
            qb.rush_yds -= 1.0;
            ++box_.teams[idx(off)].rush_att;
            box_.teams[idx(off)].rush_yds -= 1.0;
        } else {
            ++stat(r.passer_id).pass_att;
            ++box_.teams[idx(off)].pass_att;
        }
        ++box_.teams[idx(off)].plays;
    }

    // The untimed down: time ran out on a down with an accepted live-ball
    // foul (not an offensive foul with loss of down).
    if (expired && accepted && g_.second_or_fourth() && result.kind == Next::Kind::Continue &&
        !(accepted->by_offense && rules_.penalty(accepted->type).loss_of_down)) {
        g_.untimed = true;
    }

    // The new state.
    SnapResult out;
    const bool possession_change = result.kind == Next::Kind::ChangeOfPossession;
    switch (result.kind) {
        case Next::Kind::Continue:
            g_.s = result.next;
            if (!result.keep_spot) g_.hash = hash_from(r.spot_x);
            break;
        case Next::Kind::ChangeOfPossession:
            g_.offense = def;
            g_.s = result.next;
            g_.hash = result.turnover_on_downs && result.keep_spot ? mirrored(g_.hash) : mirrored(hash_from(r.spot_x));
            if (result.next.yard_line == rules_.touchback && (r.interception || r.fumble_lost || r.outcome == "fumble_out_of_bounds")) {
                g_.hash = Hash::Middle;
            }
            out.end = SnapEnd::Turnover;
            break;
        case Next::Kind::Touchdown:
            g_.score[idx(off)] += 6;
            out = {SnapEnd::Touchdown, off};
            break;
        case Next::Kind::DefenseTouchdown:
            g_.score[idx(def)] += 6;
            out = {SnapEnd::Touchdown, def};
            break;
        case Next::Kind::Safety:
            g_.score[idx(def)] += 2;
            out = {SnapEnd::Safety, def};
            break;
    }
    const bool score = out.end == SnapEnd::Touchdown || out.end == SnapEnd::Safety;

    // The clock for the next snap.
    ClockEvent ce;
    ce.incomplete = r.outcome == "incomplete";
    ce.out_of_bounds = r.outcome == "out_of_bounds" || r.outcome == "fumble_out_of_bounds";
    ce.first_down = result.first_down;
    ce.change_of_possession = possession_change;
    ce.score = score;
    ce.penalty = penalty;
    ce.injury = injury;
    ce.late = g_.late(rules_.two_minute) || (g_.second_or_fourth() && clock_before <= rules_.two_minute);
    g_.clock_state = clock_after(ce);
    if (timed && g_.second_or_fourth() && !g_.two_minute_done && clock_before > rules_.two_minute &&
        g_.clock <= rules_.two_minute && g_.clock > 0.0) {
        g_.two_minute_done = true;
        if (g_.clock_state.start == ClockStart::Running) g_.clock_state.start = ClockStart::OnReady;
        g_.clock_state.admin = true;
        event("two_minute_timeout");
    }

    // A 10-second runoff option: a foul stopped what would have been a
    // running clock in the last minute.
    if (accepted && timed && !expired && g_.second_or_fourth() && g_.clock <= rules_.last_minute &&
        clock_after({.incomplete = ce.incomplete, .out_of_bounds = ce.out_of_bounds, .first_down = ce.first_down,
                     .change_of_possession = ce.change_of_possession, .score = ce.score, .late = ce.late})
                .start == ClockStart::Running) {
        const int offended = accepted->by_offense ? def : off;
        const Json c = ask("runoff", offended, [&] { return Json{{"take", coach(offended).runoff(g_)}}; });
        if (c.value("take", false)) {
            g_.clock = std::max(0.0, g_.clock - rules_.runoff);
            event("runoff", {{"seconds", rules_.runoff}});
        }
    }

    Json pl = {{"type", "play"},
               {"tick", g_.tick},
               {"quarter", g_.quarter},
               {"clock", clock_text(g_.clock)},
               {"offense", team_key(off)},
               {"situation", before_text},
               {"call", call.play}};
    if (call.flip) pl["flip"] = true;
    if (!defense_play.empty()) pl["defense_call"] = defense_play;
    pl["outcome"] = r.outcome;
    pl["yards"] = r2(r.yards);
    pl["summary"] = r.summary;
    if (!fouls.empty()) pl["fouls"] = fouls;
    if (was_untimed) pl["untimed"] = true;
    if (score) {
        pl["home"] = g_.score[0];
        pl["away"] = g_.score[1];
    } else {
        pl["next"] = (possession_change ? std::string(team_key(g_.offense)) + " ball, " : std::string()) + situation_text();
    }
    line(pl);
    return out;
}

std::vector<Json> parse_log(std::istream& in, Json& header, std::vector<std::string>& lines) {
    std::vector<Json> inputs;
    std::string text;
    while (std::getline(in, text)) {
        if (text.empty()) continue;
        lines.push_back(text);
        Json j = Json::parse(text);
        const std::string type = j.value("type", "");
        if (type == "header") header = j;
        if (type == "input") inputs.push_back(std::move(j));
    }
    return inputs;
}

}  // namespace

GameOptions game_options(const GameSetup& setup, std::uint64_t fallback_seed) {
    GameOptions o;
    o.home = setup.home;
    o.away = setup.away;
    o.seed = setup.seed.value_or(fallback_seed);
    o.crowd = setup.crowd;
    o.neutral = setup.neutral_site;
    return o;
}

GameResult play_game(const DataSet& data, const GameOptions& options, std::ostream* log) {
    Game game(data, options, log, nullptr);
    return game.run();
}

ReplayReport replay_game(const DataSet& data, std::istream& log,
                         std::function<std::unique_ptr<std::ostream>(const std::string& name)> tracking) {
    ReplayReport report;
    Json header;
    std::vector<std::string> original;
    std::vector<Json> inputs;
    try {
        inputs = parse_log(log, header, original);
    } catch (const std::exception& e) {
        report.error = std::string("can't read the log: ") + e.what();
        return report;
    }
    if (header.is_null()) {
        report.error = "the log has no header line";
        return report;
    }
    GameOptions o;
    o.home = header.value("home", "");
    o.away = header.value("away", "");
    o.seed = header.value("seed", std::uint64_t{0});
    o.crowd = header.value("crowd", 0.5);
    o.neutral = header.value("neutral", false);
    o.tracking = std::move(tracking);

    std::ostringstream out;
    try {
        Game game(data, o, &out, &inputs);
        report.result = game.run();
    } catch (const std::exception& e) {
        report.error = e.what();
    }
    std::vector<std::string> regenerated;
    std::istringstream again(out.str());
    for (std::string text; std::getline(again, text);) regenerated.push_back(text);
    const std::size_t n = std::max(original.size(), regenerated.size());
    for (std::size_t k = 0; k < n; ++k) {
        const std::string a = k < original.size() ? original[k] : "(end of log)";
        const std::string b = k < regenerated.size() ? regenerated[k] : "(end of replay)";
        if (a != b) {
            report.line = k + 1;
            report.expected = a;
            report.got = b;
            report.lines = k;
            return report;
        }
    }
    report.lines = n;
    report.ok = report.error.empty();
    return report;
}

}  // namespace cfbsim
