// The C API (cfbsim/sim_api.h): thin wrappers that turn JSON strings into
// calls on a Match, and C++ exceptions into error codes. No exception may
// cross into C code, so every entry point catches everything.

#include "cfbsim/sim_api.h"

#include <memory>
#include <sstream>
#include <string>

#include "cfbsim/api/match.h"
#include "cfbsim/data/load.h"
#include "cfbsim/field/geometry.h"
#include "cfbsim/log/tracking.h"
#include "cfbsim/play/kick_sim.h"
#include "cfbsim/play/play_sim.h"
#include "cfbsim/version.h"

// The opaque handle. C callers only ever hold a pointer to it.
struct cfbsim_match {
    cfbsim::DataSet data;  // declared before the Match, so it outlives it
    std::unique_ptr<cfbsim::Match> match;
    std::string out;       // the last string returned
};

namespace {

thread_local std::string last_error;

const char* fail(const std::string& message) {
    last_error = message;
    return nullptr;
}

}  // namespace

extern "C" {

const char* cfbsim_version(void) {
    static const std::string v(cfbsim::version());
    return v.c_str();
}

const char* cfbsim_last_error(void) { return last_error.c_str(); }

cfbsim_match* cfbsim_match_create(const char* data_dir, const char* options_json) {
    using cfbsim::Json;
    try {
        last_error.clear();
        const Json o = Json::parse(options_json ? options_json : "{}");
        auto h = std::make_unique<cfbsim_match>();
        cfbsim::LoadReport report = cfbsim::load_data_dir(data_dir ? data_dir : "data");
        if (!report.ok()) {
            fail("the data folder has errors: " + report.errors.front());
            return nullptr;
        }
        h->data = std::move(report.data);
        cfbsim::MatchOptions mo;
        mo.game.home = o.value("home", "clemson_2018");
        mo.game.away = o.value("away", "clemson_2025");
        mo.game.seed = o.value("seed", std::uint64_t{1});
        mo.game.crowd = o.value("crowd", 0.5);
        mo.game.neutral = o.value("neutral", false);
        for (const Json& side : o.value("user", Json::array())) {
            if (side == "home") mo.user[0] = true;
            else if (side == "away") mo.user[1] = true;
            else {
                fail("user sides are \"home\" and \"away\"");
                return nullptr;
            }
        }
        mo.replay_log = o.value("replay_log", "");
        mo.tracking = o.value("tracking", true);
        if (mo.replay_log.empty()) {
            for (const std::string& id : {mo.game.home, mo.game.away}) {
                if (!h->data.teams.contains(id)) {
                    fail("unknown team '" + id + "'");
                    return nullptr;
                }
            }
        }
        h->match = std::make_unique<cfbsim::Match>(h->data, std::move(mo));
        return h.release();
    } catch (const std::exception& e) {
        fail(e.what());
        return nullptr;
    }
}

void cfbsim_match_destroy(cfbsim_match* match) { delete match; }

const char* cfbsim_match_advance(cfbsim_match* match) {
    if (!match) return fail("no match");
    try {
        match->out = match->match->advance().dump();
        return match->out.c_str();
    } catch (const std::exception& e) {
        return fail(e.what());
    }
}

int cfbsim_match_command(cfbsim_match* match, const char* command_json) {
    if (!match) return fail("no match"), -1;
    try {
        const std::optional<std::string> error = match->match->command(cfbsim::Json::parse(command_json));
        if (error) return fail(*error), -1;
        return 0;
    } catch (const std::exception& e) {
        return fail(e.what()), -1;
    }
}

const char* cfbsim_match_log(cfbsim_match* match) {
    if (!match) return fail("no match");
    match->out = match->match->log();
    return match->out.c_str();
}

const char* cfbsim_run_play(const char* data_dir, const char* options_json) {
    using cfbsim::Json;
    // There's no handle to own the result, so it lives in a per-thread string
    // (like the last error).
    thread_local std::string out;
    try {
        last_error.clear();
        const Json o = Json::parse(options_json ? options_json : "{}");
        cfbsim::PlayOptions po;
        po.team = o.value("offense", "");
        po.play = o.value("play", "");
        if (po.team.empty() || po.play.empty()) return fail("run_play needs \"offense\" and \"play\"");
        po.defense = o.value("defense", "");
        po.defense_play = o.value("def_play", "");
        if (!po.defense.empty() && po.defense_play.empty()) return fail("a defense needs \"def_play\"");
        po.seed = o.value("seed", std::uint64_t{1});
        const auto hash = cfbsim::from_name(cfbsim::kHashNames, o.value("hash", "middle"));
        if (!hash) return fail("hash must be left, middle, or right");
        po.hash = *hash;
        po.yard_line = o.value("yard_line", 25.0);
        const std::string direction = o.value("direction", "right");
        if (direction != "right" && direction != "left") return fail("direction must be right or left");
        po.offense_right = direction == "right";
        po.flip = o.value("flip", false);
        po.huddle = o.value("huddle", false);
        po.target = o.value("target", "");
        po.hold_ball = o.value("hold", false);

        cfbsim::LoadReport report = cfbsim::load_data_dir(data_dir ? data_dir : "data");
        if (!report.ok()) return fail("the data folder has errors: " + report.errors.front());
        if (!report.data.teams.contains(po.team)) return fail("unknown team '" + po.team + "'");
        if (!po.defense.empty() && !report.data.teams.contains(po.defense)) {
            return fail("unknown team '" + po.defense + "'");
        }

        std::ostringstream tracking;
        cfbsim::TrackingWriter writer(tracking);
        const cfbsim::PlayResult r = cfbsim::run_play(report.data, po, &writer);
        out = Json{{"summary", r.summary}, {"outcome", r.outcome}, {"yards", r.yards}, {"tracking", tracking.str()}}
                  .dump();
        return out.c_str();
    } catch (const std::exception& e) {
        return fail(e.what());
    }
}

const char* cfbsim_run_kick(const char* data_dir, const char* options_json) {
    using cfbsim::Json;
    using cfbsim::PlayType;
    thread_local std::string out;
    try {
        last_error.clear();
        const Json o = Json::parse(options_json ? options_json : "{}");
        cfbsim::KickOptions ko;
        ko.kicking_team = o.value("kicking", "");
        ko.receiving_team = o.value("receiving", "");
        ko.kick_play = o.value("kick", "");
        if (ko.kicking_team.empty() || ko.receiving_team.empty() || ko.kick_play.empty()) {
            return fail("run_kick needs \"kicking\", \"receiving\", and \"kick\"");
        }
        ko.return_play = o.value("return", "");
        ko.seed = o.value("seed", std::uint64_t{1});
        const auto hash = cfbsim::from_name(cfbsim::kHashNames, o.value("hash", "middle"));
        if (!hash) return fail("hash must be left, middle, or right");
        ko.hash = *hash;
        const std::string direction = o.value("direction", "right");
        if (direction != "right" && direction != "left") return fail("direction must be right or left");
        ko.kicking_right = direction == "right";
        ko.return_aggression = o.value("return_aggression", 50.0);

        cfbsim::LoadReport report = cfbsim::load_data_dir(data_dir ? data_dir : "data");
        if (!report.ok()) return fail("the data folder has errors: " + report.errors.front());
        const cfbsim::DataSet& data = report.data;
        const auto book_of = [&](const std::string& team) -> const cfbsim::Playbook* {
            const auto t = data.teams.find(team);
            if (t == data.teams.end()) return nullptr;
            const auto b = data.playbooks.find(t->second.special_playbook);
            return b == data.playbooks.end() ? nullptr : &b->second;
        };
        if (!data.teams.contains(ko.kicking_team)) return fail("unknown team '" + ko.kicking_team + "'");
        if (!data.teams.contains(ko.receiving_team)) return fail("unknown team '" + ko.receiving_team + "'");
        const cfbsim::Playbook* kicks = book_of(ko.kicking_team);
        const cfbsim::Play* kick = kicks ? kicks->find(ko.kick_play) : nullptr;
        if (!kick) return fail("no kick '" + ko.kick_play + "' in " + ko.kicking_team + "'s special teams playbook");
        PlayType answer;
        double yard_line;
        switch (kick->type) {
            case PlayType::Kickoff: answer = PlayType::KickoffReturn; yard_line = 35.0; break;
            case PlayType::Punt: answer = PlayType::PuntReturn; yard_line = 30.0; break;
            case PlayType::FieldGoal: answer = PlayType::FieldGoalBlock; yard_line = 70.0; break;
            default: return fail("'" + ko.kick_play + "' isn't a kickoff, punt, or field goal");
        }
        ko.yard_line = o.value("yard_line", yard_line);
        if (ko.return_play.empty()) {
            // The receiving team's first answer to that kind of kick.
            if (const cfbsim::Playbook* returns = book_of(ko.receiving_team)) {
                for (const cfbsim::Play& p : returns->plays) {
                    if (p.type == answer) {
                        ko.return_play = p.id;
                        break;
                    }
                }
            }
            if (ko.return_play.empty()) return fail(ko.receiving_team + " has no " + std::string(cfbsim::to_string(answer)) + " play");
        }

        std::ostringstream tracking;
        cfbsim::TrackingWriter writer(tracking);
        const cfbsim::KickResult r = cfbsim::run_kick(data, ko, &writer);
        out = Json{{"summary", r.summary}, {"outcome", r.result}, {"yards", r.return_yards}, {"tracking", tracking.str()}}
                  .dump();
        return out.c_str();
    } catch (const std::exception& e) {
        return fail(e.what());
    }
}

}  // extern "C"
