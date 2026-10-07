// Play art for the play-call menu (docs/PROPOSALS.md#the-play-menu). The
// layout follows the snap's setup in play_sim.cpp (PlaySim::setup,
// route_behavior, qb_behavior, carrier_behavior), minus noise and the defense.

#include "cfbsim/play/play_art.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

#include "cfbsim/field/geometry.h"
#include "cfbsim/play/play_sim.h"
#include "cfbsim/play/routes.h"

namespace cfbsim {

namespace {

// Any yard line works: the picture is in the play frame.
PlayFrame art_frame(bool flipped) { return PlayFrame(25.0, Hash::Middle, true, flipped); }

std::string role_of(const FormationSlot& slot) {
    if (!slot.pos) return "";
    switch (*slot.pos) {
        case Position::QB:
            return "qb";
        case Position::RB:
        case Position::FB:
            return "back";
        case Position::WR:
            return "wr";
        case Position::TE:
            return "te";
        case Position::OT:
        case Position::OG:
        case Position::C:
            return "ol";
        case Position::DE:
        case Position::DT:
            return "dl";
        case Position::MLB:
        case Position::OLB:
            return "lb";
        case Position::CB:
            return "cb";
        case Position::FS:
        case Position::SS:
            return "s";
        default:
            return "";
    }
}

std::vector<ArtPlayer> place(const Formation& f, const PlayFrame& frame) {
    std::vector<Vec2> aligned;
    try {
        aligned = align_formation(f, frame);
    } catch (const GeometryError& e) {
        throw PlayError("formation " + f.id + ": " + e.what());
    }
    std::vector<ArtPlayer> out;
    for (std::size_t k = 0; k < f.slots.size(); ++k) out.push_back({f.slots[k].name, role_of(f.slots[k]), aligned[k]});
    return out;
}

bool is_qb_stance(Stance s) { return s == Stance::UnderCenter || s == Stance::Gun || s == Stance::Pistol; }

}  // namespace

FormationArt formation_art(const Formation& formation) {
    return {formation.id, formation.name, formation.personnel, place(formation, art_frame(false))};
}

PlayArt play_art(const DataSet& data, const Play& play, bool flipped) {
    if (play.type != PlayType::Pass && play.type != PlayType::Run) {
        throw PlayError("play " + play.id + ": only pass and run plays have art");
    }
    const auto fit = data.formations.find(play.formation);
    if (fit == data.formations.end()) throw PlayError("play " + play.id + ": unknown formation '" + play.formation + "'");
    const Formation& f = fit->second;
    const PlayFrame frame = art_frame(flipped);

    PlayArt art;
    art.id = play.id;
    art.name = play.name;
    art.formation = play.formation;
    art.type = play.type;
    art.flip = play.flip;
    art.players = place(f, frame);

    const auto pos_of = [&](std::string_view slot) -> std::optional<Vec2> {
        for (const ArtPlayer& p : art.players) {
            if (p.slot == slot) return p.pos;
        }
        return std::nullopt;
    };
    std::optional<Vec2> center;
    std::optional<Vec2> qb;
    std::vector<double> line_x;
    for (std::size_t k = 0; k < f.slots.size(); ++k) {
        const FormationSlot& s = f.slots[k];
        if (s.stance == Stance::Snap && !center) center = art.players[k].pos;
        if (is_qb_stance(s.stance) && !qb) qb = art.players[k].pos;
        if (s.on_line) line_x.push_back(art.players[k].pos.x);
    }
    if (!qb) throw PlayError("formation " + f.id + " has no QB (under center or gun)");
    const double cx = center ? center->x : 0.0;
    std::optional<LineGaps> gaps;
    try {
        gaps = find_gaps(cx, line_x);
    } catch (const GeometryError&) {
        // Only a problem if something anchors to a gap (reported then).
    }

    // Authored play-frame points, mirrored with the play.
    const auto mirrored_points = [&](const std::vector<Vec2>& pts) {
        std::vector<Vec2> out;
        for (Vec2 p : pts) out.push_back({p.x * frame.mirror(), p.y});
        return out;
    };

    // Routes: each slot's first route job, its default branch.
    const std::string primary = play.pass && !play.pass->progression.empty() ? play.pass->progression.front() : "";
    for (const FormationSlot& s : f.slots) {
        const auto a = play.assignments.find(s.name);
        if (a == play.assignments.end() || a->second.sequence.empty()) continue;
        const std::vector<Job>& seq = a->second.sequence;
        const Job* job = nullptr;
        for (const Job& j : seq) {
            if (j.kind == JobKind::Route) {
                job = &j;
                break;
            }
        }
        if (!job) continue;
        const auto route = data.routes.find(job->route.route);
        if (route == data.routes.end()) {
            throw PlayError("play " + play.id + ": " + s.name + " runs unknown route '" + job->route.route + "'");
        }
        const Vec2 at = *pos_of(s.name);
        Side side = flipped ? mirrored(job->route.side) : job->route.side;
        double outside = outside_sign(at.x, side);
        // A back coming out of his protection releases on his side of the
        // center (PlaySim::route_behavior).
        if (seq.front().kind == JobKind::Block && seq.front().action == "check_release" && side != Side::None &&
            std::abs(at.x - cx) > 0.3) {
            side = at.x > cx ? Side::Right : Side::Left;
            outside = outside_sign(at.x, side);
        }
        RoutePlan plan = plan_route(route->second, job->route.depth, at, outside);
        clamp_to_field(plan, frame, data.tuning.play.sideline_margin);
        ArtRoute r{s.name, {at}, plan.end, s.name == primary};
        for (const PlannedPoint& p : plan.points) r.points.push_back(p.pos);
        art.routes.push_back(std::move(r));
    }

    if (play.pass) {
        const auto drop = data.qb.drops.find(play.pass->drop);
        if (drop == data.qb.drops.end()) throw PlayError("play " + play.id + ": unknown drop '" + play.pass->drop + "'");
        art.qb.push_back(*qb);
        for (Vec2 p : mirrored_points(drop->second.points)) art.qb.push_back(p);
        return art;
    }

    const RunSpec& run = *play.run;
    const std::optional<Vec2> carrier = pos_of(run.carrier);
    if (!carrier) throw PlayError("play " + play.id + ": no carrier slot " + run.carrier);
    AnchorContext ctx;
    ctx.frame = &frame;
    ctx.gaps = gaps ? &*gaps : nullptr;
    ctx.slot_pos = pos_of;
    art.path.push_back(*carrier);
    try {
        for (const PathPoint& p : run.path) art.path.push_back({resolve_x(p.x, ctx), resolve_y(p.y, ctx)});
    } catch (const GeometryError& e) {
        throw PlayError("play " + play.id + ": run path: " + e.what());
    }
    if (!run.qb_then.empty()) {
        const auto fake = data.qb.fakes.find(run.qb_then);
        if (fake == data.qb.fakes.end()) throw PlayError("play " + play.id + ": unknown fake '" + run.qb_then + "'");
        art.qb.push_back(*qb);
        for (Vec2 p : mirrored_points(fake->second.points)) art.qb.push_back(p);
    }
    // Pullers: flat behind the line to the play side, then up through the
    // hole: a kick-out to the D gap, a lead through the C gap. The real aim
    // depends on the defense; this is the picture.
    if (gaps) {
        const Side playside = flipped ? mirrored(run.direction) : run.direction;
        for (const FormationSlot& s : f.slots) {
            const auto a = play.assignments.find(s.name);
            if (a == play.assignments.end() || a->second.sequence.empty()) continue;
            const std::string& action = a->second.sequence.front().action;
            if (action != "pull_kick" && action != "pull_lead") continue;
            const Vec2 at = *pos_of(s.name);
            const double x = gaps->x(playside, action == "pull_kick" ? 'D' : 'C');
            art.pulls.push_back({s.name, {at, {at.x, at.y - 1.2}, {x, at.y - 1.2}, {x, at.y + 1.5}}});
        }
    }
    return art;
}

namespace {

double round2(double v) { return std::round(v * 100.0) / 100.0; }

Json point_json(Vec2 p) { return Json::array({round2(p.x), round2(p.y)}); }

Json points_json(const std::vector<Vec2>& pts) {
    Json out = Json::array();
    for (Vec2 p : pts) out.push_back(point_json(p));
    return out;
}

Json players_json(const std::vector<ArtPlayer>& players) {
    Json out = Json::array();
    for (const ArtPlayer& p : players) {
        out.push_back({{"slot", p.slot}, {"role", p.role}, {"x", round2(p.pos.x)}, {"y", round2(p.pos.y)}});
    }
    return out;
}

}  // namespace

Json playbook_art_json(const DataSet& data, const std::string& team_id) {
    const auto team = data.teams.find(team_id);
    if (team == data.teams.end()) throw PlayError("unknown team '" + team_id + "'");
    const auto book = data.playbooks.find(team->second.offense_playbook);
    if (book == data.playbooks.end()) throw PlayError("unknown playbook '" + team->second.offense_playbook + "'");

    // The formations the menu shows: the playbook's, in its order, then any
    // other a play uses, keeping only those with a pass or run play.
    std::vector<std::string> order = book->second.formations;
    Json plays = Json::array();
    for (const Play& p : book->second.plays) {
        if (p.type != PlayType::Pass && p.type != PlayType::Run) continue;
        if (std::find(order.begin(), order.end(), p.formation) == order.end()) order.push_back(p.formation);
        const PlayArt art = play_art(data, p);
        Json routes = Json::array();
        for (const ArtRoute& r : art.routes) {
            routes.push_back({{"slot", r.slot},
                              {"points", points_json(r.points)},
                              {"end", to_name(kRouteEndNames, r.end)},
                              {"primary", r.primary}});
        }
        Json pulls = Json::array();
        for (const ArtLine& l : art.pulls) pulls.push_back({{"slot", l.slot}, {"points", points_json(l.points)}});
        plays.push_back({{"id", art.id},
                         {"name", art.name},
                         {"formation", art.formation},
                         {"type", to_name(kPlayTypeNames, art.type)},
                         {"flip", art.flip},
                         {"routes", routes},
                         {"qb", points_json(art.qb)},
                         {"path", art.path.empty() ? Json(nullptr) : points_json(art.path)},
                         {"pulls", pulls}});
    }
    Json formations = Json::array();
    for (const std::string& id : order) {
        const bool used = std::any_of(plays.begin(), plays.end(), [&](const Json& p) { return p["formation"] == id; });
        const auto f = data.formations.find(id);
        if (!used || f == data.formations.end()) continue;
        const FormationArt art = formation_art(f->second);
        formations.push_back({{"id", art.id}, {"name", art.name}, {"personnel", art.personnel}, {"players", players_json(art.players)}});
    }
    return {{"team", team_id}, {"playbook", book->first}, {"formations", formations}, {"plays", plays}};
}

namespace {

// A diagram has fixed technique/width landmarks, not an opposing lineup.
// Normalize context-dependent anchors before using the regular geometry
// helpers, so slot dependencies and field coordinates retain their semantics.
class DefensiveBase {
  public:
    explicit DefensiveBase(const DataSet& data)
        : tuning_(data.tuning.defense), contain_outside_(data.tuning.blocking.contain_outside) {}

