// The player behaviors (internal). Each is a subclass of
// Behavior (play/play_state.h). `final` means nothing can subclass it further,
// which also lets the compiler skip the virtual lookup when it knows the type.
#pragma once

#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cfbsim/core/rng.h"
#include "cfbsim/play/routes.h"
#include "play/lanes.h"
#include "play/play_state.h"

namespace cfbsim {

// ---------------------------------------------------------------------------
// Runs a route, then plays the ball when it's thrown to him.
// ---------------------------------------------------------------------------
class RouteRunner final : public Behavior {
  public:
    // `plan` is in the play frame; `start` is seconds after the snap.
    RouteRunner(const RoutePlan& plan, const PlayFrame& frame, double start, bool busted);

    std::string_view name() const override { return "route"; }
    Intent decide(const PlayState& play, int self, Actions& actions) override;
    std::optional<Vec2> predict(const PlayState& play, int self, double seconds) const override;
    void draw(const PlayState& play, int self, Json& shapes) const override;
    double last_break() const override { return last_break_; }

  private:
    struct Point {
        Vec2 pos;  // field frame
        bool is_break = false;
        bool decel = false;
    };

    Vec2 last_direction() const;
    Intent run_route(const PlayState& play, const Agent& me, std::size_t& next,
                     double since_snap) const;
    // Settle routes: the open spot near the end of the route (field frame).
    Vec2 settle_spot(const PlayState& play) const;

    std::string route_;
    Vec2 origin_;
    std::vector<Point> points_;
    RouteEnd end_;
    double start_;
    bool busted_;
    std::size_t next_ = 0;  // the waypoint he's heading for
    double last_break_ = -std::numeric_limits<double>::infinity();
    std::optional<Vec2> settle_;  // where he's settling (field frame)
    double settle_checked_ = -1.0;
};

// ---------------------------------------------------------------------------
// Pass protection (cfbsim_design.md#pass-protection): own an area,
// pick the rusher threatening it, set between him and the QB. A back
// (`check_release`) steps up and looks for a free rusher: with none by his
// check time he releases into his route; one who blocks releases later.
// ---------------------------------------------------------------------------
class PassBlocker final : public Behavior {
  public:
    struct Setup {
        double area_x = 0.0;    // play frame: the center of his area
        double area_half = 0.8;
        bool back = false;      // a back checking for a free rusher
        double release_at = std::numeric_limits<double>::infinity();  // back: s after the snap
        double block_release_at = std::numeric_limits<double>::infinity();  // back who stays in: s after the snap
        double lag = 0.2;       // s: how late he sees rushers
        bool misread = false;   // back: commits to the wrong rusher this snap
        double misread_hold = 0.6;  // s he stays on the wrong man
        double max_depth = 2.5; // yd behind his stance he may set
        Vec2 home;              // field frame: his set spot with nobody to block
        double start_y = -1.0;  // play frame: his stance
    };
    explicit PassBlocker(Setup setup) : s_(setup) {}

    std::string_view name() const override { return s_.back ? "check_release" : "pass_set"; }
    Intent decide(const PlayState& play, int self, Actions& actions) override;
    bool done(const PlayState& play, int self) const override;
    void draw(const PlayState& play, int self, Json& shapes) const override;
    Key key() const override { return Key::PassSet; }
    int block_target() const override { return target_; }
    BlockStyle block_style() const override { return BlockStyle::Pass; }

    // How much a rusher is his: sideways yd from his area (lower is
    // stronger); kNoClaim if the rusher isn't coming or is out of range.
    double claim(const PlayState& play, int defender) const;

    static constexpr double kNoClaim = 1e9;
    static constexpr double kBackClaim = 100.0;

  private:
    // The rusher he should take; `wrong`, if given, gets the best of the
    // ones he should leave because another blocker is going for him (not
    // one already blocked: nobody mistakes him for a free rusher).
    int pick_target(const PlayState& play, int self, int* wrong = nullptr) const;
    bool rushing(const PlayState& play, int defender, Vec2& pos, Vec2& vel) const;

