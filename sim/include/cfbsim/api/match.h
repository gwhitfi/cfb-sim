// A game you can step through and play (phase 11;
// docs/design/rules-and-flow.md#interactive-games-phase-11). The C API
// (cfbsim/sim_api.h) and the server are built on this.
//
// The game loop (`play_game`) is ordinary straight-line code that asks for
// decisions as it goes. To pause it for a user's answer, a Match runs it on
// its own thread and passes control back and forth like a baton: exactly one
// of the two threads runs at any moment, so the game sees the same sequence
// of events as a headless run and stays deterministic.
#pragma once

#include <array>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "cfbsim/rules/game.h"

namespace cfbsim {

struct MatchOptions {
    GameOptions game;            // teams, seed, crowd, neutral site (the Match sets the hooks)
    std::array<bool, 2> user{};  // sides a user controls (home, away); the auto coach plays the rest
    std::string replay_log;      // a game log's text: watch that game instead (no user inputs)
    bool tracking = true;        // capture a tracking file for every snap
    bool pause_each_play = true; // advance() also returns after every play
};

// Not thread-safe: call it from one thread at a time (the server holds a lock).
class Match {
  public:
    // `data` must outlive the Match.
    Match(const DataSet& data, MatchOptions options);
    ~Match();
    Match(const Match&) = delete;
    Match& operator=(const Match&) = delete;

    // Runs the game until a user must decide, a play ends (pause_each_play),
    // or the game is over. Returns what happened since the last call:
    //   {"messages": [...], "prompt": {...} | null, "done": bool, "error": "...",
    //    "live": [open decisions between plays], "window": bool (stopped between plays), "tick": game tick}
    // messages: {"type": "line", "line": {game log line}, "state": {...}} and
    //           {"type": "tracking", "name": "...", "data": "tracking file text"}
    // While a prompt is unanswered, advance() returns it again and does nothing.
    Json advance();

    // Commands from the user:
    //   {"type": "answer", ...fields}  answer the pending prompt ({"type": "answer", "auto": true}:
    //                                   take the coach's suggestion)
    //   {"type": "auto", "team": "home", "on": true}  let the coach take over a side (or give it back)
    //   {"type": "timeout", "team": "home"}           call a timeout (between plays: at once)
    //   {"type": "run_to", "tick": 1234}              between plays, run the clock this far, no
    //                                                  further (a live viewer's pace; without it,
    //                                                  each open decision waits for its answer)
    // Between plays a user side's decisions are live prompts ("live" in
    // advance()): an answer may name its "input" ("call_play", "check",
    // "hike"); without one it goes to the prompt advance() returned.
    // Returns an error message, or nullopt.
    std::optional<std::string> command(const Json& cmd);

    const std::optional<Prompt>& prompt() const { return pending_; }
    bool done() const { return done_; }
    std::string log() const;  // the game log so far (a replay: the log being watched)

  private:
    struct Cancelled {};  // thrown on the game thread to unwind it when the Match is destroyed
    void game_thread();
    // Game thread: hand control to the caller and wait to get it back.
    void yield_to_caller(std::unique_lock<std::mutex>& lock);
    std::optional<Json> decide(const Prompt& p);
    std::optional<Json> decide_live(const Prompt& p);
    void window_tick(std::int64_t tick);
    void flush_live();  // the between-plays file written so far, as a part
    static Json prompt_json(const Prompt& p);
    void observe(const Json& line, const Json& state);
    void tracked(const std::string& name, std::string text);

    const DataSet& data_;
    MatchOptions opt_;

    std::mutex m_;
    std::condition_variable cv_;
    bool game_turn_ = false;  // who holds the baton
    bool cancel_ = false;
    bool started_ = false;
    bool done_ = false;
    std::string error_;
    std::thread thread_;

    std::optional<Prompt> pending_;   // the prompt waiting for the user
    bool answered_ = false;
    std::optional<Json> answer_;      // nullopt with answered_: take the suggestion
    std::array<int, 2> timeout_requests_{};
    Json outbox_ = Json::array();

    // Between plays (live).
    using Key = std::pair<std::string, int>;  // (input, team)
    std::map<Key, Prompt> live_, live_next_;  // open decisions of user sides (this tick, the next)
    std::set<Key> answered_live_;             // answered while open
    std::optional<Key> paused_for_;           // the new decision the game stopped for
    std::vector<std::pair<Key, Json>> queue_; // answers waiting for the next tick
    std::int64_t tick_ = 0;
    bool in_window_ = false;                  // stopped between plays (the clock is the caller's)
    std::int64_t to_tick_ = 0;
    bool clocked_ = false;                    // a caller sets the pace (run_to)
    std::ostringstream* live_stream_ = nullptr;  // the between-plays file being written
    std::string live_name_;
    std::size_t live_sent_ = 0;
    std::ostringstream log_;
};

}  // namespace cfbsim
