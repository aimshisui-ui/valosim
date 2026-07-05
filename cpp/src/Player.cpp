#include "Player.h"

#include "Coach.h"
#include "Match.h"
#include "Names.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <random>
#include <string>
#include <unordered_set>

namespace vlr {

namespace {

double avg_attrs(const Attributes& a) {
    long long sum = 0;
    for (int v : a) sum += v;
    return static_cast<double>(sum) / static_cast<double>(kAttrCount);
}

// Player Overall Rating (OVR) — intuitive scaling so visible attributes
// match the published OVR at a glance (user feedback 2026-05-28: the old
// `mean*0.92 - 9` made 75+-attr players show as 55-60 OVR which felt broken).
// Mapping `mean*0.95 + 1.0` clamped [1, 95]:
//   attr avg 50 -> OVR ~49 (average pro)
//   attr avg 65 -> OVR ~63 (typical above-average / role player)
//   attr avg 75 -> OVR ~72 (star tier)
//   attr avg 80 -> OVR ~77 (superstar candidate)
//   attr avg 85 -> OVR ~82 (MVP-tier)
//   attr avg 95 -> OVR ~91 (once-in-a-generation)
//   hard cap 95 preserved.
double ovr_from_attrs(const Attributes& a) {
    double mean = avg_attrs(a);
    double scaled = mean * 0.95 + 1.0;
    if (scaled < 1.0) scaled = 1.0;
    if (scaled > 95.0) scaled = 95.0;   // hard ceiling - "impossible"
    return scaled;
}
bool has_badge(const Player& p, std::string_view n) {
    for (auto& b : p.badges) if (b == n) return true;
    return false;
}

double role_salary_mult(Role r) {
    switch (r) {
        case Role::Duelist:    return 1.30;
        case Role::Controller: return 1.20;
        case Role::Initiator:  return 1.05;
        case Role::Sentinel:   return 0.85;
        default:               return 1.00;
    }
}

// Region of the player's BIRTH country — i.e. their "native" region. Used by
// the import-adaptation tracker in save_history_and_progress() to detect when
// a player has signed outside their nationality and how long they've had to
// gel with the local scene. Returns the canonical name string ("Americas",
// "EMEA", "Pacific") so callers can compare directly against `Player::region`
// (which itself stores the team's regional affiliation as a string). Returns
// an empty string if the ISO code isn't found in the country pool — caller
// MUST handle that case (we treat unknown as "no penalty").
std::string nation_region_of_country_iso(std::string_view iso) {
    if (iso.empty()) return {};
    const Country* c = find_country_iso(iso);
    if (!c) return {};
    return std::string(region_name(c->region));
}

// === Mastery decay helper ================================================
// Single source of truth for agent_mastery + map_mastery year-end decay.
// 15%/yr match-count decay, 3%/yr avg-rating decay, prune entries whose
// last_played_year < year-4. AgentMastery and MapMastery share the same
// shape (matches, avg_rating, last_played_year fields) so the template
// resolves identically for both. Identical-behavior to the previous
// inline duplicated blocks — single source so they can never drift again.
template <typename MasteryMap>
void decay_mastery_map(MasteryMap& mm, int year) {
    std::vector<std::string> to_erase;
    for (auto& kv : mm) {
        auto& m = kv.second;
        int years_unused = (m.last_played_year > 0)
            ? (year - m.last_played_year) : 0;
        if (years_unused >= 1) {
            m.matches = std::max(0, static_cast<int>(m.matches * 0.85));
            m.avg_rating *= 0.97;
            if (years_unused >= 4 || m.matches == 0) {
                to_erase.push_back(kv.first);
            }
        }
    }
    // Iterator-safe erase — collect keys, then drop after the loop.
    for (auto& k : to_erase) mm.erase(k);
}

}  // namespace

Player::Player(std::string n, int a, Attributes attrs, std::string r,
               int pot, int we, int cons)
    : name(std::move(n)),
      region(std::move(r)),
      age(a),
      attributes(attrs) {
    id = next_entity_id();
    contract_years = rng().irange(1, 4);

    int avg = static_cast<int>(avg_attrs(attributes));
    potential   = (pot  >= 0) ? pot  : std::min(99, avg + rng().irange(5, 20));
    work_ethic  = (we   >= 0) ? we   : rng().irange(20, 99);
    consistency = (cons >= 0) ? cons : rng().irange(30, 95);

    double r1 = rng().uniform();
    if (r1 < 0.10) {
        growth_archetype = GrowthArchetype::EarlyPeaker;
        peak_age = rng().irange(21, 23);
    } else if (r1 < 0.20) {
        growth_archetype = GrowthArchetype::LateBloomer;
        peak_age = rng().irange(26, 29);
    } else {
        growth_archetype = GrowthArchetype::Standard;
        peak_age = rng().irange(24, 25);
    }

    for (auto& m : maps()) map_stats[m.name] = MapStat{};

    generate_badges();
    update_agent_pool();

    Contract c0 = gen_contract(/*current_year=*/0, true, true);
    contract = c0;
}

double Player::ovr() const noexcept { return ovr_from_attrs(attributes); }

bool Player::is_form_at_risk() const noexcept {
    // Need a meaningful sample before declaring "at risk" — a player with
    // 1-3 matches at sub-baseline could just be having a bad start. Same
    // 4-match floor + 0.95 rating threshold the Manager form tracker used
    // to hard-code in UI.
    if (season_matches < 4) return false;
    double srat = season_rating_total / static_cast<double>(season_matches);
    return srat < 0.95;
}

// =========================================================================
// Role compatibility (2026-05-28)
// =========================================================================
// Attribute-driven 0..1 score: how naturally would this player play role r?
// 1.00 = the player's own primary_role (natural fit, no penalty).
// Other roles weight a small key-attr composite normalised to 0..1:
//   Duelist     : Aim, Headshot, Entry, Aggressiveness, Movement
//   Initiator   : Utility, Communication, GameSense, Intelligence, MidRound
//   Controller  : Intelligence, DecisionMaking, Communication, MidRound, GS
//   Sentinel    : Positioning, Anchor, Clutch, SpikeHandle, Adaptability
// Each role weighted average lives in roughly [0.30, 0.85] for the typical
// pool. Mid-bucket fits (~0.55-0.70) are realistic "could play that role
// with prep" candidates; sub-0.40 should feel like a hard mismatch.
double Player::role_fit_score(Role r) const {
    if (r == primary_role) return 1.00;
    auto a = [&](Attr at) { return ::vlr::at(attributes, at) / 99.0; };
    double s = 0.50;
    switch (r) {
        case Role::Duelist:
            s = 0.28 * a(Attr::Aim)
              + 0.22 * a(Attr::Headshot)
              + 0.20 * a(Attr::Entry)
              + 0.18 * a(Attr::Aggressiveness)
              + 0.12 * a(Attr::Movement);
            break;
        case Role::Initiator:
            s = 0.26 * a(Attr::Utility)
              + 0.22 * a(Attr::Communication)
              + 0.20 * a(Attr::GameSense)
              + 0.18 * a(Attr::Intelligence)
              + 0.14 * a(Attr::MidRoundCalling);
            break;
        case Role::Controller:
            s = 0.26 * a(Attr::Intelligence)
              + 0.22 * a(Attr::DecisionMaking)
              + 0.20 * a(Attr::Communication)
              + 0.18 * a(Attr::MidRoundCalling)
              + 0.14 * a(Attr::GameSense);
            break;
        case Role::Sentinel:
            s = 0.26 * a(Attr::Positioning)
              + 0.22 * a(Attr::Anchor)
              + 0.20 * a(Attr::Clutch)
              + 0.18 * a(Attr::SpikeHandle)
              + 0.14 * a(Attr::Adaptability);
            break;
        default: break;
    }
    if (s < 0.0) s = 0.0;
    if (s > 1.0) s = 1.0;
    return s;
}

double Player::agent_fit_score(const Agent& a) const noexcept {
    // Same a1/a2/a3 weighting get_rating uses for its agent_match term, but
    // normalized to 0..1 so it is directly comparable to role_fit_score. A
    // player who has the SPECIFIC agent's signature attributes scores high
    // even if their generic ROLE profile is mediocre (the Controller->Killjoy /
    // Controller->Sova case the role-level score misjudges).
    auto v = [&](Attr at) { return ::vlr::at(attributes, at) / 99.0; };
    double s = 0.45 * v(a.a1) + 0.32 * v(a.a2) + 0.23 * v(a.a3);
    if (s < 0.0) s = 0.0;
    if (s > 1.0) s = 1.0;
    return s;
}

const char* Player::role_fit_verdict(Role r) const {
    if (r == primary_role) return "Natural";
    double s = role_fit_score(r);
    if (s >= 0.75) return "Good fit";
    if (s >= 0.55) return "Possible";
    if (s >= 0.35) return "Stretch";
    return "Mismatch";
}

double Player::kd_ratio() const noexcept {
    return std::round((static_cast<double>(career_kills) / std::max(1, career_deaths)) * 100.0) / 100.0;
}
double Player::avg_match_rating() const noexcept {
    return std::round((career_rating_total / std::max(1, career_matches)) * 100.0) / 100.0;
}
double Player::solo_kd() const noexcept {
    return std::round((static_cast<double>(solo_kills) / std::max(1, solo_deaths)) * 100.0) / 100.0;
}
std::string Player::rank_name() const { return get_rank_from_mmr(solo_mmr); }

double Player::kast() const noexcept {
    // Prefer the proper per-round binary KAST tracker. Fall back to the
    // (over-counting) sum estimate for older saves where it's never been
    // populated.
    double denom = static_cast<double>(std::max(1, season_rounds));
    if (season_rounds_with_kast > 0) {
        return std::round((season_rounds_with_kast / denom) * 1000.0) / 10.0;
    }
    double num = static_cast<double>(season_kills + season_assists + season_survivals + season_trades);
    return std::round((num / denom) * 1000.0) / 10.0;
}
double Player::entry_success() const noexcept {
    int denom = std::max(1, season_fb + season_fd);
    return std::round((static_cast<double>(season_fb) / denom) * 1000.0) / 10.0;
}

// Career rate stats — used by the League Leaders leaderboards. All return
// 0.0 if the player has no rounds yet, so leaderboards can sort safely.
double Player::career_adr() const noexcept {
    if (career_rounds <= 0) return 0.0;
    return static_cast<double>(career_damage) / career_rounds;
}
double Player::career_apr() const noexcept {
    if (career_rounds <= 0) return 0.0;
    return static_cast<double>(career_assists) / career_rounds;
}
double Player::career_kpr() const noexcept {
    if (career_rounds <= 0) return 0.0;
    return static_cast<double>(career_kills) / career_rounds;
}
double Player::career_kast_pct() const noexcept {
    if (career_rounds <= 0) return 0.0;
    return std::round((static_cast<double>(career_rounds_with_kast) / career_rounds) * 1000.0) / 10.0;
}
double Player::career_hs_pct() const noexcept {
    if (career_kills <= 0) return 0.0;
    return std::round((static_cast<double>(career_hs_hits) / career_kills) * 1000.0) / 10.0;
}
double Player::career_entry_pct() const noexcept {
    int denom = std::max(1, career_fb + career_fd);
    return std::round((static_cast<double>(career_fb) / denom) * 1000.0) / 10.0;
}

int Player::get_rating(const Agent& agent, const GameMap& map) const noexcept {
    double base_ovr = ovr() * 0.60;
    // Weighted attribute fit (matches the agent's signature-attr weighting in
    // update_agent_pool — a1 dominates because that's the agent's identity
    // attribute). Was an unweighted average; rebalanced for the new system.
    double agent_match = at(attributes, agent.a1) * 0.45
                       + at(attributes, agent.a2) * 0.32
                       + at(attributes, agent.a3) * 0.23;
    double map_match = (at(attributes, map.a1) + at(attributes, map.a2) + at(attributes, map.a3)) / 3.0;
    double role_bonus = (map.favored_role == agent.role) ? 3.5 : 0.0;
    // MAP SIGNATURE AGENT: the 2-3 agents a map meta-demands (Viper on Breeze,
    // Killjoy/Cypher on lockdown maps, Sova on recon maps). A slightly-stronger-
    // than-role bonus so each map slot prefers the agent the map actually wants,
    // and a forced off-role flex lands on the map's demanded agent rather than
    // an arbitrary same-role pick. Doesn't override a much-better-fitting agent.
    double sig_bonus = is_map_signature_agent(agent.name, map.name) ? 4.0 : 0.0;
    // Mastery: experienced players are stronger on their signature agent.
    // Capped at +12 by mastery_bonus_for; here we count it at 0.5x weight so
    // total mastery contribution to get_rating tops out around +6.
    double mastery = mastery_bonus_for(agent.name) * 0.5;
    double v = base_ovr + agent_match * 0.25 + map_match * 0.15
             + role_bonus + sig_bonus + mastery;
    int iv = static_cast<int>(std::round(v));
    if (iv > 98) iv = 98;
    if (iv < 1)  iv = 1;
    return iv;
}

int Player::get_transfer_value() const noexcept {
    double progress = clamp_v((ovr() - 40.0) / 55.0, 0.0, 1.0);
    double base_value = 50000.0 + std::pow(progress, 2.0) * 300000.0;
    double age_factor = 1.0;
    if (age > 24) age_factor = std::max(0.15, 1.0 - (age - 24) * 0.15);
    else if (age < 21) age_factor = 1.2;
    // FAME premium: a famous name commands a market premium, an unknown a discount
    // (~0.7..1.5x). Reputation lags OVR, so a fading star still fetches a name-value
    // fee for a season or two, and a rising prospect is cheap until they're known.
    double rep_mult = 1.0 + (reputation - 5000) / 5000.0 * 0.45;
    if (rep_mult < 0.7) rep_mult = 0.7;
    if (rep_mult > 1.5) rep_mult = 1.5;
    int val = static_cast<int>(base_value * age_factor * rep_mult);
    return clamp_v(val, 10000, 350000);
}

Contract Player::gen_contract(int current_year, bool randomize_amount, bool randomize_exp) const {
    // Legacy 3-arg overload — delegate to the 4-arg team-aware form with
    // no context. Behaviour identical to the pre-Desire codepath.
    return gen_contract(current_year, randomize_amount, randomize_exp, nullptr);
}

Contract Player::gen_contract(int current_year, bool randomize_amount, bool randomize_exp,
                              const Team* team_ctx) const {
    // Realistic dynamic contract value built from many signals. New ones
    // marked NEW. Player-only signals — caller can layer additional
    // team-context multipliers on top via amount_with_mood / contract
    // negotiation UI.
    //
    // Base value blends current OVR (55%) and potential (45%) -> the "pure
    // skill" baseline. Then layered multipliers:
    //   - role_salary_mult        (Duelists 1.30, Controllers 1.20, etc.)
    //   - career_phase_mult       (peak +30%, decline -25%, rookie -20%)
    //   - star_status_mult        (titles + MVPs + sustained 1.20+ rating)
    //   - leadership_value_mult   (IGLs +12% with Leadership 80+)
    //   - upside_premium          (potential >> ovr earns young bonus)
    //   - risk_discount           (low work_ethic -> 5-10% discount)
    //   - market_noise            (random ±10%)
    //   - last_season_perf_mult   NEW (last season rating >> 1 boosts ask)
    //   - recent_trophy_premium   NEW (current-year [W]/[M]/[T] wins)
    //   - veteran_discount        NEW (35+ asks less for "ring chase" deals)

    double value_p = 0.55 * ovr() + 0.45 * potential;

    // Career phase multiplier — the U-curve of pro pay
    double phase_mult;
    if (age <= 19)        phase_mult = 0.65;       // rookie deal
    else if (age <= 21)   phase_mult = 0.85;
    else if (age <= peak_age)            phase_mult = 1.05;
    else if (age <= peak_age + 2)        phase_mult = 1.30;  // peak prime
    else if (age <= peak_age + 5)        phase_mult = 1.10;
    else                                 phase_mult = 0.75;  // decline

    // Star status: career awards + MVPs + sustained avg rating
    int star_pts = 0;
    star_pts += career_mvps * 4;
    star_pts += static_cast<int>(awards.size()) * 5;
    if (avg_match_rating() >= 1.20) star_pts += 12;
    else if (avg_match_rating() >= 1.10) star_pts += 5;
    if (career_matches >= 80) star_pts += 5;
    double star_mult = 1.0 + clamp_v(star_pts / 100.0, 0.0, 0.50);

    // Leadership value: IGLs with high Leadership earn a meaningful premium
    double lead_mult = 1.0;
    if (is_igl && at(attributes, Attr::Leadership) >= 80) lead_mult = 1.12;
    else if (is_igl) lead_mult = 1.05;

    // Upside premium: young player whose potential vastly exceeds current OVR
    double upside_mult = 1.0;
    int upside_gap = potential - static_cast<int>(ovr());
    if (age <= 22 && upside_gap >= 15) upside_mult = 1.18;
    else if (age <= 23 && upside_gap >= 10) upside_mult = 1.08;

    // Risk discount: low work_ethic = harder to coach = market discount
    double risk_mult = 1.0;
    if (work_ethic < 35) risk_mult = 0.92;
    else if (work_ethic < 50) risk_mult = 0.97;

    // NEW: last-season performance — looks at the most recent history
    // entry. A breakout year materially bumps an FA's asking price.
    double last_season_mult = 1.0;
    if (!history.empty()) {
        const auto& h = history.back();
        if (h.rating >= 1.30)      last_season_mult = 1.22;
        else if (h.rating >= 1.20) last_season_mult = 1.14;
        else if (h.rating >= 1.10) last_season_mult = 1.06;
        else if (h.rating <= 0.85) last_season_mult = 0.92;
    }

    // NEW: recent tournament success — wins in the current year add a
    // big premium (these are the players GMs are competing for).
    int recent_trophies = 0;
    {
        std::string yr_str = std::to_string(current_year);
        std::string yr_minus_1 = std::to_string(current_year - 1);
        for (auto& a : awards) {
            if (a.find(yr_str) == std::string::npos &&
                a.find(yr_minus_1) == std::string::npos) continue;
            if (a.find("World Champion") != std::string::npos) recent_trophies += 4;
            else if (a.find("Masters Champ") != std::string::npos) recent_trophies += 2;
            else if (a.find("Regional Champ") != std::string::npos) recent_trophies += 1;
        }
    }
    double trophy_mult = 1.0 + std::min(0.40, recent_trophies * 0.05);

    // NEW: veteran ring-chase discount — at 33+, players often take pay
    // cuts to join contenders for one last shot. Soft 5-10% discount.
    double veteran_discount = 1.0;
    if (age >= 35)      veteran_discount = 0.88;
    else if (age >= 33) veteran_discount = 0.93;

    // Compute base amount via a piecewise schedule. Rebalanced 2026-05 to
    // bring salaries in line with team budgets (max ~$2M elite, $500K-900K
    // mid). See PROJECT_GUIDE.md §4.4 + §7. Absolute ceiling is kSalaryCapK
    // (180) — post-multiplier chain can still push superstar peaks into the
    // 150-180 band, but the days of $999K asks are gone.
    double base_amount;
    int absolute_max;
    if (value_p > 85.0) {
        absolute_max = kSalaryCapK;                          // 180 cap
        base_amount = 80.0 + (value_p - 85.0) * 5.0;         // 80..150 typical range (superstars)
    } else if (value_p > 70.0) {
        absolute_max = 110;
        base_amount = 50.0 + (value_p - 70.0) * (40.0/15.0); // 50..90 (stars)
    } else if (value_p > 55.0) {
        absolute_max = 75;
        base_amount = 30.0 + (value_p - 55.0) * 2.0;         // 30..60 (strong starters)
    } else if (value_p > 40.0) {
        absolute_max = 45;
        base_amount = 18.0 + (value_p - 40.0) * (17.0/15.0); // 18..35 (mid/starter)
    } else {
        absolute_max = 25;
        // value_p can fall below the 25 anchor (down to ~22 for deep-bench
        // spawns); clamp the spread term at >=0 so the base never dips below
        // the intended $10K floor (a negative term used to compute base ~2).
        base_amount = 10.0 + std::max(0.0, value_p - 25.0) * (8.0/15.0);  // 10..18 (bench/low)
    }

    double amount = base_amount;
    amount *= phase_mult;
    amount *= star_mult;
    amount *= lead_mult;
    amount *= upside_mult;
    amount *= risk_mult;
    amount *= last_season_mult;
    amount *= trophy_mult;
    amount *= veteran_discount;
    amount *= role_salary_mult(primary_role);
    // FAME premium on the ASK: a high-reputation star demands to be paid like a
    // star (~0.85..1.35x), an unknown accepts less. Reputation lags OVR, so this
    // tracks the same fame curve as the transfer-value premium.
    {
        double rep_wage = 1.0 + (reputation - 5000) / 5000.0 * 0.35;
        if (rep_wage < 0.85) rep_wage = 0.85;
        if (rep_wage > 1.35) rep_wage = 1.35;
        amount *= rep_wage;
    }

    // Personality lean on the ASK: greedy / egotistical players demand more,
    // humble ones less (centered at 50 = 1.0x). Bounded so it shifts within the
    // existing salary bands rather than redefining them, keeping smoke #12/#13.
    {
        double greed_lean = 1.0 + (greed - 50) / 49.0 * 0.12;   // ±12%
        double ego_lean   = 1.0 + (ego   - 50) / 49.0 * 0.06;   // ±6%
        amount *= clamp_v(greed_lean * ego_lean, 0.82, 1.22);
    }

    if (randomize_amount) {
        std::normal_distribution<double> g(1.0, 0.10);
        double noise = clamp_v(g(rng().engine()), 0.85, 1.20);
        amount *= noise;
    }

    // Desire-driven contract preference (Phase A — Agent B). Layers on top
    // of the existing player-only signals when the caller passed a team
    // context. Modest magnitude (roughly 0.80..1.30) so smoke tests #12/13
    // still see baseline values in range; the multiplier shifts asks within
    // those bands rather than redefining them.
    if (team_ctx != nullptr) {
        amount *= desire_salary_mult(*team_ctx);
    }

    // Years: career-phase logic + star demand for short deals
    int years;
    if (age <= 21 && potential >= 75)         years = rng().irange(3, 4); // dev deal
    else if (age <= 23 && potential >= 65)    years = rng().irange(2, 3);
    else if (ovr() >= 85.0)                   years = rng().irange(1, 2); // star short deal
    else if (age >= 28)                       years = rng().irange(1, 2); // vet short stay
    else if (potential < 55)                  years = 1;
    else                                      years = rng().irange(2, 3);

    if (randomize_exp) {
        // For initial-game noise, allow shorter deals to spread expiration years.
        years = rng().irange(1, std::max(1, years));
    }

    int amt = clamp_v(static_cast<int>(std::round(amount)), kSalaryFloorK, absolute_max);
    Contract c;
    c.amount_k = amt;
    // Use the global world year as a baseline when no explicit year is passed
    // (the ctor calls this with current_year=0 to seed an FA placeholder); this
    // keeps the placeholder sensible for a rookie generated mid-game instead of
    // a near-zero exp_year.
    int base_year = (current_year > 0) ? current_year : current_world_year();
    c.exp_year = base_year + years - 1;
    return c;
}

double Player::perf_score() const {
    // Recent on-server form: career rating with a tilt toward the most recent
    // season. ~0.55 (poor) .. ~1.70 (elite); 1.0 when there is no data yet.
    double career = avg_match_rating();
    if (career <= 0.0) career = 1.0;
    double recent = history.empty() ? career : history.back().rating;
    if (recent <= 0.0) recent = career;
    double blended = 0.45 * career + 0.55 * recent;
    return clamp_v(blended, 0.55, 1.70);
}

double Player::accolade_score() const {
    // Career hardware: MVPs + international titles + total award count.
    // 0 (undecorated) .. ~1.3 (a legend).
    double pts = 0.0;
    pts += career_mvps                       * 0.18;
    pts += award_count_by_prefix("[W] ")     * 0.20;   // World/Champions titles
    pts += award_count_by_prefix("[M] ")     * 0.10;   // Masters titles
    pts += static_cast<int>(awards.size())   * 0.03;   // regional / role awards
    return clamp_v(pts, 0.0, 1.30);
}

Player::MoodBreakdown Player::mood_breakdown(const Team& team, int current_year) const {
    MoodBreakdown b;

    // --- Team form (win/loss record this season) ---
    {
        int g = team.wins + team.losses;
        if (g >= 3) {
            double winpct = static_cast<double>(team.wins) / g;
            b.team_perf_mod = clamp_v(static_cast<int>(std::lround((winpct - 0.5) * 30.0)), -15, 15);
        }
    }
    // --- Own form (season rating vs ~1.0 baseline, ego-amplified) ---
    if (season_matches > 0) {
        double r = season_rating_total / season_matches;
        double ego01 = ego / 99.0;
        b.own_perf_mod = clamp_v(
            static_cast<int>(std::lround((r - 1.0) * 30.0 * (0.8 + 0.4 * ego01))), -15, 15);
    }
    // --- Club history / pedigree (prestige + dynasty), ambition cares more ---
    {
        int h = 0;
        if      (team.prestige >= 80) h += 12;
        else if (team.prestige >= 60) h += 7;
        else if (team.prestige <  35) h -= 5;
        h += clamp_v(static_cast<int>(team.dynasty_tier(current_year)), 0, 8);
        double amb01 = ambition / 99.0;
        b.history_mod = clamp_v(static_cast<int>(std::lround(h * (0.7 + 0.6 * amb01))), -8, 20);
    }
    // --- Chemistry with the current roster ---
    {
        double chem = 0.0; int n = 0;
        for (auto& tp : team.roster) {
            if (!tp || tp.get() == this) continue;
            chem += team.chemistry_between(*this, *tp);
            ++n;
        }
        if (n > 0) b.chemistry_mod = clamp_v(static_cast<int>(std::lround((chem / n) * 12.0)), -15, 15);
    }
    // --- Teammate personalities: big egos in the room create friction ---
    {
        int hot = 0;
        for (auto& tp : team.roster)
            if (tp && tp.get() != this && tp->ego >= 75) ++hot;
        b.teammate_mod = clamp_v(-hot * 3, -8, 0);
    }
    // --- Personality vs situation: ambitious player on a weak/strong roster ---
    {
        double avg_ovr = 0.0; int n = 0;
        for (auto& tp : team.roster) { if (tp) { avg_ovr += tp->ovr(); ++n; } }
        if (n > 0) avg_ovr /= n;
        double amb01 = ambition / 99.0;
        int pm = 0;
        if      (avg_ovr <  65.0) pm -= static_cast<int>(std::lround(amb01 * 8.0));
        else if (avg_ovr >= 78.0) pm += static_cast<int>(std::lround(amb01 * 6.0));
        b.personality_mod = clamp_v(pm, -10, 10);
    }
    // --- Playtime (a benched starter is unhappy) ---
    b.playtime_mod = (season_matches >= 8) ? 4 : (season_matches >= 4 ? -3 : -12);

    // --- Standing grievance: per-org mood + global discontent (drag only) ---
    b.grievance_mod = -static_cast<int>(std::lround(40.0 * mood_for(team.name)))
                      -static_cast<int>(std::lround(25.0 * discontent));

    b.total = clamp_v(b.base_score + b.team_perf_mod + b.own_perf_mod + b.history_mod
                      + b.chemistry_mod + b.teammate_mod + b.personality_mod
                      + b.playtime_mod + b.grievance_mod, 0, 100);

    if      (b.total >= 80) b.verdict = "THRIVING";
    else if (b.total >= 62) b.verdict = "HAPPY";
    else if (b.total >= 42) b.verdict = "CONTENT";
    else if (b.total >= 25) b.verdict = "UNHAPPY";
    else                    b.verdict = "MISERABLE";

    auto add = [&](const char* nm, int v) { if (v != 0) b.labels.emplace_back(nm, v); };
    add("Team form",     b.team_perf_mod);
    add("Own form",      b.own_perf_mod);
    add("Club pedigree", b.history_mod);
    add("Chemistry",     b.chemistry_mod);
    add("Teammates",     b.teammate_mod);
    add("Personality",   b.personality_mod);
    add("Playtime",      b.playtime_mod);
    add("Grievance",     b.grievance_mod);
    return b;
}

int Player::overall_mood(const Team& team, int current_year) const {
    return mood_breakdown(team, current_year).total;
}

double Player::mood_for(std::string_view team_name) const {
    auto it = mood.per_team.find(std::string(team_name));
    return (it == mood.per_team.end()) ? 0.0 : it->second;
}
void Player::bump_mood(std::string_view team_name, double delta) {
    auto& m = mood.per_team[std::string(team_name)];
    m = clamp_v(m + delta, 0.0, 1.0);
}
int Player::amount_with_mood(int base_amount_k, std::string_view team_name) const {
    double m = mood_for(team_name);
    double a = base_amount_k * (1.0 + 0.20 * m);
    return clamp_v(static_cast<int>(std::round(a)), kSalaryFloorK, kSalaryCapK);
}
bool Player::refuses_to_negotiate(int offer_k, std::string_view team_name) const {
    double m = mood_for(team_name);
    if (m <= 0.0) return false;
    // Mood-inflated DIGNITY FLOOR. The player ghosts you only when your
    // offer is below a threshold that GROWS with how mad they are at this
    // org. Higher offer monotonically REDUCES refusal odds — never raises
    // them. Previous formula was (offer × mood > 100) which inverted the
    // intended semantics: a higher offer was making the player walk.
    double floor_k = 30.0 + 70.0 * m;   // $30K floor at mood=0+, $100K at mood=1.0
    return static_cast<double>(offer_k) < floor_k;
}
void Player::decay_mood(double delta) {
    for (auto& kv : mood.per_team) kv.second = clamp_v(kv.second - delta, 0.0, 1.0);
}
void Player::decay_demands() {
    if (rng().chance(0.33)) contract.amount_k -= 1;
    if (contract.amount_k < kSalaryFloorK) contract.amount_k = kSalaryFloorK;
    contract.amount_k = static_cast<int>(std::round(contract.amount_k * 0.995));
    if (contract.amount_k < kSalaryFloorK) contract.amount_k = kSalaryFloorK;
}

void Player::apply_attribute_delta(Attr a, int delta) {
    if (a == Attr::Count) return;
    auto& v = at(attributes, a);
    v = clamp_attr(v + delta);
}
void Player::apply_badge(std::string_view name) {
    const Badge* b = find_badge(name);
    if (!b) return;
    badges.emplace_back(b->name);
    for (auto& m : b->mods) apply_attribute_delta(m.stat, m.delta);
}

bool Player::god_set_badge(std::string_view name, bool on) {
    const Badge* b = find_badge(name);
    if (!b) return false;
    bool present = false;
    for (const auto& bn : badges) if (bn == name) { present = true; break; }
    if (on) {
        if (present) return true;            // idempotent — no double-apply of mods
        apply_badge(name);                   // append + apply deltas (uses b->name)
        return true;
    }
    if (!present) return false;
    for (const auto& m : b->mods) apply_attribute_delta(m.stat, -m.delta);   // reverse mods
    badges.erase(std::remove(badges.begin(), badges.end(), std::string(name)), badges.end());
    return true;
}

void Player::generate_badges() {
    // Badges should feel rare and special. Per-badge spawn chance kept low
    // and capped at 1 spawn-time badge per player so the rare ones aren't
    // diluted. Dynamic in-career badges (check_dynamic_badges) layer on top
    // for players who actually earn them through performance.
    int spawned = 0;
    for (auto& b : ::vlr::badges()) {
        if (spawned >= 1) break;
        if (rng().uniform() < 0.00045) {
            this->badges.emplace_back(b.name);
            for (auto& m : b.mods) apply_attribute_delta(m.stat, m.delta);
            ++spawned;
        }
    }
}

void Player::update_agent_pool() {
    // === Pillar 1: weighted attribute-fit scoring ===
    // Each agent has a tuple (a1, a2, a3) of "this is what you need to play
    // this agent". We weight a1 heaviest because the agent's identity hinges
    // on that signature attribute (Jett -> Headshot, Neon -> Aim, etc.).
    //
    // base = 0.45*v1 + 0.32*v2 + 0.23*v3   (max ~99 -> ~99 when all elite)
    //
    // Bonuses stacked on top:
    //   primary_role match: +6.0 (modest steer; agents off-role can still win)
    //   mastery:            up to ~+12 (signature-main lock-in for veterans)
    //
    // The role match is intentionally not huge — a Duelist with insanely
    // high Lurking attributes can still get Cypher in their pool, which is
    // exactly the "specialised flex" feeling the user asked for.
    auto score_agent = [this](const Agent& a) {
        const double v1 = static_cast<double>(at(attributes, a.a1));
        const double v2 = static_cast<double>(at(attributes, a.a2));
        const double v3 = static_cast<double>(at(attributes, a.a3));
        double s = v1 * 0.45 + v2 * 0.32 + v3 * 0.23;

        if (a.role == primary_role) s += 3.0;   // softened 6->3 so genuinely
        s += mastery_bonus_for(a.name);          // high off-role fits compete
        return s;
    };

    const auto& AG = agents();
    if (AG.empty()) { agent_pool.clear(); return; }

    // === Identify primary_role ===
    // Use raw attribute fit (without role/mastery boosts) so role choice is
    // grounded in the player's actual mechanical/positional traits.
    auto raw_fit = [this](const Agent& a) {
        return at(attributes, a.a1) * 0.45 + at(attributes, a.a2) * 0.32
             + at(attributes, a.a3) * 0.23;
    };
    auto compute_role_score = [&](Role role) {
        std::vector<double> scores;
        for (auto& a : AG) if (a.role == role) scores.push_back(raw_fit(a));
        std::sort(scores.begin(), scores.end(), std::greater<double>());
        double s = 0.0;
        for (std::size_t i = 0; i < std::min<std::size_t>(2, scores.size()); ++i) s += scores[i];
        return s;
    };
    double best_score = -1.0;
    Role best = Role::Duelist;
    for (std::size_t i = 0; i < static_cast<std::size_t>(Role::Count); ++i) {
        double s = compute_role_score(static_cast<Role>(i));
        if (s > best_score) { best_score = s; best = static_cast<Role>(i); }
    }
    // ROLE LOCK: a SIGNED player (contract_role != Count) keeps the role they
    // were contracted into regardless of how their attributes drift — only the
    // user (or an explicit AI signing) ever changes it. FA-pool players
    // (contract_role == Count) still re-derive their natural role so the pool
    // stays fresh. The agent-pool ordering below always uses `best`.
    if (contract_role == Role::Count) primary_role = best;

    // === Sort all agents by full score (now that primary_role is set) ===
    std::vector<const Agent*> all;
    all.reserve(AG.size());
    for (auto& a : AG) all.push_back(&a);
    std::sort(all.begin(), all.end(),
              [&](const Agent* x, const Agent* y) {
                  return score_agent(*x) > score_agent(*y);
              });

    agent_pool.clear();

    // Step 1: best primary-role agent is ALWAYS first in the pool — preserves
    // the "Duelist always has a Duelist" invariant the engine relies on for
    // comp-building. Mastery + compat bonuses are included in the score, so
    // a Jett main will get Jett pinned even if attribute drift would have
    // demoted them.
    const Agent* primary_top = nullptr;
    for (auto* a : all) if (a->role == primary_role) { primary_top = a; break; }
    if (primary_top) agent_pool.push_back(primary_top);

    // Step 1b: RESERVE cross-role agents (VCT-accurate — most pros cover their
    // role PLUS 2+ agents OUTSIDE it). Pull the best kCross non-primary-role
    // agents BEFORE the generic fill so same-role picks don't crowd them out.
    // Pools of size <= 2 stay pure one/two-tricks (no reserve).
    int kCross = (agent_pool_size <= 2) ? 0
               : std::min(cross_role_min, agent_pool_size - 1);
    for (auto* a : all) {
        if (static_cast<int>(agent_pool.size()) >= 1 + kCross) break;
        if (a == primary_top || a->role == primary_role) continue;  // off-role only
        agent_pool.push_back(a);
    }

    // Step 2: fill the remainder up to agent_pool_size from the next-highest
    // scorers across ANY role (skipping anything already pinned/reserved).
    int target = std::max(1, std::min<int>(agent_pool_size, static_cast<int>(AG.size())));
    for (auto* a : all) {
        if (static_cast<int>(agent_pool.size()) >= target) break;
        if (a == primary_top) continue;
        if (std::find(agent_pool.begin(), agent_pool.end(), a) != agent_pool.end()) continue;
        agent_pool.push_back(a);
    }
}

// === Pillar 2: agent mastery system =======================================

double Player::mastery_bonus_for(const std::string& agent_name) const {
    auto it = agent_mastery.find(agent_name);
    if (it == agent_mastery.end()) return 0.0;
    const auto& m = it->second;

    // Match-count factor: log curve, caps at +8 (~50 matches gets you most
    // of the way there). 50+ matches on one agent is signature territory.
    double match_factor = std::min(8.0, std::log1p(static_cast<double>(m.matches)) * 2.5);

    // Quality factor: 1.0 rating is neutral, 1.3 rating is elite.
    // Range clamped to [-3, +4] so a few bad games don't tank a main.
    double quality_factor = (m.avg_rating - 0.95) * 12.0;
    if (quality_factor > 4.0)  quality_factor = 4.0;
    if (quality_factor < -3.0) quality_factor = -3.0;

    // Live staleness penalty — gentle. Year-end decay does the heavy work,
    // but if a player hasn't touched an agent for a few years and the year
    // counter has advanced (e.g. mid-season pool refresh), apply a soft hit.
    double bonus = match_factor + quality_factor;
    int wy = current_world_year();
    if (wy > 0 && m.last_played_year > 0) {
        int gap = wy - m.last_played_year;
        if (gap >= 2) bonus -= 1.0 * (gap - 1);
    }

    if (bonus < 0.0) bonus = 0.0;     // never penalise a player below baseline
    if (bonus > 12.0) bonus = 12.0;   // hard cap
    return bonus;
}

void Player::record_agent_performance(const std::string& agent_name,
                                      double match_rating,
                                      int year) {
    if (agent_name.empty() || agent_name == "?") return;

    auto& m = agent_mastery[agent_name];
    m.matches += 1;

    // EMA with alpha=0.15 — quick enough to react to a hot streak, slow
    // enough to ignore single bad games. First match seeds directly.
    constexpr double kEmaAlpha = 0.15;
    if (m.matches == 1) m.avg_rating = match_rating;
    else                m.avg_rating = kEmaAlpha * match_rating
                                     + (1.0 - kEmaAlpha) * m.avg_rating;

    if (match_rating > m.peak_rating) m.peak_rating = match_rating;
    if (year > 0) m.last_played_year = year;
    else if (m.last_played_year == 0) m.last_played_year = current_world_year();
}

std::string Player::signature_agent() const {
    if (agent_mastery.empty()) return {};
    const std::string* best_name = nullptr;
    double best_score = -1.0;
    for (auto& kv : agent_mastery) {
        const auto& m = kv.second;
        if (m.matches < 5) continue;            // need real evidence
        double s = static_cast<double>(m.matches) * (m.avg_rating + 0.5);
        if (s > best_score) { best_score = s; best_name = &kv.first; }
    }
    return best_name ? *best_name : std::string{};
}

// === Pillar 2b: map mastery system =======================================
// Mirrors the agent_mastery design exactly — same EMA alpha, same log curve,
// same staleness shape, same total clamp. Per-map experience drives comfort
// map identification and feeds per-map agent selection in Team.cpp.

void Player::record_map_performance(const std::string& map_name,
                                    double match_rating,
                                    int    year) {
    if (map_name.empty()) return;

    auto& m = map_mastery[map_name];
    m.matches += 1;

    constexpr double kEmaAlpha = 0.15;
    if (m.matches == 1) m.avg_rating = match_rating;
    else                m.avg_rating = kEmaAlpha * match_rating
                                     + (1.0 - kEmaAlpha) * m.avg_rating;

    if (match_rating > m.peak_rating) m.peak_rating = match_rating;

    int eff_year = (year > 0) ? year : current_world_year();
    m.last_played_year = eff_year;
}

double Player::map_mastery_bonus(const std::string& map_name) const {
    auto it = map_mastery.find(map_name);
    if (it == map_mastery.end()) return 0.0;
    const auto& m = it->second;

    // Match-count factor: log curve, caps at +8.
    double match_factor = std::min(8.0, std::log1p(static_cast<double>(m.matches)) * 2.5);

    // Quality factor: clamped to [-3, +4] same as agent mastery.
    double quality_factor = (m.avg_rating - 0.95) * 12.0;
    if (quality_factor > 4.0)  quality_factor = 4.0;
    if (quality_factor < -3.0) quality_factor = -3.0;

    // Staleness penalty — -1.0 per year past 1yr unused.
    int yr = current_world_year();
    int years_unused = (m.last_played_year > 0) ? (yr - m.last_played_year) : 0;
    double staleness = (years_unused > 1) ? -1.0 * (years_unused - 1) : 0.0;

    double total = match_factor + quality_factor + staleness;
    if (total < 0.0)  total = 0.0;
    if (total > 12.0) total = 12.0;
    return total;
}

std::string Player::comfort_map() const {
    std::string best;
    double best_score = -1.0;
    for (auto& kv : map_mastery) {
        if (kv.second.matches < 5) continue;
        double score = kv.second.matches * (kv.second.avg_rating + 0.5);
        if (score > best_score) { best_score = score; best = kv.first; }
    }
    return best;
}

// === Playstyle identity (pure diagnostic accessor) ========================
// Cascade evaluated in priority order: the most specific / earned identities
// fire first, so a player who qualifies for both "Ice-cold Closer" and
// "Clutch Specialist" reads as the rarer Ice-cold Closer. Pure function —
// safe to call every render frame.
std::string Player::playstyle_identity() const {
    const int aim       = at(attributes, Attr::Aim);
    const int hs        = at(attributes, Attr::Headshot);
    const int reaction  = at(attributes, Attr::Reaction);
    const int lurking   = at(attributes, Attr::Lurking);
    const int positioning = at(attributes, Attr::Positioning);
    const int gamesense = at(attributes, Attr::GameSense);
    const int anchor    = at(attributes, Attr::Anchor);
    const int spike     = at(attributes, Attr::SpikeHandle);
    const int entry     = at(attributes, Attr::Entry);
    const int aggro     = at(attributes, Attr::Aggressiveness);
    const int movement  = at(attributes, Attr::Movement);
    const int utility   = at(attributes, Attr::Utility);
    const int comm      = at(attributes, Attr::Communication);
    const int clutch    = at(attributes, Attr::Clutch);
    const int adapt     = at(attributes, Attr::Adaptability);

    // Ice-cold Closer — rarest. Must clear bar on clutch + consistency AND
    // have actually won grand-final clutches multiple times.
    if (clutch >= 90 && consistency >= 80 && career_grand_final_clutches >= 2) {
        return "Ice-cold Closer";
    }

    // Mechanical Demon — pure aim profile.
    if (aim >= 88 && hs >= 85 && reaction >= 80) {
        return "Mechanical Demon";
    }

    // Smart Lurker — info denial / map control specialist.
    if (lurking >= 80 && positioning >= 80 && gamesense >= 75) {
        return "Smart Lurker";
    }

    // Passive Anchor — site holder identity.
    if (anchor >= 85 && positioning >= 80 && spike >= 75) {
        return "Passive Anchor";
    }

    // Aggressive Entry — duelist mind.
    if (entry >= 85 && aggro >= 80 && movement >= 75) {
        return "Aggressive Entry";
    }

    // Utility Support — gives up frag for impact.
    if (utility >= 85 && comm >= 80 && aim < 75) {
        return "Utility Support";
    }

    // Clutch Specialist — proven on the biggest stage.
    if (clutch >= 88 && anchor >= 75 && career_grand_final_clutches >= 1) {
        return "Clutch Specialist";
    }

    // Hyper-consistent Veteran — never tilts, big sample.
    if (consistency >= 85 && age >= 25 && career_matches >= 100) {
        return "Hyper-consistent Veteran";
    }

    // Momentum Player — wild swings, but has the firepower.
    if (consistency < 65 && (aim >= 80 || hs >= 80)) {
        return "Momentum Player";
    }

    // LAN Choker — long career, low adaptability, zero grand-final closure.
    if (adapt < 50 && career_grand_final_clutches == 0 && career_matches > 50) {
        return "LAN Choker";
    }

    return "Generalist";
}

// === World-year accessor (used by Match.cpp mastery hook) =================
// Match.cpp doesn't carry a GameManager pointer; rather than thread one
// through, GameManager calls set_current_world_year() at the top of every
// advance_day(). Cheap and avoids pollution of the Match interface.
namespace {
int g_world_year = 0;
}
void set_current_world_year(int y) noexcept { g_world_year = y; }
int  current_world_year() noexcept { return g_world_year; }

void Player::check_dynamic_badges() {
    // Cap a player at 4 badges total — including spawn-time. Any more
    // dilutes how special they feel.
    if (badges.size() >= 4) return;

    // Thresholds intentionally HIGH — these are earned for genuinely
    // exceptional performance, not normal pro form.
    if (entry_success() >= 95.0 && season_fb >= 110 && at(attributes, Attr::Aggressiveness) >= 88
        && !has_badge(*this, "Entry Fragger")) {
        badges.emplace_back("Entry Fragger");
        apply_attribute_delta(Attr::Entry, 8);
        apply_attribute_delta(Attr::Aggressiveness, 5);
    }
    if (has_badge(*this, "Clutch Minister") && season_matches >= 30 && kast() >= 95.0) {
        apply_attribute_delta(Attr::Clutch, 4);
    }
    double avg_rating = season_matches > 0 ? season_rating_total / season_matches : 0.0;
    if (avg_rating >= 1.95 && kd_ratio() >= 1.85 && !has_badge(*this, "Ace Machine")) {
        badges.emplace_back("Ace Machine");
        apply_attribute_delta(Attr::Aim, 6);
        apply_attribute_delta(Attr::Headshot, 10);
    }
    if (season_fb >= 130 && !has_badge(*this, "First Strike")) {
        badges.emplace_back("First Strike");
        apply_attribute_delta(Attr::Entry, 7);
    }
    if (season_survivals >= 180 && kast() >= 96.0 && !has_badge(*this, "Unkillable")) {
        badges.emplace_back("Unkillable");
        apply_attribute_delta(Attr::Clutch, 5);
    }
    if (!has_badge(*this, "Map Specialist")) {
        for (auto& kv : map_stats) {
            if (kv.second.count >= 100 && (kv.second.rating_total / std::max(1, kv.second.count)) >= 1.70) {
                badges.emplace_back("Map Specialist");
                apply_attribute_delta(Attr::GameSense, 5);
                break;
            }
        }
    }
}

// === Awards integrity + query helpers ====================================
//
// Background: tournament-award pinning lives in Tournament::award_event_titles
// and checks `if (a == award) exists` before push_back, so steady-state
// duplicates shouldn't happen. A prior trophy bug, however, double-emitted
// `[T]/[M]/[W]` awards on some careers. `dedupe_awards()` is the one-shot
// cleanup hook called at every year-end; it's stable and idempotent.

int Player::dedupe_awards() {
    int removed = 0;
    std::vector<std::string> seen;
    seen.reserve(awards.size());
    // Hand-rolled stable filter — keep the FIRST occurrence of each string.
    for (auto it = awards.begin(); it != awards.end(); ) {
        bool dup = false;
        for (auto& s : seen) {
            if (s == *it) { dup = true; break; }
        }
        if (dup) {
            it = awards.erase(it);
            ++removed;
        } else {
            seen.push_back(*it);
            ++it;
        }
    }
    // Refresh the hash mirror — any external mutation may have desynced us.
    rebuild_awards_seen();
    return removed;
}

void Player::add_award(const std::string& award) {
    if (award.empty()) return;
    // O(1) dedup at the source — the year-end dedupe_awards() pass is now
    // belt-and-braces against external mutations rather than the primary line
    // of defense.
    if (awards_seen_.find(award) != awards_seen_.end()) return;
    awards.push_back(award);
    awards_seen_.insert(award);
}

void Player::rebuild_awards_seen() {
    awards_seen_.clear();
    awards_seen_.reserve(awards.size());
    for (const auto& a : awards) awards_seen_.insert(a);
}

int Player::award_count_by_prefix(const std::string& prefix) const {
    if (prefix.empty()) return 0;
    int n = 0;
    for (auto& a : awards) {
        if (a.size() >= prefix.size() &&
            a.compare(0, prefix.size(), prefix) == 0) {
            ++n;
        }
    }
    return n;
}

bool Player::has_award_with_prefix(const std::string& prefix) const noexcept {
    if (prefix.empty()) return false;
    for (auto& a : awards) {
        if (a.size() >= prefix.size() &&
            a.compare(0, prefix.size(), prefix) == 0) {
            return true;
        }
    }
    return false;
}

bool Player::ever_won_international() const noexcept {
    return has_award_with_prefix("[M] ") || has_award_with_prefix("[W] ");
}

bool Player::ever_won_role_award() const noexcept {
    // [A]-prefixed season awards covering "<Role> of the Year". Excludes
    // MVP (covered separately) and IGL of the Year (not a role award per
    // PROJECT_GUIDE §4.15 criterion 2).
    for (auto& a : awards) {
        if (a.size() < 4 || a.compare(0, 4, "[A] ") != 0) continue;
        if (a.find("of the Year") == std::string::npos)   continue;
        if (a.find("MVP")         != std::string::npos)   continue;
        if (a.find("IGL")         != std::string::npos)   continue;
        return true;
    }
    return false;
}

bool Player::ever_won_mvp() const noexcept {
    // Award format from compute_season_awards: "[A] MVP YYYY".
    for (auto& a : awards) {
        if (a.size() < 4 || a.compare(0, 4, "[A] ") != 0) continue;
        if (a.find("MVP") != std::string::npos) return true;
    }
    return false;
}

// === Read-only career aggregators ==========================================
//
// All four (trophy_summary / career_timeline / best_season / top_n_seasons /
// head_to_head) are pure — they only READ the existing player state. The UI
// (gui_main.cpp) + GameManager call into these for the Player Profile and
// season-recap screens; main session owns the call sites.

namespace {

// Extract a 4-digit year suffix from an award string of the form
// "[X] Something YYYY" or "... YYYY)". Returns 0 if no plausible year
// (>= 1990 sanity floor) can be parsed. Robust against odd whitespace
// because we hop backwards from the end of the string.
int parse_year_suffix(const std::string& s) {
    if (s.size() < 4) return 0;
    // Walk backwards skipping any trailing non-digit characters (e.g. ')').
    std::size_t end = s.size();
    while (end > 0 && !std::isdigit(static_cast<unsigned char>(s[end - 1]))) --end;
    if (end < 4) return 0;
    std::size_t start = end - 4;
    for (std::size_t i = start; i < end; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) return 0;
    }
    int y = std::stoi(s.substr(start, 4));
    if (y < 1990 || y > 9999) return 0;
    return y;
}

// Pick a stable ordering priority for CareerEvent so multiple events in the
// same year sort into a sensible narrative (debut first, retirement last).
int event_priority(const std::string& category) {
    if (category == "debut")      return 0;
    if (category == "transfer")   return 1;
    if (category == "milestone")  return 2;
    if (category == "award")      return 3;
    if (category == "trophy")     return 4;
    if (category == "retirement") return 5;
    return 9;
}

}  // namespace

