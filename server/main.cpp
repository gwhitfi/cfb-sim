// cfbsim-server: hosts games for the browser viewer over a local WebSocket
// (phase 11; docs/design/rules-and-flow.md#interactive-games-phase-11).
//
// Each browser connection gets its own session with at most one match. The
// matches run through the C API (cfbsim/sim_api.h), the same way a game
// engine would embed the sim. IXWebSocket calls our message callback on a
// thread per connection, so each session has a mutex.
//
// Messages (JSON text) from the viewer:
//   {"type": "hello"}                          -> {"type": "hello", "version", "teams": [...], "logs": [...]}
//       (each team: {"id", "name", "abbr", "offense": [plays], "defense": [plays],
//        "kicking": [kickoffs, punts, field goals], "receiving": [returns, blocks]},
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
// A finished live game's log is saved to out/games/ and announced with
// {"type": "saved", "path": "..."}. Errors come back as {"type": "error", "message"}.

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocketServer.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>

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
};

struct MatchDeleter {
    void operator()(cfbsim_match* m) const { cfbsim_match_destroy(m); }
};

struct Session {
    std::mutex m;
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
            const cfbsim::LoadReport report = cfbsim::load_data_dir(c_.data);
            if (report.ok()) teams_ = catalog(report.data);
            return {Json{{"type", "hello"}, {"version", cfbsim_version()}, {"teams", teams_}, {"logs", list_logs(c_)}}.dump()};
        }
        if (type == "run_play") return run_play(msg);
        if (type == "run_kick") return run_kick(msg);
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
            teams.push_back({{"id", id}, {"name", t.name}, {"abbr", t.abbr},
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
    Json teams_ = Json::array();
};

int usage() {
    std::cerr << "usage: cfbsim-server [--port N] [--host ADDR] [--data DIR] [--out DIR] [--plays DIR]\n"
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
            std::cout << "connected: " << id << std::endl;
        } else if (msg->type == ix::WebSocketMessageType::Close) {
            std::shared_ptr<Session> gone;
            {
                std::lock_guard lock(sessions_mutex);
                gone = sessions[id];
                sessions.erase(id);
            }
            std::cout << "disconnected: " << id << std::endl;
        } else if (msg->type == ix::WebSocketMessageType::Message) {
            std::shared_ptr<Session> s;
            {
                std::lock_guard lock(sessions_mutex);
                s = sessions[id];
                if (!s) s = sessions[id] = std::make_shared<Session>();
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