    Side side(Side s) const {
        if (s == Side::Strong || s == Side::Field) return Side::Right;
        if (s == Side::Weak || s == Side::Boundary) return Side::Left;
        if (s == Side::Left || s == Side::Right) return s;
        throw GeometryError("base diagram anchor needs a side");
    }

    double gap(Side s, const std::string& letter) const {
        if (letter.size() != 1) throw GeometryError("gap must be A..D");
        return gaps_.x(side(s), letter.front());
    }

    double contain(Side s) const { return sign(side(s)) * (3.0 + contain_outside_); }

    Anchor x(const Anchor& a) const {
        switch (a.kind) {
            case AnchorKind::Tech:
                return Anchor::at(sign(side(a.side)) * (tech(a.ref) + a.offset));
            case AnchorKind::Receiver: {
                const Side s = side(a.side);
                double landmark;
                if (a.ref == "#1") landmark = frame_.numbers_x(s);
                else if (a.ref == "#2") landmark = frame_.hash_x(s);
                else if (a.ref == "#3") landmark = sign(s) * outer();
                else throw GeometryError("unknown base width anchor '" + a.ref + "'");
                return Anchor::at(landmark + sign(s) * a.offset);
            }
            case AnchorKind::Gap:
                return Anchor::at(gap(a.side, a.ref) + a.offset);
            case AnchorKind::Numbers:
            case AnchorKind::Hash:
            case AnchorKind::Sideline: {
                Anchor out = a;
                out.side = side(a.side);
                // Relative sides sign offsets outward; absolute sides sign x.
                if (a.side != Side::Left && a.side != Side::Right) out.offset *= sign(out.side);
                return out;
            }
            default:
                return a;
        }
    }

