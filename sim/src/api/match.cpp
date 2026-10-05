// A game you can step through and play (see cfbsim/api/match.h).

#include "cfbsim/api/match.h"

#include <algorithm>
#include <functional>

namespace cfbsim {

namespace {

int team_index(const Json& cmd) {
    const std::string t = cmd.value("team", "");
    return t == "home" ? 0 : t == "away" ? 1 : -1;
}

// A stream that hands its text to a callback when it's destroyed: the game
// closes a snap's tracking file right after the play, and the Match passes
// the whole file on as one message.
class CaptureStream : public std::ostringstream {
  public:
    explicit CaptureStream(std::function<void(std::string)> done) : done_(std::move(done)) {}
    ~CaptureStream() override { done_(str()); }

  private:
    std::function<void(std::string)> done_;
};

}  // namespace

Match::Match(const DataSet& data, MatchOptions options) : data_(data), opt_(std::move(options)) {
    // The hooks run on the game thread, which only runs while it holds the
    // baton, so they can touch the Match's members freely.
    opt_.game.decide = [this](const Prompt& p) { return decide(p); };
    opt_.game.observe = [this](const Json& line, const Json& state) { observe(line, state); };
    opt_.game.live = [this](int team) { return opt_.user[static_cast<std::size_t>(team)]; };
    opt_.game.window = [this](std::int64_t tick) { window_tick(tick); };
    if (opt_.tracking) {
        opt_.game.tracking = [this](const std::string& name) -> std::unique_ptr<std::ostream> {
            auto s = std::make_unique<CaptureStream>([this, name](std::string text) { tracked(name, std::move(text)); });
            if (name.find("_between") != std::string::npos) {
                // Between plays: streamed in parts as it's written (flush_live).
                live_stream_ = s.get();
                live_name_ = name;
                live_sent_ = 0;
            }
            return s;
        };
    }
}

Match::~Match() {
    if (!thread_.joinable()) return;
    {
        std::lock_guard lock(m_);
        cancel_ = true;
        game_turn_ = true;
    }
    cv_.notify_all();
    thread_.join();
}

void Match::game_thread() {
    std::unique_lock lock(m_);
    cv_.wait(lock, [this] { return game_turn_; });
    lock.unlock();
    try {
        if (!cancel_) {
            if (opt_.replay_log.empty()) {
                play_game(data_, opt_.game, &log_);
            } else {
                std::istringstream in(opt_.replay_log);
                rerun_game(data_, in, opt_.game);
            }
        }
    } catch (const Cancelled&) {
        // The Match is going away.
    } catch (const std::exception& e) {
        lock.lock();
        error_ = e.what();
        lock.unlock();
    }
    lock.lock();
    done_ = true;
    pending_.reset();
    game_turn_ = false;
    lock.unlock();
    cv_.notify_all();
}

void Match::yield_to_caller(std::unique_lock<std::mutex>& lock) {
    game_turn_ = false;
    cv_.notify_all();
    cv_.wait(lock, [this] { return game_turn_; });
    if (cancel_) throw Cancelled{};
}

std::optional<Json> Match::decide(const Prompt& p) {
    if (p.live) return decide_live(p);
    std::unique_lock lock(m_);
    const std::size_t t = static_cast<std::size_t>(p.team);
    if (p.optional) {
        // Optional prompts (timeouts between plays) never wait: they take a
        // timeout the user asked for, and otherwise pass. (For the coach's
        // side, silence would pass too, so its answer is handed back.)
        if (!opt_.user[t]) return p.suggestion.is_null() ? std::nullopt : std::optional<Json>(p.suggestion);
        if (p.input == "timeout" && timeout_requests_[t] > 0) {
            --timeout_requests_[t];
            return Json::object();
        }
        return std::nullopt;
    }
    if (!opt_.user[t]) return std::nullopt;
    // A timeout asked for while the team has the ball is taken before the snap.
    if (p.input == "hike" && timeout_requests_[t] > 0 && p.options.value("timeouts", 0) > 0) {
        --timeout_requests_[t];
        return Json{{"play_clock", p.options.value("play_clock", 40.0)}, {"timeout", true}};
    }
    pending_ = p;
    answered_ = false;
    answer_.reset();
    while (!answered_) yield_to_caller(lock);
    pending_.reset();
    return std::move(answer_);
}

std::optional<Json> Match::decide_live(const Prompt& p) {
    std::lock_guard lock(m_);
    const std::size_t t = static_cast<std::size_t>(p.team);
    if (!opt_.user[t]) return p.suggestion.is_null() ? std::nullopt : std::optional<Json>(p.suggestion);
    if (p.input == "timeout") {
        if (timeout_requests_[t] <= 0) return std::nullopt;
        --timeout_requests_[t];
        return Json::object();
    }
    if (p.input == "hurry") {
        // (Like a timeout: a button any time, never a decision the game stops for.)
        if (!hurry_requests_[t]) return std::nullopt;
        hurry_requests_[t] = false;
        hurried_[t] = true;
        return Json::object();
    }
    // The user's answer, if one is waiting; otherwise the decision stays open.
    const Key key{p.input, p.team};
    live_next_[key] = p;  // (open this tick either way: answered, it stays answered while open)
    const auto it = std::find_if(queue_.begin(), queue_.end(), [&](const auto& q) { return q.first == key; });
    if (it != queue_.end()) {
        Json a = std::move(it->second);
        queue_.erase(it);
        return a;
    }
    return std::nullopt;
}

void Match::flush_live() {
    if (!live_stream_) return;
    std::string text = live_stream_->str();
    if (text.size() <= live_sent_) return;
    outbox_.push_back({{"type", "tracking"}, {"name", live_name_}, {"data", text.substr(live_sent_)}, {"partial", true}});
    live_sent_ = text.size();
}

void Match::window_tick(std::int64_t tick) {
    std::unique_lock lock(m_);
    flush_live();
    if (tick < 0) {
        // The window closed: nothing in it can be answered any more.
        live_.clear();
        live_next_.clear();
        answered_live_.clear();
        queue_.clear();
        hurry_requests_ = {false, false};  // (a hurry is for one window)
        hurried_ = {false, false};
        paused_for_.reset();
        in_window_ = false;
        return;
    }
    tick_ = tick;
    in_window_ = true;
    // The decisions open after this tick; a new one stops the game so the
    // caller sees it (and with no pace set, each waits for its answer).
    std::map<Key, Prompt> open = std::move(live_next_);
    live_next_.clear();
    std::optional<Key> fresh;
    for (const auto& [key, p] : open) {
        if (!live_.contains(key) && !fresh) fresh = key;
    }
    for (auto it = answered_live_.begin(); it != answered_live_.end();) {
        it = open.contains(*it) ? std::next(it) : answered_live_.erase(it);
    }
    live_ = std::move(open);
    if (fresh) paused_for_ = fresh;
    const auto unanswered = [&] {
        return std::any_of(live_.begin(), live_.end(), [&](const auto& kv) { return !answered_live_.contains(kv.first); });
    };
    for (bool stop = fresh.has_value();; stop = false) {
        // A side handed to the coach stops waiting for its user.
        std::erase_if(live_, [&](const auto& kv) { return !opt_.user[static_cast<std::size_t>(kv.first.second)]; });
        if (!queue_.empty() || timeout_requests_[0] > 0 || timeout_requests_[1] > 0 || hurry_requests_[0] ||
            hurry_requests_[1]) {
            return;  // on to the next tick
        }
        const bool hold = clocked_ ? tick >= to_tick_ : unanswered();
        if (!stop && !hold) return;
        yield_to_caller(lock);
    }
}

Json Match::prompt_json(const Prompt& p) {
    Json j = {{"input", p.input},
              {"team", p.team == 0 ? "home" : "away"},
              {"suggestion", p.live ? p.hint : p.suggestion},
              {"options", p.options},
              {"state", p.state}};
    if (p.live) j["live"] = true;
    return j;
}

void Match::observe(const Json& line, const Json& state) {
    std::unique_lock lock(m_);
    outbox_.push_back({{"type", "line"}, {"line", line}, {"state", state}});
    if (opt_.pause_each_play && line.value("type", "") == "play") yield_to_caller(lock);
}

void Match::tracked(const std::string& name, std::string text) {
    std::lock_guard lock(m_);
    if (live_stream_ && name == live_name_) {
        live_stream_ = nullptr;
        if (live_sent_ > 0) {
            // The rest of a streamed file.
            outbox_.push_back({{"type", "tracking"}, {"name", name},
                               {"data", text.substr(std::min(live_sent_, text.size()))}, {"partial", false}});
            return;
        }
        // (None of it went out as it was written, e.g. the walk-on before a
        // kick: it's sent whole, like any other file.)
    }
    outbox_.push_back({{"type", "tracking"}, {"name", name}, {"data", std::move(text)}});
}

Json Match::advance() {
    std::unique_lock lock(m_);
    const bool waiting = pending_ && !answered_;
    if (!done_ && !waiting) {
        if (!started_) {
            started_ = true;
            thread_ = std::thread([this] { game_thread(); });
        }
        game_turn_ = true;
        cv_.notify_all();
        cv_.wait(lock, [this] { return !game_turn_; });
    }
    Json out = {{"messages", std::move(outbox_)}, {"prompt", nullptr}, {"done", done_}, {"tick", tick_}};
    outbox_ = Json::array();
    Json live = Json::array();
    if (!done_) {
        for (const auto& [key, p] : live_) live.push_back(prompt_json(p));
    }
    out["live"] = live;
    out["window"] = in_window_ && !done_;
    if (pending_ && !answered_) {
        out["prompt"] = prompt_json(*pending_);
    } else if (!done_) {
        // Stopped between plays: the decision it stopped for (or one still waiting).
        const Prompt* p = nullptr;
        if (paused_for_ && live_.contains(*paused_for_) && !answered_live_.contains(*paused_for_)) p = &live_.at(*paused_for_);
        for (const auto& [key, q] : live_) {
            if (!p && !answered_live_.contains(key)) p = &q;
        }
        if (p) out["prompt"] = prompt_json(*p);
    }
    if (!error_.empty()) out["error"] = error_;
    return out;
}

std::optional<std::string> Match::command(const Json& cmd) {
    std::lock_guard lock(m_);
    if (!cmd.is_object()) return "a command must be a JSON object";
    const std::string type = cmd.value("type", "");
    if (type == "answer" && (!pending_ || answered_)) {
        // Between plays: a live decision (named, or the one advance() returned).
        const Prompt* p = nullptr;
        const std::string input = cmd.value("input", "");
        const int team = team_index(cmd);  // (needed only when both sides are the user's)
        for (const auto& [key, q] : live_) {
            if (input.empty() ? (paused_for_ && key == *paused_for_) : key.first == input && (team < 0 || key.second == team)) {
                p = &q;
            }
        }
        if (!p && input.empty()) {
            for (const auto& [key, q] : live_) {
                if (!p && !answered_live_.contains(key)) p = &q;
            }
        }
        if (!p) return input.empty() ? "nothing to answer" : "no " + input + " to answer now";
        Json a = cmd;
        a.erase("type");
        a.erase("input");
        a.erase("team");
        if (cmd.value("auto", false)) {
            if (p->hint.is_null()) return "the coach has no call here";
            a = p->hint;
        }
        if (const std::optional<std::string> error = check_answer(*p, a)) return error;
        const Key key{p->input, p->team};
        queue_.emplace_back(key, std::move(a));
        answered_live_.insert(key);
        return std::nullopt;
    }
    if (type == "run_to") {
        if (!cmd.contains("tick") || !cmd["tick"].is_number()) return "run_to needs a tick";
        to_tick_ = cmd["tick"].get<std::int64_t>();
        clocked_ = true;
        return std::nullopt;
    }
    if (type == "answer") {
        if (!pending_ || answered_) return "nothing to answer";
        if (cmd.value("auto", false)) {
            answer_.reset();
        } else {
            Json a = cmd;
            a.erase("type");
            if (const std::optional<std::string> error = check_answer(*pending_, a)) return error;
            answer_ = std::move(a);
        }
        answered_ = true;
        return std::nullopt;
    }
    const int t = team_index(cmd);
    if (type == "auto") {
        if (t < 0) return "team must be home or away";
        opt_.user[static_cast<std::size_t>(t)] = !cmd.value("on", true);
        // Turning the coach on answers a waiting prompt with its suggestion.
        if (cmd.value("on", true) && pending_ && !answered_ && pending_->team == t) {
            answer_.reset();
            answered_ = true;
        }
        return std::nullopt;
    }
    if (type == "timeout") {
        if (t < 0) return "team must be home or away";
        if (!opt_.user[static_cast<std::size_t>(t)]) return "the coach is running that team";
        ++timeout_requests_[static_cast<std::size_t>(t)];
        // A timeout during the hike prompt is taken right away.
        if (pending_ && !answered_ && pending_->input == "hike" && pending_->team == t &&
            pending_->options.value("timeouts", 0) > 0) {
            --timeout_requests_[static_cast<std::size_t>(t)];
            answer_ = Json{{"play_clock", cmd.value("play_clock", pending_->options.value("play_clock", 40.0))},
                           {"timeout", true}};
            answered_ = true;
        }
        return std::nullopt;
    }
    if (type == "hurry") {
        if (t < 0) return "team must be home or away";
        if (!opt_.user[static_cast<std::size_t>(t)]) return "the coach is running that team";
        if (!in_window_) return "only between plays";
        if (!hurried_[static_cast<std::size_t>(t)]) hurry_requests_[static_cast<std::size_t>(t)] = true;
        return std::nullopt;
    }
    return "unknown command '" + type + "'";
}

std::string Match::log() const {
    if (!opt_.replay_log.empty()) return opt_.replay_log;
    return log_.str();
}

}  // namespace cfbsim
