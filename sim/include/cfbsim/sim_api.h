/* The sim's C API (phase 11; docs/cfbsim_design.md#the-c-api).
 *
 * Plain C, so any language or engine that can call a C function can host the
 * sim: a C++ server, a game engine plugin, Python through ctypes, ... Every
 * structured value crosses as a JSON string, and a match is an opaque handle
 * (a pointer to a struct the caller never sees inside).
 *
 * Strings returned by the API belong to the API: they stay valid until the
 * next call on the same match (cfbsim_last_error: on the same thread). */
#ifndef CFBSIM_SIM_API_H
#define CFBSIM_SIM_API_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cfbsim_match cfbsim_match;

/* The sim's version, e.g. "0.1.0". */
const char* cfbsim_version(void);

/* The last error on this thread ("" if none). */
const char* cfbsim_last_error(void);

/* Loads the data folder and sets up a match. `options_json`:
 *   {"home": "clemson_2018", "away": "clemson_2025", "seed": 1, "crowd": 0.5, "neutral": false,
 *    "user": ["home"],          sides the caller controls (default: none)
 *    "replay_log": "...",       a game log's text: watch that game instead
 *    "tracking": true}          a tracking file per snap in the messages
 * Returns NULL on error (see cfbsim_last_error). */
cfbsim_match* cfbsim_match_create(const char* data_dir, const char* options_json);

void cfbsim_match_destroy(cfbsim_match* match);

/* Runs until the caller must decide, a play ends, or the game is over.
 * Returns {"messages": [...], "prompt": {...} | null, "done": bool, "error"?: "..."}. */
const char* cfbsim_match_advance(cfbsim_match* match);

/* A command: {"type": "answer", ...}, {"type": "auto", "team": "home", "on": true},
 * or {"type": "timeout", "team": "home"}. Returns 0, or -1 on error (see cfbsim_last_error). */
int cfbsim_match_command(cfbsim_match* match, const char* command_json);

/* The game log so far (JSON Lines). */
const char* cfbsim_match_log(cfbsim_match* match);

/* Runs one play outside a game, like `cfbsim run-play`. It loads the data
 * folder on every call, so edited data takes effect at once. `options_json`:
 *   {"offense": "clemson_2025", "play": "gun_inside_zone",          required
 *    "defense": "clemson_2018", "def_play": "43_over_cover3",        optional: an empty field without them
 *    "seed": 1, "hash": "middle", "yard_line": 25, "direction": "right",
 *    "flip": false, "huddle": false, "target": "Z", "hold": false}
 * Returns {"summary": "...", "outcome": "...", "yards": 3.2, "tracking": "<JSON Lines>"},
 * or NULL on error (see cfbsim_last_error). The string stays valid until the
 * next cfbsim_run_play call on the same thread. */
const char* cfbsim_run_play(const char* data_dir, const char* options_json);

/* Runs one kicking play outside a game, like `cfbsim run-kick`. It loads the
 * data folder on every call. `options_json`:
 *   {"kicking": "clemson_2025", "kick": "kickoff_deep",  required (a kickoff, punt, or field goal)
 *    "receiving": "clemson_2018",                        required
 *    "return": "kr_middle",                              optional: the receiving team's first
 *                                                        return (or block) for that kind of kick
 *    "seed": 1, "hash": "middle",
 *    "yard_line": 35,                                    the kicking team's own yard line; default
 *                                                        35 kickoff, 30 punt, 70 field goal
 *    "direction": "right", "return_aggression": 50}
 * Returns {"summary": "...", "outcome": "<result>", "yards": <return yards>,
 * "tracking": "<JSON Lines>"}, or NULL on error (see cfbsim_last_error). The
 * string stays valid until the next cfbsim_run_kick call on the same thread. */
const char* cfbsim_run_kick(const char* data_dir, const char* options_json);

#ifdef __cplusplus
}
#endif

#endif