Player::TrophySummary Player::trophy_summary() const {
    TrophySummary s;
    // Walk awards once for counts; the prefix probes mirror PROJECT_GUIDE
    // §4.14 / §4.9.1 formats (`[T] Regional Champ YYYY`, `[M] Masters Champ
    // YYYY`, `[W] World Champion YYYY`, `[A] {Category} YYYY`).
    for (const auto& a : awards) {
        if (a.size() < 4) continue;
        if (a.compare(0, 4, "[T] ") == 0) {
            ++s.regional;
        } else if (a.compare(0, 4, "[M] ") == 0) {
            ++s.masters;
        } else if (a.compare(0, 4, "[W] ") == 0) {
            ++s.worlds;
        } else if (a.compare(0, 4, "[A] ") == 0) {
            // Differentiate within the [A] bucket. Order matters: check the
            // narrower patterns BEFORE the generic role-award fallback so
            // "[A] IGL of the Year 2030" doesn't get double-counted into
            // role_awards.
            if (a.find("MVP") != std::string::npos) {
                ++s.mvps;
            } else if (a.find("IGL of the Year") != std::string::npos) {
                ++s.igl_oty;
            } else if (a.find("of the Year") != std::string::npos &&
                       (a.find("Duelist")    != std::string::npos ||
                        a.find("Initiator")  != std::string::npos ||
                        a.find("Controller") != std::string::npos ||
                        a.find("Sentinel")   != std::string::npos)) {
                ++s.role_awards;
            }
        }
    }
    // Deduplicated ordered title list — keep first occurrence per string.
    s.all_titles.reserve(awards.size());
    for (const auto& a : awards) {
        bool seen = false;
        for (const auto& t : s.all_titles) { if (t == a) { seen = true; break; } }
        if (!seen) s.all_titles.push_back(a);
    }
    return s;
}

