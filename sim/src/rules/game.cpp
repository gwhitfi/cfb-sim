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

#include "cfbsim/play/interlude.h"
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

// The game log's format. 2: timed between plays (docs/design/game-flow.md):
// the calls, the check, and the hike each at their own tick; kicks are calls.
// 3: between plays decided tick by tick (live): the hike is logged when it's
// given, audibles and hot routes on their own, timeouts any time.
constexpr int kLogFormat = 3;

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
        if (!opt_.observe) return;
        Json state = state_json();
        // After each play, try, and at the end: the box score so far, for the viewer's stats panel.
        const std::string type = j.value("type", "");
        const std::string ev = j.value("event", "");
        if (type == "play" || type == "final" || ev == "extra_point" || ev == "two_point_try") {
            BoxScore box = box_;
            box.teams[0].points = g_.score[0];
            box.teams[1].points = g_.score[1];
            state["box"] = box_score_json(box);
        }
        opt_.observe(j, state);
    }
    void event(const std::string& name, Json fields = Json::object()) {
        Json j = {{"type", "event"}, {"tick", g_.tick}, {"event", name}};
        for (auto& [k, v] : fields.items()) j[k] = v;
        line(j);
    }
    // `options`: what an outside decider may answer (built only when there is one).
    using Options = std::function<Json()>;
    Json ask(const std::string& input, int team, const std::function<Json()>& decide, const Options& options = {});
    // `live`: a decision between plays, asked every tick it's open (`hint`:
    // what the coach would do, for a side coached live).
    std::optional<Json> ask_optional(const std::string& input, int team,
                                     const std::function<std::optional<Json>()>& decide,
                                     const Options& options = {}, bool live = false, const Json& hint = Json());
    // A decision made for a side (the coach's call at a deadline): logged
    // like any input, never asked.
    Json ask_forced(const std::string& input, int team, const Json& answer);
    // The outside decider's answer, checked (nullopt: keep the coach's).
    std::optional<Json> outside(const std::string& input, int team, bool optional, const Json& suggestion,
                                const Options& options, bool live = false, const Json& hint = Json());

    // --- for an outside decider (phase 11) ----------------------------------
    Json state_json() const;
    // `kicks`: kick plays the offense may call (punt, field_goal, extra_point);
    // `offense_play`: for the defense, the offense's call (its personnel shows).
    Json call_options(int team, bool offense, bool try_play, const std::vector<std::string>& kicks = {},
                      const Play* offense_play = nullptr) const;
    Json play_json(const Play& p, bool offense) const;
    // The defense at the line: the plays it can check to from `called`.
    Json check_options(int team, const std::string& called, const Formation& offense_formation) const;
    // `earliest`: the highest play clock reading the offense can snap at.
    // `now`: the reading as it's asked (live, every tick at the line).
    Json hike_options(int team, const PlayCall& call, bool kick, double play_clock, double earliest, double now) const;
    static Json choices(const std::string& decision, std::initializer_list<const char*> c) {
        Json j = {{"decision", decision}, {"choices", Json::array()}};
        for (const char* x : c) j["choices"].push_back(x);
        return j;
    }
    void charge_timeout(int team);

    // --- the flow ----------------------------------------------------------
    void coin_toss();
    SnapResult kickoff(int kicking, bool free_kick);
    void end_quarter();
    void overtime();
    void series(int team, int ot);
    SnapResult snap();
    void attempt_try(int team, int ot);
    // `saved`: the game state to go back to after the try.
    bool run_try_play(int team, const PlayCall& call, const GameState& saved);
    // Runs the play, writing a tracking file when the options ask for one
    // (and the between-plays file before it).
    PlayResult run_tracked(const PlayOptions& o, const std::string& label);
    // A snap's tracking file name: "007_q1_1432_home_<label>".
    std::string snap_name(int team, const std::string& label) const;

    // --- the between-plays animation (game-flow.md; visual only) -----------
    // Only with tracking; nothing in the game reads it. A file opens at the
    // start of a window (`window`: the play clock runs; else a kickoff or a
    // try, walked straight on) with who's on the field (`fresh`: start them
    // in the huddles), and closes at the snap with the play's own lineup.
    TeamSide side_of(int t) const { return t == 0 ? TeamSide::Home : TeamSide::Away; }
    void open_between(int number, const PlayFrame& frame, int offense, bool huddle, const std::vector<Spot>& on_field,
                      bool fresh, std::int64_t ball_tick, bool window);
    void finish_between(const std::vector<Spot>& exact, const std::string& last = "");
    // A kickoff or a try: everyone walks straight to `to`.
    void walk_on(const std::vector<Spot>& to, const PlayFrame& frame, int offense);
    // The players and spots for a kick (no seed drawn).
    KickOptions kick_options(int kicking, double yard_line, PlayType type, const std::string& kind, Hash hash);
    void ended(const std::vector<Spot>& end) {
        last_end_ = end;
        last_end_quarter_ = g_.quarter;
        fresh_ = false;
    }

    // --- one snap's pieces -------------------------------------------------
    // The players for a snap, picked as each call comes in, then the rest.
    void fill_offense(PlayOptions& o, const Play& play);
    void fill_defense(PlayOptions& o, const std::string& defense_play);
    // `draw_seed`: false for a look at the lineup only (the game RNG is left alone).
    void finish_options(PlayOptions& o, const PlayCall& call, bool draw_seed = true);
    // The offense's frame for this snap.
    PlayFrame frame(bool flip) const {
        return PlayFrame(std::clamp(g_.s.yard_line, 0.5, 99.5), g_.hash, (g_.offense == 0) == (g_.quarter % 2 == 1), flip);
    }
    // A team's special teams play of `type` (`kind`: deep, squib, onside; empty: the first).
    const Play* kick_play_of(const Team& t, PlayType type, const std::string& kind) const;
    PlayResult kneel_or_spike(const std::string& kind);
    // Between plays: the play clock starts (a segment: from the whistle, or a
    // timeout).
    void begin_segment(double play_clock);
    void recover_to(double used);
    bool between_plays(double used);  // the snap at `used`; false: the period ended before it
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
    std::string situation_text() const { return situation_text(g_.offense, g_.s); }
    std::string situation_text(int offense, const Situation& s) const;
    // What a play or a penalty leaves behind, for `offense` (the team that had the ball).
    std::string next_text(int offense, const Next& n) const;
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
    std::int64_t segment_start_ = 0;       // the tick the play clock started
    double segment_clock_ = 40.0;          // its length
    double segment_used_ = -1.0;           // s of it used so far (under 0: not between plays)
    double recovered_ = 0.0;               // s of it the lineups have recovered for
    // The between-plays animation's inputs (only used with tracking).
    std::vector<Spot> last_end_;           // where the last play left everyone
    int last_end_quarter_ = 0;             // (a new period starts everyone fresh)
    bool fresh_ = true;                    // start in the huddle (after a timeout, a new period)
    std::unique_ptr<Between> between_;     // the file being written
    std::int64_t between_start_ = 0;       // its tick 0 (game ticks)
    int between_snap_ = -1, between_count_ = 0;  // files for this snap number so far
};

