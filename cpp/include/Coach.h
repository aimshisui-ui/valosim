#pragma once

#include "Common.h"

#include <memory>
#include <string>
#include <vector>

namespace vlr {

// 16 distinct coaching archetypes — each one gives the coach a flavour that
// drives roster decisions, tactics, dev focus, and mid-season behaviour.
//
// The archetype is *hidden* by default in the UI (only revealed via god mode)
// so players have to learn coach reputations through observation.
enum class CoachPersonality : std::uint8_t {
    Disciplinarian,     // strict, low tolerance for poor form -> fast cuts
    PlayerFocused,      // morale-first, slow to bench
    TacticalGenius,     // huge tactical, average dev/leadership
    DevelopmentCoach,   // huge development, signs young rookies
    AggressiveRiskTaker,// pushes high-variance comps + signings
    StructuredMacro,    // disciplined utility/economy, stable
    EmotionDriven,      // form-driven, swings hot/cold
    AnalyticsHeavy,     // scouts by stats, ignores reputation
    VeteranMentor,      // experience cap, leans on older players
    HarshRebuilder,     // tears down underperformers fast
    Motivator,          // average tactics, big leadership
    Pragmatist,         // wins-now, signs proven veterans
    Innovator,          // experiments with off-meta comps
    DefensiveAnchor,    // sentinel-favouring, slow methodical
    EntryLover,         // duelist-favouring, aggro signings
    BudgetBalancer      // signs cheap, develops in-house
};

// Compact summary of personality for tooltips/UI.
const char* coach_personality_name(CoachPersonality p) noexcept;
const char* coach_personality_blurb(CoachPersonality p) noexcept;

// How aggressively a coach drops underperformers mid-season. Returns 0..1
// where 0.0 = never benches, 1.0 = cuts at first sign of weakness.
double personality_replacement_aggressiveness(CoachPersonality p) noexcept;

class Coach {
public:
    Coach(std::string name, std::string region);

    std::string name;
    std::string region;
    std::string country;        // English country name
    std::string country_iso;    // 2-letter ISO for the flag
    std::string team_name = "Free Agent";

    int tactical    = 50;
    int development = 50;
    int leadership  = 50;
    int experience  = 50;
    int salary_k    = 50;

    // Contract length in years remaining. Decremented each year-end. When it
    // hits 0 the coach becomes a free agent. Editable via god-mode UI so the
    // commissioner can extend or cut contracts without firing/rehiring.
    int  contract_years = 2;
    int  contract_exp_year = 0;   // year (game year) the contract ends

    int  age = 35;
    bool is_retired = false;

    CoachPersonality personality = CoachPersonality::Pragmatist;

    double match_synergy_mult() const noexcept;
    double dev_chance_mult() const noexcept;
    int    requested_salary_k() const noexcept;
};

using CoachPtr = std::shared_ptr<Coach>;

// Generate a new coach with a real first/last name drawn from the region's
// dominant countries. Personality is rolled from the 16 archetypes; stats
// are biased by the personality (e.g. TacticalGenius spawns with high
// tactical, DevelopmentCoach with high development).
CoachPtr generate_coach(std::string region);

// Free-agent coach pool — used by the Manager / Coach Market tab when the
// user wants to hire/fire. Refreshed each year-end.
std::vector<CoachPtr> generate_coach_market(int count_per_region);

}  // namespace vlr