std::vector<Player::CareerEvent> Player::career_timeline() const {
    std::vector<CareerEvent> out;
    // --- Debut: first year in history ---
    if (!history.empty()) {
        int debut_year = history.front().year;
        // (history is appended chronologically by save_history_and_progress
        //  — the FIRST entry IS the debut season.)
        out.push_back(CareerEvent{debut_year, "Pro debut", "debut"});
    }

    // --- Milestone: any season with rating >= 1.15 or an award that year ---
    for (const auto& h : history) {
        bool standout_rating = (h.rating >= 1.15);
        bool had_award       = false;
        std::string year_str = std::to_string(h.year);
        for (const auto& a : awards) {
            if (a.find(year_str) != std::string::npos) { had_award = true; break; }
        }
        if (standout_rating || had_award) {
            char buf[96];
            std::snprintf(buf, sizeof(buf),
                          "%s — %.2f rating in %d match%s",
                          h.team.c_str(), h.rating, h.matches,
                          h.matches == 1 ? "" : "es");
            out.push_back(CareerEvent{h.year, std::string(buf), "milestone"});
        }
    }

    // --- Award + trophy events: parse year suffix from each award string ---
    for (const auto& a : awards) {
        int y = parse_year_suffix(a);
        if (y == 0) continue;
        std::string cat = "award";
        if (a.size() >= 4) {
            char tag = a[1];
            if (tag == 'T' || tag == 'M' || tag == 'W') cat = "trophy";
        }
        out.push_back(CareerEvent{y, a, cat});
    }

    // --- Transfer events: any year where salary_log shows a different team
    //     from the prior history entry's team. salary_log entries are
    //     (year, amount_k) — they don't carry the team name, so we lean on
    //     the team field of each MatchHistoryEntry instead. (Pre-existing
    //     entries before transfer-detection existed simply emit nothing.)
    {
        std::string prev_team;
        for (const auto& h : history) {
            if (!prev_team.empty() && h.team != prev_team &&
                h.team != "Free Agent" && h.team != "Retired") {
                out.push_back(CareerEvent{
                    h.year, "Signed with " + h.team, "transfer"});
            }
            prev_team = h.team;
        }
    }

    // --- Retirement: if flagged retired, use the LAST history year as the
    //     stamping point (we don't store an explicit retirement_year — best
    //     effort using available data).
    if (is_retired && !history.empty()) {
        int ret_year = history.back().year;
        out.push_back(CareerEvent{ret_year, "Retired from competition", "retirement"});
    }

    // Sort by year asc, then category priority (debut → transfer →
    // milestone → award → trophy → retirement) so the UI gets a clean
    // chronological narrative.
    std::sort(out.begin(), out.end(),
              [](const CareerEvent& a, const CareerEvent& b) {
        if (a.year != b.year) return a.year < b.year;
        return event_priority(a.category) < event_priority(b.category);
    });
    return out;
}

Player::SeasonHighlight Player::best_season() const {
    SeasonHighlight best;
    // Walk history; pick highest rating among 8+ match seasons. (8-match
    // gate per PROJECT_GUIDE §4.14 — also the awards qualification floor.)
    for (const auto& h : history) {
        if (h.matches < 8) continue;
        if (h.rating <= best.rating) continue;
        best.year      = h.year;
        best.rating    = h.rating;
        best.matches   = h.matches;
        best.kills     = h.kills;
        best.deaths    = h.deaths;
        best.assists   = h.assists;
        best.team_name = h.team;
        best.awards_that_year.clear();
        std::string yr = std::to_string(h.year);
        for (const auto& a : awards) {
            if (a.find(yr) != std::string::npos) best.awards_that_year.push_back(a);
        }
    }
    // Returns year=0 if nothing qualified.
    return best;
}

std::vector<Player::SeasonHighlight> Player::top_n_seasons(int n) const {
    std::vector<SeasonHighlight> out;
    if (n <= 0) return out;
    out.reserve(history.size());
    for (const auto& h : history) {
        if (h.matches < 8) continue;   // qualification gate
        SeasonHighlight sh;
        sh.year      = h.year;
        sh.rating    = h.rating;
        sh.matches   = h.matches;
        sh.kills     = h.kills;
        sh.deaths    = h.deaths;
        sh.assists   = h.assists;
        sh.team_name = h.team;
        std::string yr = std::to_string(h.year);
        for (const auto& a : awards) {
            if (a.find(yr) != std::string::npos) sh.awards_that_year.push_back(a);
        }
        out.push_back(std::move(sh));
    }
    std::sort(out.begin(), out.end(),
              [](const SeasonHighlight& a, const SeasonHighlight& b) {
        return a.rating > b.rating;
    });
    if (static_cast<int>(out.size()) > n) out.resize(static_cast<std::size_t>(n));
    return out;
}

Player::HeadToHeadRecord Player::head_to_head(const Player& other) const {
    HeadToHeadRecord r;
    // Self-vs-self => empty record. Guards UI from a nonsensical row.
    if (&other == this) return r;

    const Player* self  = this;
    const Player* opp   = &other;
    double rating_accum = 0.0;

    for (const auto& rec : pro_match_history) {
        if (!rec)                  continue;
        if (!rec->history_record)  continue;  // can't determine sides without it

        // Determine which side each player was on for THIS recorded map.
        // blue_team / red_team are vector<Player*> raw pointers (see
        // PROJECT_GUIDE §8 pitfall #14). Compare by raw pointer identity.
        const auto& blue = rec->history_record->blue_team;
        const auto& red  = rec->history_record->red_team;
        bool self_blue = false, self_red = false;
        for (auto* p : blue) if (p == self) { self_blue = true; break; }
        if (!self_blue) {
            for (auto* p : red) if (p == self) { self_red = true; break; }
        }
        if (!self_blue && !self_red) continue;   // self not actually in this match

        bool opp_blue = false, opp_red = false;
        for (auto* p : blue) if (p == opp) { opp_blue = true; break; }
        if (!opp_blue) {
            for (auto* p : red) if (p == opp) { opp_red = true; break; }
        }
        if (!opp_blue && !opp_red) continue;     // other not in this match

        // Same side? Skip — teammate matches don't count as H2H.
        if ((self_blue && opp_blue) || (self_red && opp_red)) continue;

        // Outcome from self's perspective.
        bool self_won = self_blue
                      ? (rec->blue_score > rec->red_score)
                      : (rec->red_score  > rec->blue_score);

        ++r.matches;
        if (self_won) ++r.wins; else ++r.losses;

        // Per-player K + rating come from PlayerMatchStats keyed by raw
        // Player* (the engine's canonical key). Defensive on null map.
        if (rec->match_stats) {
            auto sit = rec->match_stats->find(const_cast<Player*>(self));
            if (sit != rec->match_stats->end()) {
                r.kills_for += sit->second.k;
                rating_accum += sit->second.rating;
            }
            auto oit = rec->match_stats->find(const_cast<Player*>(opp));
            if (oit != rec->match_stats->end()) {
                r.kills_against += oit->second.k;
            }
        }
    }

    if (r.matches > 0) {
        r.avg_rating_when_facing = rating_accum / static_cast<double>(r.matches);
    }
    return r;
}

void Player::save_history_and_progress(int year, std::string_view team_placement) {
    check_dynamic_badges();

    // One-shot duplicate cleanup. Idempotent — runs every year-end so any
    // duplicates surfaced by future bugs get scrubbed automatically.
    dedupe_awards();

    // === Reputation drift (FM-style fame; DETERMINISTIC, no rng) ==========
    // Fame LAGS reality: each year-end reputation moves a fraction of the way
    // toward a target built from current OVR (standing) + career accolades
    // (trophies/MVPs, via accolade_score) + this season's form. Winning and
    // big-stage performance raise it; a poor season nudges it down. The 0.22
    // step means a promoted club's players take ~3-4 seasons for their
    // reputation to catch up to their true level (the "slow growth" the user
    // wants). Reads season_matches/season_rating_total BEFORE the reset below.
    if (team_name != "Retired") {
        double ov = ovr();
        double target = 500.0 + (ov - 45.0) * 130.0 + accolade_score() * 900.0
                      + (age - 20) * 45.0;
        if (season_matches >= 8) {
            double sr = season_rating_total / static_cast<double>(season_matches);
            if      (sr >= 1.25) target += 320.0;
            else if (sr >= 1.10) target += 130.0;
            else if (sr <= 0.85) target -= 180.0;
        }
        target = clamp_v(target, 150.0, 10000.0);
        reputation += static_cast<int>(std::lround((target - reputation) * 0.22));
        reputation = static_cast<int>(clamp_v(static_cast<double>(reputation), 1.0, 10000.0));
    }

    // Benched-promise grievance: a player GUARANTEED a starting slot who played
    // ZERO matches this season feels betrayed — sour their mood toward the team
    // (raising their next ask / refusal floor). Reads season_matches BEFORE the
    // year-end reset below. The promise stays live for the rest of the deal, so
    // it can fire each benched season; a season actually played costs nothing.
    if (contract.promise_active && contract.promised_starter && season_matches == 0
        && team_name != "Free Agent" && team_name != "Retired") {
        bump_mood(team_name, 0.30);
    }

    // === Discontent / transfer-request (global "I want out" signal) ========
    // Distinct from per-org mood. Decay last year first (a good season heals),
    // then accrue grievances — benched on a starter promise, low playtime, a
    // losing/relegated season — each scaled by ego/ambition. >= 0.6 hands in a
    // transfer request; < 0.3 withdraws it. Reads season_matches BEFORE reset.
    if (team_name != "Free Agent" && team_name != "Retired") {
        discontent *= 0.6;                       // heal over time
        double ego01 = ego / 99.0, amb01 = ambition / 99.0;
        if (contract.promise_active && contract.promised_starter && season_matches == 0)
            discontent += 0.35 * (0.7 + 0.6 * ego01);
        if (season_matches < 8) discontent += 0.15 * (0.7 + 0.6 * ego01);
        if (season_matches < 4) discontent += 0.10 * (0.7 + 0.6 * ego01);
        std::string tp(team_placement);
        bool losing = tp.find("Relegat") != std::string::npos
                   || tp.find("Bottom")  != std::string::npos
                   || tp.find("Last")    != std::string::npos
                   || tp.find("Missed")  != std::string::npos;
        if (losing) discontent += 0.10 * (0.7 + 0.8 * amb01);
        // Chronic OFF-ROLE grievance (P5.1): a player deployed mostly off his
        // primary role grows restless, ego-scaled (a bigger ego resents the
        // shift more). Gated at a real commitment (>=34% of >=8 maps off-role)
        // and hard-capped at +0.12 so it nudges the transfer-request odds
        // without dominating the mood model. Reads season_role_maps BEFORE the
        // year-end reset below.
        {
            int total_role_maps = 0;
            for (int rm : season_role_maps) total_role_maps += rm;
            if (total_role_maps >= 8) {
                double offrole_share =
                    static_cast<double>(season_offrole_matches) / total_role_maps;
                if (offrole_share >= 0.34)
                    discontent += std::min(0.12,
                        (offrole_share - 0.34) * 0.30 * (0.7 + 0.6 * ego01));
            }
        }
        discontent = clamp_v(discontent, 0.0, 1.0);
        if (discontent >= 0.6 && !transfer_requested) {
            transfer_requested    = true;
            transfer_request_year = year;
        } else if (discontent < 0.3 && transfer_requested) {
            transfer_requested = false;
        }

        // === Anti-dynasty RESTLESSNESS (force 3, personality-keyed D3) ======
        // Success + ambition makes a star itch to leave even a WINNING,
        // well-managed team — DISTINCT from grievance discontent. A loyal /
        // stability player BONDS with a decorated org (restlessness decays to
        // 0, a one-club legend); an ambitious / ego / low-loyalty star who has
        // won here grows restless and forces a move regardless of management.
        // Evaluated for AI + user identically (the same pressure pass).
        tenure_years_at_org = (joined_year > 0) ? std::max(0, year - joined_year) : 0;
        double loy01 = loyalty / 99.0;
        double tenure01 = std::min(tenure_years_at_org, 5) / 5.0;
        double success  = std::min(1.0, titles_with_org * 0.25);       // 4+ titles = max
        double drive    = 0.55 * amb01 + 0.35 * ego01 - 0.45 * loy01;  // ambition/ego push, loyalty pulls
        double accrual  = std::max(0.0, drive) * (0.40 * success + 0.22 * tenure01);
        double bond     = 0.16 * loy01 + 0.035 * std::min(titles_with_org, 5);  // loyal legends settle in
        restlessness = clamp_v(restlessness * 0.78 + accrual - bond, 0.0, 1.0);
        // Restlessness can INDEPENDENTLY hand in a transfer request (real
        // contributors only — star floor). The release pass acts on it.
        if (restlessness >= 0.55 && ovr() >= 75.0 && !transfer_requested) {
            transfer_requested    = true;
            transfer_request_year = year;
        }
    } else {
        restlessness = 0.0;   // FAs/retired carry no org itch
    }

    if (season_matches > 0) {
        MatchHistoryEntry e;
        e.year = year;
        e.age = age;
        e.team = team_name;
        e.rating = std::round((season_rating_total / season_matches) * 100.0) / 100.0;
        e.kd = std::round((static_cast<double>(season_kills) / std::max(1, season_deaths)) * 100.0) / 100.0;
        e.kast = kast();
        e.placement = team_placement;
        e.awards = awards;
        // Capture raw season counters BEFORE they reset at the bottom of
        // this fn. Powers best_season() / top_n_seasons() and the per-year
        // K/D/A column in the Player Profile Season History tab.
        e.matches = season_matches;
        e.kills   = season_kills;
        e.deaths  = season_deaths;
        e.assists = season_assists;
        e.ovr     = static_cast<int>(std::round(ovr()));  // career-arc trend sample
        // Per-season earnings: pull from the most recent salary_log entry
        // matching this year (pay_payroll appends one each year-end).
        for (auto it = salary_log.rbegin(); it != salary_log.rend(); ++it) {
            if (it->first == year) { e.salary_k = it->second; break; }
        }
        if (e.salary_k == 0) e.salary_k = contract.amount_k;  // fallback
        history.emplace_back(std::move(e));
        // Count this as a played season for HoF qualification.
        if (season_matches >= 8) ++career_seasons_played;
    }
    // Pro history persists across years so users can browse past seasons.
    // Solo Q history likewise. (No clear here — bounded by per-vector cap.)

    if (team_name == "Free Agent") years_unsigned += 1;
    else                            years_unsigned = 0;

    // Hard rule: 4 unsigned years -> mandatory retirement. Players who
    // can't find a team after that long age out of the scene. Earlier
    // retirement can also fire from age + motivation factors handled in
    // GameManager::run_end_of_year.
    if (years_unsigned >= 4) { is_retired = true; team_name = "Retired"; }

    age += 1;
    // Removed: contract_years now stays as the original duration snapshot;
    // years_left is derived from exp_year. See Player.h field doc.
    // contract_years -= 1;

    // NOTE: a year-end attribute growth/decline block used to live here, but it
    // DOUBLE-COUNTED with apply_monthly_progression (the authoritative
    // per-attribute age curve, applied ~monthly). Stacking the two made
    // veterans decline ~2x too fast and under-peak prospects double-grow.
    // Aging is now owned ENTIRELY by apply_monthly_progression; this block was
    // removed. (age += 1 above and the mastery decay below still belong here.)

    // === Mastery year-end decay ===
    // Gentle 15%/yr match-count decay + 3%/yr avg-rating decay for any agent
    // or map not played this year. After 4 untouched years, drop the entry
    // entirely — keeps the map small and lets a player's pool re-shape if
    // they genuinely change identity (e.g. role swap mid-career). Agent and
    // map mastery share identical decay shape — see decay_mastery_map below.
    decay_mastery_map(agent_mastery, year);
    decay_mastery_map(map_mastery, year);

    update_agent_pool();

    // === Import adaptation tracker (year-end) ===
    // Detect whether the player is currently signed to a team OUTSIDE their
    // nationality region. If so, either start a fresh 24-month gel period (if
    // the region changed since last tick) or decay 12 months off the existing
    // counter. Returning to their native region instantly clears the counter.
    //
    // Note: we look at `region` (the player's CURRENT regional affiliation
    // string — propagated by Team signing logic) rather than poking the team
    // pointer directly, since this header has no Team* on Player.
    {
        std::string native_region = nation_region_of_country_iso(country_iso);
        bool is_currently_import = !region.empty() && !native_region.empty() &&
                                   region != native_region;
        if (is_currently_import) {
            if (last_seen_team_region != region) {
                // First year on a new non-native region — reset to 24 months.
                adaptation_months_remaining = 24;
            } else {
                // Same non-native region as before — decay 12 months/year.
                adaptation_months_remaining = std::max(0, adaptation_months_remaining - 12);
            }
        } else {
            // Returned to native region (or unknown native — be charitable).
            adaptation_months_remaining = 0;
        }
        last_seen_team_region = region;
    }

    season_kills = season_deaths = season_assists = 0;
    season_fb = season_fd = season_survivals = season_trades = season_rounds = 0;
    season_rounds_with_kast = 0;
    season_matches = 0;
    season_rating_total = 0.0;
    season_damage = season_hs_hits = 0;

    // === IGL impact + MVP support — per-season counters reset ===
    // Career totals (igl_impact_total, igl_match_count) are NEVER reset —
    // they accumulate across the player's whole career and feed HoF + GOAT.
    igl_impact_season      = 0.0;
    igl_impact_season_peak = 0.0;
    season_pressure_matches = 0;
    season_intl_matches     = 0;
    season_clutch_pts       = 0;
    // Off-role accounting (P4.0) — reset alongside the other season_* counters.
    season_offrole_matches      = 0;
    season_offrole_rating_total = 0.0;
    season_role_maps.fill(0);
}

