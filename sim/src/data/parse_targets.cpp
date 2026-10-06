// The calibration targets (data/calibration/targets.json).

#include <cmath>
#include <iomanip>
#include <set>
#include <sstream>

#include "cfbsim/data/load.h"
#include "data/json_reader.h"

namespace cfbsim {

Targets parse_targets(const Json& json, const std::string& source) {
    using detail::Reader;
    const Reader root(json, source);
    root.allow_only({"format_version", "targets", "game"});
    detail::require_format_version(root);

    Targets out;
    std::set<std::pair<std::string, std::string>> seen;
    root.at("targets").for_each([&](const Reader& t) {
        t.allow_only({"metric", "kind", "value", "span", "note"});
        Target target;
        target.metric = t.string("metric");
        target.kind = t.string("kind");
        if (target.kind != "pass" && target.kind != "run" && target.kind != "both" && target.kind != "kickoff" &&
            target.kind != "punt" && target.kind != "field_goal") {
            t.at("kind").fail("expected pass, run, both, kickoff, punt, or field_goal");
        }
        target.value = t.number("value");
        target.span = t.number("span");
        if (!std::isfinite(target.value)) t.at("value").fail("expected a finite number");
        if (!(target.span > 0.0)) t.at("span").fail("expected a number above 0");
        target.note = t.string_or("note", "");
        if (!seen.emplace(target.metric, target.kind).second) {
            t.fail("a second target for '" + target.metric + "' (" + target.kind + ")");
        }
        out.list.push_back(std::move(target));
    });

    // The full-game targets: a value of its own, or one taken from a per-play target
    // (times a scale: a 0.61 completion rate is 61 in the batch table).
    if (const auto game = root.find("game")) {
        std::set<std::string> rows;
        game->for_each([&](const Reader& g) {
            g.allow_only({"row", "value", "from", "text"});
            GameTarget target;
            target.row = g.string("row");
            if (!rows.insert(target.row).second) g.fail("a second target for row '" + target.row + "'");
            const bool has_value = g.has("value"), has_from = g.has("from");
            if (has_value == has_from) g.fail("give either \"value\" or \"from\"");
            if (has_value) {
                target.value = g.number("value");
            } else {
                const Reader from = g.at("from");
                from.allow_only({"metric", "kind", "scale"});
                const std::string metric = from.string("metric"), kind = from.string("kind");
                const Target* found = nullptr;
                for (const Target& t : out.list) {
                    if (t.metric == metric && t.kind == kind) found = &t;
                }
                if (!found) from.fail("no per-play target for '" + metric + "' (" + kind + ")");
                target.value = found->value * from.number_or("scale", 1.0);
            }
            if (!std::isfinite(target.value)) g.fail("expected a finite number");
            if (const auto text = g.find("text")) {
                target.text = text->as_string();
            } else {
                std::ostringstream os;
                os << "~" << std::setprecision(3) << target.value;
                target.text = os.str();
            }
            out.game.push_back(std::move(target));
        });
    }
    return out;
}

Json to_json(const Targets& targets) {
    Json out = Json::array();
    for (const Target& t : targets.list) {
        out.push_back({{"metric", t.metric}, {"kind", t.kind}, {"value", t.value}, {"span", t.span}, {"note", t.note}});
    }
    return out;
}

}  // namespace cfbsim
