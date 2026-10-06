// cfbsim-server: hosts games for the browser viewer over a local WebSocket
// (phase 11; docs/cfbsim_design.md#the-server).
//
// Each browser connection gets its own session with at most one match. The
// matches run through the C API (cfbsim/sim_api.h), the same way a game
// engine would embed the sim. IXWebSocket calls our message callback on a
// thread per connection, so each session has a mutex.
//
// Messages (JSON text) from the viewer:
//   {"type": "hello"}                          -> {"type": "hello", "version", "teams": [...], "logs": [...]}
//       (each team: {"id", "name", "abbr", "offense": [plays], "defense": [plays],
//        "kicking": [kickoffs, punts, field goals], "receiving": [returns, blocks],
//        "nickname", "scratch", "colors": {"primary", "secondary"}},
//        a play {"id", "name", "type", "formation", "formation_name", "tags"})
//   {"type": "run_play", "offense", "play", "defense"?, "def_play"?, "seed"?, "hash"?, "yard_line"?, "flip"?, "huddle"?}
//                                              -> {"type": "play", "summary", "outcome", "yards", "file", "tracking"}
//       one play outside a game (cfbsim_run_play), saved to out/plays/ as well
//   {"type": "run_kick", "kicking", "kick", "receiving", "return"?, "seed"?, "hash"?, "yard_line"?}
//                                              -> {"type": "play", ...} as for run_play (cfbsim_run_kick)
//   {"type": "new_game", "home", "away", "seed"?, "user": ["home"]}
//   {"type": "replay", "file": "out/games/x.gamelog.jsonl"} (a file from "logs")
//   {"type": "advance", "to_tick"?: N}         -> {"type": "advance", "result": {...}}
//                                                (to_tick: between plays, run the clock this far)
//   {"type": "command", "command": {...}}      -> {"type": "command", "ok": bool, "error"?}
//   {"type": "run_matrix", "offense", "defense", "plays"?: [ids], "defs"?: [ids], "runs"?, "seed"?, "hash"?,
//                          "yard_line"?, "huddle"?, "threads"?}
//                                              -> {"type": "matrix_started", "info": {...}, "total": cells}, then one
//                                                 {"type": "matrix_cell", "row", "col", "stats": {...}, "done", "total"}
//                                                 per finished cell (any order), then {"type": "matrix_done",
//                                                 "cells", "cancelled", "file"} (cfbsim_matrix_*; the cells run on
//                                                 all cores; the run is saved to out/matrix/ too)
//       "kind": "kicks" runs a kick matrix instead ("kicking", "receiving", "kicks", "returns", "runs", "seed", "hash"?,
//       "yard_line"?, "return_aggression"?): rows are kicks, columns returns or blocks; a cell that doesn't apply
//       (a punt against a kickoff return) arrives with "applies": false. Same messages as above.
//   {"type": "cancel_matrix"}                  -> stops the run; matrix_done follows with "cancelled": true
//   {"type": "list_matrix"}                    -> {"type": "matrix_list", "runs": [{"file", "offense", "defense",
//                                                 "runs", "rows", "cols", "cells", "cancelled"}]} (saved runs, newest first)
//   {"type": "get_matrix", "file": "out/matrix/x.json"}
//                                              -> {"type": "matrix_loaded", "file", "settings", "info", "cancelled",
//                                                 "cells": [{"row", "col", "stats", "samples"}]} (a file from the list)
//   {"type": "list_playbooks"}                -> {"type": "playbooks", "playbooks": [{"id", "name", "side", "scheme", "plays", "formations"}]}
//   {"type": "get_team", "id"}                 -> {"type": "team", ...} (identity, colors, playbooks, scratch, roster summary)
//   {"type": "save_team", "team": {...}, "roster": {...}, "overwrite"?}
//                                              -> {"type": "team_saved", "id", "team_file", "roster_file", "created"},
//                                                 then {"type": "teams", "teams": [...]} (the refreshed list); see
//                                                 cfbsim_save_team. Nothing is written if anything is wrong: the error
//                                                 carries "problems": [...] (cfbsim/gen/team_builder.h)
//   {"type": "delete_team", "id"}              -> {"type": "team_deleted", "id"}, then "teams" (scratch teams only)
// A finished live game's log is saved to out/games/ and announced with
// {"type": "saved", "path": "..."}. Errors come back as {"type": "error", "message"}.

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocketServer.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "cfbsim/data/load.h"
#include "cfbsim/sim_api.h"