// === Monthly progression ===================================================
//
// Every 30 in-game days every active player gets a progression pass. Inputs
// considered (in order of importance):
//
//   * Recent performance: average rating since the last snapshot, plus per-
//     stat deltas (FB rate, HS rate, KAST, ADR-ish proxy, clutch points,
//     survivals, comms-driven assists). For free agents with no pro matches,
//     ranked performance (MMR delta + W-L) substitutes.
//
//   * Coach: only relevant for signed pros — the team coach's
//     dev_chance_mult() multiplies the growth chance directly. FAs use a
//     baseline 0.85x (no coaching staff).
//
//   * Age curve: peak_age + growth_archetype. Late bloomers get a temporary
//     boost between 23-27, early peakers post-21 already declining.
//
//   * Potential gap: closer to potential -> harder growth. Past peak: real
//     regression risk targeting mechanical attributes (aim/HS/entry).
//
//   * Work ethic and consistency: work_ethic gates how *often* a roll
//     succeeds; consistency reduces the variance of which attributes get
//     hit.
//
// Result: 0-3 attribute moves per month for typical players. The targeted
// attributes match the player's recent role (a player who got 80 first-bloods
// in the window grows entry/aim, a clutch monster grows clutch/composure).

Player::PerfBuckets Player::compute_perf_buckets(int new_kills,  int new_deaths,
                                                  int new_fb,     int new_fd,
                                                  int new_assists,int new_survivals,
                                                  int new_rounds, int new_trades,
                                                  double avg_rating) const {
    PerfBuckets b;
    if (new_rounds <= 0) return b;
    double kpr = static_cast<double>(new_kills) / new_rounds;
    double dpr = static_cast<double>(new_deaths) / new_rounds;
    double fb_total = std::max(1, new_fb + new_fd);
    double fb_rate  = static_cast<double>(new_fb) / fb_total;
    double apr = static_cast<double>(new_assists) / new_rounds;
    double sr  = static_cast<double>(std::max(0, new_rounds - new_deaths)) / new_rounds;

    // Signals are clamped 0-1 with sensible mid-points.
    b.aim_signal      = clamp_v(kpr / 1.0, 0.0, 1.5);          // 1.0 KPR is elite
    b.headshot_signal = clamp_v(avg_rating / 1.20, 0.0, 1.5);
    b.entry_signal    = clamp_v(fb_rate * 1.5, 0.0, 1.5);
    b.utility_signal  = clamp_v(apr / 0.50, 0.0, 1.5);          // 0.5 APR is elite
    b.comm_signal     = clamp_v(apr / 0.40, 0.0, 1.5) * 0.7
                       + clamp_v(static_cast<double>(new_trades) / std::max(1, new_rounds) / 0.20, 0.0, 1.0) * 0.3;
    b.survival_signal = clamp_v(sr / 0.45, 0.0, 1.5);           // 45% survival = good
    b.clutch_signal   = clamp_v((1.0 - dpr) * (kpr - 0.5), -0.5, 1.5);
    b.clutch_signal   = std::max(0.0, b.clutch_signal);
    return b;
}

// === Attribute aging classes ============================================
// Three classes drive distinct progression curves. Mechanical decays sharply
// after 26; Game-IQ climbs into the late 20s and ages gracefully; Athletic is
// the middle ground.
Player::AttrAgingClass Player::attr_aging_class(Attr a) noexcept {
    switch (a) {
        // Mechanical — peaks 22-25, sharp post-26 decline.
        case Attr::Aim:
        case Attr::Headshot:
        case Attr::Reaction:
        case Attr::CrosshairPlacement:
        case Attr::Movement:
            return AttrAgingClass::Mechanical;
        // Game-IQ — climbs into 28+, gentle decline.
        case Attr::GameSense:
        case Attr::Intelligence:
        case Attr::DecisionMaking:
        case Attr::MidRoundCalling:
        case Attr::AntiStrat:
        case Attr::Adaptability:
        case Attr::EconomyMgmt:
        case Attr::Communication:
        case Attr::Leadership:
            return AttrAgingClass::GameIQ;
        // Athletic — middle ground.
        case Attr::Stamina:
        case Attr::SpikeHandle:
        case Attr::Anchor:
        case Attr::Lurking:
        case Attr::Positioning:
        case Attr::Aggressiveness:
        case Attr::Entry:
        case Attr::Utility:
        case Attr::Clutch:
        case Attr::Awping:
            return AttrAgingClass::Athletic;
        default:
            return AttrAgingClass::Athletic;
    }
}

namespace {

// Piecewise-linear age->baseline-delta tables, one per AttrAgingClass.
// Numbers come straight from the design spec. The "anchor" array gives the
// baseline Δ at each anchor age; values between anchors interpolate linearly.
// Outside the anchor range we clamp to the nearest endpoint.
struct AgingCurvePoint { int age; double delta; };

double interp_curve(const AgingCurvePoint* pts, int n, int age) {
    if (n <= 0) return 0.0;
    if (age <= pts[0].age) return pts[0].delta;
    if (age >= pts[n - 1].age) return pts[n - 1].delta;
    for (int i = 1; i < n; ++i) {
        if (age <= pts[i].age) {
            double t = static_cast<double>(age - pts[i - 1].age)
                     / static_cast<double>(pts[i].age - pts[i - 1].age);
            return pts[i - 1].delta + t * (pts[i].delta - pts[i - 1].delta);
        }
    }
    return pts[n - 1].delta;
}

double baseline_delta(Player::AttrAgingClass cls, int age) {
    // Mechanical (aim/HS/reaction): rises hard while young, holds a plateau
    // through 26-27, then declines from ~28. Curves shifted ~2y later + the
    // young end steepened so a 19-22yo prospect actually climbs, and a 27yo
    // still holds (the old curve declined from 25, which read as "27yo not
    // developing"). Real decline still bites at 30+.
    static const AgingCurvePoint kMech[] = {
        {18,  3.6}, {21,  2.2}, {24,  0.9}, {27,  0.0}, {30, -1.6}, {33, -3.0}
    };
    // Game-IQ (game sense/intel/comms/lead): keeps climbing well into the late
    // 20s — this is why veterans/IGLs keep getting better even as mechanics
    // fade. Only turns negative in the mid-30s.
    static const AgingCurvePoint kIQ[] = {
        {18,  2.0}, {22,  1.8}, {26,  1.2}, {29,  0.7}, {33,  0.2}, {37, -0.3}
    };
    // Athletic (movement/positioning/entry/clutch): middle ground — strong
    // young growth, gentle plateau to ~27, then a measured decline.
    static const AgingCurvePoint kAth[] = {
        {18,  2.6}, {21,  1.6}, {24,  0.7}, {27,  0.0}, {30, -1.0}, {33, -2.0}
    };
    switch (cls) {
        case Player::AttrAgingClass::Mechanical:
            return interp_curve(kMech, sizeof(kMech)/sizeof(kMech[0]), age);
        case Player::AttrAgingClass::GameIQ:
            return interp_curve(kIQ,   sizeof(kIQ)/sizeof(kIQ[0]),   age);
        case Player::AttrAgingClass::Athletic:
        default:
            return interp_curve(kAth,  sizeof(kAth)/sizeof(kAth[0]), age);
    }
}

double class_stddev(Player::AttrAgingClass cls) {
    switch (cls) {
        case Player::AttrAgingClass::Mechanical: return 0.8;
        case Player::AttrAgingClass::GameIQ:     return 0.6;
        case Player::AttrAgingClass::Athletic:   return 0.7;
    }
    return 0.7;
}

}  // namespace

ProgressionReport Player::apply_monthly_progression(const Coach* coach,
                                                     int year, int day) {
    ProgressionReport r;
    r.player_name = name;
    r.year = year;
    r.day  = day;
    r.was_pro = (team_name != "Free Agent" && team_name != "Retired");

    // Compute deltas vs last snapshot. New deltas describe the last 30 days.
    int new_matches  = career_matches  - last_snapshot.career_matches;
    int new_rounds   = career_rounds   - last_snapshot.career_rounds;
    (void)new_rounds;
    double new_rating_total = career_rating_total - last_snapshot.career_rating_total;

    int new_solo_w   = solo_wins  - last_snapshot.solo_wins;
    int new_solo_l   = solo_losses- last_snapshot.solo_losses;
    int mmr_delta    = solo_mmr   - last_snapshot.solo_mmr;
    int new_solo     = new_solo_w + new_solo_l;

    double avg_rating = 1.0;
    if (new_matches > 0) avg_rating = new_rating_total / new_matches;
    r.matches_in_window = new_matches;
    r.avg_rating        = avg_rating;
    r.ranked_played     = new_solo;
    r.mmr_delta         = mmr_delta;

    // === Age-bucketed Δrating with per-attribute aging profiles ===========
    //
    // Three aging classes (Mechanical / Game-IQ / Athletic) each have their
    // own piecewise-linear age->baseline curve. Per attribute we:
    //   1. Look up baseline_delta(class, age)        — annual rating change
    //   2. Multiply by configured multipliers        — work_ethic / coach /
    //      consistency / archetype / growth / potential pressure
    //   3. Add a class-specific gaussian noise (with the asymmetric young-
    //      player "breakout map" tail for age <= 23)
    //   4. Scale by (1/12) since this tick is monthly                       )
    //   5. Round to int, clamp [1,99], record nonzero changes               )
    //
    // The numeric design (anchor ages + multipliers + noise) lives in the
    // spec — adjusting it here will change the league's age-curve feel.

    // Per-tick scaling — baseline_delta numbers are per-YEAR rating changes,
    // so divide by 12 for monthly application.
    constexpr double kMonthlyScale = 1.0 / 12.0;

    // --- Common multipliers ---
    double work_mult = clamp_v(work_ethic / 65.0, 0.30, 2.00);
    double cons_factor = clamp_v(consistency / 65.0, 0.7, 1.3);

    // Coach: +/- a small dev influence. Default to 1.0 when no coach is bound.
    double coach_mult = coach ? clamp_v(coach->dev_chance_mult(), 0.7, 1.4) : 0.95;
    if (coach) {
        switch (coach->personality) {
            case CoachPersonality::DevelopmentCoach: coach_mult *= 1.10; break;
            case CoachPersonality::AnalyticsHeavy:   coach_mult *= 1.04; break;
            case CoachPersonality::TacticalGenius:   coach_mult *= 1.04; break;
            default: break;
        }
    }

    // Archetype consistency_mod (the Match-time stable archetype carries a
    // dev-relevant consistency lean). Range nominally ±0.12.
    double arch_cons = clamp_v(static_cast<double>(archetype_profile(archetype).consistency_mod),
                               -0.20, +0.20);
    double arch_mult = 1.0 + arch_cons * 0.5;  // ~0.94..1.06

    // Growth archetype effect on the AGE_FOR_CURVE we look up — early peakers
    // get +2 years of effective age (peak 2y earlier, decline 2y faster); late
    // bloomers get -2 years (peak 2y later, gentler decline).
    int age_for_curve = age;
    if (growth_archetype == GrowthArchetype::EarlyPeaker) age_for_curve = age + 2;
    else if (growth_archetype == GrowthArchetype::LateBloomer) age_for_curve = age - 2;

    // Potential pressure (positive deltas only): growth slows as ovr nears
    // potential. delta *= max(0.08, 1 - (ovr/pot)^3). The CUBE (was square)
    // is gentler in the mid-range — a prospect at 65% of potential is barely
    // dampened (0.73 vs the old 0.58) so high-ceiling youngsters actually
    // climb — but still clamps hard right at the ceiling. Declines bypass.
    double cur_ovr = ovr();
    double pot_pressure_pos = 1.0;
    if (potential > 0) {
        double ratio = clamp_v(cur_ovr / static_cast<double>(potential), 0.0, 1.5);
        pot_pressure_pos = std::max(0.08, 1.0 - ratio * ratio * ratio);
    }

    // Future-star acceleration: a young player (<=23) sitting well below their
    // potential develops FASTER toward it — this is what turns a high-ceiling
    // 19-22yo into a breakout star instead of a slow climber. Scales with the
    // remaining gap (potential - ovr) and fades to 1.0 by age 24. Applied to
    // POSITIVE movement only (deltas + noise); never speeds decline.
    double youth_accel = 1.0;
    if (age <= 23 && potential > 0) {
        double gap = clamp_v((static_cast<double>(potential) - cur_ovr) / 35.0, 0.0, 1.0);
        double age_factor = clamp_v((24.0 - age) / 5.0, 0.0, 1.0);  // 1.0 at <=19 -> 0 at 24
        youth_accel = 1.0 + 1.7 * gap * age_factor;                 // up to ~2.7x
    }

    // Asymmetric young-player noise: archetypes that thrive on breakout get
    // an extra +2 years on the age threshold.
    bool breakout_archetype = (archetype == Archetype::RookieMechFreak)
                           || (archetype == Archetype::HighCeilingLowFloor)
                           || (archetype == Archetype::MomentumFragger);
    int young_threshold = breakout_archetype ? 25 : 23;
    bool young = (age <= young_threshold);

    // Per-attribute application
    auto& engine = rng().engine();
    int growth_moves = 0;
    int decline_moves = 0;

    for (std::size_t i = 0; i < kAttrCount; ++i) {
        Attr a = static_cast<Attr>(i);
        AttrAgingClass cls = attr_aging_class(a);
        double base = baseline_delta(cls, age_for_curve);
        double sigma = class_stddev(cls);

        // Multipliers stack on baseline. work_ethic / consistency / archetype
        // / coach scale both positives and negatives — they're "how much each
        // tick moves at all". potential_pressure ONLY applies to positives.
        double scale = work_mult * cons_factor * arch_mult * coach_mult;

        double delta_year = base * scale;
        if (delta_year > 0.0) delta_year *= pot_pressure_pos * youth_accel;

        double delta_month = delta_year * kMonthlyScale;

        // Noise — class-specific gauss stddev for the per-attr baseline noise
        // (mech 0.8 / iq 0.6 / ath 0.7 per the spec). Asymmetric clamps then
        // kick in based on age cohort to deliver the spec's "breakout map"
        // mechanic for young players vs the tighter adult bands.
        std::normal_distribution<double> class_noise(0.0, sigma);
        double noise_year = class_noise(engine);
        double noise_month;
        if (young) {
            // young: bigger stddev (5/yr) and right-skewed clamps
            // [-4, +20] per spec — bad outcomes capped softly, breakout
            // outcomes can spike to +20.
            std::normal_distribution<double> ng(0.0, 5.0);
            double n = ng(engine);
            n = clamp_v(n, -4.0, +20.0);
            noise_month = n * kMonthlyScale;
        } else {
            // adult: tighter stddev (3/yr) and symmetric-ish clamps [-2, +4].
            std::normal_distribution<double> ng(0.0, 3.0);
            double n = ng(engine);
            n = clamp_v(n, -2.0, +4.0);
            noise_month = n * kMonthlyScale;
        }
        // Mix in a smaller fraction of the per-class baseline noise so each
        // attribute class has its own variance signature on top of the
        // age-cohort noise. Weight kept light to preserve the right-skewed
        // young-player feel.
        noise_month += noise_year * kMonthlyScale * 0.25;

        // Potential ceiling damps POSITIVE noise too (not just the deterministic
        // curve), so a prospect can't noise-walk past their potential as the
        // fractional carry below lands. Youth acceleration also lifts positive
        // noise so breakout spikes land harder for high-ceiling youngsters.
        if (noise_month > 0.0) noise_month *= pot_pressure_pos * youth_accel;

        double total = delta_month + noise_month;

        // Fractional carry: accumulate the real-valued monthly delta so slow
        // deterministic curves (e.g. +0.09/month) actually LAND over time
        // instead of rounding to 0 every tick and never moving the attribute.
        // The remainder carries to the next tick (truncation toward zero works
        // for both signs).
        attr_growth_carry[i] += total;
        int delta_int = static_cast<int>(attr_growth_carry[i]);
        attr_growth_carry[i] -= static_cast<double>(delta_int);

        if (delta_int == 0) continue;

        int before = at(attributes, a);
        int after = clamp_attr(before + delta_int);
        if (after == before) continue;

        attributes[i] = after;
        int signed_delta = after - before;
        r.changes.emplace_back(a, signed_delta);
        if (signed_delta > 0) ++growth_moves;
        else                  ++decline_moves;
    }

    // Belt-and-braces global clamp (some external code may set attrs out of
    // range; guarantee [1,99] post-tick).
    for (auto& v : attributes) v = clamp_attr(v);

    // === Update career_max_ovr — used by should_retire stage 2 ===========
    int now_ovr = static_cast<int>(std::round(ovr()));
    if (now_ovr > career_max_ovr) career_max_ovr = now_ovr;

    // === Development trajectory stamp (item 8) ===========================
    // Recompute the trajectory CLASS from REAL recent movement so the
    // indicator tracks form + development rather than a static gap/age guess,
    // and updates every tick instead of going stale. Uses: OVR delta since the
    // last tick, the recent 30-day window rating (avg_rating, falling back to
    // the cached value if no matches this window), the potential gap, and age.
    {
        int prev_ovr  = (ovr_at_last_tick > 0) ? ovr_at_last_tick : now_ovr;
        int ovr_delta = now_ovr - prev_ovr;
        ovr_at_last_tick = now_ovr;
        double rr = (new_matches > 0) ? avg_rating : last_form_rating;
        if (new_matches > 0) last_form_rating = avg_rating;
        int gap = potential - now_ovr;

        TrajClass tc;
        if (age <= 23 && gap >= 6 && rr > 0.0 && rr < 0.92)
            tc = TrajClass::Slump;        // high ceiling but underperforming NOW
        else if (age <= 23 && gap >= 8 && potential >= 86 && ovr_delta >= 0)
            tc = TrajClass::FutureStar;   // young, elite ceiling, not regressing
        else if (age <= 25 && gap >= 6 && ovr_delta > 0)
            tc = TrajClass::Rising;       // climbing toward a real ceiling
        else if (age >= 28 && ovr_delta < 0)
            tc = TrajClass::Declining;    // veteran losing OVR
        else if (age >= 30 && gap <= 2)
            tc = TrajClass::Twilight;     // late-career, near ceiling
        else if (gap >= 4 && ovr_delta >= 0)
            tc = TrajClass::Developing;   // slow positive growth
        else
            tc = TrajClass::Established;
        trajectory_class = tc;
    }

    // === Build human-readable explanation ===
    char buf[512];
    if (r.was_pro && new_matches > 0) {
        std::snprintf(buf, sizeof(buf),
            "Played %d matches, avg rating %.2f. Coach %s (%.2fx dev), age %d. %s%s",
            new_matches, avg_rating,
            coach ? coach->name.c_str() : "(none)",
            coach_mult, age,
            growth_moves > 0 ? "Earned growth." : "No growth this month.",
            decline_moves > 0 ? " Some attributes regressed." : "");
    } else if (!r.was_pro && new_solo > 0) {
        std::snprintf(buf, sizeof(buf),
            "Free agent — %d ranked games, %+d MMR (%dW-%dL). %s",
            new_solo, mmr_delta, new_solo_w, new_solo_l,
            growth_moves > 0 ? "Earned growth." : "Stagnant month.");
    } else {
        std::snprintf(buf, sizeof(buf),
            "No matches played this month. %s",
            growth_moves > 0 ? "Trained anyway." : "");
    }
    r.explanation = buf;

    // Take a fresh snapshot for the next monthly tick.
    last_snapshot.career_kills    = career_kills;
    last_snapshot.career_deaths   = career_deaths;
    last_snapshot.career_assists  = career_assists;
    last_snapshot.career_fb       = career_fb;
    last_snapshot.career_fd       = career_fd;
    last_snapshot.career_survivals= career_survivals;
    last_snapshot.career_trades   = career_trades;
    last_snapshot.career_rounds   = career_rounds;
    last_snapshot.career_matches  = career_matches;
    last_snapshot.career_rating_total = career_rating_total;
    last_snapshot.solo_mmr        = solo_mmr;
    last_snapshot.solo_wins       = solo_wins;
    last_snapshot.solo_losses     = solo_losses;
    last_snapshot.solo_kills      = solo_kills;
    last_snapshot.solo_deaths     = solo_deaths;

    return r;
}

