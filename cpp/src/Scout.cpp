#include "Scout.h"

#include "Country.h"
#include "Names.h"

#include <array>

namespace vlr {

Scout::Scout(std::string n, std::string r)
    : name(std::move(n)), region(std::move(r)) { id = next_entity_id(); }

double Scout::quality01() const noexcept {
    return clamp_v((judgement * 0.40 + network * 0.25 +
                    projection * 0.25 + experience * 0.10) / 99.0, 0.0, 1.0);
}

int Scout::reveal_credits() const noexcept {
    // Network + reputation drive how much of the market the club's scout sees.
    // Floor 3 (a scoutless/weak user is never fully blind), elite ~10-12.
    return clamp_v(3 + network / 18 + reputation / 40, 3, 12);
}

double Scout::band_tighten_mult() const noexcept {
    // judgement <=50 -> 0 (today's full band); 99 -> ~0.6 (~40% width).
    return clamp_v((judgement - 50) / 82.0, 0.0, 0.6);
}

double Scout::discovery_quality_mult() const noexcept {
    return 1.0 + 0.30 * clamp_v((network - 50) / 49.0, 0.0, 1.0);
}

int Scout::discovery_pool_bonus() const noexcept {
    return clamp_v(network / 16, 0, 6);
}

int Scout::requested_salary_k() const noexcept {
    // Mirror Coach::requested_salary_k over the scout's four attrs.
    double quality = (judgement * 0.40) + (network * 0.25) +
                     (projection * 0.25) + (experience * 0.10);
    double q01 = clamp_v(quality / 99.0, 0.0, 1.0);
    int base = 30 + static_cast<int>(q01 * q01 * 600.0);
    double rep_mult = 0.85 + 0.45 * (reputation / 99.0);
    base = static_cast<int>(base * rep_mult);
    return clamp_v(base, 15, kSalaryCapK);
}

const char* scout_personality_name(ScoutPersonality p) noexcept {
    switch (p) {
        case ScoutPersonality::RegionalSpecialist: return "Regional Specialist";
        case ScoutPersonality::YouthHunter:        return "Youth Hunter";
        case ScoutPersonality::NetworkBroker:      return "Network Broker";
        case ScoutPersonality::AnalyticsScout:     return "Analytics Scout";
        case ScoutPersonality::JourneymanBirdDog:  return "Journeyman Bird-Dog";
        case ScoutPersonality::StarChaser:         return "Star Chaser";
        case ScoutPersonality::BargainHunter:      return "Bargain Hunter";
        case ScoutPersonality::Generalist:         return "Generalist";
    }
    return "?";
}

const char* scout_personality_blurb(ScoutPersonality p) noexcept {
    switch (p) {
        case ScoutPersonality::RegionalSpecialist: return "Deep local knowledge, narrow reach";
        case ScoutPersonality::YouthHunter:        return "Elite eye for raw young ceilings";
        case ScoutPersonality::NetworkBroker:      return "Connections everywhere, wide reach";
        case ScoutPersonality::AnalyticsScout:     return "Trusts the numbers, sharp reads";
        case ScoutPersonality::JourneymanBirdDog:  return "Seasoned, well-rounded judge";
        case ScoutPersonality::StarChaser:         return "Drawn to proven, established names";
        case ScoutPersonality::BargainHunter:      return "Unearths cheap hidden value";
        case ScoutPersonality::Generalist:         return "Balanced across the board";
    }
    return "";
}

namespace {

// Bias the four core stats {judgement, network, projection, experience} around
// the archetype (parallel to coach personality_stat_bias).
std::array<int, 4> personality_stat_bias(ScoutPersonality p) {
    using A = std::array<int, 4>;
    switch (p) {
        case ScoutPersonality::RegionalSpecialist: return A{78, 45, 60, 70};
        case ScoutPersonality::YouthHunter:        return A{70, 55, 90, 50};
        case ScoutPersonality::NetworkBroker:      return A{60, 90, 60, 60};
        case ScoutPersonality::AnalyticsScout:     return A{88, 60, 70, 55};
        case ScoutPersonality::JourneymanBirdDog:  return A{70, 65, 65, 88};
        case ScoutPersonality::StarChaser:         return A{72, 75, 55, 60};
        case ScoutPersonality::BargainHunter:      return A{75, 60, 72, 60};
        case ScoutPersonality::Generalist:         return A{62, 62, 62, 62};
    }
    return A{55, 55, 55, 55};
}

ScoutPersonality random_personality() {
    return static_cast<ScoutPersonality>(rng().irange(0, 7));
}

}  // namespace

ScoutPtr generate_scout(std::string region) {
    Region reg = region_from_str(region);
    const Country& country = pick_country_in_region(reg);
    PlayerIdentity ident = make_identity(country);
    std::string display_name = ident.first + " " + ident.last;

    auto sc = std::make_shared<Scout>(std::move(display_name), std::move(region));
    sc->country     = country.name;
    sc->country_iso = country.iso;
    sc->personality = random_personality();

    auto bias = personality_stat_bias(sc->personality);
    int spread = 10;
    sc->judgement  = clamp_attr(bias[0] + rng().irange(-spread, spread));
    sc->network    = clamp_attr(bias[1] + rng().irange(-spread, spread));
    sc->projection = clamp_attr(bias[2] + rng().irange(-spread, spread));
    sc->experience = clamp_attr(bias[3] + rng().irange(-spread, spread));
    sc->age         = rng().irange(28, 55);
    // Vary starting reputation BEFORE salary so the ask reflects standing.
    sc->reputation  = rng().irange(28, 72);
    sc->career_seasons = rng().irange(0, 6);
    sc->salary_k    = sc->requested_salary_k();
    sc->contract_years = rng().irange(1, 4);
    return sc;
}

std::vector<ScoutPtr> generate_scout_market(int count_per_region) {
    std::vector<ScoutPtr> out;
    out.reserve(static_cast<std::size_t>(count_per_region) * 3);
    for (auto* r : kRegions) {
        for (int i = 0; i < count_per_region; ++i) {
            out.push_back(generate_scout(r));
        }
    }
    return out;
}

// === Scouting knowledge model =================================================
int scout_read_depth(const Scout& s) {
    double d = s.judgement * 0.6 + s.projection * 0.4;   // accuracy of the read
    return clamp_v(static_cast<int>(d + 0.5), 0, 100);
}

// Shared reach factor: how much of the geographic base a scout actually holds,
// given network reach + personality + (for abroad) reputation.
static double coverage_factor(const Scout& s, bool same_region) {
    double net01 = clamp_v(s.network / 100.0, 0.0, 1.0);
    double rep01 = clamp_v(s.reputation / 100.0, 0.0, 1.0);
    bool spec   = (s.personality == ScoutPersonality::RegionalSpecialist);
    bool broker = (s.personality == ScoutPersonality::NetworkBroker);
    double cover;
    if (same_region) cover = 0.65 + 0.35 * net01 + (spec ? 0.15 : 0.0) - (broker ? 0.05 : 0.0);
    else             cover = (broker ? 0.55 : 0.20) * net01 + 0.18 * rep01 - (spec ? 0.15 : 0.0);
    return clamp_v(cover, 0.0, 1.0);
}

int scout_country_coverage(const Scout& s, const std::string& target_iso) {
    const Country* home = find_country_iso(s.country_iso);
    const Country* tgt  = find_country_iso(target_iso);
    if (!tgt) return 0;
    Region home_region = home ? home->region : region_from_str(s.region);
    bool same_region = (tgt->region == home_region);
    double geo;
    if (!s.country_iso.empty() && s.country_iso == target_iso) geo = 1.00;   // home turf
    else if (are_neighbors(s.country_iso, target_iso))         geo = 0.90;   // neighbor cluster
    else if (same_region)                                     geo = 0.78;   // wider home region
    else                                                      geo = 0.35;   // abroad
    return clamp_v(static_cast<int>(geo * coverage_factor(s, same_region) * 100.0 + 0.5), 0, 100);
}

int scout_region_coverage(const Scout& s, const std::string& region) {
    const Country* home = find_country_iso(s.country_iso);
    Region home_region = home ? home->region : region_from_str(s.region);
    bool is_home = (region_from_str(region) == home_region);
    double geo = is_home ? 0.85 : 0.35;
    return clamp_v(static_cast<int>(geo * coverage_factor(s, is_home) * 100.0 + 0.5), 0, 100);
}

}  // namespace vlr
