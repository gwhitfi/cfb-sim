// A game you can step through and play (see cfbsim/api/match.h).

#include "cfbsim/api/match.h"

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
    if (opt_.tracking) {
        opt_.game.tracking = [this](const std::string& name) -> std::unique_ptr<std::ostream> {
            return std::make_unique<CaptureStream>([this, name](std::string text) { tracked(name, std::move(text)); });
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

void Match::observe(const Json& line, const Json& state) {
    std::unique_lock lock(m_);
    outbox_.push_back({{"type", "line"}, {"line", line}, {"state", state}});
    if (opt_.pause_each_play && line.value("type", "") == "play") yield_to_caller(lock);
}

void Match::tracked(const std::string& name, std::string text) {
    std::lock_guard lock(m_);
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
    Json out = {{"messages", std::move(outbox_)}, {"prompt", nullptr}, {"done", done_}};
    outbox_ = Json::array();
    if (pending_ && !answered_) {
        out["prompt"] = {{"input", pending_->input},
                         {"team", pending_->team == 0 ? "home" : "away"},
                         {"suggestion", pending_->suggestion},
                         {"options", pending_->options},
                         {"state", pending_->state}};
    }
    if (!error_.empty()) out["error"] = error_;
    return out;
}

std::optional<std::string> Match::command(const Json& cmd) {
    std::lock_guard lock(m_);
    if (!cmd.is_object()) return "a command must be a JSON object";
    const std::string type = cmd.value("type", "");
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
    return "unknown command '" + type + "'";
}

std::string Match::log() const {
    if (!opt_.replay_log.empty()) return opt_.replay_log;
    return log_.str();
}

}  // namespace cfbsim