// === Rookie archetype tables ============================================
//
// Each archetype gives the spawned player a flavour that stays with them
// for their whole career. Stats are nudged at spawn time, growth_archetype
// + peak_age + work_ethic + consistency are biased per archetype, and
// IGLness is forced for archetypes that make sense (TacticalIGL).
//
// Designed so any single archetype reads as a recognisable stereotype
// when scouting via attribute sliders. Hidden by default (god-mode reveal).

const char* rookie_archetype_name(RookieArchetype a) noexcept {
    switch (a) {
        case RookieArchetype::None:                   return "Standard";
        case RookieArchetype::MechanicalProdigy:      return "Mechanical Prodigy";
        case RookieArchetype::RawAimer:               return "Raw Aimer";
        case RookieArchetype::TacticalIGL:            return "Tactical IGL";
        case RookieArchetype::ClutchSpecialist:       return "Clutch Specialist";
        case RookieArchetype::AggressiveEntry:        return "Aggressive Entry";
        case RookieArchetype::SupportMastermind:      return "Support Mastermind";
        case RookieArchetype::FlexibleUtility:        return "Flexible Utility";
        case RookieArchetype::SoloQueueDemon:         return "Solo Queue Demon";
        case RookieArchetype::HighPotentialProject:   return "High-Potential Project";
        case RookieArchetype::InconsistentSuperstar:  return "Inconsistent Superstar";
        case RookieArchetype::VeteranStyleRookie:     return "Veteran-Style Rookie";
        case RookieArchetype::DefensiveAnchor:        return "Defensive Anchor";
        case RookieArchetype::LANPerformer:           return "LAN Performer";
        case RookieArchetype::FragileConfidence:      return "Fragile Confidence";
        case RookieArchetype::HighIQStrategist:       return "High-IQ Strategist";
        case RookieArchetype::TeamChemistryPlayer:    return "Team Chemistry";
    }
    return "?";
}

const char* rookie_archetype_blurb(RookieArchetype a) noexcept {
    switch (a) {
        case RookieArchetype::MechanicalProdigy:      return "Sky-high aim and reaction; thin macro game.";
        case RookieArchetype::RawAimer:               return "Pure point-and-click; util/comm both poor.";
        case RookieArchetype::TacticalIGL:            return "Born shotcaller; trades frag power for control.";
        case RookieArchetype::ClutchSpecialist:       return "Late-round monster; calm under pressure.";
        case RookieArchetype::AggressiveEntry:        return "First through every door; positioning suffers.";
        case RookieArchetype::SupportMastermind:      return "Util-first, comm-heavy; unselfish playstyle.";
        case RookieArchetype::FlexibleUtility:        return "Plays anything; deepest agent pool.";
        case RookieArchetype::SoloQueueDemon:         return "Climbs ranked fast; team play takes time.";
        case RookieArchetype::HighPotentialProject:   return "Raw stats today, huge ceiling tomorrow.";
        case RookieArchetype::InconsistentSuperstar:  return "Capable of insane peaks; hot-and-cold form.";
        case RookieArchetype::VeteranStyleRookie:     return "Mature day one; minimal further growth.";
        case RookieArchetype::DefensiveAnchor:        return "Sentinel-locked anchor with iron positioning.";
        case RookieArchetype::LANPerformer:           return "Steps up on LAN — pro performance > solo.";
        case RookieArchetype::FragileConfidence:      return "Talented but mentally fragile; needs care.";
        case RookieArchetype::HighIQStrategist:       return "Reads opponents; weakest mech in the room.";
        case RookieArchetype::TeamChemistryPlayer:    return "Glue guy; everyone plays better with them.";
        case RookieArchetype::None:                   return "Generic spawn (legacy).";
    }
    return "";
}

namespace {

// Apply archetype-specific stat adjustments + meta tweaks (potential bump,
// growth_archetype override, IGL force, work ethic / consistency biases).
void apply_rookie_archetype(Player& p, RookieArchetype a) {
    auto bump = [&](Attr k, int d) { p.apply_attribute_delta(k, d); };
    switch (a) {
        case RookieArchetype::MechanicalProdigy:
            bump(Attr::Aim,18); bump(Attr::Headshot,16); bump(Attr::Reaction,15);
            bump(Attr::CrosshairPlacement,14); bump(Attr::Movement,8);
            bump(Attr::GameSense,-6); bump(Attr::Intelligence,-4);
            p.consistency = std::max(p.consistency, 70);
            break;
        case RookieArchetype::RawAimer:
            bump(Attr::Aim,20); bump(Attr::Headshot,18);
            bump(Attr::Utility,-12); bump(Attr::Communication,-8);
            bump(Attr::DecisionMaking,-6);
            break;
        case RookieArchetype::TacticalIGL:
            // A2: apply the FULL canonical IGL stat shift IFF is_igl is
            // transitioning false->true via this archetype. The shift
            // exactly mirrors the one in generate_player()'s IGL roll so
            // two TacticalIGLs with different roll histories end up with
            // identical attribute profiles. If is_igl is already true,
            // skip the shift (it was applied at the roll site).
            if (!p.is_igl) {
                p.apply_attribute_delta(Attr::Aim,            -12);
                p.apply_attribute_delta(Attr::Headshot,       -10);
                p.apply_attribute_delta(Attr::Entry,           -8);
                p.apply_attribute_delta(Attr::Aggressiveness,  -4);
                p.apply_attribute_delta(Attr::Leadership,     +14);
                p.apply_attribute_delta(Attr::Intelligence,   +10);
                p.apply_attribute_delta(Attr::Communication,  +10);
                p.apply_attribute_delta(Attr::GameSense,       +8);
                p.apply_attribute_delta(Attr::DecisionMaking,  +8);
                p.apply_attribute_delta(Attr::MidRoundCalling,+12);
                p.apply_attribute_delta(Attr::EconomyMgmt,    +12);
                p.apply_attribute_delta(Attr::AntiStrat,      +10);
            }
            // Archetype-specific flavor bumps ON TOP of the canonical IGL
            // shift. These mark the player as a TacticalIGL specifically
            // rather than a generic shotcaller.
            bump(Attr::Intelligence,16); bump(Attr::DecisionMaking,14);
            bump(Attr::Leadership,18); bump(Attr::MidRoundCalling,18);
            bump(Attr::EconomyMgmt,12); bump(Attr::AntiStrat,12);
            bump(Attr::Aim,-6); bump(Attr::Headshot,-6);
            // Force IGL stamping ONLY if the player satisfies the same gates
            // roll_is_igl_at_spawn enforces (mental ≥ 0.55 generally,
            // Duelists need mental ≥ 0.78 post-shift). This preserves the
            // "Duelist IGLs are extremely rare and require elite mental"
            // invariant — pitfall #36. Without this check, a freshly-rolled
            // Duelist with average mental who happens to roll TacticalIGL
            // would bypass the floor.
            {
                // Inline mental composite (mirrors mental_score() / roll_is_igl_at_spawn).
                auto getA = [&](Attr k) { return (double)at(p.attributes, k); };
                double mental_after = (0.20 * getA(Attr::GameSense)
                                     + 0.18 * getA(Attr::Communication)
                                     + 0.18 * getA(Attr::MidRoundCalling)
                                     + 0.15 * getA(Attr::Intelligence)
                                     + 0.12 * getA(Attr::Adaptability)
                                     + 0.12 * getA(Attr::Leadership)
                                     + 0.05 * getA(Attr::DecisionMaking)) / 99.0;
                bool gate_ok = (mental_after >= 0.55) &&
                               !(p.primary_role == Role::Duelist && mental_after < 0.78);
                // Duelists: the archetype's huge mental boost makes the 0.78
                // gate trivially passable, which used to AUTO-force nearly
                // every TacticalIGL duelist into is_igl — the dominant source
                // of the old ~26% Duelist-IGL rate. A born shot-caller is
                // realistically a utility player, not a duelist main, so a
                // duelist who rolls TacticalIGL only RARELY stamps IGL (and
                // even then must clear the gate). Non-duelists force normally.
                if (gate_ok) {
                    if (p.primary_role == Role::Duelist) {
                        if (rng().chance(0.12)) p.is_igl = true;
                    } else {
                        p.is_igl = true;
                    }
                }
            }
            p.tend_play_aggressive = 30 + (int)(rng().uniform()*40);
            p.tend_vocal = 60 + (int)(rng().uniform()*35);
            break;
        case RookieArchetype::ClutchSpecialist:
            bump(Attr::Clutch,20); bump(Attr::Anchor,14);
            bump(Attr::Stamina,12); bump(Attr::SpikeHandle,12);
            bump(Attr::Positioning,8);
            break;
        case RookieArchetype::AggressiveEntry:
            bump(Attr::Entry,18); bump(Attr::Aggressiveness,20);
            bump(Attr::Movement,12); bump(Attr::Aim,6);
            bump(Attr::Utility,-10); bump(Attr::Positioning,-8);
            break;
        case RookieArchetype::SupportMastermind:
            bump(Attr::Utility,20); bump(Attr::Communication,16);
            bump(Attr::GameSense,12); bump(Attr::Lurking,8);
            bump(Attr::Aim,-8); bump(Attr::Entry,-6);
            break;
        case RookieArchetype::FlexibleUtility:
            for (auto k : {Attr::Aim, Attr::Utility, Attr::GameSense,
                            Attr::Anchor, Attr::Entry}) bump(k, 4);
            p.cross_role_min = 3;   // a true flex reliably covers 3 roles
            bump(Attr::Adaptability,16);
            // Flex players actually play more agents — bump pool size.
            p.agent_pool_size = std::max(p.agent_pool_size, 5);
            break;
        case RookieArchetype::SoloQueueDemon:
            bump(Attr::Aim,12); bump(Attr::Aggressiveness,14);
            bump(Attr::Headshot,8); bump(Attr::Communication,-10);
            bump(Attr::Adaptability,-6);
            // High motivation in ranked but harder to coach
            p.work_ethic = std::min(99, p.work_ethic + 10);
            break;
        case RookieArchetype::HighPotentialProject:
            // Low base bump now, big potential gap. Late bloomer.
            for (auto k : {Attr::Aim, Attr::Utility, Attr::GameSense}) bump(k, -6);
            p.potential = std::min(99, p.potential + 15);
            p.growth_archetype = GrowthArchetype::LateBloomer;
            p.peak_age = std::max(p.peak_age, 26);
            break;
        case RookieArchetype::InconsistentSuperstar:
            for (auto k : {Attr::Aim, Attr::Headshot, Attr::Entry}) bump(k, 12);
            p.potential = std::min(99, p.potential + 10);
            p.consistency = std::max(15, p.consistency - 25);
            break;
        case RookieArchetype::VeteranStyleRookie:
            // Already mature; minimal further growth (early peak).
            for (auto k : {Attr::DecisionMaking, Attr::Positioning,
                           Attr::GameSense, Attr::Intelligence}) bump(k, 8);
            p.growth_archetype = GrowthArchetype::EarlyPeaker;
            p.peak_age = std::min(p.peak_age, 22);
            p.consistency = std::min(99, p.consistency + 12);
            break;
        case RookieArchetype::DefensiveAnchor:
            bump(Attr::Anchor,18); bump(Attr::Positioning,14);
            bump(Attr::SpikeHandle,12); bump(Attr::Clutch,8);
            bump(Attr::Aggressiveness,-10);
            break;
        case RookieArchetype::LANPerformer:
            // Subtle bumps to clutch + reaction + adaptability —
            // simulation already weighs these in pro matches.
            bump(Attr::Reaction,10); bump(Attr::Clutch,10);
            bump(Attr::Adaptability,12); bump(Attr::DecisionMaking,8);
            p.consistency = std::min(99, p.consistency + 8);
            break;
        case RookieArchetype::FragileConfidence:
            for (auto k : {Attr::Aim, Attr::Headshot, Attr::Entry}) bump(k, 8);
            p.consistency = std::max(10, p.consistency - 30);
            p.work_ethic  = std::max(20, p.work_ethic  - 6);
            break;
        case RookieArchetype::HighIQStrategist:
            bump(Attr::Intelligence,18); bump(Attr::DecisionMaking,14);
            bump(Attr::AntiStrat,14); bump(Attr::GameSense,10);
            bump(Attr::Aim,-10); bump(Attr::Headshot,-8);
            break;
        case RookieArchetype::TeamChemistryPlayer:
            bump(Attr::Communication,16); bump(Attr::Adaptability,14);
            bump(Attr::Leadership,8); bump(Attr::DecisionMaking,6);
            // Glue guys are durable — slight consistency boost
            p.consistency = std::min(99, p.consistency + 10);
            break;
        case RookieArchetype::None: default: break;
    }
    p.update_agent_pool();
}

// ~75% of new spawns get an archetype (rest stay generic). The roll
// table is roughly uniform across archetypes; rare ones (Tactical IGL,
// High Potential Project) intentionally less common.
RookieArchetype roll_rookie_archetype() {
    if (rng().uniform() > 0.75) return RookieArchetype::None;
    int n = rng().irange(1, 16);
    static_assert(static_cast<int>(RookieArchetype::TeamChemistryPlayer) == 16,
                  "RookieArchetype enum must have 16 + None entries");
    return static_cast<RookieArchetype>(n);
}

}  // namespace

// === IGL spawn weighting helpers ==========================================
//
// IGLs are no longer rolled at a flat 25% across every primary_role. The
// player-facing distribution after a full season (VCT-realistic — INITIATORS
// lead the IGL role because recon/info feeds the shot-caller's mid-round read:
// FNS, Saadhak, ANGE1, Redgar, Boaster) lands at roughly:
//   * ~42% Initiators (the canonical IGL home — leads but not lopsided)
//   * ~36% Controllers
//   * ~22% Sentinels
//   * ~0-5% Duelists (rare — only elite minds, decentralised teams)
// See docs/VCT_REALISM_SPEC.md and the [14c] smoke test.
//
// Implementation: per-role base chance, mental-stat gate (must clear 0.55
// composite to qualify at all; Duelists need 0.78), then a mental_mult that
// scales the base chance up across the 0.55..1.0 mental band. The team-level
// Team::igl_candidate_score role_mult must track the SAME ordering so spawn
// and re-election agree.

static double base_igl_chance_for_role(Role r) noexcept {
    // NOTE: Initiator's base is set LOWER than Controller/Sentinel on purpose.
    // Initiators carry a natural IGL-mental edge (GameSense / Communication /
    // MidRoundCalling are core to the info role), so they win the team-level
    // election (Team::enforce_one_igl) more often than their spawn rate alone
    // implies. With these bases the player-facing distribution lands at roughly
    // Initiator 42% / Controller 36% / Sentinel 22% / Duelist ~0% — initiator
    // leads (the user's ~43% target) without being lopsided. Tuned against the
    // [14c] smoke test; see docs/VCT_REALISM_SPEC.md.
    switch (r) {
        case Role::Initiator:  return 0.24;
        case Role::Controller: return 0.27;
        case Role::Sentinel:   return 0.27;
        case Role::Duelist:    return 0.03;   // rare
        default:               return 0.10;
    }
}

// Composite mental score in [0..1]. Weighted toward calling/leadership
// attrs — the exact mix the IGL stat shift later rewards.
static double mental_score(const Player& p) noexcept {
    const auto& a = p.attributes;
    double s = (at(a, Attr::GameSense)       * 0.20
              + at(a, Attr::Communication)   * 0.18
              + at(a, Attr::MidRoundCalling) * 0.18
              + at(a, Attr::Intelligence)    * 0.15
              + at(a, Attr::Adaptability)    * 0.12
              + at(a, Attr::Leadership)      * 0.12
              + at(a, Attr::DecisionMaking)  * 0.05) / 99.0;
    return s;
}

// Returns true iff the player should be flagged is_igl at spawn given their
// primary_role + attribute profile. Pure read; mutates nothing.
static bool roll_is_igl_at_spawn(const Player& p) {
    double mental = mental_score(p);

    // Mental gate — must be at least average mind to ever be an IGL.
    if (mental < 0.55) return false;

    // Duelist hard floor — only elite-mind Duelists ever become IGLs.
    if (p.primary_role == Role::Duelist && mental < 0.78) return false;

    double base = base_igl_chance_for_role(p.primary_role);

    // Scale base chance by mental quality (above the 0.55 baseline).
    // Maps mental 0.55..1.00 -> mult 0.5..1.4.
    double mental_mult = 0.5 + (mental - 0.55) * 2.0;
    double prob = base * mental_mult;
    if (prob < 0.0) prob = 0.0;
    if (prob > 1.0) prob = 1.0;

    return rng().chance(prob);
}

// =========================================================================
// === Archetype: simulation-driving playstyle (THIRD system) ==============
// =========================================================================
//
// archetype_profile() is a PURE, O(1), table-driven switch — no allocation,
// no rng. Match.cpp calls it per duel. Every one of the 32 archetypes is
// hand-tuned to feel DISTINCT and to match its name. All values stay inside
// the ranges documented in ArchetypeProfile's struct comments.
ArchetypeProfile archetype_profile(Archetype a) {
    ArchetypeProfile p;   // neutral defaults (all mults 1.0, biases 0.0)
    switch (a) {
        case Archetype::MechanicalDemon:
            p = {1.15f, +0.5f, 0.0f, 1.10f, -0.04f, 0.0f, +0.2f, 0.0f,
                 1.05f, 0.0f, 0.0f, +0.12f, 0.0f, 0.0f, "Mechanical Demon"};
            break;
        case Archetype::HyperAggroEntry:
            p = {1.25f, +0.8f, 0.0f, 0.95f, -0.06f, +0.4f, +0.7f, 0.0f,
                 1.15f, -0.2f, 0.0f, +0.06f, +0.3f, 0.0f, "Hyper-Aggro Entry"};
            break;
        case Archetype::SmartLurker:
            p = {0.95f, -0.3f, +0.3f, 1.05f, +0.03f, -0.4f, -0.2f, 0.0f,
                 0.95f, +0.2f, +0.03f, 0.0f, +0.1f, +0.3f, "Smart Lurker"};
            break;
        case Archetype::IceColdClutcher:
            p = {1.0f, 0.0f, +0.1f, 1.25f, +0.08f, -0.1f, 0.0f, +0.05f,
                 0.80f, +0.2f, +0.06f, 0.0f, +0.2f, +0.1f, "Ice-Cold Clutcher"};
            break;
        case Archetype::UtilityGenius:
            p = {0.90f, -0.2f, +0.9f, 1.0f, +0.04f, 0.0f, -0.1f, 0.0f,
                 0.95f, +0.3f, +0.03f, 0.0f, +0.7f, +0.2f, "Utility Genius"};
            break;
        case Archetype::PassiveAnchor:
            p = {0.80f, -0.6f, +0.2f, 1.05f, +0.06f, -0.5f, -0.6f, 0.0f,
                 0.85f, +0.3f, +0.05f, 0.0f, +0.3f, 0.0f, "Passive Anchor"};
            break;
        case Archetype::TempoController:
            p = {1.0f, 0.0f, +0.3f, 1.0f, +0.03f, +0.3f, 0.0f, 0.0f,
                 1.0f, +0.5f, +0.02f, 0.0f, +0.4f, +0.4f, "Tempo Controller"};
            break;
        case Archetype::MomentumFragger:
            p = {1.10f, +0.4f, 0.0f, 1.05f, -0.10f, +0.2f, +0.4f, 0.0f,
                 1.40f, -0.1f, 0.0f, +0.13f, 0.0f, 0.0f, "Momentum Fragger"};
            break;
        case Archetype::LANDemon:
            p = {1.05f, +0.2f, 0.0f, 1.12f, +0.03f, 0.0f, +0.1f, +0.10f,
                 1.0f, 0.0f, +0.03f, +0.05f, +0.1f, +0.1f, "LAN Demon"};
            break;
        case Archetype::OnlineFarmer:
            p = {1.05f, +0.2f, 0.0f, 0.98f, -0.02f, +0.1f, +0.1f, -0.08f,
                 1.05f, 0.0f, 0.0f, +0.03f, 0.0f, 0.0f, "Online Farmer"};
            break;
        case Archetype::TacticalGenius:
            p = {0.95f, -0.1f, +0.5f, 1.05f, +0.05f, 0.0f, -0.1f, +0.03f,
                 0.95f, +0.6f, +0.04f, 0.0f, +0.5f, +0.8f, "Tactical Genius"};
            break;
        case Archetype::RiskyPlaymaker:
            p = {1.15f, +0.5f, 0.0f, 1.05f, -0.10f, +0.3f, +0.9f, 0.0f,
                 1.25f, -0.3f, 0.0f, +0.14f, 0.0f, +0.1f, "Risky Playmaker"};
            break;
        case Archetype::ConsistencyMachine:
            p = {1.0f, 0.0f, +0.2f, 1.0f, +0.12f, 0.0f, -0.2f, +0.02f,
                 0.75f, +0.3f, +0.10f, -0.02f, +0.3f, +0.1f, "Consistency Machine"};
            break;
        case Archetype::ConfidencePlayer:
            p = {1.10f, +0.3f, 0.0f, 1.05f, -0.06f, +0.2f, +0.3f, 0.0f,
                 1.35f, -0.1f, 0.0f, +0.10f, 0.0f, 0.0f, "Confidence Player"};
            break;
        case Archetype::SlowMethodical:
            p = {0.88f, -0.3f, +0.3f, 1.0f, +0.05f, -0.7f, -0.4f, 0.0f,
                 0.90f, +0.6f, +0.04f, 0.0f, +0.3f, +0.2f, "Slow Methodical"};
            break;
        case Archetype::HighCeilingLowFloor:
            // consistency_floor modeled as NEGATIVE -> wider downside.
            p = {1.12f, +0.4f, 0.0f, 1.05f, -0.12f, +0.2f, +0.5f, 0.0f,
                 1.30f, -0.2f, -0.06f, +0.15f, 0.0f, 0.0f, "High Ceiling, Low Floor"};
            break;
        case Archetype::AggressiveAWPer:
            p = {1.10f, +0.4f, 0.0f, 1.05f, -0.03f, +0.1f, +0.3f, 0.0f,
                 1.10f, +0.1f, 0.0f, +0.08f, 0.0f, 0.0f, "Aggressive AWPer"};
            break;
        case Archetype::SupportiveFacilitator:
            p = {0.85f, -0.3f, +0.6f, 1.0f, +0.05f, 0.0f, -0.3f, 0.0f,
                 0.95f, +0.4f, +0.04f, 0.0f, +0.9f, +0.3f, "Supportive Facilitator"};
            break;
        case Archetype::FlexGenius:
            p = {1.0f, 0.0f, +0.3f, 1.0f, +0.04f, +0.1f, 0.0f, 0.0f,
                 1.0f, +0.2f, +0.03f, +0.02f, +0.5f, +0.9f, "Flex Genius"};
            break;
        case Archetype::AntiStratMaster:
            p = {0.95f, -0.1f, +0.5f, 1.05f, +0.04f, 0.0f, -0.1f, +0.03f,
                 0.95f, +0.4f, +0.03f, 0.0f, +0.4f, +0.8f, "Anti-Strat Master"};
            break;
        case Archetype::EcoSpecialist:
            p = {1.0f, +0.1f, +0.2f, 1.0f, +0.02f, +0.1f, +0.2f, 0.0f,
                 1.05f, +0.9f, +0.02f, 0.0f, +0.3f, +0.2f, "Eco Specialist"};
            break;
        case Archetype::EntrySacrifice:
            p = {1.20f, +0.7f, +0.1f, 0.90f, -0.04f, +0.3f, +0.6f, 0.0f,
                 1.05f, -0.1f, 0.0f, +0.05f, +0.8f, 0.0f, "Entry Sacrifice"};
            break;
        case Archetype::TeamFirstGlue:
            p = {0.90f, -0.2f, +0.5f, 1.0f, +0.07f, 0.0f, -0.2f, 0.0f,
                 0.90f, +0.4f, +0.08f, 0.0f, +0.9f, +0.3f, "Team-First Glue"};
            break;
        case Archetype::OverheatingSuperstar:
            p = {1.15f, +0.5f, 0.0f, 1.15f, -0.12f, +0.3f, +0.5f, 0.0f,
                 1.40f, -0.2f, 0.0f, +0.15f, 0.0f, 0.0f, "Overheating Superstar"};
            break;
        case Archetype::RookieMechFreak:
            p = {1.15f, +0.5f, 0.0f, 1.0f, -0.11f, +0.2f, +0.4f, -0.05f,
                 1.20f, -0.2f, 0.0f, +0.13f, 0.0f, 0.0f, "Rookie Mech Freak"};
            break;
        case Archetype::VeteranStabilizer:
            p = {0.95f, -0.1f, +0.3f, 1.05f, +0.10f, -0.1f, -0.3f, +0.06f,
                 0.70f, +0.4f, +0.12f, -0.03f, +0.4f, +0.4f, "Veteran Stabilizer"};
            break;
        case Archetype::ClinicalCloser:
            p = {1.0f, +0.1f, +0.1f, 1.22f, +0.05f, 0.0f, 0.0f, +0.03f,
                 0.90f, +0.3f, +0.05f, +0.02f, +0.2f, +0.1f, "Clinical Closer"};
            break;
        case Archetype::ChaosAgent:
            p = {1.15f, +0.5f, -0.1f, 1.05f, -0.12f, +0.4f, +1.0f, 0.0f,
                 1.35f, -0.4f, 0.0f, +0.12f, -0.1f, 0.0f, "Chaos Agent"};
            break;
        case Archetype::ZoneController:
            p = {0.88f, -0.2f, +0.7f, 1.0f, +0.04f, -0.2f, -0.2f, 0.0f,
                 0.95f, +0.3f, +0.04f, 0.0f, +0.5f, +0.2f, "Zone Controller"};
            break;
        case Archetype::RetakeSpecialist:
            p = {0.95f, -0.1f, +0.6f, 1.10f, +0.04f, -0.1f, 0.0f, 0.0f,
                 0.95f, +0.2f, +0.04f, 0.0f, +0.3f, +0.3f, "Retake Specialist"};
            break;
        case Archetype::DuelistDiva:
            p = {1.20f, +0.6f, -0.2f, 1.08f, -0.05f, +0.3f, +0.5f, 0.0f,
                 1.20f, -0.2f, 0.0f, +0.12f, -0.4f, 0.0f, "Duelist Diva"};
            break;
        case Archetype::QuietProfessional:
        default:
            // Safe near-neutral default. Slightly steadier + mild teamplay.
            p = {1.0f, 0.0f, +0.1f, 1.0f, +0.04f, 0.0f, 0.0f, 0.0f,
                 1.0f, +0.1f, +0.02f, +0.01f, +0.3f, +0.1f, "Quiet Professional"};
            break;
    }
    return p;
}

