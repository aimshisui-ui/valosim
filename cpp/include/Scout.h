#pragma once

#include "Common.h"

#include <memory>
#include <string>
#include <vector>

namespace vlr {

// 8 scouting archetypes — bias generation only (cosmetic in WS-A; they do NOT
// yet drive AI discovery flavour). Hidden by default like coach personalities.
enum class ScoutPersonality : std::uint8_t {
    RegionalSpecialist,  // deep local knowledge, narrow reach
    YouthHunter,         // elite youth / ceiling projection
    NetworkBroker,       // huge reach + connections
    AnalyticsScout,      // numbers-driven, accurate reads
    JourneymanBirdDog,   // seasoned generalist
    StarChaser,          // drawn to proven names
    BargainHunter,       // finds cheap value
    Generalist           // balanced
};

const char* scout_personality_name(ScoutPersonality p) noexcept;
const char* scout_personality_blurb(ScoutPersonality p) noexcept;

// A managed staff member, mirroring Coach: each club has a head_scout whose
// quality drives WHAT/HOW WELL the club discovers talent and (for the user)
// the fog-of-war reveal. Hire/fire/poach via the scout market.
class Scout {
public:
    Scout(std::string name, std::string region);

    std::uint64_t id = 0;   // stable, unique id (stamped in ctor; save handle)

    std::string name;
    std::string region;
    std::string country;        // English country name
    std::string country_iso;    // 2-letter ISO for the flag
    std::string team_name = "Free Agent";

    // Four core attributes (parallel to Coach's tactical/development/leadership/
    // experience). judgement = potential-read accuracy; network = breadth/reach;
    // projection = youth/ceiling read; experience = seasoning.
    int judgement  = 50;
    int network    = 50;
    int projection = 50;
    int experience = 50;
    int salary_k   = 50;

    int  contract_years = 2;
    int  contract_exp_year = 0;   // year the contract ends

    int  age = 35;
    bool is_retired = false;

    ScoutPersonality personality = ScoutPersonality::Generalist;

    int reputation     = 50;   // market standing, drives salary + the scout market
    int career_finds   = 0;    // gems discovered (parallels coach career_titles)
    int career_seasons = 0;

    // === Derived effects — the single chain scouts -> pipeline/fog key off ===
    // Composite quality 0..1 (the one number everything reads).
    double quality01() const noexcept;
    // The USER's fog-of-war reveal assignments per season (replaces the old flat
    // 6 + prestige/25). Floors at 3 (never fully blind), elite ~10-12.
    int    reveal_credits() const noexcept;
    // Fractional shrink of the potential band toward the true value (0 at
    // judgement<=50, ~0.6 at 99) — a sharper scout reads the ceiling tighter.
    double band_tighten_mult() const noexcept;
    // AI discovery: small multiplier so a strong department lands the better find.
    double discovery_quality_mult() const noexcept;
    // AI discovery: relax the potential bar so a strong network surfaces more
    // borderline prospects (0..~6).
    int    discovery_pool_bonus() const noexcept;
    int    requested_salary_k() const noexcept;
};

using ScoutPtr = std::shared_ptr<Scout>;

// === Scouting KNOWLEDGE model (grounded — two axes) ==========================
// Coverage BREADTH (0..100): does this scout have eyes/contacts on that country or
// region? Derived from home country + neighbor clusters + home region + the
// `network` reach, shaped by `personality` (RegionalSpecialist = narrow+deep,
// NetworkBroker = broad) and gated by `reputation` for cross-region reach. This is
// what makes a low-tier scout "home + neighbors only", a typical tier-1 scout cover
// their whole region, and a world-class one either own one region or span two.
int scout_country_coverage(const Scout& s, const std::string& target_iso);
int scout_region_coverage(const Scout& s, const std::string& region);
// Read DEPTH / accuracy (0..100): how precise the read is once they DO cover the
// player — from judgement (potential accuracy) + projection (ceiling read).
int scout_read_depth(const Scout& s);

// Generate a scout with a region-appropriate name + personality-biased stats,
// exactly mirroring generate_coach.
ScoutPtr generate_scout(std::string region);

// Free-agent scout pool for the scout market. Refreshed each year-end.
std::vector<ScoutPtr> generate_scout_market(int count_per_region);

}  // namespace vlr