namespace fs = std::filesystem;
using cfbsim::Json;

namespace {

struct Config {
    int port = 8765;
    std::string host = "127.0.0.1";
    std::string data = "data";
    std::string out = "out/games";
    std::string plays = "out/plays";  // single plays (run_play)
    std::string matrix = "out/matrix";  // finished matrix runs (run_matrix)
};

struct MatchDeleter {
    void operator()(cfbsim_match* m) const { cfbsim_match_destroy(m); }
};

struct MatrixDeleter {
    void operator()(cfbsim_matrix* m) const { cfbsim_matrix_close(m); }
};

// A matrix run in progress: a coordinator thread that starts the workers,
// each taking the next cell until none are left or the run is cancelled.
struct Job {
    std::unique_ptr<cfbsim_matrix, MatrixDeleter> matrix;
    std::atomic<bool> cancel{false};
    std::atomic<bool> finished{false};
    std::thread thread;
};

struct Session {
    ~Session() { stop_job(); }

    // Cancels the matrix run, if any, and waits for it to stop.
    void stop_job() {
        if (!job) return;
        job->cancel = true;
        if (job->thread.joinable()) job->thread.join();
        job.reset();
    }

    // Sends to the browser from any thread (the matrix workers do).
    void send_text(const std::string& text) {
        std::lock_guard lock(send_m);
        if (send) send(text);
    }

    std::mutex m;
    std::mutex send_m;
    std::function<void(const std::string&)> send;  // set from the connection's callbacks
    std::unique_ptr<Job> job;
    std::unique_ptr<cfbsim_match, MatchDeleter> match;
    bool live = false;   // a game being played (saved at the end), not a replay
    bool saved = false;
    std::string name;    // file name for the saved log
};

// Game logs the viewer may open: the repo root and out/games.
std::vector<std::string> list_logs(const Config& c) {
    std::vector<std::string> found;
    for (const fs::path& dir : {fs::path("."), fs::path(c.out)}) {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            const std::string name = e.path().filename().string();
            if (e.is_regular_file() && name.ends_with(".gamelog.jsonl")) {
                found.push_back((dir == "." ? fs::path(name) : dir / name).generic_string());
            }
        }
    }
    std::sort(found.begin(), found.end());
    return found;
}