const char* archetype_name(Archetype a) {
    return archetype_profile(a).name;
}

// === Spawn-time Archetype assignment ======================================
//
// Scores every archetype's "fit" against the freshly generated player's
// standout attributes + role + rookie_archetype, then picks among the top
// few with rng weighting so two same-role players USUALLY differ. Always
// assigns a valid value. Pure scoring + a single weighted pick at the end.
static Archetype pick_spawn_archetype(const Player& p) {
    const auto& a = p.attributes;
    auto A = [&](Attr k) { return at(a, k); };

    constexpr int N = static_cast<int>(Archetype::Count);
    std::array<double, N> sc{};
    sc.fill(1.0);   // every archetype always has a small floor chance

    auto add = [&](Archetype k, double v) { sc[static_cast<int>(k)] += v; };

    const Role  role = p.primary_role;
    const bool  igl  = p.is_igl;
    const RookieArchetype ra = p.rookie_archetype;
    const int   cons = p.consistency;
    const int   pot  = p.potential;
    const int   age  = p.age;

    // --- Mechanical / fragging cluster ---
    double mech = (A(Attr::Aim) + A(Attr::Headshot) + A(Attr::Reaction)
                   + A(Attr::CrosshairPlacement)) / 4.0;
    if (mech >= 78) add(Archetype::MechanicalDemon, (mech - 70) * 0.9);
    if (mech >= 72 && age <= 21 && pot >= 75)
        add(Archetype::RookieMechFreak, (mech - 65) * 0.8 + (pot - 70) * 0.4);
    if (mech >= 75 && cons < 60)
        add(Archetype::HighCeilingLowFloor, (mech - 65) * 0.7 + (60 - cons) * 0.5);
    if (mech >= 75 && cons < 55)
        add(Archetype::ChaosAgent, (mech - 65) * 0.5 + (55 - cons) * 0.6
            + A(Attr::Aggressiveness) * 0.2);

    // --- Entry / aggression cluster ---
    double entry = (A(Attr::Entry) + A(Attr::Aggressiveness)
                    + A(Attr::Movement)) / 3.0;
    if (entry >= 70) add(Archetype::HyperAggroEntry, (entry - 60) * 0.8);
    if (entry >= 65 && A(Attr::Communication) >= 60)
        add(Archetype::EntrySacrifice, (entry - 58) * 0.6
            + A(Attr::Communication) * 0.2);
    if (A(Attr::Aim) >= 75 && A(Attr::Aggressiveness) >= 70 && cons < 70)
        add(Archetype::MomentumFragger, (A(Attr::Aim) - 60) * 0.4
            + (70 - cons) * 0.4);
    if (A(Attr::Awping) >= 70)
        add(Archetype::AggressiveAWPer, (A(Attr::Awping) - 55) * 1.1);

    // --- Clutch / mental closer cluster ---
    double clutch = (A(Attr::Clutch) + A(Attr::DecisionMaking)) / 2.0;
    if (A(Attr::Clutch) >= 78)
        add(Archetype::IceColdClutcher, (A(Attr::Clutch) - 68) * 0.8
            + cons * 0.2);
    if (clutch >= 75 && A(Attr::GameSense) >= 65)
        add(Archetype::ClinicalCloser, (clutch - 65) * 0.7);

    // --- Utility / support cluster ---
    double util = (A(Attr::Utility) + A(Attr::Communication)) / 2.0;
    if (A(Attr::Utility) >= 72)
        add(Archetype::UtilityGenius, (A(Attr::Utility) - 62) * 0.8);
    if (util >= 68 && A(Attr::Aim) < 75)
        add(Archetype::SupportiveFacilitator, (util - 58) * 0.7);
    if (A(Attr::Communication) >= 70 && A(Attr::Adaptability) >= 65)
        add(Archetype::TeamFirstGlue, (A(Attr::Communication) - 58) * 0.5
            + cons * 0.15);

    // --- Lurk / positioning cluster ---
    if (A(Attr::Lurking) >= 72)
        add(Archetype::SmartLurker, (A(Attr::Lurking) - 60) * 0.9
            + A(Attr::Positioning) * 0.15);

    // --- IGL-brain / tactical cluster ---
    double brain = (A(Attr::Intelligence) + A(Attr::MidRoundCalling)
                    + A(Attr::AntiStrat)) / 3.0;
    if (brain >= 65)
        add(Archetype::TacticalGenius, (brain - 55) * 0.7 + (igl ? 8.0 : 0.0));
    if (A(Attr::AntiStrat) >= 68)
        add(Archetype::AntiStratMaster, (A(Attr::AntiStrat) - 56) * 0.8
            + (igl ? 5.0 : 0.0));
    if (A(Attr::EconomyMgmt) >= 70)
        add(Archetype::EcoSpecialist, (A(Attr::EconomyMgmt) - 58) * 0.8);
    if (A(Attr::Adaptability) >= 72 && p.agent_pool_size >= 4)
        add(Archetype::FlexGenius, (A(Attr::Adaptability) - 60) * 0.7
            + p.agent_pool_size * 1.5);
    if (igl) {
        add(Archetype::TempoController, 6.0 + A(Attr::DecisionMaking) * 0.1);
        add(Archetype::SlowMethodical, 4.0);
    }

    // --- Tempo / pacing cluster ---
    if (A(Attr::Positioning) >= 70 && A(Attr::GameSense) >= 65
        && A(Attr::Aggressiveness) < 60)
        add(Archetype::SlowMethodical, (A(Attr::Positioning) - 58) * 0.4);

    // --- Consistency / stability cluster ---
    if (cons >= 80)
        add(Archetype::ConsistencyMachine, (cons - 65) * 0.8);
    if (cons >= 78 && age >= 25)
        add(Archetype::VeteranStabilizer, (cons - 65) * 0.6 + (age - 24) * 1.2);
    if (cons < 62)
        add(Archetype::ConfidencePlayer, (62 - cons) * 0.6
            + A(Attr::Aim) * 0.1);
    if (cons < 55 && (A(Attr::Aim) >= 78 || A(Attr::Headshot) >= 78))
        add(Archetype::OverheatingSuperstar, (55 - cons) * 0.7
            + std::max(A(Attr::Aim), A(Attr::Headshot)) * 0.2);

    // --- LAN / online split (use Stamina + Adaptability as proxy) ---
    if (A(Attr::Stamina) >= 70 && A(Attr::Adaptability) >= 60)
        add(Archetype::LANDemon, (A(Attr::Stamina) - 58) * 0.5);
    if (A(Attr::Adaptability) < 55)
        add(Archetype::OnlineFarmer, (55 - A(Attr::Adaptability)) * 0.5
            + A(Attr::Aim) * 0.1);

    // --- Risk lean ---
    if (A(Attr::Aggressiveness) >= 72 && cons < 70)
        add(Archetype::RiskyPlaymaker, (A(Attr::Aggressiveness) - 60) * 0.5
            + (70 - cons) * 0.4);

    // --- Role gating: bias archetypes that fit the player's position ---
    switch (role) {
        case Role::Duelist:
            add(Archetype::DuelistDiva,    6.0 + A(Attr::Aim) * 0.12);
            add(Archetype::HyperAggroEntry, 5.0);
            add(Archetype::EntrySacrifice,  4.0);
            add(Archetype::MechanicalDemon, 3.0);
            break;
        case Role::Controller:
            add(Archetype::ZoneController,  7.0 + A(Attr::Utility) * 0.10);
            add(Archetype::SlowMethodical,  3.0);
            add(Archetype::TempoController, 3.0);
            break;
        case Role::Sentinel:
            add(Archetype::PassiveAnchor,    6.0 + A(Attr::Anchor) * 0.10);
            add(Archetype::RetakeSpecialist, 5.0);
            add(Archetype::IceColdClutcher,  3.0);
            break;
        case Role::Initiator:
            add(Archetype::UtilityGenius,          5.0);
            add(Archetype::SupportiveFacilitator,  4.0);
            add(Archetype::TacticalGenius,         3.0);
            break;
        default: break;
    }
    if (igl) add(Archetype::AntiStratMaster, 4.0);

    // --- Rookie-archetype affinity nudges (the growth template biases the
    //     stable simulation archetype, but never forces it) ---
    switch (ra) {
        case RookieArchetype::MechanicalProdigy:
        case RookieArchetype::RawAimer:
            add(Archetype::MechanicalDemon, 6.0);
            add(Archetype::RookieMechFreak, 4.0); break;
        case RookieArchetype::TacticalIGL:
        case RookieArchetype::HighIQStrategist:
            add(Archetype::TacticalGenius, 6.0);
            add(Archetype::AntiStratMaster, 4.0); break;
        case RookieArchetype::ClutchSpecialist:
            add(Archetype::IceColdClutcher, 6.0);
            add(Archetype::ClinicalCloser, 4.0); break;
        case RookieArchetype::AggressiveEntry:
            add(Archetype::HyperAggroEntry, 6.0);
            add(Archetype::EntrySacrifice, 4.0); break;
        case RookieArchetype::SupportMastermind:
            add(Archetype::UtilityGenius, 6.0);
            add(Archetype::SupportiveFacilitator, 4.0); break;
        case RookieArchetype::FlexibleUtility:
            add(Archetype::FlexGenius, 6.0); break;
        case RookieArchetype::SoloQueueDemon:
            add(Archetype::OnlineFarmer, 6.0);
            add(Archetype::DuelistDiva, 3.0); break;
        case RookieArchetype::HighPotentialProject:
            add(Archetype::RookieMechFreak, 5.0);
            add(Archetype::HighCeilingLowFloor, 4.0); break;
        case RookieArchetype::InconsistentSuperstar:
            add(Archetype::OverheatingSuperstar, 6.0);
            add(Archetype::HighCeilingLowFloor, 4.0); break;
        case RookieArchetype::VeteranStyleRookie:
            add(Archetype::VeteranStabilizer, 5.0);
            add(Archetype::ConsistencyMachine, 4.0); break;
        case RookieArchetype::DefensiveAnchor:
            add(Archetype::PassiveAnchor, 6.0);
            add(Archetype::RetakeSpecialist, 4.0); break;
        case RookieArchetype::LANPerformer:
            add(Archetype::LANDemon, 7.0); break;
        case RookieArchetype::FragileConfidence:
            add(Archetype::ConfidencePlayer, 6.0);
            add(Archetype::HighCeilingLowFloor, 3.0); break;
        case RookieArchetype::TeamChemistryPlayer:
            add(Archetype::TeamFirstGlue, 6.0);
            add(Archetype::SupportiveFacilitator, 3.0); break;
        case RookieArchetype::None:
        default: break;
    }

    // Take the top ~4 fits and pick among them with rng weighting so
    // two same-role players USUALLY land on different archetypes.
    std::array<int, N> idx{};
    for (int i = 0; i < N; ++i) idx[i] = i;
    std::partial_sort(idx.begin(), idx.begin() + 4, idx.end(),
                      [&](int x, int y) { return sc[x] > sc[y]; });

    std::vector<double> w;
    w.reserve(4);
    for (int i = 0; i < 4; ++i) w.push_back(std::max(0.01, sc[idx[i]]));
    int chosen = idx[rng().weighted_index(w)];
    return static_cast<Archetype>(chosen);
}

// === Desire name + blurb tables ===========================================
const char* desire_name(Desire d) noexcept {
    switch (d) {
        case Desire::Greedy:          return "Greedy";
        case Desire::Loyal:           return "Loyal";
        case Desire::RingChaser:      return "Ring Chaser";
        case Desire::Mercenary:       return "Mercenary";
        case Desire::StabilitySeeker: return "Stability Seeker";
        case Desire::Competitor:      return "Competitor";
        case Desire::Count:           break;
    }
    return "?";
}

const char* desire_blurb(Desire d) noexcept {
    switch (d) {
        case Desire::Greedy:          return "Max salary, short deals — re-evaluates the market every year.";
        case Desire::Loyal:           return "Stays put when he can; loves the re-sign, hates moving.";
        case Desire::RingChaser:      return "Joins contenders for less; runs from rebuilders.";
        case Desire::Mercenary:       return "No team loyalty; short contracts, max ask everywhere.";
        case Desire::StabilitySeeker: return "Long deals only; avoids aggressive or win-now orgs.";
        case Desire::Competitor:      return "Wants strong teammates; allergic to rebuilds.";
        case Desire::Count:           break;
    }
    return "?";
}

// === Spawn-time Desire assignment =========================================
//
// Scores all 6 desire archetypes against the freshly-generated player's
// attributes + age + archetype + rookie_archetype, then picks among the top
// 4 with rng weighting. Default floor 1.0 per desire so any desire can
// occasionally surface. Deterministic given a fixed rng seed (matches the
// project guide's smoke-test reproducibility requirement).
static Desire pick_spawn_desire(const Player& p) {
    const auto& a = p.attributes;
    auto A = [&](Attr k) { return at(a, k); };

    constexpr int N = static_cast<int>(Desire::Count);
    std::array<double, N> sc{};
    sc.fill(1.0);   // floor — every desire keeps a small chance

    auto add = [&](Desire k, double v) { sc[static_cast<int>(k)] += v; };

    const int   age  = p.age;
    const int   cons = p.consistency;
    const double ovr_now = p.ovr();
    const RookieArchetype ra = p.rookie_archetype;
    const Archetype       ar = p.archetype;
    const int chemistry_proxy = (A(Attr::Communication) + A(Attr::Adaptability)) / 2;

    // Trophy count proxy. Player has no career_trophies counter; the closest
    // pure-read equivalent is trophy_summary() which prefix-counts [T]/[M]/[W]
    // awards. Cheap (a single string-prefix sweep) so OK to call inside spawn.
    const int career_trophies = p.trophy_summary().total_trophies();

    // --- Greedy: pure ego (aggression, low consistency, young) ---
    if (A(Attr::Aggressiveness) >= 75) add(Desire::Greedy, 6.0);
    if (cons < 55)                     add(Desire::Greedy, 4.0);
    if (age < 22)                      add(Desire::Greedy, 3.0);

    // --- Loyal: chemistry-leaning, mature, team-chem archetype ---
    if (chemistry_proxy >= 70)                            add(Desire::Loyal, 6.0);
    if (ra == RookieArchetype::TeamChemistryPlayer)       add(Desire::Loyal, 5.0);
    if (age >= 26)                                         add(Desire::Loyal, 4.0);

    // --- RingChaser: veteran without a ring, still good ---
    if (age >= 28)                                                         add(Desire::RingChaser, 6.0);
    if (career_trophies == 0 && ovr_now >= 70.0)                           add(Desire::RingChaser, 5.0);
    if (ra == RookieArchetype::VeteranStyleRookie)                         add(Desire::RingChaser, 4.0);

    // --- Mercenary: aggro, low comm, "diva"-style archetypes ---
    if (A(Attr::Aggressiveness) >= 80)        add(Desire::Mercenary, 6.0);
    if (A(Attr::Communication) < 55)          add(Desire::Mercenary, 4.0);
    if (ar == Archetype::DuelistDiva ||
        ar == Archetype::ChaosAgent  ||
        ar == Archetype::OverheatingSuperstar) add(Desire::Mercenary, 4.0);

    // --- StabilitySeeker: high consistency, steady-archetype, older ---
    if (cons >= 75)                            add(Desire::StabilitySeeker, 6.0);
    if (ar == Archetype::ConsistencyMachine ||
        ar == Archetype::VeteranStabilizer  ||
        ar == Archetype::QuietProfessional  ||
        ar == Archetype::TeamFirstGlue)        add(Desire::StabilitySeeker, 4.0);
    if (age >= 27)                             add(Desire::StabilitySeeker, 3.0);

    // --- Competitor: clutch genes, brain archetypes, IGLs ---
    if (A(Attr::Clutch) >= 75)                 add(Desire::Competitor, 6.0);
    if (ar == Archetype::IceColdClutcher ||
        ar == Archetype::ClinicalCloser  ||
        ar == Archetype::TacticalGenius  ||
        ar == Archetype::AntiStratMaster)      add(Desire::Competitor, 5.0);
    if (p.is_igl)                              add(Desire::Competitor, 4.0);

    // Take the top 4 fits and pick among them via weighted_index. Tie
    // resolution: weighted_index picks proportionally to weight, so a true
    // tie leaves the result up to the rng; the spec's "default to
    // Competitor on tie" intent is honoured by Competitor having the
    // widest, most-likely-to-fire bonus list so it naturally dominates the
    // wash case.
    std::array<int, N> idx{};
    for (int i = 0; i < N; ++i) idx[i] = i;
    std::partial_sort(idx.begin(), idx.begin() + 4, idx.end(),
                      [&](int x, int y) { return sc[x] > sc[y]; });

    std::vector<double> w;
    w.reserve(4);
    for (int i = 0; i < 4; ++i) w.push_back(std::max(0.01, sc[idx[i]]));
    int chosen = idx[rng().weighted_index(w)];
    return static_cast<Desire>(chosen);
}

// === Desire-driven negotiation accessors ==================================
// All three are pure read functions. Magnitudes intentionally modest so
// the existing gen_contract baselines + smoke-test #12/#13 ranges still
// hold; Desire layers on top, it does not replace.

namespace {
    // Average OVR of a team's roster, skipping empty slots. Defaults to 70
    // when fewer than 3 players are present (early-world or near-empty)
    // per the spec.
    double avg_roster_ovr(const Team& team) {
        double sum = 0.0;
        int    n   = 0;
        for (const auto& pp : team.roster) {
            if (!pp) continue;
            sum += pp->ovr();
            ++n;
        }
        if (n < 3) return 70.0;
        return sum / static_cast<double>(n);
    }

    // Helpers — kept here so the table-shape of each accessor stays readable.
    bool team_is_contender(const Team& t) {
        return t.strategy == Team::Strategy::Contender
            || t.strategy == Team::Strategy::WinNow;
    }
    bool team_is_rebuilder(const Team& t) {
        return t.strategy == Team::Strategy::Rebuilding
            || t.strategy == Team::Strategy::TalentFarm;
    }
}

double Player::desire_salary_mult(const Team& team) const {
    switch (desire) {
        case Desire::Greedy:
            return 1.20;   // loud asks regardless of opponent

        case Desire::Loyal:
            return (team.name == team_name) ? 1.05 : 1.10;

        case Desire::RingChaser:
            return team_is_contender(team) ? 0.85 : 1.15;

        case Desire::Mercenary:
            return 1.18;

        case Desire::StabilitySeeker: {
            if (team.strategy == Team::Strategy::DevelopmentFocus
             || team.strategy == Team::Strategy::Bridge
             || team.strategy == Team::Strategy::BudgetRoster) return 0.92;
            if (team.personality == Personality::Aggressive
             || team.strategy   == Team::Strategy::WinNow)     return 1.08;
            return 1.00;
        }

        case Desire::Competitor: {
            double avg = avg_roster_ovr(team);
            if (avg >= 78.0)                return 0.90;   // join contenders cheap
            if (team_is_rebuilder(team))    return 1.20;   // overpay-to-avoid
            return 1.00;
        }

        case Desire::Count: break;
    }
    return 1.00;
}

