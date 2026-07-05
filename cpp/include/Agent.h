#pragma once

#include "Common.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace vlr {

enum class Role : std::uint8_t { Duelist, Initiator, Controller, Sentinel, Count };

const char* role_name(Role r) noexcept;
Role role_from_str(std::string_view s) noexcept;

struct Agent {
    std::string name;
    Role role;
    Attr a1;
    Attr a2;
    Attr a3;
};

struct GameMap {
    std::string name;
    Role favored_role;
    Attr a1;
    Attr a2;
    Attr a3;
};

const std::vector<Agent>& agents();
const std::vector<GameMap>& maps();

// Look up an agent by exact name. Returns nullptr if not found.
// Backed by a name->index hash built lazily on first call. Used by both
// Match.cpp (per-agent identity bonuses) and Player.cpp (mastery /
// pool selection by named agent).
const Agent* find_agent_by_name(std::string_view name);

struct CompPlan {
    std::array<int, static_cast<std::size_t>(Role::Count)> need{};
};

const std::vector<CompPlan>& valid_comps();
CompPlan random_comp();

enum class CompTag : std::uint8_t {
    DoubleInitiator = 0,
    DoubleDuelist   = 1,
    DoubleController = 2,
    DoubleSentinel  = 3,
    Count
};

const char* comp_tag_name(CompTag t) noexcept;
CompTag     comp_tag_of(const CompPlan& p);
const CompPlan& comp_by_tag(CompTag t);

// Per-map preference vector that biases comp / agent selection. Each map
// declares a primary + secondary preferred CompTag plus three flavour
// flags (lurk-heavy, dive-heavy, anchor-heavy). Used by the new per-map
// agent picker in Team::build_round_selection so different maps within a
// single series produce different comps.
struct MapCompPreference {
    CompTag primary   = CompTag::DoubleInitiator;
    CompTag secondary = CompTag::DoubleInitiator;
    bool    favors_lurk   = false;
    bool    favors_dive   = false;
    bool    favors_anchor = false;
};
const MapCompPreference& map_pref(const std::string& map_name);

// Returns 0..3 — sums 1.0 for each lurk/dive/anchor flavour where the
// agent is in the curated string-match list AND the map favours that
// flavour. Hand-curated by name inside Agent.cpp.
double agent_flavor_fit(const Agent& a, const MapCompPreference& pref);

// Per-map SIGNATURE agents — the 2-3 agents that are meta-near-mandatory on a
// map (Viper on Breeze/Icebox, Killjoy/Cypher on lockdown maps, Sova on recon
// maps, ...). Unlike map_pref (a role/flavour bias), this names SPECIFIC agents
// a map demands, so the per-map picker prefers them and a forced off-role flex
// targets the agent the map actually needs. Hand-curated in Agent.cpp; an empty
// list for an unknown map. is_map_signature_agent is the cheap hot-path check.
const std::vector<std::string>& map_signature_agents(const std::string& map_name);
bool is_map_signature_agent(const std::string& agent_name,
                            const std::string& map_name);

// Per-map result snapshot used by Series for in-series adaptation.
// Kept in Agent.h (instead of Series.h) so it can be passed by-pointer
// into Team::build_round_selection without dragging Series.h around.
struct MapResultEntry {
    std::string map_name;
    CompTag     t1_comp = CompTag::DoubleInitiator;
    CompTag     t2_comp = CompTag::DoubleInitiator;
    int         t1_score = 0;
    int         t2_score = 0;
    bool        t1_won  = false;  // false also covers ties / t2 wins
};

struct BadgeMod {
    Attr stat;
    int  delta;
};
struct Badge {
    std::string name;
    std::vector<BadgeMod> mods;
};

const std::vector<Badge>& badges();
const Badge* find_badge(std::string_view name);

}  // namespace vlr
