// Parser for playbooks/*.json (plays are written inline in their playbook).

#include <algorithm>
#include <set>
#include <span>

#include "data/parse_common.h"
#include "data/vocab.h"

namespace cfbsim {

using detail::Reader;

namespace {

// Which job kinds each context accepts.
constexpr std::array kOffenseJobs{JobKind::Route, JobKind::Block};
constexpr std::array kDefenseRunJobs{JobKind::Gap, JobKind::Support, JobKind::Contain,
                                     JobKind::Pursue};
constexpr std::array kDefensePassJobs{JobKind::Rush, JobKind::Zone,  JobKind::Man,
                                      JobKind::Spy,  JobKind::Blitz, JobKind::Stunt};
constexpr std::array kSpecialJobs{
    JobKind::Kick,      JobKind::Punt,        JobKind::LongSnap, JobKind::Hold,    JobKind::Protect,
    JobKind::CoverLane, JobKind::Contain,     JobKind::Safety,   JobKind::Gunner,  JobKind::Jam,
    JobKind::HoldUp,    JobKind::ReturnBlock, JobKind::Return,   JobKind::Recover, JobKind::Rush,
    JobKind::Block,     JobKind::Route};

// std::span is a non-owning view of a contiguous array: it lets one function
// accept any of the std::arrays above, whatever their length.
bool allowed(std::span<const JobKind> kinds, JobKind kind) {
    return std::find(kinds.begin(), kinds.end(), kind) != kinds.end();
}

std::string gap_letter(const Reader& r) {
    const std::string gap = r.as_string();
    if (gap != "A" && gap != "B" && gap != "C" && gap != "D") r.fail("gap must be A, B, C, or D");
    return gap;
}

Side job_side(const Reader& r) {
    const Side side = r.as_enum(kSideNames, "side");
    if (side == Side::None) r.fail("side must not be 'none'");
    return side;
}

void require_true(const Reader& r, JobKind kind) {
    if (!r.is_bool() || !r.as_bool()) {
        r.fail("'" + std::string(to_string(kind)) + "' takes the value true");
    }
}

RouteRef parse_route_ref(const Reader& r, const Reader& route_value) {
    RouteRef ref;
    ref.route = route_value.as_string();
    if (auto depth = r.find("depth")) ref.depth = depth->as_number();
    if (auto side = r.find("side")) ref.side = job_side(*side);
    return ref;
}

// Parses one job object, e.g. {"route": "curl", "depth": 10} or
// {"block": {"do": "protect"}}. Does not follow "then".
Job parse_job(const Reader& r, std::span<const JobKind> kinds, std::string_view context) {
    r.require_object();
    std::optional<JobKind> found;
    std::string found_key;
    for (const auto& [key, value] : r.json().items()) {
        if (auto kind = from_name(kJobKindNames, key)) {
            if (found) r.fail("more than one job ('" + found_key + "' and '" + key + "')");
            if (!allowed(kinds, *kind)) {
                r.at(key).fail("'" + key + "' is not a valid " + std::string(context) + " job");
            }
            found = kind;
            found_key = key;
        } else if (key != "depth" && key != "side" && key != "option" && key != "then") {
            r.at(key).fail("unknown field '" + key + "'");
        }
    }
    if (!found) r.fail("no job given for this " + std::string(context) + " assignment");

    Job job;
    job.kind = *found;
    const Reader value = r.at(found_key);
    const bool route_like = job.kind == JobKind::Route;
    if (!route_like && (r.has("depth") || r.has("option"))) {
        r.fail("'depth' and 'option' only apply to routes");
    }
    if (!route_like && job.kind != JobKind::Gap && r.has("side")) {
        r.at("side").fail("'side' doesn't apply to '" + found_key + "'");
    }

    switch (job.kind) {
        case JobKind::Route:
            job.route = parse_route_ref(r, value);
            if (auto option = r.find("option")) {
                option->for_each_member([&](const std::string& when, const Reader& o) {
                    if (when != "vs_man" && when != "vs_zone") {
                        o.fail("option condition must be vs_man or vs_zone");
                    }
                    o.allow_only({"route", "depth", "side"});
                    job.options.push_back({when, parse_route_ref(o, o.at("route"))});
                });
            }
            break;
        case JobKind::Block:
            value.allow_only({"do", "target"});
            job.action = value.string("do");
            job.target = value.string_or("target", "");
            if (!detail::contains(detail::kBlockActions, job.action) &&
                !detail::contains(detail::kAssignmentOnlyBlockActions, job.action)) {
                value.at("do").fail("unknown block action '" + job.action + "'");
            }
            if (!job.target.empty() && !detail::contains(detail::kBlockTargets, job.target)) {
                value.at("target").fail("unknown block target '" + job.target + "'");
            }
            break;
        case JobKind::Gap:
            job.gap = gap_letter(value);
            job.side = job_side(r.at("side"));
            break;
        case JobKind::Support:
        case JobKind::Zone:
            job.ref = value.as_string();
            break;
        case JobKind::Man:
            job.target = value.as_string();
            break;
        case JobKind::Rush:
            value.allow_only({"lane", "gap", "side"});
            job.ref = value.string_or("lane", "");
            if (auto gap = value.find("gap")) job.gap = gap_letter(*gap);
            if (auto side = value.find("side")) job.side = job_side(*side);
            if (job.ref.empty() && job.gap.empty()) value.fail("rush needs a 'lane' or a 'gap'");
            break;
        case JobKind::Blitz:
            value.allow_only({"gap", "side"});
            job.gap = gap_letter(value.at("gap"));
            job.side = job_side(value.at("side"));
            break;
        case JobKind::Stunt:
            value.allow_only({"lane"});
            job.ref = value.string("lane");
            break;
        case JobKind::CoverLane:
            value.allow_only({"x"});
            job.lane_x = value.number("x");
            break;
        case JobKind::ReturnBlock:
            value.allow_only({"target", "rule"});
            job.target = value.string_or("target", "");
            job.ref = value.string_or("rule", "");
            if (job.target.empty() == job.ref.empty()) {
                value.fail("return_block needs exactly one of 'target' or 'rule'");
            }
            break;
        default:  // flag-style jobs: {"safety": true}
            require_true(value, job.kind);
            break;
    }
    return job;
}

// A job plus everything chained after it with "then".
std::vector<Job> parse_sequence(const Reader& r, std::span<const JobKind> kinds,
                                std::string_view context) {
    std::vector<Job> jobs{parse_job(r, kinds, context)};
    if (auto then = r.find("then")) {
        for (Job& next : parse_sequence(*then, kinds, context)) jobs.push_back(std::move(next));
    }
    return jobs;
}

Assignment parse_assignment(const Reader& r, Unit unit) {
    Assignment a;
    switch (unit) {
        case Unit::Offense:
            a.sequence = parse_sequence(r, kOffenseJobs, "offense");
            break;
        case Unit::Defense:
            r.allow_only({"run", "pass"});
            a.run = parse_job(r.at("run"), kDefenseRunJobs, "run defense");
            a.pass = parse_job(r.at("pass"), kDefensePassJobs, "pass defense");
            break;
        case Unit::Kicking:
        case Unit::Receiving:
            a.sequence = parse_sequence(r, kSpecialJobs, "special teams");
            break;
    }
    return a;
}

PassSpec parse_pass_spec(const Reader& r) {
    r.allow_only({"drop", "progression", "read_type", "hot", "protection"});
    PassSpec p;
    p.drop = r.string("drop");
    if (!detail::contains(detail::kDrops, p.drop))
        r.at("drop").fail("unknown drop '" + p.drop + "'");
    p.progression = r.at("progression").as_strings();
    if (p.progression.empty()) r.at("progression").fail("progression can't be empty");
    p.read_type = r.string_or("read_type", "progression");
    if (!detail::contains(detail::kReadTypes, p.read_type)) {
        r.at("read_type").fail("unknown read_type '" + p.read_type + "'");
    }
    p.hot = r.string_or("hot", "");
    const Reader protection = r.at("protection");
    protection.allow_only({"scheme", "direction"});
    p.protection = protection.string("scheme");
    if (auto dir = protection.find("direction")) p.protection_direction = job_side(*dir);
    return p;
}

RunSpec parse_run_spec(const Reader& r) {
    r.allow_only({"scheme", "direction", "carrier", "qb_action", "path", "free_after", "reads"});
    RunSpec s;
    s.scheme = r.string("scheme");
    s.direction = job_side(r.at("direction"));
    s.carrier = r.string("carrier");
    if (auto qb = r.find("qb_action")) {
        qb->allow_only({"do", "then"});
        s.qb_action = qb->string("do");
        s.qb_then = qb->string_or("then", "");
    }
    r.at("path").for_each(
        [&](const Reader& p) { s.path.push_back(detail::parse_path_point(p, Unit::Offense)); });
    if (s.path.empty()) r.at("path").fail("a run path needs at least one point");
    s.free_after = r.string_or("free_after", "");
    if (!s.free_after.empty()) {
        bool matches = false;
        for (const PathPoint& p : s.path) matches = matches || p.type == s.free_after;
        if (!matches) r.at("free_after").fail("no path point has type '" + s.free_after + "'");
    }
    s.reads = r.strings_or_empty("reads");
    for (const std::string& read : s.reads) {
        if (!detail::contains(detail::kRunReads, read))
            r.at("reads").fail("unknown read '" + read + "'");
    }
    return s;
}

Play parse_play(const Reader& r) {
    r.allow_only({"id", "name", "side", "type", "formation", "flip", "coverage", "pass", "run",
                  "motion", "kick", "return", "assignments", "tags"});
    Play play;
    play.id = r.string("id");
    play.name = r.string("name");
    play.unit = r.at("side").as_enum(kUnitNames, "side");
    play.formation = r.string("formation");
    play.flip = r.boolean_or("flip", false);

    // Which play types each unit may use.
    if (play.unit == Unit::Defense) {
        play.type = PlayType::Defense;
        if (auto type = r.find("type")) {
            if (type->as_enum(kPlayTypeNames, "play type") != PlayType::Defense) {
                type->fail("defensive plays have type 'defense'");
            }
        }
        play.coverage = r.string_or("coverage", "");
    } else {
        play.type = r.at("type").as_enum(kPlayTypeNames, "play type");
        const PlayType t = play.type;
        const bool ok =
            (play.unit == Unit::Offense && (t == PlayType::Pass || t == PlayType::Run)) ||
            (play.unit == Unit::Kicking &&
             (t == PlayType::Kickoff || t == PlayType::Punt || t == PlayType::FieldGoal)) ||
            (play.unit == Unit::Receiving &&
             (t == PlayType::KickoffReturn || t == PlayType::PuntReturn ||
              t == PlayType::FieldGoalBlock));
        if (!ok) {
            r.at("type").fail("type '" + std::string(to_string(t)) + "' isn't valid for a " +
                              std::string(to_string(play.unit)) + " play");
        }
    }

    // Play-wide blocks: required for their type, rejected otherwise.
    if (play.type == PlayType::Pass) {
        play.pass = parse_pass_spec(r.at("pass"));
    } else if (r.has("pass")) {
        r.at("pass").fail("'pass' is only for pass plays");
    }
    if (play.type == PlayType::Run) {
        play.run = parse_run_spec(r.at("run"));
    } else if (r.has("run")) {
        r.at("run").fail("'run' is only for run plays");
    }
    if (auto motion = r.find("motion")) {
        motion->allow_only({"slot", "path", "type"});
        MotionSpec m;
        m.slot = motion->string("slot");
        m.type = motion->string_or("type", "");
        motion->at("path").for_each(
            [&](const Reader& p) { m.path.push_back(detail::parse_path_point(p, play.unit)); });
        play.motion = std::move(m);
    }
    if (auto kick = r.find("kick")) {
        kick->allow_only({"kind", "target", "hang"});
        KickSpec k;
        k.kind = kick->string("kind");
        k.hang = kick->string_or("hang", "");
        if (auto target = kick->find("target")) {
            if (!(target->is_string() && target->as_string() == "auto")) {
                k.target = detail::parse_path_point(*target, play.unit);
            }
        }
        play.kick = std::move(k);
    }
    if (auto ret = r.find("return")) {
        ret->allow_only({"scheme", "aim", "free_after"});
        ReturnSpec s;
        s.scheme = ret->string("scheme");
        s.aim = detail::parse_path_point(ret->at("aim"), play.unit);
        s.free_after = ret->string_or("free_after", "");
        play.return_spec = std::move(s);
    }

    r.at("assignments").for_each_member([&](const std::string& slot, const Reader& a) {
        play.assignments.emplace(slot, parse_assignment(a, play.unit));
    });

    play.tags = r.strings_or_empty("tags");
    for (const std::string& tag : play.tags) {
        const bool known = play.unit == Unit::Defense ? detail::contains(detail::kDefenseTags, tag)
                                                      : detail::contains(detail::kOffenseTags, tag);
        if (!known) r.at("tags").fail("unknown tag '" + tag + "'");
    }
    return play;
}

bool unit_fits(PlaybookSide side, Unit unit) {
    switch (side) {
        case PlaybookSide::Offense:
            return unit == Unit::Offense;
        case PlaybookSide::Defense:
            return unit == Unit::Defense;
        case PlaybookSide::Special:
            return unit == Unit::Kicking || unit == Unit::Receiving;
    }
    return false;
}

}  // namespace

Playbook parse_playbook(const Json& json, const std::string& source) {
    const Reader root(json, source);
    root.allow_only(
        {"format_version", "id", "name", "side", "scheme", "strength_call", "formations", "plays"});
    detail::require_format_version(root);

    Playbook book;
    book.id = root.string("id");
    book.name = root.string("name");
    book.side = root.at("side").as_enum(kPlaybookSideNames, "playbook side");
    book.scheme = root.string("scheme");
    book.strength_call = root.strings_or_empty("strength_call");
    for (const std::string& call : book.strength_call) {
        if (!detail::contains(detail::kStrengthCalls, call)) {
            root.at("strength_call").fail("unknown strength call '" + call + "'");
        }
    }
    if (book.side == PlaybookSide::Defense && book.strength_call.empty()) {
        root.fail("defensive playbooks need a 'strength_call'");
    }
    book.formations = root.at("formations").as_strings();

    std::set<std::string> ids;
    root.at("plays").for_each([&](const Reader& r) {
        Play play = parse_play(r);
        if (!unit_fits(book.side, play.unit)) {
            r.at("side").fail("a " + std::string(to_string(play.unit)) + " play can't go in a " +
                              std::string(to_string(book.side)) + " playbook");
        }
        if (!ids.insert(play.id).second) r.at("id").fail("duplicate play id '" + play.id + "'");
        book.plays.push_back(std::move(play));
    });
    return book;
}

}  // namespace cfbsim