std::string timestamp() {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
    localtime_r(&now, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%d_%H%M%S", &tm);
    return buf;
}

std::string error_message(const std::string& message) {
    return Json{{"type", "error"}, {"message", message}}.dump();
}

class Server {
  public:
    explicit Server(Config c) : c_(std::move(c)) {
        const cfbsim::LoadReport report = cfbsim::load_data_dir(c_.data);
        if (!report.ok()) throw std::runtime_error("data folder has errors: " + report.errors.front());
        teams_ = catalog(report.data);
    }

    // Handles one message; returns the replies, in order.
    std::vector<std::string> handle(Session& s, const std::string& text) {
        Json msg;
        try {
            msg = Json::parse(text);
        } catch (const std::exception& e) {
            return {error_message(std::string("not JSON: ") + e.what())};
        }
        const std::string type = msg.value("type", "");
        if (type == "hello") {
            // Reload the data, so plays added since the server started show up.
            refresh_teams();
            return {Json{{"type", "hello"}, {"version", cfbsim_version()}, {"teams", teams()}, {"logs", list_logs(c_)}}.dump()};
        }
        if (type == "run_play") return run_play(msg);
        if (type == "run_kick") return run_kick(msg);
        if (type == "run_matrix") return run_matrix(s, msg);
        if (type == "list_playbooks") return team_call(cfbsim_list_playbooks(c_.data.c_str()), "playbooks", "playbooks");
        if (type == "get_team") return team_call(cfbsim_get_team(c_.data.c_str(), msg.value("id", "").c_str()), "team");
        if (type == "save_team") {
            Json o = msg;
            o.erase("type");
            return team_call(cfbsim_save_team(c_.data.c_str(), o.dump().c_str()), "team_saved", "", true);
        }
        if (type == "delete_team") {
            return team_call(cfbsim_delete_team(c_.data.c_str(), msg.value("id", "").c_str()), "team_deleted", "", true);
        }
        if (type == "list_matrix") return {Json{{"type", "matrix_list"}, {"runs", list_matrix()}}.dump()};
        if (type == "get_matrix") return get_matrix(msg);
        if (type == "cancel_matrix") {
            if (s.job) s.job->cancel = true;
            return {};
        }
        if (type == "new_game") return new_game(s, msg);
        if (type == "replay") return replay(s, msg);
        if (!s.match) return {error_message("no game: send new_game or replay first")};
        if (type == "advance") {
            if (msg.contains("to_tick") && msg["to_tick"].is_number()) {
                const std::string run = Json{{"type", "run_to"}, {"tick", msg["to_tick"]}}.dump();
                if (cfbsim_match_command(s.match.get(), run.c_str()) != 0) return {error_message(cfbsim_last_error())};
            }
            const char* r = cfbsim_match_advance(s.match.get());
            if (!r) return {error_message(cfbsim_last_error())};
            std::vector<std::string> out{std::string(R"({"type":"advance","result":)") + r + "}"};
            if (s.live && !s.saved && Json::parse(r).value("done", false)) out.push_back(save(s));
            return out;
        }
        if (type == "command") {
            const std::string cmd = msg.value("command", Json::object()).dump();
            if (cfbsim_match_command(s.match.get(), cmd.c_str()) != 0) {
                return {Json{{"type", "command"}, {"ok", false}, {"error", cfbsim_last_error()}}.dump()};
            }
            return {Json{{"type", "command"}, {"ok", true}}.dump()};
        }
        return {error_message("unknown message type '" + type + "'")};
    }

  private:
    // Wraps a team call's JSON in a reply (merged in, or under `key`), or turns its
    // error into one with the separate problems listed. A change also sends the
    // refreshed team list, and the server's own list is reloaded.
    std::vector<std::string> team_call(const char* result, const std::string& type, const std::string& key = "", bool changes = false) {
        if (!result) {
            const std::string message = cfbsim_last_error();
            Json problems = Json::array();
            std::istringstream lines(message);
            for (std::string line; std::getline(lines, line);) problems.push_back(line);
            return {Json{{"type", "error"}, {"message", message}, {"problems", problems}}.dump()};
        }
        Json reply = Json::parse(result);
        if (!key.empty()) reply = Json{{key, std::move(reply)}};
        reply["type"] = type;
        std::vector<std::string> out{reply.dump()};
        if (changes) {
            refresh_teams();
            out.push_back(Json{{"type", "teams"}, {"teams", teams()}}.dump());
        }
        return out;
    }

    // Reads the data folder again for the team list (a team was made or deleted).
    void refresh_teams() {
        const cfbsim::LoadReport report = cfbsim::load_data_dir(c_.data);
        if (!report.ok()) return;
        std::lock_guard lock(teams_m_);
        teams_ = catalog(report.data);
    }

    Json teams() {
        std::lock_guard lock(teams_m_);
        return teams_;
    }

    // The teams and the plays in their playbooks, for the viewer's menus.
    static Json catalog(const cfbsim::DataSet& data) {
        using cfbsim::PlayType;
        const auto plays = [&](const std::string& playbook, std::initializer_list<PlayType> types) {
            Json out = Json::array();
            const auto it = data.playbooks.find(playbook);
            if (it == data.playbooks.end()) return out;
            for (const cfbsim::Play& p : it->second.plays) {
                if (std::find(types.begin(), types.end(), p.type) == types.end()) continue;
                const auto f = data.formations.find(p.formation);
                out.push_back({{"id", p.id}, {"name", p.name}, {"type", std::string(cfbsim::to_string(p.type))},
                               {"formation", p.formation},
                               {"formation_name", f != data.formations.end() ? f->second.name : p.formation},
                               {"tags", p.tags}});
            }
            return out;
        };
        Json teams = Json::array();
        for (const auto& [id, t] : data.teams) {
            teams.push_back({{"id", id}, {"name", t.name}, {"nickname", t.nickname}, {"abbr", t.abbr}, {"scratch", t.scratch},
                             {"colors", {{"primary", t.color_primary}, {"secondary", t.color_secondary}}},
                             {"offense", plays(t.offense_playbook, {PlayType::Pass, PlayType::Run})},
                             {"defense", plays(t.defense_playbook, {PlayType::Defense})},
                             {"kicking", plays(t.special_playbook, {PlayType::Kickoff, PlayType::Punt, PlayType::FieldGoal})},
                             {"receiving", plays(t.special_playbook, {PlayType::KickoffReturn, PlayType::PuntReturn,
                                                                      PlayType::FieldGoalBlock})}});
        }
        return teams;
    }

    std::vector<std::string> run_play(const Json& msg) {
        Json o = msg;
        o.erase("type");
        const char* r = cfbsim_run_play(c_.data.c_str(), o.dump().c_str());
        if (!r) return {error_message(cfbsim_last_error())};
        const std::string name = o.value("offense", "") + "_" + o.value("play", "") + "_vs_" +
                                 (o.value("defense", "").empty() ? std::string("empty")
                                                                 : o.value("defense", "") + "_" + o.value("def_play", "")) +
                                 "_seed" + std::to_string(o.value("seed", std::uint64_t{1})) + ".jsonl";
        return {played(o, Json::parse(r), name)};
    }

    std::vector<std::string> run_kick(const Json& msg) {
        Json o = msg;
        o.erase("type");
        const char* r = cfbsim_run_kick(c_.data.c_str(), o.dump().c_str());
        if (!r) return {error_message(cfbsim_last_error())};
        const std::string name = o.value("kicking", "") + "_" + o.value("kick", "") + "_vs_" + o.value("receiving", "") +
                                 (o.value("return", "").empty() ? std::string() : "_" + o.value("return", "")) + "_seed" +
                                 std::to_string(o.value("seed", std::uint64_t{1})) + ".jsonl";
        return {played(o, Json::parse(r), name)};
    }

    // The saved matrix runs (out/matrix/*.json), newest first: what each was,
    // read from the file's settings. File names start with the time.
    Json list_matrix() const {
        std::vector<fs::path> files;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(c_.matrix, ec)) {
            if (e.is_regular_file() && e.path().extension() == ".json") files.push_back(e.path());
        }
        std::sort(files.rbegin(), files.rend());
        Json runs = Json::array();
        for (const fs::path& file : files) {
            if (runs.size() >= 50) break;
            try {
                std::ifstream in(file);
                const Json j = Json::parse(in);
                const Json& info = j.at("info");
                runs.push_back({{"file", file.generic_string()},
                                {"kind", info.value("kind", "scrimmage")},
                                {"offense", info.value("offense", info.value("kicking", ""))},
                                {"defense", info.value("defense", info.value("receiving", ""))},
                                {"runs", info.value("runs", 0)},
                                {"rows", info.at("rows").size()},
                                {"cols", info.at("cols").size()},
                                {"cells", j.at("cells").size()},
                                {"cancelled", j.value("cancelled", false)}});
            } catch (const std::exception&) {
                // Not a matrix file we wrote: left out of the list.
            }
        }
        return runs;
    }

    std::vector<std::string> get_matrix(const Json& msg) const {
        const std::string file = msg.value("file", "");
        bool listed = false;
        for (const Json& r : list_matrix()) listed = listed || r.value("file", "") == file;
        if (!listed) return {error_message("not a saved matrix run: " + file)};
        try {
            std::ifstream in(file);
            Json j = Json::parse(in);
            j["type"] = "matrix_loaded";
            j["file"] = file;
            return {j.dump()};
        } catch (const std::exception& e) {
            return {error_message(std::string("couldn't read ") + file + ": " + e.what())};
        }
    }

    // Starts a matrix run on background threads and returns at once; the
    // cells arrive as messages (see the list at the top).
    std::vector<std::string> run_matrix(Session& s, const Json& msg) {
        if (s.job && !s.job->finished) return {error_message("a matrix is already running")};
        s.stop_job();  // joins a finished one

        Json o = msg;
        o.erase("type");
        const int threads_wanted = o.value("threads", 0);
        o.erase("threads");
        auto job = std::make_unique<Job>();
        job->matrix.reset(cfbsim_matrix_open(c_.data.c_str(), o.dump().c_str()));
        if (!job->matrix) return {error_message(cfbsim_last_error())};
        const Json info = Json::parse(cfbsim_matrix_info(job->matrix.get()));
        const int rows = static_cast<int>(info["rows"].size());
        const int cols = static_cast<int>(info["cols"].size());
        const int total = rows * cols;
        const int cores = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
        const int workers = std::clamp(threads_wanted > 0 ? threads_wanted : cores, 1, std::max(1, total));
        std::cout << "matrix: " << o.dump() << " (" << total << " cells, " << workers << " threads)" << std::endl;

        Job* j = job.get();
        j->thread = std::thread([this, &s, j, o, info, rows, cols, total, workers] {
            std::mutex results_m;
            std::vector<Json> cells;
            std::atomic<int> next{0};
            std::vector<std::thread> pool;
            for (int w = 0; w < workers; ++w) {
                pool.emplace_back([&] {
                    for (;;) {
                        const int index = next++;
                        if (index >= total || j->cancel) return;
                        const char* r = cfbsim_matrix_cell(j->matrix.get(), index / cols, index % cols);
                        if (!r) {
                            j->cancel = true;
                            s.send_text(error_message(cfbsim_last_error()));
                            return;
                        }
                        Json cell = Json::parse(r);
                        int done;
                        {
                            std::lock_guard lock(results_m);
                            cells.push_back(cell);
                            done = static_cast<int>(cells.size());
                        }
                        cell["type"] = "matrix_cell";
                        cell["done"] = done;
                        cell["total"] = total;
                        s.send_text(cell.dump());
                    }
                });
            }
            for (std::thread& t : pool) t.join();
            const bool cancelled = static_cast<int>(cells.size()) < total;
            Json done = {{"type", "matrix_done"}, {"cells", cells.size()}, {"cancelled", cancelled}};
            // Save the run (cells in row, column order) so it can be reopened.
            std::sort(cells.begin(), cells.end(), [](const Json& a, const Json& b) {
                return std::pair{a["row"].get<int>(), a["col"].get<int>()} <
                       std::pair{b["row"].get<int>(), b["col"].get<int>()};
            });
            std::error_code ec;
            fs::create_directories(c_.matrix, ec);
            const bool kicks = o.value("kind", "scrimmage") == "kicks";
            const std::string first = o.value(kicks ? "kicking" : "offense", "");
            const std::string second = o.value(kicks ? "receiving" : "defense", "");
            const fs::path path = fs::path(c_.matrix) / (timestamp() + (kicks ? "_kicks_" : "_") + first + "_vs_" +
                                                         (second.empty() ? std::string("empty") : second) + ".json");
            std::ofstream f(path);
            f << Json{{"settings", o}, {"info", info}, {"cancelled", cancelled}, {"cells", cells}}.dump();
            if (f) done["file"] = path.generic_string();
            std::cout << "matrix done: " << cells.size() << "/" << total << " cells" << std::endl;
            j->finished = true;
            s.send_text(done.dump());
        });
        s.job = std::move(job);
        return {Json{{"type", "matrix_started"}, {"info", info}, {"total", total}, {"threads", workers}}.dump()};
    }

    // A run play or kick's reply, also saved to out/plays/ so it can be
    // reopened later (Replays, cfbsim tools).
    std::string played(const Json& o, Json reply, const std::string& name) {
        std::error_code ec;
        fs::create_directories(c_.plays, ec);
        const fs::path path = fs::path(c_.plays) / name;
        std::ofstream f(path);
        f << reply["tracking"].get<std::string>();
        if (f) reply["file"] = path.generic_string();
        reply["type"] = "play";
        std::cout << "play: " << o.dump() << " -> " << reply["summary"].get<std::string>() << std::endl;
        return reply.dump();
    }

    std::vector<std::string> new_game(Session& s, const Json& msg) {
        // A game without a seed gets one from the wall clock (the sim itself never reads it).
        const std::uint64_t seed = msg.contains("seed") && msg["seed"].is_number_unsigned()
                                       ? msg["seed"].get<std::uint64_t>()
                                       : static_cast<std::uint64_t>(std::chrono::system_clock::now().time_since_epoch().count()) % 1000000007ULL;
        const Json o = {{"home", msg.value("home", "clemson_2018")},
                        {"away", msg.value("away", "clemson_2025")},
                        {"seed", seed},
                        {"user", msg.value("user", Json::array())},
                        {"tracking", true}};
        cfbsim_match* m = cfbsim_match_create(c_.data.c_str(), o.dump().c_str());
        if (!m) return {error_message(cfbsim_last_error())};
        s.match.reset(m);
        s.live = true;
        s.saved = false;
        s.name = o["home"].get<std::string>() + "_vs_" + o["away"].get<std::string>() + "_" + timestamp() + "_seed" +
                 std::to_string(seed) + ".gamelog.jsonl";
        std::cout << "new game: " << o.dump() << std::endl;
        return {Json{{"type", "started"}, {"mode", "live"}, {"options", o}}.dump()};
    }

    std::vector<std::string> replay(Session& s, const Json& msg) {
        const std::string file = msg.value("file", "");
        const auto logs = list_logs(c_);
        if (std::find(logs.begin(), logs.end(), file) == logs.end()) return {error_message("not a game log: " + file)};
        std::ifstream in(file);
        std::stringstream text;
        text << in.rdbuf();
        const Json o = {{"replay_log", text.str()}, {"tracking", true}};
        cfbsim_match* m = cfbsim_match_create(c_.data.c_str(), o.dump().c_str());
        if (!m) return {error_message(cfbsim_last_error())};
        s.match.reset(m);
        s.live = false;
        std::cout << "replay: " << file << std::endl;
        return {Json{{"type", "started"}, {"mode", "replay"}, {"file", file}}.dump()};
    }

    std::string save(Session& s) {
        s.saved = true;
        std::error_code ec;
        fs::create_directories(c_.out, ec);
        const fs::path path = fs::path(c_.out) / s.name;
        std::ofstream f(path);
        f << cfbsim_match_log(s.match.get());
        if (!f) return error_message("couldn't save the log to " + path.string());
        std::cout << "saved " << path.generic_string() << std::endl;
        return Json{{"type", "saved"}, {"path", path.generic_string()}}.dump();
    }

    Config c_;
    std::mutex teams_m_;  // teams_ is read by every session and replaced when a team changes
    Json teams_ = Json::array();
};

