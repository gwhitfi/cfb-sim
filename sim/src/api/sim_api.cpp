// The C API (cfbsim/sim_api.h): thin wrappers that turn JSON strings into
// calls on a Match, and C++ exceptions into error codes. No exception may
// cross into C code, so every entry point catches everything.

#include "cfbsim/sim_api.h"

#include <memory>
#include <string>

#include "cfbsim/api/match.h"
#include "cfbsim/data/load.h"
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
        mo.game.home = o.value("home", "state_u");
        mo.game.away = o.value("away", "tech");
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

}  // extern "C"