    Setup s_;
    int target_ = -1;
    bool picked_up_ = false;  // a back with a rusher to block (now) stays in
    bool misread_done_ = false;
    double committed_until_ = -1.0;  // s after the snap: on the wrong man until then
};

// ---------------------------------------------------------------------------
// Run blocking (cfbsim_design.md#run-blocking): one behavior set
// up per action. Targets are resolved at the snap from the scheme's rules.
// ---------------------------------------------------------------------------
class RunBlocker final : public Behavior {
  public:
    enum class Action : std::uint8_t { Drive, Reach, Down, Combo, PullKick, PullLead, Stalk, Fire };
    struct Setup {
        Action action = Action::Fire;
        int target = -1;         // agent index, or -1 (Fire: block whoever shows up)
        int climb_to = -1;       // Combo, PullLead: the linebacker
        double playside = 1.0;   // play-frame x sign of the play side
        double lag = 0.2;
        std::vector<Vec2> path;  // field frame: a pull's path before he turns up
        Vec2 fire_spot;          // field frame: where he fires out with no target
    };
    explicit RunBlocker(Setup setup) : s_(std::move(setup)), target_(s_.target) {}

    std::string_view name() const override;
    Intent decide(const PlayState& play, int self, Actions& actions) override;
    void draw(const PlayState& play, int self, Json& shapes) const override;
    Key key() const override { return s_.action == Action::Stalk ? Key::None : Key::RunBlock; }
    int block_target() const override { return target_; }
    BlockStyle block_style() const override;

  private:
    Vec2 drive_direction(const PlayState& play) const;  // field frame

    Setup s_;
    int target_;
    bool climbing_ = false;
    std::size_t path_next_ = 0;
};

// ---------------------------------------------------------------------------
// Pass play QB: take the snap, drop, set, go through his reads, throw.
// ---------------------------------------------------------------------------
class PassingQb final : public Behavior {
  public:
    struct Setup {
        std::vector<Vec2> drop;  // field frame
        double set_seconds = 0.0;
        std::vector<int> reads;  // agent indices in progression order
        double look_seconds = 0.5;
        double delivery_seconds = 0.3;
        double perceive_sd = 0.1;   // sd of his bias on a read's openness
        double threshold = 0.6;     // perceived openness he needs, before it decays
        double peripheral_rate = 1.0;  // per second
        int hot = -1;                  // agent index of the hot read, or -1
        double pocket_lag = 0.2;       // s: how late he senses rushers
        double blind_lag = 0.1;        // s extra on his blind side
        double escape_chance = 0.5;    // throws it away instead of taking a sack
        bool hold = false;             // never throws (timing a protection)
        Vec2 snapper;                  // play frame: the snapper at the snap
        LaneSkill lane;                // how he sees the field when he runs
        double scramble_rate = 0.4;    // chance per second he takes off with nothing open
    };

    // `seed`: his own random stream (perception noise, peripheral vision),
    // drawn from the match RNG when he's created.
    PassingQb(Setup setup, std::uint64_t seed);

    std::string_view name() const override { return "qb_pass"; }
    Intent decide(const PlayState& play, int self, Actions& actions) override;
    void draw(const PlayState& play, int self, Json& shapes) const override;
    Key key() const override { return Key::Drop; }
    int eyes() const override;
    double pressure() const override { return pressure_; }
    bool carrying() const override { return tucked_; }
    bool committed() const override { return tucked_; }
    bool went_hot() const { return went_hot_; }
    bool scrambled() const { return stage_ == Stage::Scramble || tucked_; }

  private:
    enum class Stage : std::uint8_t { WaitForBall, Drop, Set, Read, Scramble, Deliver, Done };

    void look_at(std::size_t read, double now);
    double threshold(const PlayState& play) const;
    // Senses the rush: sets pressure_, threat_, and unblocked_.
    void sense(const PlayState& play, int self);
    // Where to stand in the pocket this tick (field frame).
    Vec2 pocket_spot(const PlayState& play, int self);
    // Takes off running (cfbsim_design.md#scrambles).
    void start_scramble(const PlayState& play, int self, Actions& actions);
    // Scrambling: runs by the lane AI, still looking to throw behind the line.
    Intent scramble(const PlayState& play, int self, Actions& actions);

    Setup s_;
    Rng rng_;

    Stage stage_ = Stage::WaitForBall;
    std::size_t drop_next_ = 0;
    std::size_t read_ = 0;      // index into reads
    double stage_start_ = 0.0;  // seconds after the snap
    double look_start_ = 0.0;   // when he started looking at the current read
    double set_time_ = -1.0;    // when he finished his drop
    double bias_ = 0.0;         // his misjudgment of the current read
    std::vector<double> seen_;  // last perceived openness of each read (-1 = never looked)
    int target_ = -1;           // who he's throwing to
    bool away_ = false;         // throwing it away

