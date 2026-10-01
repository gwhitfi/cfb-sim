#include "cfbsim/data/attributes.h"

#include <gtest/gtest.h>

#include <set>
#include <string>

#include "cfbsim/data/player.h"

using namespace cfbsim;

TEST(Attributes, KeysAreUniqueAndRoundTrip) {
    std::set<std::string_view> keys;
    for (const AttrInfo& row : kAttrInfo) {
        EXPECT_TRUE(keys.insert(row.key).second) << "duplicate key " << row.key;
        EXPECT_EQ(attr_from_key(row.key), row.attr);
    }
    EXPECT_EQ(keys.size(), kAttrCount);
    EXPECT_FALSE(attr_from_key("playbook_knowledge"));  // per scheme, not in the table
}

TEST(Attributes, IndexedByEnum) {
    Attributes a;
    a[Attr::Speed] = 91;
    a[Attr::PuntAccuracy] = 12;
    EXPECT_EQ(a[Attr::Speed], 91);
    EXPECT_EQ(a[Attr::PuntAccuracy], 12);
    EXPECT_EQ(a[Attr::Catching], 0);  // zero-initialized
}

TEST(Player, UnlistedKnowledgeAndFamiliarityDefaultToOne) {
    Player p;
    p.playbook_knowledge["spread"] = 78;
    p.position_familiarity[Position::WR] = 100;
    EXPECT_EQ(p.playbook_knowledge_for("spread"), 78);
    EXPECT_EQ(p.playbook_knowledge_for("pro_style"), 1);
    EXPECT_EQ(p.familiarity_at(Position::WR), 100);
    EXPECT_EQ(p.familiarity_at(Position::CB), 1);
}