    std::vector<ArtPlayer> players(Formation f, const Play* play = nullptr) const {
        if (f.unit != Unit::Defense) throw PlayError("formation " + f.id + " is not defensive");
        for (FormationSlot& s : f.slots) {
            if (play) {
                const auto a = play->assignments.find(s.name);
                if (a != play->assignments.end()) {
                    if (a->second.align_x) s.x = *a->second.align_x;
                    if (a->second.align_y) s.y = *a->second.align_y;
                }
            }
            s.x = x(s.x);
        }
        return place(f, frame_);
    }

    ArtZone zone(const Zone& z) const {
        AnchorContext ctx;
        ctx.frame = &frame_;
        const auto point = [&](const Anchor& ax, const Anchor& ay) {
            return Vec2{resolve_x(x(ax), ctx), resolve_y(ay, ctx)};
        };
        ArtZone out;
        out.drop = point(z.drop_x, z.drop_y);
        out.width = z.width;
        out.depth_min = z.depth_min;
        out.depth_max = z.depth_max;
        out.radius = z.radius;
        out.end_a = out.end_b = out.drop;
        if (z.ends) {
            out.end_a = point((*z.ends)[0].x, (*z.ends)[0].y);
            out.end_b = point((*z.ends)[1].x, (*z.ends)[1].y);
        }
        return out;
    }