    double pressure_ = 0.0;     // sensed this tick
    double pressure_time_ = 0.0;  // integral of pressure over time (speeds his decay)
    int threat_ = -1;           // the biggest threat
    Vec2 escape_;               // field frame: away from the threats, weighted by threat
    bool unblocked_ = false;    // an unblocked rusher about to get to him
    bool went_hot_ = false;
    bool panic_rolled_ = false;
    bool frozen_ = false;       // he failed to get rid of it: he'll take the sack
    std::optional<Vec2> pocket_;  // field frame: where he's standing in the pocket

    // Scrambles.
    LaneRunner lane_;
    double last_read_done_ = -1.0;  // when he finished his progression the first time
    double next_probe_ = 0.0;       // s: when he next checks for a running lane
    double scramble_start_ = 0.0;
    bool tucked_ = false;           // past the line: he's a runner, no more throws
    std::vector<int> tried_;        // tacklers he has tried a move on
    double juke_seen_ = -1.0;
};

// ---------------------------------------------------------------------------
// Run play QB: take the snap, meet the carrier at the mesh, hand off, fake.
// ---------------------------------------------------------------------------
class HandoffQb final : public Behavior {
  public:
    // Field frame: where he meets the carrier, and the fake after the handoff.
    HandoffQb(Vec2 mesh_spot, int carrier, std::vector<Vec2> fake)
        : mesh_spot_(mesh_spot), carrier_(carrier), fake_(std::move(fake)) {}

    std::string_view name() const override { return "qb_handoff"; }
    Intent decide(const PlayState& play, int self, Actions& actions) override;
    Key key() const override { return Key::Handoff; }

  private:
    Vec2 mesh_spot_;
    int carrier_;
    std::vector<Vec2> fake_;
    bool handed_off_ = false;
    std::size_t fake_next_ = 0;
};

// ---------------------------------------------------------------------------
// Ball carrier (cfbsim_design.md#ball-carrier-pursuit-and-tackling):
// follow the designed path, make the run read, then run free by the lane AI,
// with evasion moves near contact. With no path he is free at once (a
// receiver after the catch, a returner).
// ---------------------------------------------------------------------------
class BallCarrier final : public Behavior {
  public:
    struct Mark {
        Vec2 pos;  // field frame
        std::string label;
    };
    struct Setup {
        std::vector<Mark> path;
        std::size_t free_after = 0;  // he is free once he reaches path[free_after]
        Vec2 start;                  // field frame
        std::vector<std::string> reads;  // bang, bend, bounce
        double playside = 1.0;       // play-frame x sign of the play side
        LaneSkill lane;
    };

    // `seed`: his own random stream (lane noise, read and move choices).
    BallCarrier(Setup setup, std::uint64_t seed);

    std::string_view name() const override { return free_ ? "run_free" : "ball_carrier"; }
    Intent decide(const PlayState& play, int self, Actions& actions) override;
    std::optional<Vec2> predict(const PlayState& play, int self, double seconds) const override;
    void draw(const PlayState& play, int self, Json& shapes) const override;
    bool carrying() const override { return true; }
    bool committed() const override { return free_; }

    bool is_free() const { return free_; }

  private:
    void make_read(const PlayState& play, int self, Actions& actions);
    void try_move(const PlayState& play, int self, Actions& actions);
    Intent run_free(const PlayState& play, int self);

    Setup s_;
    Rng rng_;
    LaneRunner lane_;
    std::size_t next_ = 0;
    bool free_ = false;
    std::optional<Vec2> read_dir_;  // field frame
    double read_until_ = -1.0;
    std::string read_;
    std::vector<int> tried_;        // tacklers he has tried a move on
    double juke_seen_ = -1.0;       // the latest juke result he has acted on
};

// Runs a free ball carrier: lane AI plus evasion moves and stepping out of
// bounds. Shared by BallCarrier and a scrambling QB.
Intent run_with_ball(const PlayState& play, int self, LaneRunner& lane, Rng& rng,
                     std::vector<int>& tried, double& juke_seen, Actions& actions,
                     std::optional<Vec2> bias = std::nullopt, double bias_cone = 0.0);

// Chases the ball carrier through his intercept point, keeping leverage
// (cfbsim_design.md#pursuit).
class Pursuer final : public Behavior {
  public:
    struct Setup {
        double quality = 1.0;  // share of the carrier's velocity he projects
        double lag = 0.2;      // s: how late he sees the carrier
        bool contain = false;  // keeps outside-in leverage
        Vec2 side{};           // field frame: if set, he stays this side of the carrier instead
                               // (kickoff coverage: the carrier on his inside shoulder)
        double effort = 1.0;   // share of top speed far from the play
        double breakdown = 4.0;  // yd/s he arrives at when breaking down in front of the carrier
        double breakdown_extra = 0.0;  // yd/s^2 added to breakdown_decel (kickoff coverage)
    };
    Pursuer() = default;
    explicit Pursuer(Setup setup) : s_(setup) {}

