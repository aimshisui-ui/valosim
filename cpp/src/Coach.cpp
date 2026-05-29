#include "Coach.h"

#include "Country.h"
#include "Names.h"

#include <array>

namespace vlr {

Coach::Coach(std::string n, std::string r)
    : name(std::move(n)), region(std::move(r)) {}

double Coach::match_synergy_mult() const noexcept {
    double t = clamp_v(tactical / 100.0, 0.0, 1.0);
    double l = clamp_v(leadership / 100.0, 0.0, 1.0);
    return 1.00 + 0.18 * t + 0.04 * l;
}

double Coach::dev_chance_mult() const noexcept {
    double d = clamp_v(development / 99.0, 0.0, 1.0);
    return 0.85 + 0.45 * d;
}

int Coach::requested_salary_k() const noexcept {
    double quality = (tactical * 0.45) + (development * 0.30) +
                     (leadership * 0.15) + (experience * 0.10);
    double q01 = clamp_v(quality / 99.0, 0.0, 1.0);
    int base = 30 + static_cast<int>(q01 * q01 * 600.0);
    return clamp_v(base, 15, 999);
}

const char* coach_personality_name(CoachPersonality p) noexcept {
    switch (p) {
        case CoachPersonality::Disciplinarian:      return "Disciplinarian";
        case CoachPersonality::PlayerFocused:       return "Player-Focused";
        case CoachPersonality::TacticalGenius:      return "Tactical Genius";
        case CoachPersonality::DevelopmentCoach:    return "Development Coach";
        case CoachPersonality::AggressiveRiskTaker: return "Aggressive Risk-Taker";
        case CoachPersonality::StructuredMacro:     return "Structured Macro";
        case CoachPersonality::EmotionDriven:       return "Emotion-Driven";
        case CoachPersonality::AnalyticsHeavy:      return "Analytics-Heavy";
        case CoachPersonality::VeteranMentor:       return "Veteran Mentor";
        case CoachPersonality::HarshRebuilder:      return "Harsh Rebuilder";
        case CoachPersonality::Motivator:           return "Motivator";
        case CoachPersonality::Pragmatist:          return "Pragmatist";
        case CoachPersonality::Innovator:           return "Innovator";
        case CoachPersonality::DefensiveAnchor:     return "Defensive Anchor";
        case CoachPersonality::EntryLover:          return "Entry Lover";
        case CoachPersonality::BudgetBalancer:      return "Budget Balancer";
    }
    return "?";
}

const char* coach_personality_blurb(CoachPersonality p) noexcept {
    switch (p) {
        case CoachPersonality::Disciplinarian:      return "Strict, low tolerance for poor form";
        case CoachPersonality::PlayerFocused:       return "Morale-first, slow to bench";
        case CoachPersonality::TacticalGenius:      return "Master of X's and O's";
        case CoachPersonality::DevelopmentCoach:    return "Builds talent from rookies up";
        case CoachPersonality::AggressiveRiskTaker: return "Bold comp + signing decisions";
        case CoachPersonality::StructuredMacro:     return "Disciplined utility and economy";
        case CoachPersonality::EmotionDriven:       return "Rides hot streaks, panics on cold";
        case CoachPersonality::AnalyticsHeavy:      return "Trusts the numbers over reputation";
        case CoachPersonality::VeteranMentor:       return "Leans on experienced veterans";
        case CoachPersonality::HarshRebuilder:      return "Tears down underperformers fast";
        case CoachPersonality::Motivator:           return "Average tactics, huge leadership";
        case CoachPersonality::Pragmatist:          return "Wins-now mentality, proven players";
        case CoachPersonality::Innovator:           return "Experiments with off-meta comps";
        case CoachPersonality::DefensiveAnchor:     return "Slow methodical sentinel-heavy play";
        case CoachPersonality::EntryLover:          return "Aggressive duelist-first signings";
        case CoachPersonality::BudgetBalancer:      return "Signs cheap, develops in-house";
    }
    return "";
}

double personality_replacement_aggressiveness(CoachPersonality p) noexcept {
    switch (p) {
        case CoachPersonality::Disciplinarian:      return 0.85;
        case CoachPersonality::HarshRebuilder:      return 0.95;
        case CoachPersonality::AggressiveRiskTaker: return 0.70;
        case CoachPersonality::EmotionDriven:       return 0.75;
        case CoachPersonality::Pragmatist:          return 0.55;
        case CoachPersonality::AnalyticsHeavy:      return 0.50;
        case CoachPersonality::TacticalGenius:      return 0.40;
        case CoachPersonality::Motivator:           return 0.25;
        case CoachPersonality::StructuredMacro:     return 0.30;
        case CoachPersonality::Innovator:           return 0.45;
        case CoachPersonality::DefensiveAnchor:     return 0.35;
        case CoachPersonality::EntryLover:          return 0.50;
        case CoachPersonality::DevelopmentCoach:    return 0.15;
        case CoachPersonality::PlayerFocused:       return 0.10;
        case CoachPersonality::VeteranMentor:       return 0.20;
        case CoachPersonality::BudgetBalancer:      return 0.40;
    }
    return 0.4;
}

namespace {

// Bias the four core stats around the archetype. Returns (tactical,
// development, leadership, experience) base values before noise.
std::array<int, 4> personality_stat_bias(CoachPersonality p) {
    using A = std::array<int, 4>;
    switch (p) {
        case CoachPersonality::Disciplinarian:      return A{60, 50, 80, 65};
        case CoachPersonality::PlayerFocused:       return A{55, 65, 80, 60};
        case CoachPersonality::TacticalGenius:      return A{88, 55, 60, 60};
        case CoachPersonality::DevelopmentCoach:    return A{55, 88, 65, 55};
        case CoachPersonality::AggressiveRiskTaker: return A{72, 55, 70, 50};
        case CoachPersonality::StructuredMacro:     return A{80, 60, 60, 70};
        case CoachPersonality::EmotionDriven:       return A{55, 50, 80, 50};
        case CoachPersonality::AnalyticsHeavy:      return A{82, 70, 50, 55};
        case CoachPersonality::VeteranMentor:       return A{65, 70, 75, 90};
        case CoachPersonality::HarshRebuilder:      return A{72, 55, 75, 65};
        case CoachPersonality::Motivator:           return A{55, 60, 92, 55};
        case CoachPersonality::Pragmatist:          return A{70, 60, 65, 70};
        case CoachPersonality::Innovator:           return A{78, 65, 55, 50};
        case CoachPersonality::DefensiveAnchor:     return A{75, 60, 60, 70};
        case CoachPersonality::EntryLover:          return A{72, 55, 65, 55};
        case CoachPersonality::BudgetBalancer:      return A{60, 75, 60, 60};
    }
    return A{55, 55, 55, 55};
}

CoachPersonality random_personality() {
    return static_cast<CoachPersonality>(rng().irange(0, 15));
}

}  // namespace

CoachPtr generate_coach(std::string region) {
    // Coach nationality: weighted pick from the region's main country pool —
    // matches how teams pick home countries. Falls back to USA if the
    // region's pool is empty.
    Region reg = region_from_str(region);
    const Country& country = pick_country_in_region(reg);
    PlayerIdentity id = make_identity(country);
    std::string display_name = id.first + " " + id.last;

    auto c = std::make_shared<Coach>(std::move(display_name), std::move(region));
    c->country     = country.name;
    c->country_iso = country.iso;
    c->personality = random_personality();

    auto bias = personality_stat_bias(c->personality);
    int spread = 10;
    c->tactical    = clamp_attr(bias[0] + rng().irange(-spread, spread));
    c->development = clamp_attr(bias[1] + rng().irange(-spread, spread));
    c->leadership  = clamp_attr(bias[2] + rng().irange(-spread, spread));
    c->experience  = clamp_attr(bias[3] + rng().irange(-spread, spread));
    c->age         = rng().irange(28, 55);
    c->salary_k    = c->requested_salary_k();
    c->contract_years = rng().irange(1, 4);
    return c;
}

std::vector<CoachPtr> generate_coach_market(int count_per_region) {
    std::vector<CoachPtr> out;
    out.reserve(static_cast<std::size_t>(count_per_region) * 3);
    for (auto* r : kRegions) {
        for (int i = 0; i < count_per_region; ++i) {
            out.push_back(generate_coach(r));
        }
    }
    return out;
}

}  // namespace vlr