  private:
    static double sign(Side s) { return s == Side::Left ? -1.0 : 1.0; }
    double outer() const { return 3.0 + tuning_.ghost_te_split; }
    double tech(const std::string& t) const {
        if (t == "0") return 0.0;
        if (t == "1") return 0.75;
        if (t == "2i") return 1.5 - tuning_.shade;
        if (t == "2") return 1.5;
        if (t == "3") return 1.5 + tuning_.shade;
        if (t == "4i") return 3.0 - tuning_.shade;
        if (t == "4") return 3.0;
        if (t == "5") return 3.0 + tuning_.shade;
        if (t == "7") return outer() - tuning_.shade;
        if (t == "6") return outer();
        if (t == "9") return outer() + tuning_.shade;
        throw GeometryError("unknown technique '" + t + "'");
    }

    const DefenseTuning& tuning_;
    double contain_outside_;
    PlayFrame frame_ = art_frame(false);
    LineGaps gaps_ = find_gaps(0.0, std::array<double, 5>{-3.0, -1.5, 0.0, 1.5, 3.0});
};

Json defensive_job_json(const DefensiveArtJob& job) {
    Json out = {{"slot", job.slot}, {"kind", to_string(job.kind)}};
    if (job.kind == JobKind::Zone) {
        const ArtZone& a = *job.area;
        Json area = {{"drop", point_json(a.drop)}};
        if (a.radius > 0.0) {
            area["shape"] = "capsule";
            area["ends"] = Json::array({point_json(a.end_a), point_json(a.end_b)});
            area["radius"] = round2(a.radius);
        } else {
            area["shape"] = "box";
            area["width"] = round2(a.width);
            area["depth"] = Json::array({round2(a.depth_min), round2(a.depth_max)});
        }
        out["area"] = std::move(area);
        out["points"] = points_json(job.points);
    } else if (job.kind == JobKind::Man || job.kind == JobKind::Spy) {
        out["at"] = point_json(job.at);
        if (job.kind == JobKind::Man) out["leverage"] = job.leverage;
    } else {
        out["points"] = points_json(job.points);
        out["contain"] = job.contain;
    }
    return out;
}

}  // namespace

DefensiveFormationArt defensive_formation_art(const DataSet& data, const Formation& formation) {
    try {
        return {formation.id, formation.name, formation.package, DefensiveBase(data).players(formation)};
    } catch (const GeometryError& e) {
        throw PlayError("formation " + formation.id + ": " + e.what());
    }
}

DefensivePlayArt defensive_play_art(const DataSet& data, const Play& play) {
    try {
        if (play.unit != Unit::Defense) throw PlayError("not a defensive play");
        const auto f = data.formations.find(play.formation);
        if (f == data.formations.end()) throw PlayError("unknown formation '" + play.formation + "'");
        const DefensiveBase base(data);
        DefensivePlayArt art{play.id, play.name, play.formation, play.coverage, base.players(f->second, &play), {}};
        for (const ArtPlayer& player : art.players) {
            const auto a = play.assignments.find(player.slot);
            if (a == play.assignments.end() || !a->second.pass) {
                throw PlayError("no pass responsibility for " + player.slot);
            }
            const Job& job = *a->second.pass;
            DefensiveArtJob drawn;
            drawn.slot = player.slot;
            drawn.kind = job.kind;
            switch (job.kind) {
                case JobKind::Zone: {
                    const auto z = data.zones.find(job.ref);
                    if (z == data.zones.end()) throw PlayError("unknown zone '" + job.ref + "' for " + player.slot);
                    drawn.area = base.zone(z->second);
                    drawn.points = {player.pos, drawn.area->drop};
                    break;
                }
                case JobKind::Man:
                    drawn.at = player.pos;
                    drawn.leverage = job.ref == "outside" ? "outside" : "inside";
                    break;
                case JobKind::Spy:
                    drawn.at = {0.0, 5.0};
                    break;
                case JobKind::Rush:
                case JobKind::Blitz:
                case JobKind::Stunt: {
                    const Side side = job.side == Side::None
                                          ? (player.pos.x < 0.0 ? Side::Left : Side::Right)
                                          : base.side(job.side);
                    const std::string& lane = job.gap.empty() ? job.ref : job.gap;
                    drawn.contain = lane.empty() || lane == "contain";
                    const double x = drawn.contain ? base.contain(side) : base.gap(side, lane);
                    drawn.points = {player.pos, {x, 0.0}, {x, -3.0}};
                    break;
                }
                default:
                    throw PlayError("unsupported pass responsibility '" + std::string(to_string(job.kind)) +
                                    "' for " + player.slot);
            }
            art.jobs.push_back(std::move(drawn));
        }
        return art;
    } catch (const GeometryError& e) {
        throw PlayError("play " + play.id + ": " + e.what());
    } catch (const PlayError& e) {
        throw PlayError("play " + play.id + ": " + e.what());
    }
}

Json defensive_playbook_art_json(const DataSet& data, const std::string& team_id) {
    const auto team = data.teams.find(team_id);
    if (team == data.teams.end()) throw PlayError("unknown team '" + team_id + "'");
    const auto book = data.playbooks.find(team->second.defense_playbook);
    if (book == data.playbooks.end()) throw PlayError("unknown playbook '" + team->second.defense_playbook + "'");
    if (book->second.side != PlaybookSide::Defense) throw PlayError("playbook " + book->first + " is not defensive");

    std::vector<std::string> order = book->second.formations;
    Json plays = Json::array();
    for (const Play& p : book->second.plays) {
        const DefensivePlayArt art = defensive_play_art(data, p);
        if (std::find(order.begin(), order.end(), p.formation) == order.end()) order.push_back(p.formation);
        Json jobs = Json::array();
        for (const DefensiveArtJob& job : art.jobs) jobs.push_back(defensive_job_json(job));
        plays.push_back({{"id", art.id}, {"name", art.name}, {"formation", art.formation}, {"coverage", art.coverage},
                         {"players", players_json(art.players)}, {"jobs", std::move(jobs)}});
    }
    Json formations = Json::array();
    for (const std::string& id : order) {
        const bool used = std::any_of(plays.begin(), plays.end(), [&](const Json& p) { return p["formation"] == id; });
        if (!used) continue;
        const DefensiveFormationArt art = defensive_formation_art(data, data.formations.at(id));
        formations.push_back({{"id", art.id}, {"name", art.name}, {"package", art.package},
                              {"players", players_json(art.players)}});
    }
    return {{"unit", "defense"}, {"team", team_id}, {"playbook", book->first},
            {"formations", std::move(formations)}, {"plays", std::move(plays)}};
}

}  // namespace cfbsim
