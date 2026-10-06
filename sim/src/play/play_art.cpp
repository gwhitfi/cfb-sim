// Play art for the play-call menu (docs/PROPOSALS.md#the-play-menu). The
// layout follows the snap's setup in play_sim.cpp (PlaySim::setup,
// route_behavior, qb_behavior, carrier_behavior), minus noise and the defense.

#include "cfbsim/play/play_art.h"

#include <algorithm>
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

}  // namespace cfbsim
