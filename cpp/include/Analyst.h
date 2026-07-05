#pragma once

#include "Common.h"

#include <memory>
#include <string>
#include <vector>

namespace vlr {

// 8 analyst archetypes — bias generation only (cosmetic, mirroring Scout/Coach
// personalities; they do NOT drive AI behaviour). The analyst is the THIRD
// hireable staff member (alongside Coach + Scout), specialised for the
// match-prep side: a strong analyst yields deeper, sharper opposition reports
// and (later, bounded) a per-map prep edge.
enum class AnalystPersonality : std::uint8_t {
    FilmGrinder,         // lives in the VOD room — deep, accurate reads
    MetaReader,          // reads the meta + comp trends fastest
    DemoAnalyst,         // data/demo-driven, sharp tendencies
    MacroStrategist,     // map control + prep planning
    AntiStratSpecialist, // elite at reading a specific opponent
    GeneralistAnalyst,   // balanced across the board
    DataModeler,         // numbers-first, cold accuracy
    VeteranTactician     // seasoned, well-rounded prep mind
};

const char* analyst_personality_name(AnalystPersonality p) noexcept;
const char* analyst_personality_blurb(AnalystPersonality p) noexcept;

// A managed staff member, mirroring Scout/Coach: each club has a head_analyst
// whose quality drives HOW DEEP / HOW ACCURATE its opposition scouting reports
// are, and (later) a bounded per-map prep edge. Hire/fire/poach via the analyst
// market. Never destroyed mid-world (retired ones persist in retired_analysts_).
class Analyst {
public:
    Analyst(std::string name, std::string region);

    std::uint64_t id = 0;   // stable, unique id (stamped in ctor; save handle)

    std::string name;
    std::string region;
    std::string country;        // English country name
    std::string country_iso;    // 2-letter ISO for the flag
    std::string team_name = "Free Agent";

    // Four core attributes (parallel to Scout's judgement/network/projection/
    // experience, but report/prep-oriented). tactical_read = comp/meta read;
    // opponent_insight = depth+accuracy of a specific-opponent report;
    // prep = pre-match preparation quality; experience = seasoning.
    int tactical_read    = 50;
    int opponent_insight = 50;
    int prep             = 50;
    int experience       = 50;
    int salary_k         = 50;

    int  contract_years = 2;
    int  contract_exp_year = 0;   // year the contract ends

    int  age = 35;
    bool is_retired = false;

    AnalystPersonality personality = AnalystPersonality::GeneralistAnalyst;

    int reputation     = 50;   // market standing, drives salary + the analyst market
    int career_reports = 0;    // opposition reports produced (parallels career_finds)
    int career_seasons = 0;

    // === Derived effects — the chain reports + per-map prep key off ===========
    // Composite quality 0..1 (the one number everything reads).
    double quality01() const noexcept;
    // How many sections the opposition report unlocks (1..5). Floor 1 so even a
    // weak/absent analyst gives the basic stub; elite unlocks key players +
    // tendencies + a recommended counter.
    int    report_depth_sections() const noexcept;
    // Fractional sharpening of derived report figures toward truth (0 at
    // opponent_insight<=50, ~0.6 at 99) — analog of Scout::band_tighten_mult.
    double report_accuracy_mult() const noexcept;
    // 0..1 prep strength the per-map-prep component folds into a BOUNDED,
    // clamped match tilt (NOT applied here — this is just the input).
    double prep_edge01() const noexcept;
    int    requested_salary_k() const noexcept;
};

using AnalystPtr = std::shared_ptr<Analyst>;

// Generate an analyst with a region-appropriate name + personality-biased stats,
// exactly mirroring generate_scout.
AnalystPtr generate_analyst(std::string region);

// Free-agent analyst pool for the analyst market. Refreshed each year-end.
std::vector<AnalystPtr> generate_analyst_market(int count_per_region);

}  // namespace vlr