    std::string_view name() const override { return "pursue"; }
    Intent decide(const PlayState& play, int self, Actions& actions) override;
    void draw(const PlayState& play, int self, Json& shapes) const override;

  private:
    Setup s_;
    std::optional<Vec2> aim_;  // field frame (drawing)
};

// Everyone goes for a loose ball.
class LooseBall final : public Behavior {
  public:
    std::string_view name() const override { return "loose_ball"; }
    Intent decide(const PlayState& play, int self, Actions& actions) override;
};

// Where and when (seconds from now) the ball in the air comes down through
// chest height (or failing that, the ground).
std::optional<std::pair<Vec2, double>> ball_drop_spot(const PlayState& play);

// ===========================================================================
// Defense (cfbsim_design.md#defense-and-coverage)
// ===========================================================================

// A zone's area in the play frame: a box (width and a depth band) or, with a
// radius, a capsule (every point within `radius` of the segment end_a..end_b;
// equal ends make it a circle). The drop is where he goes with no threat.
struct ZoneArea {
    std::string id;
    Vec2 drop;  // the landmark
    double width = 10.0;
    double depth_min = 0.0;
    double depth_max = 15.0;
    Vec2 end_a, end_b;    // capsule: the segment's ends
    double radius = 0.0;  // > 0: a capsule
    bool deep = false;    // plays the deepest threat

    bool capsule() const { return radius > 0.0; }

    // The point on the capsule's segment nearest p.
    Vec2 nearest_on_segment(Vec2 p) const {
        const Vec2 seg = end_b - end_a;
        const double len2 = seg.length_squared();
        if (len2 <= 0.0) return end_a;
        return end_a + seg * std::clamp((p - end_a).dot(seg) / len2, 0.0, 1.0);
    }

    // In the area, or within `margin` yd of its edge.
    bool contains(Vec2 p, double margin = 0.0) const {
        if (capsule()) return (p - nearest_on_segment(p)).length() <= radius + margin;
        return std::abs(p.x - drop.x) <= width / 2.0 + margin && p.y >= depth_min - margin &&
               p.y <= depth_max + margin;
    }

    // The nearest point to p in the area grown by `margin`.
    Vec2 clamp(Vec2 p, double margin = 0.0) const {
        if (capsule()) {
            const Vec2 c = nearest_on_segment(p);
            const Vec2 d = p - c;
            const double len = d.length();
            const double r = radius + margin;
            return len <= r ? p : c + d * (r / len);
        }
        return {std::clamp(p.x, drop.x - width / 2.0 - margin, drop.x + width / 2.0 + margin),
                std::clamp(p.y, depth_min - margin, depth_max + margin)};
    }
};

// ---------------------------------------------------------------------------
// One defender: his run job, his pass job, and his belief about which this
// play is. The jobs are behaviors themselves; this one owns them (through
// std::unique_ptr) and passes each tick to whichever job his belief says.
// After a throw he plays the ball, after a catch he pursues.
// ---------------------------------------------------------------------------
class Defender final : public Behavior {
  public:
    // `pursuit`: how he chases. `disciplined`: he stays in his run fit until
    // the carrier commits (else he chases as soon as he sees the handoff).
    // `run_hold`: s after the snap before he plays a run (a linebacker in
    // coverage; 0 for everyone else).
    Defender(std::unique_ptr<Behavior> run_job, std::unique_ptr<Behavior> pass_job,
             double read_delay, double read_rate, double prior, Pursuer::Setup pursuit,
             bool disciplined, double run_hold = 0.0);

    std::string_view name() const override { return "defender"; }
    Intent decide(const PlayState& play, int self, Actions& actions) override;
    void draw(const PlayState& play, int self, Json& shapes) const override;
    int man_target() const override { return pass_job_->man_target(); }
    int covering() const override { return playing_pass() ? pass_job_->covering() : -1; }
    std::string job() const override { return pass_job_->job(); }
    int responsible() const override { return pass_job_->responsible(); }

    double belief() const { return p_pass_; }
    bool playing_pass() const { return p_pass_ >= 0.5; }

  private:
    void update_belief(const PlayState& play, int self);
    bool should_pursue(const PlayState& play, int self, int carrier) const;