// ---------------------------------------------------------------------------
// Inputs
// ---------------------------------------------------------------------------

std::optional<Json> Game::outside(const std::string& input, int team, bool optional, const Json& suggestion,
                                  const Options& options, bool live, const Json& hint) {
    if (!opt_.decide) return std::nullopt;
    Prompt p;
    p.input = input;
    p.team = team;
    p.optional = optional;
    p.live = live;
    p.hint = hint;
    p.suggestion = suggestion;
    p.options = options ? options() : Json::object();
    p.state = state_json();
    std::optional<Json> answer = opt_.decide(p);
    if (!answer) return std::nullopt;
    if (const std::optional<std::string> error = check_answer(p, *answer)) {
        throw GameError(input + " from " + team_key(team) + ": " + *error);
    }
    return answer;
}

Json Game::ask(const std::string& input, int team, const std::function<Json()>& decide, const Options& options) {
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
        Json answer = decide();
        if (std::optional<Json> o = outside(input, team, false, answer, options)) answer = std::move(*o);
        j = {{"type", "input"}, {"tick", g_.tick}, {"input", input}, {"team", team_key(team)}};
        for (auto& [k, v] : answer.items()) j[k] = v;
    }
    line(j);
    return j;
}

Json Game::ask_forced(const std::string& input, int team, const Json& answer) {
    Json j;
    if (replay_) {
        if (next_input_ >= replay_->size()) throw ReplayError("the log ran out of inputs (wanted " + input + ")");
        j = (*replay_)[next_input_++];
        if (j.value("input", "") != input || j.value("team", "") != team_key(team) ||
            j.value("tick", std::int64_t{-1}) != g_.tick) {
            throw ReplayError("input " + std::to_string(next_input_) + ": the game made " + input + " for " +
                              team_key(team) + " at tick " + std::to_string(g_.tick) + ", the log has " + j.dump());
        }
    } else {
        j = {{"type", "input"}, {"tick", g_.tick}, {"input", input}, {"team", team_key(team)}};
        for (auto& [k, v] : answer.items()) j[k] = v;
    }
    line(j);
    return j;
}