bool Player::desire_accepts(int offer_k, const Team& team) const {
    // gen_contract's stored asking baseline lives on the player's own
    // `contract` field (refreshed by the FA path); fall back to amount_k
    // for the comparison thresholds the spec calls out.
    const int ask = contract.amount_k;

    switch (desire) {
        case Desire::Greedy:
            // Won't take ANY discount worth speaking of.
            return offer_k >= static_cast<int>(ask * 0.95);

        case Desire::Loyal:
            // Never rejects the current team's offer. Otherwise needs
            // 90%+ of asking price from outside suitors.
            if (team.name == team_name) return true;
            return offer_k >= static_cast<int>(ask * 0.90);

        case Desire::RingChaser:
            // Refuses to spend his remaining ring years on a rebuild.
            if (age >= 28 && team_is_rebuilder(team))                return false;
            if (age >= 28 && team.strategy == Team::Strategy::BudgetRoster) return false;
            return true;

        case Desire::Mercenary:
            // Refuses long deals — period. The Team-side signing path is
            // expected to consult desire_length_pref BEFORE making the
            // offer; this gate is the belt-and-braces backup.
            if (desire_length_pref(team) >= 0.7) return false;
            return true;

        case Desire::StabilitySeeker:
            if (team.personality == Personality::Aggressive)      return false;
            if (team.strategy    == Team::Strategy::WinNow)       return false;
            return true;

        case Desire::Competitor:
            // Won't sign with a weak team in his prime.
            if (age <= 26 && avg_roster_ovr(team) < 65.0) return false;
            return true;

        case Desire::Count: break;
    }
    return true;
}

double Player::desire_length_pref(const Team& team) const {
    switch (desire) {
        case Desire::Greedy:          return 0.20;
        case Desire::Loyal:           return (team.name == team_name) ? 0.85 : 0.50;
        case Desire::RingChaser:      return team_is_contender(team) ? 0.15 : 0.55;
        case Desire::Mercenary:       return 0.10;
        case Desire::StabilitySeeker: return 0.90;
        case Desire::Competitor:      return 0.65;
        case Desire::Count:           break;
    }
    return 0.50;
}

// === Re-sign offer API ===================================================
// All the logic for "what would this player ask for if we approached him?".
// Builds the ResignOffer in one O(roster) pass and applies desire-driven
// loyalty/ring/mercenary modifiers on top of gen_contract's existing
// team-aware baseline.
Player::ResignOffer Player::propose_resign_offer(const Team& team, int current_year) const {
    ResignOffer out;

    // ---- amount_k ----------------------------------------------------------
    // Start from gen_contract's team-aware baseline (already routes through
    // desire_salary_mult internally for the 4-arg overload).
    Contract base = gen_contract(current_year, true, false, &team);
    double amount = static_cast<double>(base.amount_k);

    const bool same_team = (team.name == team_name);

    // Loyal discount: ~15% off for staying with the current org.
    if (desire == Desire::Loyal && same_team) {
        amount *= 0.85;
    }
    // RingChaser discount: ~15% off to chase rings on a contender.
    if (desire == Desire::RingChaser
        && (team.strategy == Team::Strategy::Contender
         || team.strategy == Team::Strategy::WinNow)) {
        amount *= 0.85;
    }
    // Mercenary premium: paid full or walks.
    if (desire == Desire::Mercenary) {
        amount *= 1.10;
    }

    out.amount_k = clamp_v(static_cast<int>(std::round(amount)),
                           kSalaryFloorK, kSalaryCapK);

    // ---- years -------------------------------------------------------------
    // Map desire_length_pref's 0..1 onto a 1..4 contract bucket. Boundaries
    // are inclusive at the top of each band: lp=0.50 -> 2 years, lp=0.75 ->
    // 3 years. Matches the spec example "0.50 player tolerates up to 3
    // years" once max_acceptable_years adds the +1 headroom below.
    double lp = desire_length_pref(team);
    if      (lp <= 0.25) out.years = 1;
    else if (lp <= 0.50) out.years = 2;
    else if (lp <= 0.75) out.years = 3;
    else                 out.years = 4;

    // max_acceptable_years — matches evaluate_resign_offer's rebalanced buckets
    // (preferred + 2): even a short-pref personality tolerates 3 years; length
    // preference prices the deal rather than hard-capping it.
    out.max_acceptable_years = std::min(5, out.years + 2);

    // ---- willingness -------------------------------------------------------
    double w = 0.50;

    // Desire-keyed swings -----------------------------------------------------
    switch (desire) {
        case Desire::Loyal:
            w += same_team ? 0.30 : -0.10;
            break;
        case Desire::RingChaser:
            if (team.strategy == Team::Strategy::Contender
             || team.strategy == Team::Strategy::WinNow) w += 0.25;
            else                                          w -= 0.20;
            break;
        case Desire::StabilitySeeker:
            w += (team.personality != Personality::Aggressive) ? 0.20 : -0.15;
            break;
        case Desire::Competitor: {
            double avg = avg_roster_ovr(team);
            if      (avg >= 78.0) w += 0.20;
            else if (avg <  65.0) w -= 0.15;
            break;
        }
        case Desire::Greedy:    w -= 0.05; break;
        case Desire::Mercenary: w -= 0.10; break;
        case Desire::Count:                break;
    }

    // Years-on-team bonus — proxy via salary_log entries (vector<pair<year,
    // amount>>). salary_log has no team breakdown, so we only count tenure
    // when same_team (otherwise this would over-credit cross-team tenure).
    // Each prior season under contract = +0.05, capped at +0.20.
    if (same_team) {
        int cnt = 0;
        for (const auto& kv : salary_log) {
            if (kv.first < current_year) ++cnt;
        }
        w += std::min(0.20, 0.05 * static_cast<double>(cnt));
    }

    // Team success bonus — any roster player has a [T]/[M]/[W] in the last
    // 2 seasons => +0.10.
    {
        bool recent_team_trophy = false;
        std::string yrs[3] = {
            std::to_string(current_year),
            std::to_string(current_year - 1),
            std::to_string(current_year - 2)
        };
        for (const auto& pp : team.roster) {
            if (!pp) continue;
            for (const auto& a : pp->awards) {
                if (a.empty()) continue;
                // Prefix-anchored — [T]/[M]/[W]
                if (a.size() >= 3
                    && a[0] == '['
                    && (a[1] == 'T' || a[1] == 'M' || a[1] == 'W')
                    && a[2] == ']') {
                    for (auto& yr : yrs) {
                        if (a.find(yr) != std::string::npos) {
                            recent_team_trophy = true;
                            break;
                        }
                    }
                }
                if (recent_team_trophy) break;
            }
            if (recent_team_trophy) break;
        }
        if (recent_team_trophy) w += 0.10;
    }

    // Prestige bonus.
    if      (team.prestige >= 80) w += 0.10;
    else if (team.prestige >= 65) w += 0.05;

    out.willingness = clamp_v(w, 0.05, 0.95);

    // ---- min_acceptable_k --------------------------------------------------
    // Greedy / Mercenary leave very little room (5%); everyone else 15%.
    double floor_frac = (desire == Desire::Greedy || desire == Desire::Mercenary)
                            ? 0.95 : 0.85;
    out.min_acceptable_k = clamp_v(
        static_cast<int>(std::round(out.amount_k * floor_frac)),
        kSalaryFloorK, kSalaryCapK);

    // ---- explainer ---------------------------------------------------------
    switch (desire) {
        case Desire::Loyal:
            out.explainer = same_team
                ? "Loyal to current org — willing to take a small discount"
                : "Loyal to current org — open to outside offers at a premium";
            break;
        case Desire::RingChaser:
            if (team.strategy == Team::Strategy::Contender
             || team.strategy == Team::Strategy::WinNow)
                out.explainer = "RingChaser — wants a contender shot";
            else
                out.explainer = "RingChaser — wary of joining a non-contender";
            break;
        case Desire::Mercenary:
            out.explainer = "Mercenary — paid full or walks";
            break;
        case Desire::Greedy:
            out.explainer = "Greedy — max salary, short deal";
            break;
        case Desire::StabilitySeeker:
            out.explainer = (team.personality != Personality::Aggressive)
                ? "StabilitySeeker — long deal with a steady org"
                : "StabilitySeeker — uneasy with aggressive org";
            break;
        case Desire::Competitor:
            out.explainer = (avg_roster_ovr(team) >= 78.0)
                ? "Competitor — joins a strong roster"
                : "Competitor — wants stronger teammates";
            break;
        case Desire::Count: break;
    }
    if (out.explainer.empty()) out.explainer = "Open to offers";

    return out;
}

// Score threshold above which an offer is accepted. Sits above the "fair"
// band so an offer at the player's ASK reliably accepts (~80 score) while
// significant discounts (~45-55 score) fall through.
static constexpr int kResignAcceptThreshold = 55;

Player::ResignBreakdown Player::evaluate_resign_offer(int amount_k, int years,
                                                       const Team& team) const {
    // 3-arg delegator — natural role, no bonus/promise.
    return evaluate_resign_offer(amount_k, years, team, primary_role, 0, false);
}

Player::ResignBreakdown Player::evaluate_resign_offer(int amount_k, int years,
                                                       const Team& team,
                                                       Role offered_role) const {
    // 4-arg delegator — role-aware, no bonus/promise.
    return evaluate_resign_offer(amount_k, years, team, offered_role, 0, false);
}

Player::ResignBreakdown Player::evaluate_resign_offer(int amount_k, int years,
                                                       const Team& team,
                                                       Role offered_role,
                                                       int signing_bonus_k,
                                                       bool promise_starter) const {
    ResignBreakdown b;
    // DETERMINISTIC ask reference — must not jitter per frame because the
    // UI calls this every frame for the live breakdown. `propose_resign_offer`
    // calls `gen_contract(randomize_amount=true)` internally, which re-rolls
    // ±10-20% per call and would make the breakdown labels oscillate (which
    // visually shifts the ImGui::Columns(2) right column → the slider jumps
    // left/right). Compute a stable ask via gen_contract(randomize=false).
    ResignOffer ro;
    Contract det = gen_contract(/*current_year=*/0,
                                /*randomize_amount=*/false,
                                /*randomize_exp=*/false,
                                &team);
    ro.amount_k = det.amount_k;
    // Min acceptable = 85% of ask (Greedy/Mercenary get 95% in the desire
    // gate). max_acceptable_years comes from desire_length_pref bucket + 1.
    // REBALANCED 2026-06-28: the old buckets (1..4 → max 2..5) made 3+ year
    // deals IMPOSSIBLE for Mercenary/Greedy/RingChaser and capped neutral
    // players at 3 — combined with the years hard-block below, no amount of
    // money could buy a longer deal (user report: "can't re-sign anyone past
    // 2 years"). New buckets: even the shortest-pref personality tolerates 3
    // years; a neutral player 4; Loyal/StabilitySeeker the full 5. Length
    // preference still prices the deal via years_mod — it's a cost, not a wall.
    double lp = desire_length_pref(team);
    int year_bucket = (lp <= 0.25) ? 2
                     : (lp <= 0.50) ? 3
                     : (lp <= 0.75) ? 4 : 5;
    ro.years = year_bucket - 1;                        // preferred length (1..4)
    ro.max_acceptable_years = std::min(5, year_bucket + 1);
    bool stiff = (desire == Desire::Greedy || desire == Desire::Mercenary);
    // Round (not truncate) AND clamp to [floor, cap] to match
    // propose_resign_offer's min_acceptable_k so the UI breakdown and the
    // engine decision never disagree.
    ro.min_acceptable_k = clamp_v(
        static_cast<int>(std::round(ro.amount_k * (stiff ? 0.95 : 0.85))),
        kSalaryFloorK, kSalaryCapK);
    bool same_team = (team.name == team_name);

    // --- 1. Salary vs ASK ------------------------------------------------
    // Monotonic in `amount_k`: every dollar above ask increases the score,
    // every dollar below decreases it. Cap raised 2026-05-28 from ±40 to
    // ±55 above (and ±35 below) so a real overpay can rescue a deal that
    // personality + bad fit would otherwise sink — matches user spec
    // "overpaying should retain a player".
    int ask = std::max(1, ro.amount_k);
    double ratio = static_cast<double>(amount_k) / ask;  // 1.0 = exact ask
    if (ratio >= 1.0) {
        // Above ask: linear up to +55 at 1.6x (60% over).
        double over = std::min(0.60, ratio - 1.0);
        b.salary_mod = static_cast<int>(std::round(over * 92.0));  // up to ~+55
    } else {
        // Below ask: linear down to -35 at 0.6x (40% below ask). Slightly
        // softer than the symmetric −40 so a small discount isn't punitive.
        double under = 1.0 - ratio;
        b.salary_mod = -static_cast<int>(std::round(std::min(0.40, under) * 88.0));
    }

    // --- 2. Team prestige ------------------------------------------------
    if      (team.prestige >= 80) b.prestige_mod = +14;
    else if (team.prestige >= 65) b.prestige_mod = +8;
    else if (team.prestige >= 40) b.prestige_mod = 0;
    else                          b.prestige_mod = -5;

    // --- 3. Contender / window fit --------------------------------------
    switch (team.window) {
        case TeamWindow::Open:    b.contender_mod = +10; break;
        case TeamWindow::Closing: b.contender_mod = +5;  break;
        case TeamWindow::Opening: b.contender_mod = 0;   break;
        case TeamWindow::Closed:  b.contender_mod = -8;  break;
    }
    // Recent trophy bonus — if anyone on the roster won [T]/[M]/[W] in the
    // last 2 seasons, the team feels like a real contender regardless of
    // window classification. Strong signal for RingChasers and Competitors.
    {
        bool recent_trophy = false;
        for (const auto& pp : team.roster) {
            if (!pp || recent_trophy) continue;
            for (const auto& a : pp->awards) {
                if (a.size() < 3 || a[0] != '[') continue;
                if (a[1] != 'T' && a[1] != 'M' && a[1] != 'W') continue;
                // Cheap year-substring check — only the prior 2 years matter.
                // We don't have current_year here directly; rely on the
                // window/strategy signals + UI evaluator scope. Award entries
                // are cumulative-snapshotted at season end, so simple
                // presence is a reasonable "recent" proxy until tenure-time
                // tracking matures.
                recent_trophy = true;
                break;
            }
        }
        if (recent_trophy) b.contender_mod += 4;
    }
    // RingChaser amplifies contender preference (was 2x, kept at 2x).
    if (desire == Desire::RingChaser) b.contender_mod *= 2;
    // Competitor checks raw roster strength.
    if (desire == Desire::Competitor) {
        double avg = avg_roster_ovr(team);
        if (avg >= 78.0)      b.contender_mod += 8;
        else if (avg < 65.0)  b.contender_mod -= 10;
    }

    // --- 4. Loyalty / tenure --------------------------------------------
    // Loyal-archetype bonus raised; tenure-on-team is now visible as a
    // real reward (was +3 flat, now scales with years on the roster up to
    // +10). The user spec calls out that long-tenured players should resist
    // walking even when other factors look unfavorable.
    if (desire == Desire::Loyal && same_team)  b.loyalty_mod += 22;
    if (desire == Desire::Loyal && !same_team) b.loyalty_mod -= 4;
    if (same_team) b.loyalty_mod += 5;
    // Tenure ramp via prior salary_log entries that predate the current
    // year. Each prior season under contract = +2 score (cap +10). A 5+y
    // vet gets the full bump even outside the Loyal archetype.
    if (same_team) {
        int prior_seasons = 0;
        for (const auto& kv : salary_log) {
            if (kv.first < current_world_year()) ++prior_seasons;
        }
        int tenure_bonus = std::min(10, prior_seasons * 2);
        b.loyalty_mod += tenure_bonus;
    }

    // --- 5. Contract length match ---------------------------------------
    // `lp` was already computed above for ro.years bucket; reuse it.
    int ideal_years = static_cast<int>(std::round(lp * 4.0));
    if (ideal_years < 1) ideal_years = 1;
    if (ideal_years > 4) ideal_years = 4;
    int dy = std::abs(years - ideal_years);
    if      (dy == 0) b.years_mod = +6;
    else if (dy == 1) b.years_mod = +1;
    else if (dy == 2) b.years_mod = -6;
    else              b.years_mod = -12;
    // Mercenary loathes long deals — independent additional penalty.
    if (desire == Desire::Mercenary && years >= 3) b.years_mod -= 10;

    // --- 6. Archetype gates (softened 2026-05-28) -----------------------
    // Previously each gate was a flat -50 which formed a hard wall that no
    // amount of salary could overcome — overpaying was useless against
    // archetype mismatch, which the user reported as too punishing. New
    // values let a strong offer (+55 salary, +14 prestige, +10 contender)
    // recover from one gate firing while still leaving meaningful
    // resistance when MULTIPLE bad signals stack.
    if (desire == Desire::StabilitySeeker
        && (team.personality == Personality::Aggressive
            || team.strategy == Team::Strategy::WinNow)) {
        b.desire_mod -= 22;
        b.reject_reason = "StabilitySeeker uneasy with aggressive / win-now org.";
    }
    if (desire == Desire::RingChaser && age >= 28
        && (team.strategy == Team::Strategy::Rebuilding
            || team.strategy == Team::Strategy::BudgetRoster
            || team.strategy == Team::Strategy::TalentFarm)) {
        b.desire_mod -= 25;
        b.reject_reason = "RingChaser at 28+ reluctant to join a rebuild.";
    }
    if (desire == Desire::Competitor && age <= 26 && avg_roster_ovr(team) < 65.0) {
        b.desire_mod -= 22;
        b.reject_reason = "Competitor in his prime would prefer a stronger roster.";
    }
    if (desire == Desire::Greedy && amount_k < static_cast<int>(ask * 0.95)) {
        b.desire_mod -= 15;
        if (b.reject_reason.empty())
            b.reject_reason = "Greedy player resists meaningful discounts.";
    }
    if (desire == Desire::Mercenary && desire_length_pref(team) >= 0.7 && years >= 3) {
        b.desire_mod -= 18;
        if (b.reject_reason.empty())
            b.reject_reason = "Mercenary skeptical of long-term commitment.";
    }
    // Mood floor (rewritten refuses_to_negotiate). If the offer is below
    // the dignity threshold for this team, the player walks — but a strong
    // overpay should be able to mend bad blood, so we soften from -40 to -22.
    if (refuses_to_negotiate(amount_k, team.name)) {
        b.desire_mod -= 22;
        if (b.reject_reason.empty())
            b.reject_reason = "Player's mood toward this org needs a higher offer.";
    }

    // --- 7. Role fit (2026-05-28) ---------------------------------------
    // If the team is offering a role that isn't the player's natural
    // primary_role, weigh how plausible the switch is via role_fit_score.
    // A Duelist offered Initiator at 0.72 fit takes a small −6; the same
    // Duelist offered Controller at 0.30 fit takes a heavy −32 that a real
    // overpay (+55 salary, +14 prestige) can recover from but a fair offer
    // cannot. Natural-role offers (Role::Count fallback or matching
    // primary_role) take no penalty. The label is always emitted so the
    // UI can show "Role Fit (Natural): +0" or "Role Fit (Stretch): −20".
    int role_mod = 0;
    {
        Role r = (offered_role == Role::Count) ? primary_role : offered_role;
        if (r == primary_role) {
            role_mod = 0;
        } else {
            double rf = role_fit_score(r);
            // SMOOTH compatibility penalty (was a 4-step cliff that dinged an
            // 0.84-fit the same as a 0.76-fit). An ~80%-compatible flex now pays
            // only a small premium (~-9); a true mismatch (duelist on
            // controller, rf~0.2) pays the full ~-45.
            role_mod = (rf >= 1.0) ? 0
                     : -static_cast<int>(std::lround((1.0 - rf) * 45.0));
            if (rf < 0.35 && b.reject_reason.empty()) {
                b.reject_reason = "Offered role doesn't fit this player's skillset.";
            }
        }
    }
    b.desire_mod += role_mod;  // folded into desire_mod for the existing label

    // --- 7b. Personality lean on the DECISION ---------------------------
    // loyalty pulls to stay (and mildly resists leaving); ambition wants a
    // contender (penalizes weak teams, rewards strong); ego stings on below-ask
    // offers. The ego term is MONOTONIC-SAFE: it is only negative when amount <
    // ask and shrinks to 0 at ask, so a bigger offer never lowers the score.
    {
        if (same_team) b.personality_mod += static_cast<int>((loyalty - 50) / 49.0 * 12.0);
        else           b.personality_mod -= static_cast<int>(std::max(0, loyalty - 50) / 49.0 * 6.0);
        double avg = avg_roster_ovr(team);
        double amb = (ambition - 50) / 49.0;
        if      (avg >= 78.0) b.personality_mod += static_cast<int>(amb * 8.0);
        else if (avg < 65.0)  b.personality_mod -= static_cast<int>(std::max(0.0, amb) * 10.0);
        if (amount_k < ask) {
            double under = static_cast<double>(ask - amount_k) / ask;   // 0..1
            b.personality_mod -= static_cast<int>(std::max(0, ego - 50) / 49.0 * under * 18.0);
        }
    }

    // --- 7c. Multi-year sweeteners (signing bonus + starter promise) -----
    // MONOTONIC sweeteners — bigger bonus / a starter guarantee can only raise
    // acceptance. Bonus is amortized over the deal (a 2y deal feels it more).
    if (signing_bonus_k > 0) {
        double eff_per_yr = static_cast<double>(signing_bonus_k) / std::max(1, years);
        b.bonus_mod = std::min(15, static_cast<int>(std::round(eff_per_yr / 8.0)));
    }
    if (promise_starter) {
        b.promise_mod = 6 + static_cast<int>(std::max(0, (ambition + ego) / 2 - 50) / 49.0 * 14.0);
        b.promise_mod = std::min(20, b.promise_mod);
    }

    b.ask_k = ro.amount_k;

    // --- Total -----------------------------------------------------------
    b.total = b.base_score + b.salary_mod + b.prestige_mod
            + b.contender_mod + b.loyalty_mod + b.years_mod + b.desire_mod
            + b.personality_mod + b.bonus_mod + b.promise_mod;
    if (b.total < 0)   b.total = 0;
    if (b.total > 100) b.total = 100;

    // Over-length is now a STEEP PRICE, not a wall (2026-06-28): one year past
    // the personality's comfort can be bought with a strong package (money +
    // bonus + prestige), two-plus is effectively unreachable. Applied AFTER the
    // clamp above would be wrong — fold it into the pre-clamp total instead.
    {
        int over_years = std::max(0, years - ro.max_acceptable_years);
        if (over_years > 0) {
            int pen = 14 * over_years + 6;   // -20 at +1, -34 at +2
            b.years_mod -= pen;
            b.total     -= pen;
            if (b.total < 0) b.total = 0;
            if (over_years >= 2 && b.reject_reason.empty())
                b.reject_reason = "That's far longer than this player will commit to.";
        }
    }

    // Hard floor: below min_acceptable_k or a nonsense length → always reject
    // regardless of score (those are the engine's invariant rules).
    bool hard_block = (amount_k < ro.min_acceptable_k) || (years < 1);
    if (hard_block) {
        b.total = std::min(b.total, kResignAcceptThreshold - 1);
        if (b.reject_reason.empty()) {
            if (years < 1) b.reject_reason = "Offer must be at least 1 year.";
            else           b.reject_reason = "Offer below player's minimum acceptable salary.";
        }
    }

    b.will_accept = (b.total >= kResignAcceptThreshold);

    // --- Counter-offer (near-miss only) ----------------------------------
    // If the player rejected but the deal is reachable (not a hard role
    // mismatch), surface the minimum salary that WOULD clear acceptance at
    // these years — a real "meet me here". Invert the monotonic salary lever
    // (salary_mod = over*92) against the score gap, computed from the RAW
    // non-salary components so the counter, when accepted, actually passes.
    if (!b.will_accept) {
        int non_salary = b.base_score + b.prestige_mod + b.contender_mod
                       + b.loyalty_mod + b.years_mod + b.desire_mod
                       + b.personality_mod + b.bonus_mod + b.promise_mod;
        int need = (kResignAcceptThreshold + 1) - non_salary;   // salary_mod needed
        bool reachable = (role_mod > -40);   // not a near-hard role mismatch
        if (reachable && need <= 55) {        // 55 = max salary_mod (the +55 cap)
            double over = (need <= 0) ? 0.0 : std::min(0.60, need / 92.0);
            int counter_amt = static_cast<int>(std::ceil(ask * (1.0 + over)));
            counter_amt = clamp_v(std::max(counter_amt, ro.min_acceptable_k),
                                  kSalaryFloorK, kSalaryCapK);
            int counter_yrs = clamp_v(years, 1, ro.max_acceptable_years);
            // ROUND-TRIP VERIFY: the salary CAP can clamp counter_amt below the
            // value that actually clears 55 (high-ask players). Re-derive the
            // salary_mod at the clamped counter and only surface the counter if
            // non_salary + that salary_mod truly accepts — otherwise the card
            // would recommend a number the player still rejects.
            int sm;
            double cr = static_cast<double>(counter_amt) / ask;
            if (cr >= 1.0) sm =  static_cast<int>(std::round(std::min(0.60, cr - 1.0) * 92.0));
            else           sm = -static_cast<int>(std::round(std::min(0.40, 1.0 - cr) * 88.0));
            bool clears = (non_salary + sm) >= kResignAcceptThreshold;
            if (clears && counter_amt > amount_k && counter_amt <= ask * 2) {
                b.has_counter      = true;
                b.counter_amount_k = counter_amt;
                b.counter_years    = counter_yrs;
            }
        }
    }

    // --- Verdict label ---------------------------------------------------
    if      (b.total >= 85) b.verdict = "OVERPAY";
    else if (b.total >= 70) b.verdict = "STRONG";
    else if (b.total >= 55) b.verdict = "FAIR";
    else if (b.total >= 35) b.verdict = "WEAK";
    else                    b.verdict = "INSULTING";

    // --- UI labels (only emit non-zero contributions) -------------------
    auto add_label = [&](const std::string& name, int v) {
        if (v != 0) b.labels.emplace_back(name, v);
    };
    add_label("Salary vs Ask",   b.salary_mod);
    add_label("Team Prestige",   b.prestige_mod);
    add_label("Contender Status",b.contender_mod);
    add_label("Loyalty / Tenure",b.loyalty_mod);
    add_label("Contract Length", b.years_mod);
    // Split the desire_mod label into "Personality" + "Role Fit" so the
    // user can see which part of the −X is the archetype gate vs the role
    // mismatch. desire_mod already includes role_mod added above; subtract
    // it back out for the personality label so the two add up correctly.
    int personality_only = b.desire_mod - role_mod;
    add_label("Personality", personality_only);
    if (offered_role != Role::Count && offered_role != primary_role) {
        std::string lbl = std::string("Role Fit (")
                        + role_fit_verdict(offered_role) + ")";
        add_label(lbl, role_mod);
    }

    return b;
}

