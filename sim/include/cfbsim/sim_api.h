/* The sim's C API (phase 11; docs/design/rules-and-flow.md#interactive-games-phase-11).
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

#ifdef __cplusplus
}
#endif

#endif