std::optional<Json> Game::ask_optional(const std::string& input, int team,
                                       const std::function<std::optional<Json>()>& decide,
                                       const Options& options, bool live, const Json& hint) {
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
        std::optional<Json> answer = decide();
        if (opt_.decide) {
            // An outside decider overrules the coach either way: its silence passes.
            answer = outside(input, team, true, answer ? *answer : Json(), options, live, hint);
        }
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

std::string Game::situation_text(int offense, const Situation& sit) const {
    static const char* kDown[] = {"1st", "2nd", "3rd", "4th"};
    std::ostringstream s;
    s << kDown[std::clamp(sit.down, 1, 4) - 1] << " & ";
    if (sit.line_to_gain() >= 100.0 - 1e-9) {
        s << "goal";
    } else {
        s << std::max(1, static_cast<int>(std::round(sit.distance)));
    }
    s << " at " << spot_text(offense, sit.yard_line);
    return s.str();
}

std::string Game::next_text(int offense, const Next& n) const {
    switch (n.kind) {
        case Next::Kind::Continue:
            return situation_text(offense, n.next);
        case Next::Kind::ChangeOfPossession:
            return team_of(1 - offense).abbr + " ball, " + situation_text(1 - offense, n.next);
        case Next::Kind::Touchdown:
            return "touchdown " + team_of(offense).abbr;
        case Next::Kind::DefenseTouchdown:
            return "touchdown " + team_of(1 - offense).abbr;
        case Next::Kind::Safety:
            return "safety";
    }
    return "";
}

// ---------------------------------------------------------------------------
// For an outside decider (phase 11)
// ---------------------------------------------------------------------------

Json Game::state_json() const {
    Json teams = Json::object();
    for (int t = 0; t < 2; ++t) {
        const Team& tm = team_of(t);
        teams[team_key(t)] = {{"id", tm.id}, {"name", tm.name}, {"abbr", tm.abbr},
                              {"color", tm.color_primary}, {"color2", tm.color_secondary}};
    }
    const bool goal = g_.s.line_to_gain() >= 100.0 - 1e-9;
    // Between plays the clocks run: the game clock as it reads now (it settles
    // at the snap; the two-minute timeout stops it at 2:00), and the play clock.
    double clock = g_.clock;
    if (segment_used_ >= 0.0 && !g_.overtime() && !g_.untimed) {
        clock = std::max(0.0, clock - runoff_before_snap(g_.clock_state, segment_used_, tuning_));
        if (g_.second_or_fourth() && !g_.two_minute_done && g_.clock > rules_.two_minute) {
            clock = std::max(clock, rules_.two_minute);
        }
    }
    Json j = {{"tick", g_.tick},
            {"quarter", g_.quarter},
            {"overtime", g_.overtime()},
            {"clock", r2(clock)},
            {"clock_text", clock_text(clock)},
            {"clock_running", g_.clock_state.start == ClockStart::Running},
            {"play_clock", g_.clock_state.admin ? rules_.admin_play_clock : rules_.play_clock},
            {"teams", teams},
            {"score", {{"home", g_.score[0]}, {"away", g_.score[1]}}},
            {"timeouts", {{"home", g_.timeouts[0]}, {"away", g_.timeouts[1]}}},
            {"offense", team_key(g_.offense)},
            {"down", g_.s.down},
            {"distance", r2(g_.s.distance)},
            {"goal_to_go", goal},
            {"yard_line", r2(g_.s.yard_line)},
            {"situation", situation_text()},
            {"hash", g_.hash == Hash::Left ? "left" : g_.hash == Hash::Right ? "right" : "middle"},
            // The offense moves toward +x in the field frame (the home team in odd quarters).
            {"offense_right", (g_.offense == 0) == (g_.quarter % 2 == 1)}};
    if (segment_used_ >= 0.0) j["play_clock_left"] = r2(segment_clock_ - segment_used_);
    return j;
}

Json Game::play_json(const Play& p, bool offense) const {
    const auto f = data_.formations.find(p.formation);
    Json j = {{"id", p.id}, {"name", p.name}, {"formation", p.formation},
              {"type", std::string(to_string(p.type))}, {"flip", p.flip}, {"tags", p.tags}};
    if (f != data_.formations.end()) {
        j["formation_name"] = f->second.name;
        j[offense ? "personnel" : "package"] = offense ? f->second.personnel : f->second.package;
    }
    return j;
}

Json Game::call_options(int team, bool offense, bool try_play, const std::vector<std::string>& kicks,
                        const Play* offense_play) const {
    const Team& t = team_of(team);
    const auto book = data_.playbooks.find(offense ? t.offense_playbook : t.defense_playbook);
    Json plays = Json::array();
    if (book != data_.playbooks.end()) {
        for (const Play& p : book->second.plays) {
            if (offense && p.type != PlayType::Pass && p.type != PlayType::Run) continue;
            if (!offense && p.type != PlayType::Defense) continue;
            plays.push_back(play_json(p, offense));
        }
    }
    Json j = {{"side", offense ? "offense" : "defense"}, {"plays", plays}};
    if (offense && !try_play) j["clock_plays"] = {"kneel", "spike"};
    if (offense && !kicks.empty()) j["kick_plays"] = kicks;
    if (try_play) j["try"] = true;
    if (offense_play) {
        if (const auto f = data_.formations.find(offense_play->formation); f != data_.formations.end()) {
            j["offense_personnel"] = f->second.personnel;
        }
    }
    return j;
}

Json Game::check_options(int team, const std::string& called, const Formation& offense_formation) const {
    const Team& t = team_of(team);
    const Playbook& book = data_.playbooks.at(t.defense_playbook);
    const auto package = [&](const Play& p) {
        const auto f = data_.formations.find(p.formation);
        return f == data_.formations.end() ? std::string() : f->second.package;
    };
    const Play* cur = book.find(called);
    Json plays = Json::array();
    for (const Play& p : book.plays) {
        if (p.type == PlayType::Defense && cur && package(p) == package(*cur)) plays.push_back(play_json(p, false));
    }
    return {{"side", "defense"}, {"play", called}, {"formation", offense_formation.id},
            {"formation_name", offense_formation.name}, {"plays", plays}};
}

Json Game::hike_options(int team, const PlayCall& call, bool kick, double play_clock, double earliest, double now) const {
    // The earliest the offense can snap is `set`; `ready` has a reading for
    // each tempo (the ranges are for a 40-second clock), none before set.
    const auto ready = [&](const std::pair<double, double>& range) {
        return std::min(earliest, r2(play_clock - (rules_.play_clock - range.second) * play_clock / rules_.play_clock));
    };
    Json j = {{"play_clock", play_clock},
              {"set", earliest},
              {"now", r2(now)},
              {"ready", {{"huddle", ready(tuning_.tempo_normal)},
                         {"no_huddle", ready(tuning_.tempo_no_huddle)},
                         {"hurry", ready(tuning_.tempo_hurry)}}},
              {"timeouts", g_.timeouts[idx(team)]},
              {"play", kick ? std::string() : call.play}};
    Json audibles = Json::array(), slots = Json::array(), routes = Json::array();
    if (const Play* called = kick ? nullptr : offense_play(team, call.play)) {
        for (const Play& p : data_.playbooks.at(team_of(team).offense_playbook).plays) {
            if (&p != called && p.formation == called->formation && (p.type == PlayType::Pass || p.type == PlayType::Run)) {
                audibles.push_back(p.id);
            }
        }
        if (called->type == PlayType::Pass) {
            for (const auto& [slot, a] : called->assignments) {
                if (std::any_of(a.sequence.begin(), a.sequence.end(), [](const Job& x) { return x.kind == JobKind::Route; })) {
                    slots.push_back(slot);
                }
            }
            for (const auto& [id, r] : data_.routes) routes.push_back(id);
        }
    }
    j["audibles"] = audibles;
    j["hot_slots"] = slots;
    j["routes"] = routes;
    return j;
}

void Game::charge_timeout(int team) {
    --g_.timeouts[idx(team)];
    ++box_.teams[idx(team)].timeouts_used;
    g_.clock_state = {ClockStart::OnSnap, true};
    event("timeout", {{"team", team_key(team)}, {"left", g_.timeouts[idx(team)]}, {"clock", clock_text(g_.clock)}});
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
    const Json c = ask(
        "coaching_decision", toss_winner_, [] { return Json{{"decision", "coin_toss"}, {"choice", "defer"}}; },
        [] { return choices("coin_toss", {"receive", "defer"}); });
    const bool defer = c.value("choice", "defer") == "defer";
    first_receiver_ = defer ? 1 - toss_winner_ : toss_winner_;
    event("coin_toss", {{"winner", team_key(toss_winner_)}, {"receives", team_key(first_receiver_)}});
}

SnapResult Game::kickoff(int kicking, bool free_kick) {
    // (rules-and-flow.md#kicks-verify-stubbed-in-phase-8) The kicking team
    // chooses deep, squib, or onside; the receivers answer with their return.
    const Json c = ask(
        "coaching_decision", kicking,
        [&] { return Json{{"decision", "kickoff"}, {"choice", coach(kicking).kickoff_kind(g_)}}; },
        [] { return choices("kickoff", {"deep", "squib", "onside"}); });
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

const Play* Game::kick_play_of(const Team& t, PlayType type, const std::string& kind) const {
    const auto book = data_.playbooks.find(t.special_playbook);
    if (book == data_.playbooks.end()) throw GameError(t.id + " has no special teams playbook");
    const Play* first = nullptr;
    for (const Play& p : book->second.plays) {
        if (p.type != type) continue;
        if (!first) first = &p;
        if (!kind.empty() && p.kick && p.kick->kind == kind) return &p;
    }
    if (!first) throw GameError(t.id + " has no " + std::string(to_string(type)) + " play");
    return first;
}

KickOptions Game::kick_options(int kicking, double yard_line, PlayType type, const std::string& kind, Hash hash) {
    const int receiving = 1 - kicking;
    const Team& kt = team_of(kicking);
    const Team& rt = team_of(receiving);
    const PlayType answer = type == PlayType::Kickoff ? PlayType::KickoffReturn
                            : type == PlayType::Punt  ? PlayType::PuntReturn
                                                      : PlayType::FieldGoalBlock;
    const Play* kp = kick_play_of(kt, type, kind);
    const Play* rp = kick_play_of(rt, answer, "");
    KickOptions o;
    o.kicking_team = kt.id;
    o.receiving_team = rt.id;
    o.kick_play = kp->id;
    o.return_play = rp->id;
    o.yard_line = std::clamp(yard_line, 0.5, 99.5);
    o.hash = hash;
    o.kicking_right = (kicking == 0) == (g_.quarter % 2 == 1);
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
    return o;
}

KickResult Game::kick_play(int kicking, double yard_line, PlayType type, const std::string& kind, Hash hash,
                           bool timed) {
    const int receiving = 1 - kicking;
    KickOptions o = kick_options(kicking, yard_line, type, kind, hash);
    o.seed = rng_.next_u64();
    const Play* kp = kick_play_of(team_of(kicking), type, kind);

    ++snaps_;
    KickResult k;
    try {
        if (opt_.tracking) {
            const std::vector<Spot> spots = kick_start(data_, o);
            if (between_) {
                finish_between(spots);
            } else {
                walk_on(spots, PlayFrame(o.yard_line, o.hash, o.kicking_right, false), kicking);
            }
            if (const std::unique_ptr<std::ostream> out = opt_.tracking(snap_name(kicking, kp->id))) {
                TrackingWriter writer(*out);
                k = run_kick(data_, o, &writer);
            } else {
                k = run_kick(data_, o);
            }
            ended(k.end);
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
          {"log_format", kLogFormat},
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
    // After a touchdown (or, from the 3rd overtime, on its own): the call is
    // the try, the extra point or a two-point play from the 3 (only two-point
    // plays from the 2nd overtime).
    const bool kick_allowed = ot < 2;
    const GameState saved = g_;
    g_.offense = team;
    g_.s = first_down_at(100.0 - rules_.try_line, rules_);
    g_.hash = Hash::Middle;
    const Json call = ask("call_play", team, [&] {
        if (kick_allowed) {
            GameState view = saved;
            view.offense = team;
            if (!coach(team).go_for_two(view)) return Json{{"play", "extra_point"}, {"flip", false}, {"try", true}};
        }
        const PlayCall c = coach(team).offense_call(g_, true);
        return Json{{"play", c.play}, {"flip", c.flip}, {"try", true}};
    }, [&] {
        return call_options(team, true, true, kick_allowed ? std::vector<std::string>{"extra_point"} : std::vector<std::string>{});
    });
    const std::string play = call.value("play", "");
    if (play != "extra_point") {
        const bool good = run_try_play(team, {play, call.value("flip", false)}, saved);
        event("two_point_try", {{"team", team_key(team)}, {"good", good}, {"home", g_.score[0]}, {"away", g_.score[1]}});
        return;
    }
    g_ = saved;
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
}

bool Game::run_try_play(int team, const PlayCall& call, const GameState& saved) {
    const Play* oplay = offense_play(team, call.play);
    if (!oplay) throw GameError(team_of(team).id + " called an unknown play '" + call.play + "'");
    PlayOptions o;
    fill_offense(o, *oplay);
    const Json dcall = ask("call_play", 1 - team, [&] {
        return Json{{"play", coach(1 - team).defense_call(g_, oplay)}};
    }, [&] { return call_options(1 - team, false, true, {}, oplay); });
    fill_defense(o, dcall.value("play", ""));
    finish_options(o, call);
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

std::string Game::snap_name(int team, const std::string& label) const {
    std::string clock = clock_text(g_.clock);
    std::erase(clock, ':');
    char num[8];
    std::snprintf(num, sizeof num, "%03d", snaps_);
    return std::string(num) + "_q" + std::to_string(g_.quarter) + "_" + clock + "_" + team_key(team) + "_" + label;
}

PlayResult Game::run_tracked(const PlayOptions& o, const std::string& label) {
    if (!opt_.tracking) return run_play(data_, o);
    const std::vector<Spot> spots = play_start(data_, o);
    if (between_) {
        finish_between(spots);
    } else {
        walk_on(spots, PlayFrame(o.yard_line, o.hash, o.offense_right, false), g_.offense);
    }
    PlayResult r;
    if (const std::unique_ptr<std::ostream> out = opt_.tracking(snap_name(g_.offense, label))) {
        TrackingWriter writer(*out);
        r = run_play(data_, o, &writer);
    } else {
        r = run_play(data_, o);
    }
    ended(r.end);
    return r;
}

void Game::open_between(int number, const PlayFrame& frame, int offense, bool huddle,
                        const std::vector<Spot>& on_field, bool fresh, std::int64_t ball_tick, bool window) {
    between_.reset();
    if (!opt_.tracking) return;
    if (number != between_snap_) {
        between_snap_ = number;
        between_count_ = 0;
    }
    char num[24];
    std::snprintf(num, sizeof num, "%03d_%d", number, between_count_++);
    std::string clock = clock_text(g_.clock);
    std::erase(clock, ':');
    // "007_0_q1_1432_between" sorts just before the snap's own "007_q1_...".
    std::unique_ptr<std::ostream> out =
        opt_.tracking(std::string(num) + "_q" + std::to_string(g_.quarter) + "_" + clock + "_between");
    if (!out) return;
    TrackingInfo info;
    info.kind = "interlude";
    info.name = "between plays";
    for (int t = 0; t < 2; ++t) {
        const Team& tm = team_of(t);
        (t == 0 ? info.home : info.away) = {tm.name, tm.abbr, tm.color_primary};
    }
    info.details = {{"situation", situation_text()}, {"clock", clock_text(g_.clock)}, {"start_tick", g_.tick}};
    if (window) info.details["play_clock"] = segment_clock_;
    between_ = std::make_unique<Between>(std::move(out), info, frame, side_of(offense), huddle, data_.tuning.movement,
                                         data_.tuning.play);
    between_start_ = g_.tick;
    std::vector<BetweenMan> men;
    for (const Spot& s : on_field) {
        men.push_back({&lineup(s.team == TeamSide::Home ? 0 : 1).player(s.id), s.team, s.label,
                       fresh ? std::nullopt : std::optional<Spot>(s)});
    }
    between_->start(men, ball_tick);
}

void Game::finish_between(const std::vector<Spot>& exact, const std::string& last) {
    if (!between_) return;
    between_->finish(g_.tick - between_start_, exact, last);
    between_.reset();
}

void Game::walk_on(const std::vector<Spot>& to, const PlayFrame& frame, int offense) {
    open_between(snaps_, frame, offense, false, last_end_, fresh_ || last_end_quarter_ != g_.quarter, 0, false);
    if (!between_) return;
    for (const TeamSide side : {TeamSide::Home, TeamSide::Away}) {
        std::vector<BetweenMan> men;
        std::vector<Spot> spots;
        for (const Spot& s : to) {
            if (s.team != side) continue;
            men.push_back({&lineup(side == TeamSide::Home ? 0 : 1).player(s.id), s.team, s.label, std::nullopt});
            spots.push_back(s);
        }
        between_->lineup(0, side, men, spots, std::nullopt, std::nullopt);
    }
    // No game time passes (the clock is stopped): as long as the walk takes, up to 10 s.
    const std::int64_t end = std::min<std::int64_t>(between_->arrived(), 10 * kTicksPerSecond) +
                             ticks(data_.tuning.play.set_seconds);
    between_->finish(end, to);
    between_.reset();
}

void Game::fill_offense(PlayOptions& o, const Play& play) {
    const int off = g_.offense;
    const Team& ot = team_of(off);
    const Formation& of = data_.formations.at(play.formation);
    std::vector<std::string> slots;
    for (const FormationSlot& sl : of.slots) slots.push_back(sl.name);
    const auto personnel = ot.depth_chart.offense_personnel.find(of.personnel);
    if (personnel == ot.depth_chart.offense_personnel.end()) {
        throw GameError(ot.id + " has no '" + of.personnel + "' personnel");
    }
    o.offense_players = lineup(off).fill(personnel->second, slots);
}

void Game::fill_defense(PlayOptions& o, const std::string& defense_play) {
    const int def = g_.defense();
    const Team& dt = team_of(def);
    const Playbook& dbook = data_.playbooks.at(dt.defense_playbook);
    const Play* dplay = dbook.find(defense_play);
    if (!dplay) throw GameError(dt.id + " called an unknown play '" + defense_play + "'");
    const Formation& df = data_.formations.at(dplay->formation);
    std::vector<std::string> slots;
    for (const FormationSlot& sl : df.slots) slots.push_back(sl.name);
    const auto package = dt.depth_chart.defense_packages.find(df.package);
    if (package == dt.depth_chart.defense_packages.end()) {
        throw GameError(dt.id + " has no '" + df.package + "' package");
    }
    o.defense = dt.id;
    o.defense_play = defense_play;
    o.defense_players = lineup(def).fill(package->second, slots);
}

void Game::finish_options(PlayOptions& o, const PlayCall& call, bool draw_seed) {
    const int off = g_.offense, def = g_.defense();
    o.team = team_of(off).id;
    o.play = call.play;
    o.flip = call.flip;
    o.hash = g_.hash;
    o.yard_line = std::clamp(g_.s.yard_line, 0.5, 99.5);
    // The home team attacks +x in odd quarters.
    o.offense_right = (off == 0) == (g_.quarter % 2 == 1);
    if (draw_seed) o.seed = rng_.next_u64();
    o.huddle = false;
    o.offense_home = off == 0;
    o.modifiers.clear();
    for (const auto& [slot, id] : o.offense_players) o.modifiers[id] = lineup(off).modifiers(id);
    for (const auto& [slot, id] : o.defense_players) o.modifiers[id] = lineup(def).modifiers(id);
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

void Game::begin_segment(double play_clock) {
    segment_start_ = g_.tick;
    segment_clock_ = play_clock;
    segment_used_ = 0.0;
    recovered_ = 0.0;
}

void Game::recover_to(double used) {
    if (used <= recovered_) return;
    lineup(0).recover(used - recovered_, last_on_field_);
    lineup(1).recover(used - recovered_, last_on_field_);
    recovered_ = used;
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
    g_.tick = segment_start_ + ticks(used);
    const double from = g_.clock;
    g_.clock = std::max(0.0, g_.clock - run);
    box_.teams[idx(g_.offense)].possession_seconds += from - g_.clock;
    recover_to(used);
    segment_used_ = -1.0;
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
        const Json c = ask("runoff", offended, [&] { return Json{{"take", coach(offended).runoff(g_)}}; },
                           [&] { return Json{{"seconds", rules_.runoff}}; });
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
        if (r.outcome == "sack") {
            ++to.sacks_taken;
            to.sack_yds += gained;
        } else if (r.scramble) {
            ++to.scrambles;
            to.scramble_yds += gained;
        } else {
            ++to.designed_runs;
            to.designed_run_yds += gained;
            to.runs_lost += gained <= 0.0 ? 1 : 0;
            to.runs_4 += gained >= 4.0 ? 1 : 0;
            to.runs_10 += gained >= 10.0 ? 1 : 0;
            to.runs_20 += gained >= 20.0 ? 1 : 0;
        }
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
    // Between plays (docs/design/game-flow.md): a window on the play clock in
    // which each side's decisions come tick by tick. The offense calls (on 4th
    // down a punt or a field goal may be the call), the defense once the
    // offense's personnel is on, and at the line the defense may check, the
    // offense audible or change routes, and snap; either may call timeout.
    // The coach acts at its moments on the schedule; a side coached live acts
    // when its user does; a replay takes each from the log at its tick. Each
    // side's players are picked at its call.
    const int off = g_.offense, def = g_.defense();
    const Tempo tempo = coach(off).tempo(g_);
    const bool huddle = tempo == Tempo::Huddle;
    const double set_seconds = data_.tuning.play.set_seconds;
    const double kick_distance = 100.0 - g_.s.yard_line + 17.0;
    const Specialists sp = specialists(off);
    const bool last_chance =
        g_.second_or_fourth() && g_.clock <= 8.0 && kick_distance <= coach(off).fg_range(sp);
    std::vector<std::string> kicks;
    if (g_.s.down == 4) {
        kicks = {"punt", "field_goal"};
    } else if (last_chance) {
        kicks = {"field_goal"};
    }
    const auto live = [&](int team) { return opt_.live && opt_.live(team); };
    // However the window ends, the caller hears it closed.
    struct Closed {
        const GameOptions& opt;
        ~Closed() {
            if (opt.window) opt.window(-1);
        }
    } closed{opt_};
    const auto rounded_down = [](double x) { return std::floor(x * 100.0) / 100.0; };

    double play_clock = g_.clock_state.admin ? rules_.admin_play_clock : rules_.play_clock;
    begin_segment(play_clock);
    BetweenPlays when = between_plays_schedule(play_clock, rules_.play_clock, tempo, 0.0, set_seconds, false, tuning_);
    double huddle_left = when.broke - when.call;
    const auto tick_of = [&](double u) { return segment_start_ + ticks(u); };

    // What's been decided.
    bool have_call = false;
    PlayCall call;
    KickChoice kick = KickChoice::Go;
    bool kicking = false, clock_play = false;
    const Play* oplay = nullptr;
    const Formation* formation = nullptr;
    double walk = 0.0;
    PlayOptions o;
    std::string defense_play, called_play, called_defense;
    std::map<std::string, std::string> hot_routes;
    // The coach's answers, worked out as each decision opens: an auto side
    // acts on them at its moment, a side coached live sees them as the hint.
    std::optional<Json> coach_call, coach_defense, coach_check;
    double coach_reading = 0.0;
    std::optional<std::int64_t> set_tick;  // when the offense got set
    std::int64_t hike_tick = 0;           // the coach's snap (or a user's "snap at :08")
    bool user_hike = false;
    bool coach_timeouts = true;           // the coach takes his at the start of the window
    bool snapped = false;
    double reading = 0.0;

    // The animation (tracking only).
    open_between(snaps_ + 1, frame(false), off, huddle, last_end_, fresh_ || last_end_quarter_ != g_.quarter,
                 3 * kTicksPerSecond, true);
    const auto lined_up = [&](bool with_defense) {
        PlayOptions look = o;
        finish_options(look, call, false);
        if (!with_defense) look.defense.clear();
        return play_start(data_, look);
    };
    const auto side_men = [&](const std::map<std::string, std::string>& players, int team) {
        std::vector<BetweenMan> men;
        for (const auto& [slot, id] : players) men.push_back({&lineup(team).player(id), side_of(team), slot, std::nullopt});
        return men;
    };
    const auto spots_of_team = [&](const std::vector<Spot>& spots, int team) {
        std::vector<Spot> out;
        for (const Spot& s : spots) {
            if (s.team == side_of(team)) out.push_back(s);
        }
        return out;
    };
    // The animation for what's known: each side walks to its huddle, or to its spots by the set.
    const auto animate_lineups = [&] {
        if (!between_) return;
        const std::int64_t now = g_.tick - between_start_;
        const std::int64_t brk = tick_of(when.broke) - between_start_;
        const std::int64_t set = tick_of(when.set) - between_start_;
        const std::optional<std::int64_t> wait = huddle ? std::optional<std::int64_t>(brk) : std::nullopt;
        if (kicking) {
            const KickOptions k = kick_options(off, g_.s.yard_line, kick == KickChoice::Punt ? PlayType::Punt
                                                                                             : PlayType::FieldGoal, "", g_.hash);
            const std::vector<Spot> spots = kick_start(data_, k);
            between_->lineup(now, side_of(off), side_men(k.kicking_players, off), spots_of_team(spots, off), wait, set);
            between_->lineup(now, side_of(def), side_men(k.receiving_players, def), spots_of_team(spots, def), wait, set);
        } else if (oplay) {
            const std::vector<Spot> spots = lined_up(!defense_play.empty());
            between_->lineup(now, side_of(off), side_men(o.offense_players, off), spots_of_team(spots, off), wait, set);
            if (!defense_play.empty()) {
                between_->lineup(now, side_of(def), side_men(o.defense_players, def), spots_of_team(spots, def), wait, set);
            }
        }
    };
    // A timeout: the time used runs off, the clock stops, and the window
    // starts again on a 25-second clock (the offense lines up again if it had called).
    const auto timeout = [&](int team) -> bool {
        if (!between_plays(segment_used_)) return false;
        finish_between({}, "timeout");
        charge_timeout(team);
        play_clock = rules_.admin_play_clock;
        begin_segment(play_clock);
        when = between_plays_schedule(play_clock, rules_.play_clock, tempo, walk, set_seconds, have_call, tuning_);
        huddle_left = when.broke - when.call;
        if (have_call) when.call = when.defense_call = 0.0;
        set_tick.reset();
        user_hike = false;
        coach_timeouts = false;
        // Out of the timeout, everyone starts in the huddles.
        std::vector<Spot> on = last_end_;
        if (oplay) {
            on.clear();
            for (const auto& [slot, id] : o.offense_players) on.push_back({id, slot, side_of(off), {}, {}});
            for (const auto& [slot, id] : o.defense_players) on.push_back({id, slot, side_of(def), {}, {}});
        }
        open_between(snaps_ + 1, frame(false), off, huddle, on, true, 0, true);
        animate_lineups();
        return true;
    };

    for (std::int64_t t = segment_start_;;) {
        g_.tick = t;
        segment_used_ = static_cast<double>(t - segment_start_) * kTickSeconds;
        const double used = segment_used_;
        if (between_) between_->run_to(t - between_start_ - 1);

        // Timeouts (the defense's first).
        bool timed_out = false;
        for (const int team : {def, off}) {
            if (timed_out || g_.timeouts[idx(team)] <= 0) continue;
            const bool coach_now = coach_timeouts && t == segment_start_;
            if (!live(team) && !coach_now && !replay_) continue;
            const std::optional<Json> to = ask_optional("timeout", team, [&]() -> std::optional<Json> {
                if (coach_now && coach(team).timeout(g_, team == off)) return Json::object();
                return std::nullopt;
            }, [&] { return Json{{"left", g_.timeouts[idx(team)]}}; }, true);
            if (!to) continue;
            if (!timeout(team)) return {SnapEnd::PeriodOver, -1};
            timed_out = true;
        }
        coach_timeouts = false;
        if (timed_out) continue;  // the window starts over at this tick

        // The offense's call.
        if (!have_call) {
            if (!coach_call) {
                coach_call = [&] {
                    if (!kicks.empty()) {
                        const KickChoice k = coach(off).fourth_down(g_, sp);
                        if (k == KickChoice::Punt && g_.s.down == 4) return Json{{"play", "punt"}, {"flip", false}};
                        if (k == KickChoice::FieldGoal) return Json{{"play", "field_goal"}, {"flip", false}};
                    }
                    const PlayCall c = coach(off).offense_call(g_, false);
                    return Json{{"play", c.play}, {"flip", c.flip}};
                }();
            }
            const bool due = t >= tick_of(when.call);
            std::optional<Json> c;
            if (live(off) || due || replay_) {
                c = ask_optional("call_play", off, [&]() -> std::optional<Json> {
                    if (due) return *coach_call;
                    return std::nullopt;
                }, [&] { return call_options(off, true, false, kicks); }, true, *coach_call);
            }
            if (c) {
                have_call = true;
                call = {c->value("play", ""), c->value("flip", false)};
                called_play = call.play;
                kick = call.play == "punt" ? KickChoice::Punt : call.play == "field_goal" ? KickChoice::FieldGoal : KickChoice::Go;
                kicking = kick != KickChoice::Go;
                clock_play = call.play == "kneel" || call.play == "spike";
                oplay = kicking || clock_play ? nullptr : offense_play(off, call.play);
                if (!kicking && !clock_play && !oplay) {
                    throw GameError(team_of(off).id + " called an unknown play '" + call.play + "'");
                }
                recover_to(used);
                if (oplay) fill_offense(o, *oplay);
                formation = oplay    ? &data_.formations.at(oplay->formation)
                            : kicking ? &data_.formations.at(
                                            kick_play_of(team_of(off), kick == KickChoice::Punt ? PlayType::Punt
                                                                                                : PlayType::FieldGoal, "")
                                                ->formation)
                                      : nullptr;
                if (formation && huddle) {
                    try {
                        walk = walk_up_seconds(*formation, frame(call.flip), data_.tuning.play);
                    } catch (const GeometryError& e) {
                        throw GameError("formation " + formation->id + ": " + e.what());
                    }
                }
                // The rest of the window runs from the call.
                when.call = used;
                when.defense_call = used + tuning_.defense_call_after;
                when.broke = used + huddle_left;
                when.set = std::max(when.defense_call, when.broke + (huddle ? walk : tuning_.line_up));
                when.earliest = when.set + set_seconds;
                if (between_) between_->event(t - between_start_, "play_called");
                animate_lineups();
            }
        }

        // The defense's call, once the offense's personnel is on; at the set
        // at the latest (the coach's, if its user hasn't called).
        if (have_call && oplay && defense_play.empty() && t >= tick_of(when.defense_call)) {
            if (!coach_defense) coach_defense = Json{{"play", coach(def).defense_call(g_, oplay)}};
            const bool due = !live(def);
            std::optional<Json> d;
            if (live(def) || due || replay_) {
                d = ask_optional("call_play", def, [&]() -> std::optional<Json> {
                    if (due) return *coach_defense;
                    return std::nullopt;
                }, [&] { return call_options(def, false, false, {}, oplay); }, true, *coach_defense);
            }
            if (!d && t >= tick_of(when.set)) d = ask_forced("call_play", def, *coach_defense);
            if (d) {
                defense_play = d->value("play", "");
                called_defense = defense_play;
                recover_to(used);
                fill_defense(o, defense_play);
                if (between_) between_->event(t - between_start_, "defense_called");
                animate_lineups();
            }
        }

        // At the line.
        const bool lined = have_call && t >= tick_of(when.set) && (!oplay || !defense_play.empty());
        const double earliest = rounded_down(play_clock - when.earliest);
        if (lined && !set_tick) {
            set_tick = t;
            if (between_) between_->event(t - between_start_, "set");
            if (oplay) {
                const FormationLook look = look_of(*formation, frame(call.flip), data_.tuning.coach.check_box);
                coach_check = Json{{"play", coach(def).defense_check(g_, oplay, look, defense_play)}};
            }
            coach_reading = std::min(r2(coach(off).hike(g_, play_clock)), earliest);
            hike_tick = std::max(tick_of(play_clock - coach_reading), tick_of(when.earliest));
        }
        if (lined && oplay) {
            const bool due = !live(def) && t == *set_tick && coach_check->value("play", "") != defense_play;
            std::optional<Json> c;
            if (live(def) || due || replay_) {
                c = ask_optional("check", def, [&]() -> std::optional<Json> {
                    if (due) return *coach_check;
                    return std::nullopt;
                }, [&] { return check_options(def, defense_play, *formation); }, true, *coach_check);
            }
            if (c && c->value("play", defense_play) != defense_play) {
                defense_play = c->value("play", defense_play);
                fill_defense(o, defense_play);
                if (between_) {
                    between_->event(t - between_start_, "check");
                    between_->respot(t - between_start_, spots_of_team(lined_up(true), def), t - between_start_ + 30);
                }
            }
        }
        const double now = r2(play_clock - used);
        if (lined && user_hike && t >= hike_tick) {
            // A user's "snap at :08" (logged when he said it): now.
            snapped = true;
            reading = now;
        } else if (lined && t >= tick_of(when.earliest)) {
            const bool due = !live(off) && t >= hike_tick;
            std::optional<Json> h;
            if (live(off) || due || replay_) {
                h = ask_optional("hike", off, [&]() -> std::optional<Json> {
                    if (due) return Json{{"snap", true}, {"play_clock", now}};
                    return std::nullopt;
                }, [&] { return hike_options(off, call, kicking, play_clock, earliest, now); }, true,
                    Json{{"snap", true}, {"play_clock", coach_reading}});
            }
            if (h && h->value("timeout", false)) {
                if (!timeout(off)) return {SnapEnd::PeriodOver, -1};
                continue;
            }
            if (h && h->contains("audible")) {
                call.play = (*h)["audible"].get<std::string>();  // same formation: same players
                hot_routes.clear();  // (they were for the old play)
                if (between_) between_->event(t - between_start_, "audible");
            }
            if (h && h->contains("hot_routes")) hot_routes = (*h)["hot_routes"].get<std::map<std::string, std::string>>();
            if (h && h->value("snap", false)) {
                const double at = h->value("play_clock", now);
                if (at < now - 1e-9) {
                    // "Snap at :08": then.
                    user_hike = true;
                    hike_tick = tick_of(play_clock - at);
                } else {
                    snapped = true;
                    reading = now;
                }
            }
        }
        // The play clock runs out: delay of game.
        if (!snapped && used >= play_clock - 1e-9) {
            snapped = true;
            reading = 0.0;
        }
        if (snapped) break;

        if (opt_.window && (live(off) || live(def))) opt_.window(t);
        if (replay_ || live(off) || live(def)) {
            ++t;
            continue;
        }
        // Nobody coaching live: straight on to the next moment something happens.
        std::int64_t next = tick_of(play_clock);
        if (!have_call) next = std::min(next, tick_of(when.call));
        if (have_call && oplay && defense_play.empty()) next = std::min(next, tick_of(when.defense_call));
        if (have_call && !set_tick) next = std::min(next, tick_of(when.set));
        if (set_tick) next = std::min(next, std::max(hike_tick, tick_of(when.earliest)));
        t = std::max(t + 1, next);
    }

    const double used = std::min(play_clock, play_clock - reading);
    if (!between_plays(used)) {
        finish_between({});
        return {SnapEnd::PeriodOver, -1};
    }
    ++snaps_;

    if (!have_call || (reading <= 0.0 && (kicking || clock_play))) {
        finish_between({}, "delay_of_game");
        enforce_presnap(Foul{"delay_of_game", true, "", "", 0.0, 0.0, 0.0});
        return {};
    }
    if (kicking) return kick == KickChoice::Punt ? punt() : field_goal();

    // The play.
    if (!clock_play) {
        finish_options(o, call);
        o.hot_routes = hot_routes;
        if (const std::optional<Foul> f = presnap_foul(o, reading)) {
            // The animation up to the flag; the next snap starts from the line.
            if (between_) {
                const std::vector<Spot> spots = play_start(data_, o);
                finish_between(spots, f->type);
                ended(spots);
            }
            enforce_presnap(*f);
            return {};
        }
    }
    PlayResult r;
    try {
        if (clock_play) finish_between({});  // (no tracking file for a kneel or a spike)
        r = clock_play ? kneel_or_spike(call.play) : run_tracked(o, call.play);
    } catch (const PlayError& e) {
        throw GameError(std::string("play ") + call.play + " at " + situation_text() + ": " + e.what());
    }

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
        }, [&] {
            // Each choice and what it leaves behind.
            Json fl = Json::array();
            for (const Foul& f : flagged) {
                const Next n = enforce(before, r, f, rules_.penalty(f.type), rules_);
                fl.push_back({{"foul", f.type}, {"team", team_key(f.by_offense ? off : def)}, {"player", f.player_id},
                              {"yards", r2(n.yards)}, {"result", next_text(off, n)}});
            }
            return Json{{"fouls", fl}, {"play", r.summary}, {"declined", next_text(off, standing)}};
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
        const Json c = ask("runoff", offended, [&] { return Json{{"take", coach(offended).runoff(g_)}}; },
                           [&] { return Json{{"seconds", rules_.runoff}}; });
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
    if (call.play != called_play) pl["audible_from"] = called_play;
    if (!hot_routes.empty()) pl["hot_routes"] = hot_routes;
    if (!defense_play.empty()) pl["defense_call"] = defense_play;
    if (defense_play != called_defense) pl["check_from"] = called_defense;
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

// A log from before the current format can't replay: say so instead of failing on its first input.
std::optional<std::string> format_error(const Json& header) {
    const int format = header.value("log_format", 1);
    if (format == kLogFormat) return std::nullopt;
    return "this game log is format " + std::to_string(format) + " (from sim " + header.value("sim_version", "?") +
           "); this sim replays format " + std::to_string(kLogFormat) + " logs only";
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

std::optional<std::string> check_answer(const Prompt& p, Json& a) {
    if (!a.is_object()) return "the answer must be a JSON object";
    const Json& o = p.options;
    const auto list = [&](const char* key) { return o.contains(key) && o[key].is_array() ? o[key] : Json::array(); };
    const auto in = [](const Json& arr, const Json& v) { return std::find(arr.begin(), arr.end(), v) != arr.end(); };
    const auto boolean = [&](const char* key, bool fallback) -> std::optional<bool> {
        if (!a.contains(key)) return fallback;
        if (!a[key].is_boolean()) return std::nullopt;
        return a[key].get<bool>();
    };
    Json out = Json::object();
    if (p.input == "call_play") {
        if (!a.contains("play") || !a["play"].is_string()) return "call_play needs a play id";
        const std::string id = a["play"].get<std::string>();
        const std::optional<bool> flip = boolean("flip", false);
        if (!flip) return "flip must be true or false";
        const bool offense = o.value("side", "offense") == "offense";
        const Json plays = list("plays");
        const auto found = std::find_if(plays.begin(), plays.end(), [&](const Json& x) { return x.value("id", "") == id; });
        if (found == plays.end() && !in(list("clock_plays"), id) && !in(list("kick_plays"), id)) {
            return "no play '" + id + "' to call here";
        }
        if (*flip && (found == plays.end() || !found->value("flip", false))) return "play " + id + " can't be flipped";
        out["play"] = id;
        if (offense) out["flip"] = *flip;
        if (offense && o.value("try", false)) out["try"] = true;
    } else if (p.input == "hike") {
        // At the line (live): snap now, or at a reading still to come ("snap
        // at :08"); an audible or hot routes on their own don't snap.
        const double play_clock = o.value("play_clock", 40.0);
        const double now = o.value("now", play_clock);
        const double set = o.value("set", play_clock);
        const std::optional<bool> timeout = boolean("timeout", false);
        if (!timeout) return "timeout must be true or false";
        const std::optional<bool> snap = boolean("snap", false);
        if (!snap) return "snap must be true or false";
        bool snapping = *snap;
        double reading = now;
        if (a.contains("play_clock")) {
            if (!a["play_clock"].is_number()) return "play_clock must be a number";
            // A reading already gone by: now.
            reading = std::min(now, a["play_clock"].get<double>());
            snapping = snapping || !*timeout;
            if (snapping && !*timeout && a["play_clock"].get<double>() > set + 1e-9) {
                return "can't snap before the offense is set (play clock " + std::to_string(set) + ")";
            }
        }
        if (snapping && !*timeout && reading > set + 1e-9) {
            return "can't snap before the offense is set (play clock " + std::to_string(set) + ")";
        }
        if (*timeout) {
            if (o.value("timeouts", 0) <= 0) return "no timeouts left";
            out["timeout"] = true;
            out["play_clock"] = std::round(now * 100.0) / 100.0;
        } else {
            if (snapping) {
                out["snap"] = true;
                out["play_clock"] = std::round(std::max(reading, -5.0) * 100.0) / 100.0;
            }
            if (a.contains("audible") && !a["audible"].is_null()) {
                if (!a["audible"].is_string() || !in(list("audibles"), a["audible"])) {
                    return "can't audible to " + a["audible"].dump() + " from this formation";
                }
                out["audible"] = a["audible"];
            }
            if (a.contains("hot_routes") && !a["hot_routes"].empty()) {
                if (!a["hot_routes"].is_object()) return "hot_routes must map slots to routes";
                if (out.contains("audible")) return "hot routes go with the play called, not an audible";
                for (const auto& [slot, route] : a["hot_routes"].items()) {
                    if (!in(list("hot_slots"), slot)) return "no route to change for " + slot;
                    if (!route.is_string() || !in(list("routes"), route)) return "unknown route " + route.dump();
                }
                out["hot_routes"] = a["hot_routes"];
            }
            if (out.empty()) return "snap, audible, change a route, or call timeout";
        }
    } else if (p.input == "check") {
        if (!a.contains("play") || !a["play"].is_string()) return "check needs a play id";
        const Json plays = list("plays");
        const std::string id = a["play"].get<std::string>();
        if (std::none_of(plays.begin(), plays.end(), [&](const Json& x) { return x.value("id", "") == id; })) {
            return "can't check to '" + id + "' from this package";
        }
        out = {{"play", id}};
    } else if (p.input == "coaching_decision") {
        if (!a.contains("choice") || !in(list("choices"), a["choice"])) {
            return "choice must be one of " + list("choices").dump();
        }
        out = {{"decision", o.value("decision", "")}, {"choice", a["choice"]}};
    } else if (p.input == "penalty_decision") {
        const std::optional<bool> accept = boolean("accept", false);
        if (!accept) return "accept must be true or false";
        const Json fouls = list("fouls");
        if (fouls.empty()) return "no fouls to decide";
        std::string foul = fouls.front().value("foul", "");
        if (*accept && fouls.size() > 1) {
            if (!a.contains("foul") || !a["foul"].is_string()) return "which foul to accept?";
            foul = a["foul"].get<std::string>();
            if (std::none_of(fouls.begin(), fouls.end(), [&](const Json& f) { return f.value("foul", "") == foul; })) {
                return "no foul " + foul + " was flagged";
            }
        }
        out = {{"accept", *accept}, {"foul", foul}};
    } else if (p.input == "runoff") {
        const std::optional<bool> take = boolean("take", false);
        if (!take) return "take must be true or false";
        out = {{"take", *take}};
    } else if (p.input == "timeout") {
        if (o.value("left", 0) <= 0) return "no timeouts left";
    } else {
        return "unknown input " + p.input;
    }
    a = std::move(out);
    return std::nullopt;
}

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

GameResult rerun_game(const DataSet& data, std::istream& log, GameOptions options) {
    Json header;
    std::vector<std::string> lines;
    std::vector<Json> inputs;
    try {
        inputs = parse_log(log, header, lines);
    } catch (const std::exception& e) {
        throw GameError(std::string("can't read the log: ") + e.what());
    }
    if (header.is_null()) throw GameError("the log has no header line");
    if (const std::optional<std::string> e = format_error(header)) throw GameError(*e);
    options.home = header.value("home", "");
    options.away = header.value("away", "");
    options.seed = header.value("seed", std::uint64_t{0});
    options.crowd = header.value("crowd", 0.5);
    options.neutral = header.value("neutral", false);
    options.decide = nullptr;
    Game game(data, options, nullptr, &inputs);
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
    if (const std::optional<std::string> e = format_error(header)) {
        report.error = *e;
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
