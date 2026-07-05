#include "Analyst.h"

#include "Country.h"
#include "Names.h"

#include <array>

namespace vlr {

Analyst::Analyst(std::string n, std::string r)
    : name(std::move(n)), region(std::move(r)) { id = next_entity_id(); }

double Analyst::quality01() const noexcept {
    return clamp_v((tactical_read * 0.35 + opponent_insight * 0.35 +
                    prep * 0.20 + experience * 0.10) / 99.0, 0.0, 1.0);
}

int Analyst::report_depth_sections() const noexcept {
    // opponent_insight + reputation gate how much of the opponent the report
    // exposes. Floor 1 (a weak/absent analyst still gives the basic stub),
    // elite ~5 (key players + tendencies + recommended counter).
    return clamp_v(1 + opponent_insight / 22 + reputation / 45, 1, 5);
}

double Analyst::report_accuracy_mult() const noexcept {
    // opponent_insight <=50 -> 0 (vaguest figures); 99 -> ~0.6 (sharp reads).
    return clamp_v((opponent_insight - 50) / 82.0, 0.0, 0.6);
}

double Analyst::prep_edge01() const noexcept {
    // prep <=50 -> 0; 99 -> ~1.0. Folded into the bounded per-map prep tilt.
    return clamp_v((prep - 50) / 49.0, 0.0, 1.0);
}

int Analyst::requested_salary_k() const noexcept {
    // Mirror Scout::requested_salary_k over the analyst's four attrs (same
    // weighting as quality01 so the ask tracks the composite).
    double quality = (tactical_read * 0.35) + (opponent_insight * 0.35) +
                     (prep * 0.20) + (experience * 0.10);
    double q01 = clamp_v(quality / 99.0, 0.0, 1.0);
    int base = 30 + static_cast<int>(q01 * q01 * 600.0);
    double rep_mult = 0.85 + 0.45 * (reputation / 99.0);
    base = static_cast<int>(base * rep_mult);
    return clamp_v(base, 15, kSalaryCapK);
}

const char* analyst_personality_name(AnalystPersonality p) noexcept {
    switch (p) {
        case AnalystPersonality::FilmGrinder:         return "Film Grinder";
        case AnalystPersonality::MetaReader:          return "Meta Reader";
        case AnalystPersonality::DemoAnalyst:         return "Demo Analyst";
        case AnalystPersonality::MacroStrategist:     return "Macro Strategist";
        case AnalystPersonality::AntiStratSpecialist: return "Anti-Strat Specialist";
        case AnalystPersonality::GeneralistAnalyst:   return "Generalist";
        case AnalystPersonality::DataModeler:         return "Data Modeler";
        case AnalystPersonality::VeteranTactician:    return "Veteran Tactician";
    }
    return "?";
}

const char* analyst_personality_blurb(AnalystPersonality p) noexcept {
    switch (p) {
        case AnalystPersonality::FilmGrinder:         return "Lives in the VOD room \xE2\x80\x94 deep, accurate reads";
        case AnalystPersonality::MetaReader:          return "Reads the meta + comp trends fastest";
        case AnalystPersonality::DemoAnalyst:         return "Data/demo-driven, sharp tendencies";
        case AnalystPersonality::MacroStrategist:     return "Map control + prep planning specialist";
        case AnalystPersonality::AntiStratSpecialist: return "Elite at reading a specific opponent";
        case AnalystPersonality::GeneralistAnalyst:   return "Balanced across the board";
        case AnalystPersonality::DataModeler:         return "Numbers-first, cold accuracy";
        case AnalystPersonality::VeteranTactician:    return "Seasoned, well-rounded prep mind";
    }
    return "";
}

namespace {

// Bias the four core stats {tactical_read, opponent_insight, prep, experience}
// around the archetype (parallel to scout personality_stat_bias).
std::array<int, 4> personality_stat_bias(AnalystPersonality p) {
    using A = std::array<int, 4>;
    switch (p) {
        case AnalystPersonality::FilmGrinder:         return A{72, 82, 75, 60};
        case AnalystPersonality::MetaReader:          return A{86, 70, 60, 60};
        case AnalystPersonality::DemoAnalyst:         return A{84, 78, 58, 55};
        case AnalystPersonality::MacroStrategist:     return A{78, 64, 84, 64};
        case AnalystPersonality::AntiStratSpecialist: return A{74, 90, 68, 56};
        case AnalystPersonality::GeneralistAnalyst:   return A{62, 62, 62, 62};
        case AnalystPersonality::DataModeler:         return A{88, 70, 56, 55};
        case AnalystPersonality::VeteranTactician:    return A{70, 68, 70, 88};
    }
    return A{55, 55, 55, 55};
}

AnalystPersonality random_personality() {
    return static_cast<AnalystPersonality>(rng().irange(0, 7));
}

}  // namespace

AnalystPtr generate_analyst(std::string region) {
    Region reg = region_from_str(region);
    const Country& country = pick_country_in_region(reg);
    PlayerIdentity ident = make_identity(country);
    std::string display_name = ident.first + " " + ident.last;

    auto an = std::make_shared<Analyst>(std::move(display_name), std::move(region));
    an->country     = country.name;
    an->country_iso = country.iso;
    an->personality = random_personality();

    auto bias = personality_stat_bias(an->personality);
    int spread = 10;
    an->tactical_read    = clamp_attr(bias[0] + rng().irange(-spread, spread));
    an->opponent_insight = clamp_attr(bias[1] + rng().irange(-spread, spread));
    an->prep             = clamp_attr(bias[2] + rng().irange(-spread, spread));
    an->experience       = clamp_attr(bias[3] + rng().irange(-spread, spread));
    an->age         = rng().irange(28, 55);
    // Vary starting reputation BEFORE salary so the ask reflects standing.
    an->reputation  = rng().irange(28, 72);
    an->career_seasons = rng().irange(0, 6);
    an->salary_k    = an->requested_salary_k();
    an->contract_years = rng().irange(1, 4);
    return an;
}

std::vector<AnalystPtr> generate_analyst_market(int count_per_region) {
    std::vector<AnalystPtr> out;
    out.reserve(static_cast<std::size_t>(count_per_region) * 3);
    for (auto* r : kRegions) {
        for (int i = 0; i < count_per_region; ++i) {
            out.push_back(generate_analyst(r));
        }
    }
    return out;
}

}  // namespace vlr
