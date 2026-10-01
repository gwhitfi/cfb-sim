// Small member functions of the data types.

#include "cfbsim/data/formation.h"
#include "cfbsim/data/library.h"
#include "cfbsim/data/play.h"
#include "cfbsim/data/player.h"

namespace cfbsim {

double Player::playbook_knowledge_for(std::string_view scheme) const {
    const auto it = playbook_knowledge.find(scheme);
    return it != playbook_knowledge.end() ? it->second : kDefaultFamiliarity;
}

double Player::familiarity_at(Position pos) const {
    const auto it = position_familiarity.find(pos);
    return it != position_familiarity.end() ? it->second : kDefaultFamiliarity;
}

const Player* Roster::find(std::string_view id) const {
    for (const Player& p : players) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

const FormationSlot* Formation::slot(std::string_view slot_name) const {
    for (const FormationSlot& s : slots) {
        if (s.name == slot_name) return &s;
    }
    return nullptr;
}

std::optional<double> Route::break_depth() const {
    for (const Waypoint& w : points) {
        if (w.is_break) return w.pos.y;
    }
    return std::nullopt;
}

const Play* Playbook::find(std::string_view play_id) const {
    for (const Play& p : plays) {
        if (p.id == play_id) return &p;
    }
    return nullptr;
}

}  // namespace cfbsim