int usage() {
    std::cerr << "usage: cfbsim-server [--port N] [--host ADDR] [--data DIR] [--out DIR] [--plays DIR] [--matrix DIR]\n"
                 "  Run from the repo root. The viewer (cd viewer && npm run dev) connects to ws://localhost:8765.\n";
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (i + 1 >= argc) return usage();
        if (a == "--port") c.port = std::stoi(argv[++i]);
        else if (a == "--host") c.host = argv[++i];
        else if (a == "--data") c.data = argv[++i];
        else if (a == "--out") c.out = argv[++i];
        else if (a == "--plays") c.plays = argv[++i];
        else if (a == "--matrix") c.matrix = argv[++i];
        else return usage();
    }

    std::unique_ptr<Server> app;
    try {
        app = std::make_unique<Server>(c);
    } catch (const std::exception& e) {
        std::cerr << "cfbsim-server: " << e.what() << "\n";
        return 1;
    }

    ix::initNetSystem();
    ix::WebSocketServer server(c.port, c.host);
    server.disablePerMessageDeflate();

    std::mutex sessions_mutex;
    std::map<std::string, std::shared_ptr<Session>> sessions;  // by connection id

    server.setOnClientMessageCallback([&](std::shared_ptr<ix::ConnectionState> state, ix::WebSocket& ws,
                                          const ix::WebSocketMessagePtr& msg) {
        const std::string id = state->getId();
        if (msg->type == ix::WebSocketMessageType::Open) {
            std::lock_guard lock(sessions_mutex);
            sessions[id] = std::make_shared<Session>();
            sessions[id]->send = [&ws](const std::string& text) { ws.sendText(text); };
            std::cout << "connected: " << id << std::endl;
        } else if (msg->type == ix::WebSocketMessageType::Close) {
            std::shared_ptr<Session> gone;
            {
                std::lock_guard lock(sessions_mutex);
                gone = sessions[id];
                sessions.erase(id);
            }
            if (gone) {
                gone->stop_job();
                std::lock_guard lock(gone->send_m);
                gone->send = nullptr;
            }
            std::cout << "disconnected: " << id << std::endl;
        } else if (msg->type == ix::WebSocketMessageType::Message) {
            std::shared_ptr<Session> s;
            {
                std::lock_guard lock(sessions_mutex);
                s = sessions[id];
                if (!s) s = sessions[id] = std::make_shared<Session>();
            }
            {
                std::lock_guard send_lock(s->send_m);
                s->send = [&ws](const std::string& text) { ws.sendText(text); };
            }
            std::lock_guard lock(s->m);
            for (const std::string& reply : app->handle(*s, msg->str)) ws.sendText(reply);
        }
    });

    const auto ok = server.listen();
    if (!ok.first) {
        std::cerr << "cfbsim-server: can't listen on " << c.host << ":" << c.port << ": " << ok.second << "\n";
        return 1;
    }
    server.start();
    std::cout << "cfbsim-server " << cfbsim_version() << " listening on ws://" << c.host << ":" << c.port
              << " (data: " << c.data << ", logs: " << c.out << ")" << std::endl;
    server.wait();
    ix::uninitNetSystem();
    return 0;
}
