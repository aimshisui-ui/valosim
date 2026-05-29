#include "Agent.h"

#include <unordered_set>

namespace vlr {

namespace {

constexpr std::array<const char*, 4> kRoleNames = {"Duelist", "Initiator", "Controller", "Sentinel"};

std::vector<Agent> g_agents;
std::vector<GameMap> g_maps;
std::vector<CompPlan> g_comps;
std::vector<Badge> g_badges;

void init_agents() {
    if (!g_agents.empty()) return;
    // Canonical 29-agent roster: 7 Controllers, 8 Duelists, 7 Initiators,
    // 7 Sentinels. Mappings reflect the user's authoritative spec — each
    // agent's three Attr values are the player attributes that "matter
    // most" for that agent's identity (used by agent_synergy_bonus + the
    // agent_identity_bonus flavour helper in Match.cpp).
    //
    // Naming notes:
    //   - "Miks" and "Veto" are taken verbatim from the user's spec.
    //   - "KAY/O" was renamed to "Kayo" per spec.
    //   - Gekko was retained even though the user didn't list it
    //     (additive, not replacement).
    g_agents = {
        // === CONTROLLERS (7) =========================================
        {"Astra",      Role::Controller, Attr::Utility,      Attr::GameSense,     Attr::Anchor},
        {"Brimstone",  Role::Controller, Attr::Utility,      Attr::GameSense,     Attr::Positioning},
        {"Clove",      Role::Controller, Attr::Aim,          Attr::Aggressiveness, Attr::Utility},
        {"Viper",      Role::Controller, Attr::Lurking,      Attr::Anchor,        Attr::GameSense},
        {"Harbor",     Role::Controller, Attr::Adaptability, Attr::Utility,       Attr::GameSense},
        {"Miks",       Role::Controller, Attr::Utility,      Attr::Adaptability,  Attr::GameSense},
        {"Omen",       Role::Controller, Attr::Utility,      Attr::Positioning,   Attr::Adaptability},

        // === DUELISTS (8) ============================================
        {"Iso",        Role::Duelist,    Attr::Aim,          Attr::Headshot,      Attr::DecisionMaking},
        {"Jett",       Role::Duelist,    Attr::Headshot,     Attr::Aim,           Attr::Reaction},
        {"Neon",       Role::Duelist,    Attr::Aim,          Attr::Movement,      Attr::Entry},
        {"Phoenix",    Role::Duelist,    Attr::Aim,          Attr::Headshot,      Attr::Entry},
        {"Raze",       Role::Duelist,    Attr::Movement,     Attr::Utility,       Attr::Aim},
        {"Reyna",      Role::Duelist,    Attr::DecisionMaking, Attr::Aim,         Attr::Headshot},
        {"Waylay",     Role::Duelist,    Attr::Movement,     Attr::Utility,       Attr::Aim},
        {"Yoru",       Role::Duelist,    Attr::GameSense,    Attr::Utility,       Attr::Intelligence},

        // === INITIATORS (7 — 6 from user + Gekko) ====================
        {"Breach",     Role::Initiator,  Attr::Utility,      Attr::GameSense,     Attr::Aggressiveness},
        {"Sova",       Role::Initiator,  Attr::Utility,      Attr::GameSense,     Attr::Communication},
        {"Fade",       Role::Initiator,  Attr::Utility,      Attr::GameSense,     Attr::Communication},
        {"Kayo",       Role::Initiator,  Attr::Utility,      Attr::GameSense,     Attr::Adaptability},
        {"Skye",       Role::Initiator,  Attr::Utility,      Attr::Adaptability,  Attr::Aggressiveness},
        {"Tejo",       Role::Initiator,  Attr::Utility,      Attr::GameSense,     Attr::Aim},
        {"Gekko",      Role::Initiator,  Attr::Utility,      Attr::Adaptability,  Attr::Communication},

        // === SENTINELS (7) ===========================================
        {"Chamber",    Role::Sentinel,   Attr::Headshot,     Attr::Lurking,       Attr::Anchor},
        {"Cypher",     Role::Sentinel,   Attr::Lurking,      Attr::Anchor,        Attr::Utility},
        {"Deadlock",   Role::Sentinel,   Attr::Utility,      Attr::GameSense,     Attr::Anchor},
        {"Killjoy",    Role::Sentinel,   Attr::Anchor,       Attr::Utility,       Attr::GameSense},
        {"Sage",       Role::Sentinel,   Attr::Anchor,       Attr::Aim,           Attr::Utility},
        {"Vyse",       Role::Sentinel,   Attr::Utility,      Attr::Positioning,   Attr::GameSense},
        {"Veto",       Role::Sentinel,   Attr::Aim,          Attr::Headshot,      Attr::Utility}
    };
}

void init_maps() {
    if (!g_maps.empty()) return;
    g_maps = {
        {"Ascent",   Role::Initiator,  Attr::Utility,    Attr::GameSense,       Attr::Positioning},
        {"Bind",     Role::Controller, Attr::GameSense,  Attr::Utility,         Attr::Communication},
        {"Haven",    Role::Sentinel,   Attr::Clutch,     Attr::Adaptability,    Attr::Positioning},
        {"Split",    Role::Sentinel,   Attr::Utility,    Attr::DecisionMaking,  Attr::GameSense},
        {"Icebox",   Role::Duelist,    Attr::Aim,        Attr::Entry,           Attr::Positioning},
        {"Breeze",   Role::Duelist,    Attr::Aim,        Attr::Communication,   Attr::Adaptability},
        {"Fracture", Role::Initiator,  Attr::Entry,      Attr::Utility,         Attr::Aggressiveness},
        {"Pearl",    Role::Controller, Attr::GameSense,  Attr::Aim,             Attr::Positioning},
        {"Lotus",    Role::Duelist,    Attr::Entry,      Attr::GameSense,       Attr::Adaptability},
        {"Sunset",   Role::Controller, Attr::GameSense,  Attr::Clutch,          Attr::DecisionMaking},
        {"Abyss",    Role::Initiator,  Attr::Aim,        Attr::Utility,         Attr::Positioning}
    };
}

// === Canonical comp shape under the 1D/1IGL/1C/1S/1Flex roster system ===
// Every team's STARTING 5 has exactly one Duelist, one IGL, one Controller,
// one Sentinel, and one Flex (the wildcard slot). The four CompTag values
// (DoubleInitiator / DoubleDuelist / DoubleController / DoubleSentinel) are
// now CLASSIFICATIONS of which role the Flex player extends on a given map:
//
//   DoubleInitiator   -> Flex plays an Initiator agent that map (most common
//                        — Initiator is the "second" role most often doubled
//                        since IGL is its own slot)
//   DoubleDuelist     -> Flex plays a Duelist agent that map (dive maps)
//   DoubleController  -> Flex plays a Controller agent that map (Bind/Pearl)
//   DoubleSentinel    -> Flex plays a Sentinel agent that map (Haven/Lotus)
//
// The CompPlan "need" array still totals 5, with one role bucket at 2; we
// preserve that data shape so all the comp_fit/comp_synergy plumbing keeps
// working. comp_tag_of() identifies which role got doubled and that doubled
// role IS the Flex's per-map agent role. See PROJECT_GUIDE §4.2 + §4.7.1.
void init_comps() {
    if (!g_comps.empty()) return;
    auto add = [](int d, int i, int c, int s) {
        CompPlan p;
        p.need[0] = d; p.need[1] = i; p.need[2] = c; p.need[3] = s;
        g_comps.push_back(p);
    };
    add(1, 2, 1, 1);  // Double Initiator
    add(2, 1, 1, 1);  // Double Duelist
    add(1, 1, 2, 1);  // Double Controller
    add(1, 1, 1, 2);  // Double Sentinel
}

void init_badges() {
    if (!g_badges.empty()) return;
    auto bd = [](const char* n, std::vector<BadgeMod> mods) {
        return Badge{std::string(n), std::move(mods)};
    };
    g_badges.reserve(28);
    g_badges.push_back(bd("Ranked Junkie",   {{Attr::Headshot,5},{Attr::Aim,5},{Attr::Utility,-3},{Attr::Aggressiveness,5}}));
    g_badges.push_back(bd("Aimmer",          {{Attr::Headshot,5},{Attr::Aggressiveness,10}}));
    g_badges.push_back(bd("Calm Aim",        {{Attr::Headshot,3},{Attr::Aim,3},{Attr::Clutch,2}}));
    g_badges.push_back(bd("Giga Brain",      {{Attr::Clutch,10},{Attr::Intelligence,10},{Attr::Utility,5},{Attr::Aggressiveness,-20}}));
    g_badges.push_back(bd("Scrimmer",        {{Attr::Utility,10},{Attr::DecisionMaking,10},{Attr::Clutch,5},{Attr::Aggressiveness,-5}}));
    g_badges.push_back(bd("Entry Fragger",   {{Attr::Entry,10},{Attr::Aggressiveness,15},{Attr::DecisionMaking,-5}}));
    g_badges.push_back(bd("Support Main",    {{Attr::Utility,15},{Attr::Intelligence,5},{Attr::Aggressiveness,-10}}));
    g_badges.push_back(bd("Clutch Minister", {{Attr::Clutch,15},{Attr::Intelligence,5},{Attr::GameSense,5}}));
    g_badges.push_back(bd("Ace Machine",     {{Attr::Aim,6},{Attr::Headshot,10}}));
    g_badges.push_back(bd("First Strike",    {{Attr::Entry,7}}));
    g_badges.push_back(bd("Unkillable",      {{Attr::Clutch,5}}));
    g_badges.push_back(bd("Map Specialist",  {{Attr::GameSense,5}}));
    g_badges.push_back(bd("Inconsistent Aim",{{Attr::Aim,5}}));
    g_badges.push_back(bd("Baiter",          {{Attr::Clutch,10},{Attr::Entry,-15},{Attr::Aggressiveness,-20}}));
    g_badges.push_back(bd("Tilt Queue",      {{Attr::Aggressiveness,15},{Attr::DecisionMaking,-20}}));
    g_badges.push_back(bd("LAN Dodging",     {{Attr::Aim,4}}));
    g_badges.push_back(bd("W-Key Warrior",   {{Attr::Entry,15},{Attr::Aggressiveness,25},{Attr::GameSense,-15},{Attr::Utility,-15}}));
    g_badges.push_back(bd("Utility Nerd",    {{Attr::Utility,15},{Attr::Aim,-10}}));
    g_badges.push_back(bd("All Brain No Aim",{{Attr::Intelligence,20},{Attr::GameSense,15},{Attr::Aim,-20},{Attr::Headshot,-15}}));
    g_badges.push_back(bd("All Aim No Brain",{{Attr::Aim,20},{Attr::Headshot,15},{Attr::Intelligence,-20},{Attr::GameSense,-15}}));
    g_badges.push_back(bd("Stat Padder",     {{Attr::Aim,5},{Attr::DecisionMaking,-10},{Attr::Utility,-5}}));
    g_badges.push_back(bd("Team Player",     {{Attr::Utility,15},{Attr::Aggressiveness,-10},{Attr::Aim,-5}}));
    g_badges.push_back(bd("IGL",             {{Attr::Leadership,20},{Attr::Communication,15},{Attr::Intelligence,10},{Attr::Aim,-10}}));
    g_badges.push_back(bd("Lurker",          {{Attr::GameSense,15},{Attr::Positioning,15},{Attr::Entry,-15},{Attr::Aggressiveness,-10}}));
    g_badges.push_back(bd("Flex God",        {{Attr::Adaptability,20},{Attr::GameSense,10},{Attr::Utility,10}}));
    g_badges.push_back(bd("Choker",          {{Attr::Clutch,-25},{Attr::Aim,10}}));
    g_badges.push_back(bd("Onliner",         {{Attr::Aim,15},{Attr::Adaptability,-15}}));
    g_badges.push_back(bd("Trade Fragger",   {{Attr::Positioning,15},{Attr::GameSense,10},{Attr::Entry,-15}}));
    g_badges.push_back(bd("Over-peeker",     {{Attr::Aggressiveness,25},{Attr::Positioning,-15},{Attr::DecisionMaking,-15}}));
}

}  // namespace

const char* role_name(Role r) noexcept {
    auto i = static_cast<std::size_t>(r);
    return i < kRoleNames.size() ? kRoleNames[i] : "?";
}
Role role_from_str(std::string_view s) noexcept {
    for (std::size_t i = 0; i < kRoleNames.size(); ++i) {
        if (s == kRoleNames[i]) return static_cast<Role>(i);
    }
    return Role::Count;
}

const std::vector<Agent>& agents() { init_agents(); return g_agents; }
const std::vector<GameMap>& maps() { init_maps(); return g_maps; }

const Agent* find_agent_by_name(std::string_view name) {
    // Lazily-built name->index lookup. Cleared on first call after
    // init_agents() populates the vector. Linear scan would also be fine
    // (29 entries) but this is O(1) and used in hot duel loops.
    static std::unordered_map<std::string, std::size_t> idx;
    const auto& v = agents();
    if (idx.size() != v.size()) {
        idx.clear();
        idx.reserve(v.size());
        for (std::size_t i = 0; i < v.size(); ++i) idx.emplace(v[i].name, i);
    }
    auto it = idx.find(std::string(name));
    if (it == idx.end()) return nullptr;
    return &v[it->second];
}

const std::vector<CompPlan>& valid_comps() { init_comps(); return g_comps; }

CompPlan random_comp() {
    auto& v = valid_comps();
    return v[static_cast<std::size_t>(rng().irange(0, static_cast<int>(v.size()) - 1))];
}

const char* comp_tag_name(CompTag t) noexcept {
    switch (t) {
        case CompTag::DoubleInitiator:  return "Double Initiator";
        case CompTag::DoubleDuelist:    return "Double Duelist";
        case CompTag::DoubleController: return "Double Controller";
        case CompTag::DoubleSentinel:   return "Double Sentinel";
        default:                        return "?";
    }
}
// Identify the comp shape from a CompPlan. Under the new 1/1/1/1/1 + Flex
// canonical system, the role whose `need` bucket is doubled IS the role
// the Flex player picks an agent in for that map. So a DoubleInitiator
// tag literally means "Flex plays an Initiator agent on this map".
CompTag comp_tag_of(const CompPlan& p) {
    if (p.need[1] == 2) return CompTag::DoubleInitiator;
    if (p.need[0] == 2) return CompTag::DoubleDuelist;
    if (p.need[2] == 2) return CompTag::DoubleController;
    if (p.need[3] == 2) return CompTag::DoubleSentinel;
    return CompTag::DoubleInitiator;
}
const CompPlan& comp_by_tag(CompTag t) {
    return valid_comps()[static_cast<std::size_t>(t)];
}

const std::vector<Badge>& badges() { init_badges(); return g_badges; }

const Badge* find_badge(std::string_view name) {
    for (auto& b : badges()) {
        if (b.name == name) return &b;
    }
    return nullptr;
}

// === Per-map comp preferences ============================================
// Hand-tuned, leans toward Valorant-meta flavour but biased for diversity
// (no two maps share the exact same primary/secondary/flavour triplet).
// Used by the per-map agent selection in Team.cpp so that comps actually
// shift map-to-map within a single series instead of locking in to the
// deterministic best-rating pick.
const MapCompPreference& map_pref(const std::string& map_name) {
    static const std::unordered_map<std::string, MapCompPreference> table = [] {
        std::unordered_map<std::string, MapCompPreference> t;
        t.emplace("Ascent",   MapCompPreference{CompTag::DoubleInitiator,  CompTag::DoubleController, false, false, true });
        t.emplace("Bind",     MapCompPreference{CompTag::DoubleController, CompTag::DoubleInitiator,  true,  false, false});
        t.emplace("Haven",    MapCompPreference{CompTag::DoubleSentinel,   CompTag::DoubleInitiator,  false, false, true });
        t.emplace("Split",    MapCompPreference{CompTag::DoubleSentinel,   CompTag::DoubleInitiator,  true,  false, true });
        t.emplace("Icebox",   MapCompPreference{CompTag::DoubleDuelist,    CompTag::DoubleInitiator,  false, true,  false});
        t.emplace("Breeze",   MapCompPreference{CompTag::DoubleInitiator,  CompTag::DoubleController, true,  false, false});
        t.emplace("Fracture", MapCompPreference{CompTag::DoubleInitiator,  CompTag::DoubleDuelist,    false, true,  false});
        t.emplace("Pearl",    MapCompPreference{CompTag::DoubleController, CompTag::DoubleInitiator,  true,  false, false});
        t.emplace("Lotus",    MapCompPreference{CompTag::DoubleSentinel,   CompTag::DoubleInitiator,  false, false, true });
        t.emplace("Sunset",   MapCompPreference{CompTag::DoubleDuelist,    CompTag::DoubleInitiator,  false, true,  false});
        t.emplace("Abyss",    MapCompPreference{CompTag::DoubleInitiator,  CompTag::DoubleSentinel,   true,  false, true });
        return t;
    }();
    static const MapCompPreference fallback{
        CompTag::DoubleInitiator, CompTag::DoubleDuelist, false, false, false
    };
    auto it = table.find(map_name);
    return it == table.end() ? fallback : it->second;
}

double agent_flavor_fit(const Agent& a, const MapCompPreference& pref) {
    // Curated lists (intersect — Cypher/Viper hit both lurk + anchor).
    static const std::unordered_set<std::string> kLurk   = {
        "Viper", "Cypher", "Yoru", "Chamber"
    };
    static const std::unordered_set<std::string> kDive   = {
        "Neon", "Raze", "Phoenix", "Jett"
    };
    static const std::unordered_set<std::string> kAnchor = {
        "Killjoy", "Cypher", "Sage", "Viper", "Chamber", "Deadlock", "Vyse"
    };
    double s = 0.0;
    if (pref.favors_lurk   && kLurk.count(a.name))   s += 1.0;
    if (pref.favors_dive   && kDive.count(a.name))   s += 1.0;
    if (pref.favors_anchor && kAnchor.count(a.name)) s += 1.0;
    return s;
}

}  // namespace vlr