bool Player::accepts_resign_offer(int amount_k, int years, const Team& team) const {
    // Single source of truth — same evaluator the UI breakdown uses.
    return evaluate_resign_offer(amount_k, years, team).will_accept;
}

int Player::register_rejected_offer(int amount_k, int years, const Team& team) {
    // A rejected offer is not free: lowball / insulting offers sour the
    // player's mood toward this org (raising the dignity floor via
    // refuses_to_negotiate + the amount_with_mood demand), so the user can't
    // just spam the slider up 1K at a time. A reasonable near-miss costs
    // nothing — that's normal negotiating. Ego amplifies the sting. Mood decays
    // across offseason days (decay_mood) so anger fades over time.
    // Returns the tier for the UI: 0 = reasonable miss, 1 = lowball, 2 = insult.
    int ask = std::max(1, evaluate_resign_offer(amount_k, years, team).ask_k);
    double ratio = static_cast<double>(amount_k) / ask;
    double ego_amp = 1.0 + std::max(0, ego - 50) / 49.0 * 0.5;   // up to +50%
    int tier; double mood_hit;
    if (ratio < 0.70)      { tier = 2; mood_hit = 0.18 * ego_amp; }   // insulting
    else if (ratio < 0.85) { tier = 1; mood_hit = 0.08 * ego_amp; }   // lowball
    else                   { tier = 0; mood_hit = 0.0;            }   // reasonable
    if (mood_hit > 0.0) bump_mood(team.name, mood_hit);
    return tier;
}

static int monte_carlo_potential(const Player& proto);  // defined below

PlayerPtr generate_player(int min_age, int max_age, std::string_view region,
                          bool deep_potential) {
    // Pick the player's birth country first; it drives the legal name pool
    // and shows up as a flag in the UI. Most players are local (90%) but
    // a chunk are imports who've stayed in this region.
    Region reg = region_from_str(region);
    bool is_import = rng().chance(0.10);  // global pool sample
    const Country& country = is_import ? pick_country_any()
                                       : pick_country_in_region(reg);
    PlayerIdentity ident = make_identity(country);

    // Ensure handle uniqueness vs other already-generated players. The
    // global name registry from take_unused_name() handles that for us.
    std::string handle = ident.handle;
    // Real de-dup: consult global_names() for collision. If the rolled
    // handle already exists, append a 2-digit suffix and re-check. Keep
    // `safety` as a retry budget so we never loop forever.
    {
        const auto& used = global_names();
        std::unordered_set<std::string> taken(used.begin(), used.end());
        int safety = 4;
        while (safety-- > 0) {
            if (taken.find(handle) == taken.end()) break;
            // Collision — append a 2-digit suffix and retry.
            handle += std::to_string(rng().irange(10, 99));
        }
    }

    double roll = rng().uniform();
    int base;
    // Base attribute mean lottery. The LIVE OVR formula is mean*0.95 + 1
    // (ovr_from_attrs), so a rookie's spawn OVR is ~= base + a small archetype
    // mean bump (~+1.5). That means OVR >= 50 corresponds to base >= ~50.
    // Bands below put only ~11% of rookies at base >= 50 so that ~85-90% of a
    // class spawns BELOW tier-1 level (OVR < 50) and only a small slice arrives
    // genuinely starter-ready — most prospects must be DEVELOPED, not plug-and-
    // play. Mapping (base -> OVR via 0.95*base+1, before archetype):
    //   base 80-90 -> OVR ~77-87 (generational)        0.05%
    //   base 70-79 -> OVR ~67-76 (elite ceiling)       ~0.35%
    //   base 60-69 -> OVR ~58-66 (high-end)            ~1.8%
    //   base 50-58 -> OVR ~48-56 (tier-1 ready)        ~8.8%
    //   base 40-49 -> OVR ~39-48 (solid pro / acad.)   ~23%
    //   base 30-39 -> OVR ~30-38 (fringe)              ~33%
    //   base 20-30 -> OVR ~20-29 (raw ranked grinder)  ~33%
    if      (roll < 0.0005) base = rng().irange(80, 90);
    else if (roll < 0.004)  base = rng().irange(70, 79);
    else if (roll < 0.022)  base = rng().irange(60, 69);
    else if (roll < 0.11)   base = rng().irange(50, 58);
    else if (roll < 0.34)   base = rng().irange(40, 49);
    else if (roll < 0.67)   base = rng().irange(30, 39);
    else                    base = rng().irange(20, 30);

    Attributes a{};
    auto fill = [&](Attr k, int spread) {
        at(a, k) = clamp_attr(base + rng().irange(-spread, spread));
    };
    fill(Attr::Aim, 6);
    fill(Attr::Entry, 6);
    fill(Attr::Utility, 6);
    fill(Attr::GameSense, 6);
    fill(Attr::Clutch, 6);
    fill(Attr::DecisionMaking, 6);
    fill(Attr::Intelligence, 6);
    fill(Attr::Aggressiveness, 15);
    fill(Attr::Headshot, 10);
    fill(Attr::Positioning, 8);
    fill(Attr::Communication, 8);
    fill(Attr::Adaptability, 8);
    fill(Attr::Leadership, 10);
    fill(Attr::Reaction, 8);
    fill(Attr::SpikeHandle, 12);
    fill(Attr::Anchor, 12);
    // New micro-attributes — keep spread similar to the established ones.
    fill(Attr::Movement, 8);
    fill(Attr::CrosshairPlacement, 8);
    fill(Attr::Lurking, 12);
    fill(Attr::Awping, 18);                // big spread; few players are awpers
    fill(Attr::Stamina, 8);
    fill(Attr::MidRoundCalling, 14);       // most players poor here unless IGL
    fill(Attr::EconomyMgmt, 14);
    fill(Attr::AntiStrat, 14);

    if (rng().uniform() < 0.005 && min_age <= 18) {
        int cluster = rng().irange(0, 2);
        std::array<Attr, 3> cl;
        if (cluster == 0)      cl = {Attr::Aim, Attr::Headshot, Attr::Entry};
        else if (cluster == 1) cl = {Attr::Utility, Attr::GameSense, Attr::Clutch};
        else                   cl = {Attr::Intelligence, Attr::DecisionMaking, Attr::GameSense};
        for (auto k : cl) at(a, k) = std::min(99, at(a, k) + 12);
    }

    int chosen_age = rng().irange(min_age, max_age);
    auto p = std::make_shared<Player>(std::move(handle), chosen_age, a, std::string(region));
    // Initialize career_max_ovr to the spawn ovr so future ratio checks are
    // well-defined for never-progressed rookies.
    p->career_max_ovr = static_cast<int>(std::round(p->ovr()));
    p->first       = ident.first;
    p->last        = ident.last;
    p->country     = country.name;
    p->country_iso = country.iso;
    p->birth_city  = pick_city(country.iso);
    if (p->birth_city.empty()) p->birth_city = country.name;
    // birth_year is filled in by GameManager when it knows the current year.

    // IGL spawn — role-aware + mental-gated. See base_igl_chance_for_role
    // and roll_is_igl_at_spawn above for the ecosystem distribution targets
    // (Controllers/Initiators dominate, Sentinels mid, Duelist-IGLs are
    // ultra-rare and only ever elite minds). Replaces the old flat 25%.
    //
    // primary_role is already set by the Player ctor's update_agent_pool()
    // call (line 103), so the role-aware base chance reads correctly here.
    //
    // Stat shift trades raw fragging for mental ceiling so flagged IGLs
    // read as different player archetypes in match.
    if (roll_is_igl_at_spawn(*p)) {
        p->is_igl = true;
        // Bug 6: deepen the gunfight tax. Pre-bump IGLs were still winning
        // raw aim duels (Aim 70+ retained mid-tier mechanics). The new
        // shift trades a bigger chunk of fragging for the leadership /
        // mental ceiling.
        p->apply_attribute_delta(Attr::Aim,            -12);
        p->apply_attribute_delta(Attr::Headshot,       -10);
        p->apply_attribute_delta(Attr::Entry,           -8);
        p->apply_attribute_delta(Attr::Aggressiveness,  -4);
        p->apply_attribute_delta(Attr::Leadership,     +14);
        p->apply_attribute_delta(Attr::Intelligence,   +10);
        p->apply_attribute_delta(Attr::Communication,  +10);
        p->apply_attribute_delta(Attr::GameSense,       +8);
        p->apply_attribute_delta(Attr::DecisionMaking,  +8);
        p->apply_attribute_delta(Attr::MidRoundCalling, +12);
        p->apply_attribute_delta(Attr::EconomyMgmt,    +12);
        p->apply_attribute_delta(Attr::AntiStrat,      +10);
        p->tend_play_aggressive = rng().irange(20, 80);
        p->tend_lurk_vs_execute = rng().irange(20, 80);
        p->tend_vocal           = rng().irange(40, 95);
        p->tend_adaptive        = rng().irange(30, 90);
    }

    // === Personality scalars (1-99) ===
    // Seeded around 50 with spread, then nudged by potential/age so they read
    // believably. Drive contract DEMANDS (greed/ego raise the ask) and
    // negotiation DECISIONS (loyalty re-sign discount, ambition wants winners,
    // ego punishes lowballs). Lightly correlated, never extreme by default.
    {
        p->ambition = clamp_attr(rng().irange(30, 70));
        p->loyalty  = clamp_attr(rng().irange(30, 70));
        p->greed    = clamp_attr(rng().irange(30, 70));
        p->ego      = clamp_attr(rng().irange(30, 70));
        int pot = p->potential;
        if (p->age <= 23 && pot >= 75) p->ambition = clamp_attr(p->ambition + 12);
        p->ego   = clamp_attr(p->ego   + (pot - 60) / 5);
        p->greed = clamp_attr(p->greed + (pot - 60) / 6);
        if (p->age >= 30) {
            p->loyalty = clamp_attr(p->loyalty + 10);
            p->greed   = clamp_attr(p->greed - 6);
        }
    }

    // === Agent pool size roll ===
    // VCT-accurate distribution (widened 2026-06-20): real pros are NOT
    // one-tricks — most cover their main role PLUS a couple of cross-role
    // agents. Modal pro is now 5 agents (was 3); one-tricks stay rare.
    //   ~3%   -> 1-2 agents (true one/two-trick)
    //   ~12%  -> 3 agents (focused)
    //   ~30%  -> 4 agents (solid flex)
    //   ~34%  -> 5 agents (modal pro)
    //   ~18%  -> 6 agents (broad flex)
    //   ~3%   -> 7 agents (ultra-flex unicorn)
    {
        double r = rng().uniform();
        int sz;
        if      (r < 0.03)  sz = rng().irange(1, 2);
        else if (r < 0.15)  sz = 3;
        else if (r < 0.45)  sz = 4;
        else if (r < 0.79)  sz = 5;
        else if (r < 0.97)  sz = 6;
        else                sz = 7;
        p->agent_pool_size = sz;
    }
    p->update_agent_pool();

    // === Rookie archetype assignment ===
    // Roll an archetype, apply its stat shifts + meta tweaks. If the
    // archetype is TacticalIGL, set is_igl AFTER the regular IGL roll
    // (apply_rookie_archetype handles the flag). The 25%-IGL roll above
    // already may have set is_igl to true; archetype shifts stack on top.
    p->rookie_archetype = roll_rookie_archetype();
    apply_rookie_archetype(*p, p->rookie_archetype);

    // === Stable simulation-driving Archetype (THIRD system) ===
    // Assigned ONCE, AFTER attributes + role + is_igl + rookie_archetype are
    // all finalized. Scored from standout attrs + role + rookie_archetype
    // with rng weighting among the top fits, so two same-role players
    // usually differ. Always a valid value. Read by Match.cpp per duel via
    // archetype_profile(p->archetype). Distinct from rookie_archetype and
    // playstyle_identity() — none merged.
    p->archetype = pick_spawn_archetype(*p);

    // === Desire (negotiation temperament) =================================
    // Assigned ONCE here, AFTER attrs/role/is_igl/rookie_archetype/Archetype
    // are all finalized so the spawn roll has full player context. Stable
    // for the player's career; read by Team-side signing logic via
    // desire_salary_mult / desire_accepts / desire_length_pref. Distinct
    // from the three other playstyle systems — none merged.
    p->desire = pick_spawn_desire(*p);

    // === Monte-Carlo potential estimation ================================
    // Now that attrs / role / archetype / rookie_archetype / IGL are all
    // finalized, fast-forward 20 copies of the player through the new
    // monthly-progression curve to peak_age + 4 (min 29). The 75th-percentile
    // terminal OVR becomes the player's "true potential", overriding the
    // simple "mean + irange(5,20)" baseline set in the Player ctor.
    //
    // Cost: ~20 sims * ~120 ticks each = 2400 ticks per rookie (microseconds).
    // Skipped for filler (deep_potential=false) — falls back to the cheap
    // heuristic already set by Player ctor (`mean + irange(5,20)`). The vast
    // majority of solo Q ladder players never get scouted, so paying ~6.5M
    // progression ticks at world boot to estimate their accurate potential
    // is wasted work. Team rosters + rookie classes still get the full MC.
    if (deep_potential) {
        p->potential = monte_carlo_potential(*p);
    }

    // === Reputation (FM-style fame) seed ================================
    // DETERMINISTIC (no rng) so the generation stream is byte-identical. At world
    // birth there's no match history, so approximate career standing from OVR
    // (skill≈renown proxy) + a tenure term (older good players are more established)
    // + a little upside for high-ceiling youth. Evolves slowly each season in
    // save_history_and_progress; a promoted club's players lag reality for years.
    {
        double ov  = p->ovr();
        double rep = 500.0 + (ov - 45.0) * 135.0 + (p->age - 19) * 55.0
                   + std::max(0, p->potential - 70) * 12.0;
        p->reputation = static_cast<int>(std::lround(clamp_v(rep, 150.0, 9600.0)));
    }

    return p;
}

PlayerPtr generate_rookie(std::string_view region) {
    return generate_player(16, 18, region);
}

// === Canonical roster position helpers ====================================
const char* position_name(Position p) noexcept {
    switch (p) {
        case Position::Duelist:    return "Duelist";
        case Position::Controller: return "Controller";
        case Position::Sentinel:   return "Sentinel";
        case Position::Initiator:  return "Initiator";
        default:                   return "?";
    }
}

// === Two-stage retirement check ==========================================
// Stage 1: logistic regression on age + potential + recent rating + seasons
//          played. The coefficients come from the spec; they produce a smooth
//          rising probability past age 28 with strong dampening for elite
//          (potential >> 80) or peak-form (recent rating ≥ 0.95) players.
// Stage 2: aging-star catchnet — if ovr() has fallen to <70% of career peak
//          AND age >= 30, force retire. Catches "former MVP refuses to retire
//          at 90% of his peak" — by 70% of peak we mandate it.
bool Player::should_retire(int current_year) const {
    (void)current_year;
    // Stage 2 fires first — overrides stage 1.
    if (age >= 30 && career_max_ovr > 0) {
        double cur = ovr();
        if (cur < 0.70 * static_cast<double>(career_max_ovr)) {
            return true;
        }
    }

    // Stage 1 logistic. Recent rating = career_rating_total / career_matches;
    // safe-default to 1.0 when no matches have happened yet (rookies).
    double career_rating = (career_matches > 0)
        ? (career_rating_total / static_cast<double>(career_matches))
        : 1.0;
    double logit = -8.0
                 + 0.18 * static_cast<double>(age - 25)
                 - 0.04 * static_cast<double>(potential - 50)
                 - 1.5  * std::max(0.0, career_rating - 0.95)
                 - 0.05 * static_cast<double>(career_seasons_played);
    double prob = 1.0 / (1.0 + std::exp(-logit));
    if (prob < 0.0) prob = 0.0;
    if (prob > 1.0) prob = 1.0;
    return rng().chance(prob);
}

// === Monte-Carlo potential estimation ===================================
// At rookie-gen time we fast-forward a copy of the player through the same
// monthly-progression curve we just defined. Take the 75th percentile of 20
// independent terminal ovr() snapshots as the player's "true potential".
//
// Cheap — each sim is ~(target_age - current_age) * 12 progression ticks. The
// helper COPIES the proto; the original is untouched.
static int monte_carlo_potential(const Player& proto) {
    constexpr int kSims = 20;
    std::vector<int> finals;
    finals.reserve(kSims);

    int target_age = std::max(proto.peak_age + 4, 29);

    for (int s = 0; s < kSims; ++s) {
        Player copy = proto;        // deep-ish copy; vectors/maps cloned
        // Run monthly progression ticks until target_age. Each year is 12
        // ticks. We do NOT step age here — apply_monthly_progression does
        // not advance age (year-end progression handles that); instead we
        // simulate the player's career trajectory by replaying ticks and
        // bumping age between years.
        while (copy.age < target_age) {
            for (int m = 0; m < 12; ++m) {
                copy.apply_monthly_progression(nullptr, 0, 0);
            }
            copy.age += 1;
        }
        finals.push_back(static_cast<int>(std::round(copy.ovr())));
    }

    std::sort(finals.begin(), finals.end());
    // 75th percentile — index = ceil(0.75 * (n-1)) for inclusive percentile.
    std::size_t idx = static_cast<std::size_t>(std::round(0.75 * (kSims - 1)));
    int p75 = finals[idx];
    if (p75 < 50) p75 = 50;
    if (p75 > 99) p75 = 99;
    return p75;
}

Position position_of(const Player& p) noexcept {
    // Position is derived from primary_role ALONE. Both is_igl and is_flex
    // are INDEPENDENT overlay flags (leadership / sub-role wildcard
    // respectively) layered on top of whichever core position the player
    // holds — neither replaces a core role slot. Queried separately via
    // p.is_igl and p.is_flex.
    switch (p.primary_role) {
        case Role::Duelist:    return Position::Duelist;
        case Role::Controller: return Position::Controller;
        case Role::Sentinel:   return Position::Sentinel;
        case Role::Initiator:  return Position::Initiator;
        default:               return Position::Initiator;  // sane fallback
    }
}

bool is_internationally_marketable(const Player& p) {
    // Proven by a real career OR by clear quality. Raw foreign youth fail this
    // and come up through the soloq -> Tier-3 pipeline instead of flooding the
    // free-agent market as if international moves were routine.
    return p.career_matches >= config().intl_fa_min_matches ||
           p.ovr() >= config().intl_fa_min_ovr;
}

}  // namespace vlr