    std::unique_ptr<Behavior> run_job_;
    std::unique_ptr<Behavior> pass_job_;
    Pursuer pursue_;
    double read_delay_;
    double read_rate_;
    double p_pass_;
    bool disciplined_;
    double run_hold_;
    bool pursuing_ = false;
};

// ---------------------------------------------------------------------------
// Man coverage on one receiver.
// ---------------------------------------------------------------------------
class ManCoverage final : public Behavior {
  public:
    // `leverage`: +1 outside, -1 inside. `cushion`: yd off the receiver at the snap.
    ManCoverage(int target, double leverage, double cushion, double lag)
        : target_(target), leverage_(leverage), cushion_(cushion), lag_(lag) {}

    std::string_view name() const override { return "man"; }
    Intent decide(const PlayState& play, int self, Actions& actions) override;
    void draw(const PlayState& play, int self, Json& shapes) const override;
    int man_target() const override { return target_; }
    int covering() const override { return target_; }

  private:
    int target_;
    double leverage_;
    double cushion_;
    double lag_;
    std::optional<double> start_y_;  // the receiver's play-frame y at the snap
    double drive_ = 0.0;             // yd of cushion given up driving on a stop
};

// ---------------------------------------------------------------------------
// Zone coverage: drop to the landmark, then play the threats in the area.
// ---------------------------------------------------------------------------
class ZoneCoverage final : public Behavior {
  public:
    // `late`: seconds he stands before dropping (a bust).
    ZoneCoverage(ZoneArea zone, double lag, double skill, double late)
        : zone_(std::move(zone)), lag_(lag), skill_(skill), late_(late) {}

    std::string_view name() const override { return "zone"; }
    Intent decide(const PlayState& play, int self, Actions& actions) override;
    void draw(const PlayState& play, int self, Json& shapes) const override;
    int covering() const override { return threat_; }
    std::string job() const override { return "zone:" + zone_.id; }
    int responsible() const override { return zone_.deep ? carrying_ : threat_; }

  private:
    Vec2 spot(const PlayState& play, int self);  // play frame; picks his threat

    ZoneArea zone_;
    double lag_;
    double skill_;
    double late_;
    Vec2 spot_;        // play frame, for drawing
    int threat_ = -1;  // the receiver he's playing (underneath zones)
    int carrying_ = -1;  // the deepest threat he's carrying (deep zones; for the record)
    Vec2 track_;         // play frame: the side-to-side velocity he's mirroring (underneath zones)
};

// ---------------------------------------------------------------------------
// Pass rush (cfbsim_design.md#pass-rush): through his lane, then
// at the QB; rush moves while he's engaged.
// ---------------------------------------------------------------------------
class Rusher final : public Behavior {
  public:
    struct Setup {
        double lane_x = 0.0;     // play frame: his lane at the line
        double contain = 0.0;    // contain rushers: the sign of his outside, else 0
        double power = 50.0;     // his power and finesse move skills
        double finesse = 50.0;
    };
    Rusher(Setup setup, std::uint64_t seed) : s_(setup), rng_(seed) {}

    std::string_view name() const override { return "rush"; }
    Intent decide(const PlayState& play, int self, Actions& actions) override;

  private:
    Setup s_;
    Rng rng_;
    double next_move_ = -1.0;  // s after the snap
    std::int64_t engaged_tick_ = -1;
    double answered_ = -1.0;   // the latest move result he has reacted to
    bool counter_ = false;     // the next move is a counter
};

// ---------------------------------------------------------------------------
// A run fit (cfbsim_design.md#run-fits): fill his gap, flowing
// with the ball carrier. A down lineman attacks it past the line.
// ---------------------------------------------------------------------------
class GapFit final : public Behavior {
  public:
    struct Setup {
        double gap_x = 0.0;      // play frame
        double depth = 1.0;      // play y he fills at
        double flow_share = 0.0; // toward the carrier's x ...
        double flow_max = 0.0;   // ... at most this far
        double lag = 0.2;
    };
    explicit GapFit(Setup setup) : s_(setup) {}

    std::string_view name() const override { return "gap"; }
    Intent decide(const PlayState& play, int self, Actions& actions) override;

  private:
    Setup s_;
};

// ---------------------------------------------------------------------------
// Go to a spot and hold it: run support and spies.
// ---------------------------------------------------------------------------
class HoldSpot final : public Behavior {
  public:
    // `spot`: field frame. `speed_share`: of his top speed.
    HoldSpot(std::string label, Vec2 spot, double speed_share)
        : label_(std::move(label)), spot_(spot), speed_share_(speed_share) {}

    std::string_view name() const override { return label_; }
    Intent decide(const PlayState& play, int self, Actions& actions) override;

  private:
    std::string label_;
    Vec2 spot_;
    double speed_share_;
};

}  // namespace cfbsim
