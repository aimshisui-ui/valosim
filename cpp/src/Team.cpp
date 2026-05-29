#include "Team.h"

#include <algorithm>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <unordered_set>

namespace vlr {

namespace {

Personality random_personality() {
    int r = rng().irange(0, 3);
    return static_cast<Personality>(r);
}

// Score a free agent by how well their attributes fit a team's target comp.
// Each comp emphasises different play patterns:
//
//   DoubleDuelist   — frag-heavy, agg, fast peeks. Wants Aim + Entry +
//                     Aggressiveness + Movement + CrosshairPlacement.
//   DoubleInitiator — info-heavy, util-driven. Wants Utility + Communication
//                     + GameSense + Lurking + AntiStrat.
//   DoubleController— tactical, methodical. Wants Intelligence + DecisionMaking
//                     + Leadership + GameSense + EconomyMgmt.
//   DoubleSentinel  — anchor-heavy, defensive. Wants Anchor + Positioning +
//                     Clutch + SpikeHandle + Stamina.
//
// Returns roughly 0..600. Higher = better comp fit.
double comp_fit_score(const Player& p, CompTag tag) {
    const auto& a = p.attributes;
    switch (tag) {
        case CompTag::DoubleDuelist:
            return at(a, Attr::Aim) * 1.4
                 + at(a, Attr::Entry) * 1.4
                 + at(a, Attr::Aggressiveness) * 1.2
                 + at(a, Attr::Movement) * 1.0
                 + at(a, Attr::CrosshairPlacement) * 0.9;
        case CompTag::DoubleInitiator:
            return at(a, Attr::Utility) * 1.4
                 + at(a, Attr::Communication) * 1.2
                 + at(a, Attr::GameSense) * 1.2
                 + at(a, Attr::Lurking) * 1.0
                 + at(a, Attr::AntiStrat) * 0.6;
        case CompTag::DoubleController:
            return at(a, Attr::Intelligence) * 1.4
                 + at(a, Attr::DecisionMaking) * 1.2
                 + at(a, Attr::GameSense) * 1.2
                 + at(a, Attr::Leadership) * 0.8
                 + at(a, Attr::EconomyMgmt) * 0.6;
        case CompTag::DoubleSentinel:
            return at(a, Attr::Anchor) * 1.5
                 + at(a, Attr::Positioning) * 1.3
                 + at(a, Attr::Clutch) * 1.0
                 + at(a, Attr::SpikeHandle) * 0.9
                 + at(a, Attr::Stamina) * 0.5;
        default:
            return p.ovr() * 5.0;
    }
}
ScoutPrefs random_scout_prefs() {
    ScoutPrefs s;
    s.youth          = rng().irange(1, 10);
    s.mechanics      = rng().irange(1, 10);
    s.smarts         = rng().irange(1, 10);
    s.aggressiveness = rng().irange(1, 10);
    s.flexibility    = rng().irange(1, 10);
    s.clutch         = rng().irange(1, 10);
    s.potential      = rng().irange(1, 10);
    s.experience     = rng().irange(1, 10);
    return s;
}

}  // namespace

Team::Team(std::string n, long long b, std::string r)
    : name(std::move(n)), budget(b), region(std::move(r)) {
    personality = random_personality();
    comp_identity = pick_comp_identity_from_personality(personality);
    target_comp = random_comp();
    scout_prefs = random_scout_prefs();
    tag = make_team_tag(name);
    seed_team_colors(*this);

    // Deterministic logo glyph from a stable hash of the team's display
    // name. Personality biases the pool toward genre-appropriate shapes
    // but is intentionally LOW intensity — a 30% chance to land in the
    // bias bucket, otherwise fall through to the full uniform draw. That
    // keeps "Aggressive" teams disproportionately Skull/Bolt/Dragon
    // without making the visual identity feel formulaic.
    constexpr int kLogoCount = static_cast<int>(LogoShape::Count);
    std::size_t h = std::hash<std::string>{}(name);
    int idx = static_cast<int>(h % static_cast<std::size_t>(kLogoCount));
    logo_shape = static_cast<LogoShape>(idx);

    // Cheap personality bias overlay (deterministic — derived from a second
    // hash mix, NOT the global Rng — so the same team gets the same logo
    // across save/load and world re-inits).
    auto bias_pick = [&](std::initializer_list<LogoShape> pool) {
        std::size_t bh = h * 2654435761u;     // Knuth multiplicative hash mix
        std::size_t bh2 = (bh >> 16) ^ bh;    // second mix, decorrelated from `bh`
        if ((bh % 100u) < 30u) {  // 30% land in bias bucket
            auto it = pool.begin();
            std::advance(it, static_cast<long>(bh2 % pool.size()));
            logo_shape = *it;
        }
    };
    switch (personality) {
        case Personality::Aggressive:
            bias_pick({LogoShape::LightningBolt, LogoShape::Skull,
                       LogoShape::Dragon, LogoShape::Flame, LogoShape::Sword});
            break;
        case Personality::Tactical:
            bias_pick({LogoShape::Crosshair, LogoShape::Hexagon,
                       LogoShape::Compass, LogoShape::Shield, LogoShape::Diamond});
            break;
        case Personality::Budget:
            bias_pick({LogoShape::Triangle, LogoShape::Circle,
                       LogoShape::Anchor, LogoShape::Mountain, LogoShape::Tree});
            break;
        case Personality::Balanced:
            // No bias — keep the uniform hash pick.
            break;
    }
}

const char* identity_name(CompIdentity ci) noexcept {
    switch (ci) {
        case CompIdentity::Balanced:          return "Balanced";
        case CompIdentity::AggressiveDive:    return "Aggressive Dive";
        case CompIdentity::UtilityHeavy:      return "Utility Heavy";
        case CompIdentity::DoubleDuelistTeam: return "Double Duelist";
        case CompIdentity::StructuredMacro:   return "Structured Macro";
        case CompIdentity::FlexExperimental:  return "Flex / Experimental";
    }
    return "?";
}

// Roll a CompIdentity weighted by Personality. Base distribution
// (40/15/15/10/10/10 = Balanced/AggressiveDive/UtilityHeavy/
// DoubleDuelistTeam/StructuredMacro/FlexExperimental) shifts per
// personality:
//   Aggressive -> AggressiveDive / DoubleDuelistTeam
//   Tactical   -> UtilityHeavy / StructuredMacro
//   Balanced   -> stays Balanced more often
//   Budget     -> often FlexExperimental
CompIdentity pick_comp_identity_from_personality(Personality p) {
    // Six-way weights, reordered to match the enum:
    //   {Balanced, AggressiveDive, UtilityHeavy, DoubleDuelistTeam,
    //    StructuredMacro, FlexExperimental}
    std::vector<int> w;
    switch (p) {
        case Personality::Aggressive:
            w = {25, 30, 5, 25, 5, 10};
            break;
        case Personality::Tactical:
            w = {25, 5, 30, 5, 25, 10};
            break;
        case Personality::Budget:
            w = {30, 10, 10, 5, 10, 35};
            break;
        case Personality::Balanced:
        default:
            // Hits the project-spec 40/15/15/10/10/10 baseline.
            w = {40, 15, 15, 10, 10, 10};
            break;
    }
    int idx = rng().weighted_index(w);
    if (idx < 0 || idx >= 6) idx = 0;
    return static_cast<CompIdentity>(idx);
}

const char* team_strategy_name(Team::Strategy s) noexcept {
    switch (s) {
        case Team::Strategy::Contender:        return "Contender";
        case Team::Strategy::Rebuilding:       return "Rebuilding";
        case Team::Strategy::Bridge:           return "Bridge";
        case Team::Strategy::BudgetRoster:     return "Budget Roster";
        case Team::Strategy::DevelopmentFocus: return "Development";
        case Team::Strategy::WinNow:           return "Win Now";
        case Team::Strategy::TalentFarm:       return "Talent Farm";
    }
    return "?";
}

const char* team_strategy_blurb(Team::Strategy s) noexcept {
    switch (s) {
        case Team::Strategy::Contender:        return "Star-driven, prioritises proven veterans";
        case Team::Strategy::Rebuilding:       return "Young talent and high potential signings";
        case Team::Strategy::Bridge:           return "Retooling between competitive cycles";
        case Team::Strategy::BudgetRoster:     return "Cheap signings, opportunistic value plays";
        case Team::Strategy::DevelopmentFocus: return "Rookies and coaching-heavy growth";
        case Team::Strategy::WinNow:           return "Overpays for proven veterans, short window";
        case Team::Strategy::TalentFarm:       return "Develops talent then sells for profit";
    }
    return "?";
}

Team::Strategy classify_team_strategy(const Team& t) noexcept {
    // Compute roster averages
    double avg_age = 0.0, avg_pot = 0.0, avg_ovr = t.ovr();
    int n = 0;
    for (auto& p : t.roster) {
        if (!p) continue;
        avg_age += p->age;
        avg_pot += p->potential;
        ++n;
    }
    if (n > 0) { avg_age /= n; avg_pot /= n; }
    long long bud = t.budget;
    int prestige = t.prestige;

    // Decision tree, not exhaustive but produces reasonable variety.
    // 2026-05 budget rebalance: salaries cap at $180K so a 5-starter team's
    // realistic payroll lands in $150K-$450K range. Thresholds rescaled so
    // WinNow/Contender lights up on $1M-$2M teams rather than the old
    // $2.5M-millions tier.
    if (bud >= 1'500'000LL && avg_ovr >= 75.0 && prestige >= 70) return Team::Strategy::WinNow;
    if (bud >=   900'000LL && avg_ovr >= 72.0)                   return Team::Strategy::Contender;
    if (avg_age <= 21.0 && avg_pot >= 78.0)                       return Team::Strategy::Rebuilding;
    if (avg_age <= 22.0 && avg_pot >= 70.0)                       return Team::Strategy::DevelopmentFocus;
    if (bud <   400'000LL)                                        return Team::Strategy::BudgetRoster;
    if (avg_pot >= 80.0 && bud < 600'000LL)                       return Team::Strategy::TalentFarm;
    return Team::Strategy::Bridge;
}

double team_import_appetite(const Team& t,
                            Role target_role,
                            const std::vector<PlayerPtr>* regional_fa_pool) {
    // Per-team "do we even want to look at imports right now" score.
    // STRICT v2: tightened so only ~2-3 teams per region clear the
    // import gate at all. Multiplied with a per-callsite base rate to
    // produce the actual coin flip.
    //
    // Organizational memory overlay: a small ±0.04 term derived from
    // `t.memory.import_success` adjusts the appetite based on past
    // import outcomes. Orgs that have made imports work in the past
    // lean in; orgs that got burned shy away. Capped to stay safely
    // inside the 0.28 strict ceiling.

    // --- Budget tier (up to +0.10) ---
    // 2026-05 rebalance: new budget scale $250K-$2M. <= $600K -> 0.00,
    // >= $1.5M -> +0.10 (linear in between). Mirrors the new salary cap
    // ($180K) so only mid+ orgs with real cap space pursue imports.
    double bud = static_cast<double>(t.budget);
    double budget_term = (bud - 600'000.0) / (1'500'000.0 - 600'000.0) * 0.10;
    if (budget_term < 0.0)  budget_term = 0.0;
    if (budget_term > 0.10) budget_term = 0.10;

    // --- Prestige (up to +0.08) ---
    // <50 -> 0, >=80 -> 0.08
    double prestige_term = (static_cast<double>(t.prestige) - 50.0)
                         / (80.0 - 50.0) * 0.08;
    if (prestige_term < 0.0)  prestige_term = 0.0;
    if (prestige_term > 0.08) prestige_term = 0.08;

    // --- Strategy (+0.06 contender/winnow, +0.05 rebuild-with-trophy) ---
    double strategy_term = 0.0;
    if (t.strategy == Team::Strategy::Contender ||
        t.strategy == Team::Strategy::WinNow) {
        strategy_term += 0.06;
    } else if (t.strategy == Team::Strategy::Rebuilding) {
        // Has the org won a tournament before? Look for any [T]/[M]/[W]
        // award on any current roster player (simple proxy for "we've
        // tasted success and want to rebuild around a star").
        int trophies = 0;
        for (const auto& p : t.roster) {
            if (!p) continue;
            for (const auto& a : p->awards) {
                if (a.find("[T]") != std::string::npos ||
                    a.find("[M]") != std::string::npos ||
                    a.find("[W]") != std::string::npos) {
                    ++trophies;
                    break;
                }
            }
            if (trophies >= 1) break;
        }
        if (trophies >= 1) strategy_term += 0.05;
    }

    // --- Recent international appearance (+0.05) ---
    // Any [M]/[W] award on any current roster player tagged within the
    // last 2 years. Award format includes the year as a substring (e.g.
    // "[M] Champ 2027"). We can't see the world year directly here so
    // fall back to "any [M]/[W] award present" — a contender that ever
    // attended Masters/Champions stays an active importer.
    bool intl_history = false;
    for (const auto& p : t.roster) {
        if (!p) continue;
        for (const auto& a : p->awards) {
            if (a.find("[M]") != std::string::npos ||
                a.find("[W]") != std::string::npos) {
                intl_history = true;
                break;
            }
        }
        if (intl_history) break;
    }
    double intl_term = intl_history ? 0.05 : 0.0;

    // --- Role scarcity in own region (up to +0.06) ---
    // If a target role is given AND we have a pool to inspect, count
    // same-role same-region FAs with OVR >= 60. Few qualified domestic
    // candidates -> high import incentive.
    double role_term = 0.0;
    if (target_role != Role::Count && regional_fa_pool != nullptr) {
        int regional_role_count = 0;
        for (const auto& p : *regional_fa_pool) {
            if (!p) continue;
            if (p->is_retired) continue;
            if (p->team_name != "Free Agent") continue;
            if (p->primary_role != target_role) continue;
            if (p->region != t.region) continue;
            if (p->ovr() < 60.0) continue;
            ++regional_role_count;
        }
        // 0 candidates -> 0.06, 5+ -> 0.0 (linear).
        if (regional_role_count <= 0) role_term = 0.06;
        else if (regional_role_count < 5) {
            role_term = 0.06 * (5.0 - regional_role_count) / 5.0;
        }
    }

    // --- Coach personality (+0.02 for analytics-flavoured coaches) ---
    double coach_term = 0.0;
    if (t.head_coach) {
        if (t.head_coach->personality == CoachPersonality::TacticalGenius ||
            t.head_coach->personality == CoachPersonality::Innovator ||
            t.head_coach->personality == CoachPersonality::AnalyticsHeavy) {
            coach_term = 0.02;
        }
    }

    // --- Organizational memory (±0.04 from import_success) ---
    // Past imports panned out -> raise appetite. Burned -> lower it.
    // Range roughly ±0.04 to keep within the 0.28 ceiling.
    double mem_term = static_cast<double>(t.memory.import_success) / 100.0 * 0.04;

    // --- Championship window bias (Phase B) ---
    // Closing-window teams desperately chase imports while their core is
    // still good; Opening-window rebuilds stay regional and let young
    // talent develop. Open / Closed teams sit neutral.
    double window_term = 0.0;
    if      (t.window == TeamWindow::Closing) window_term =  0.05;
    else if (t.window == TeamWindow::Opening) window_term = -0.03;

    double base = budget_term + prestige_term + strategy_term
                + intl_term + role_term + coach_term + mem_term
                + window_term;

    // --- Region multiplier ---
    // Americas leans more import-friendly; Pacific leans heavily domestic.
    // STRICT v2: all three regions dialed down so Pacific imports become
    // genuinely rare and EMEA is no longer the easy import market.
    double region_mult = 1.0;
    if (t.region == "Americas")     region_mult = 1.05;
    else if (t.region == "EMEA")    region_mult = 0.90;
    else if (t.region == "Pacific") region_mult = 0.75;

    double appetite = base * region_mult;
    if (appetite < 0.0)  appetite = 0.0;
    if (appetite > 0.28) appetite = 0.28;  // tighter ceiling — v2 strict cap
    return appetite;
}

void seed_team_colors(Team& t) {
    // Hash the team name to a deterministic primary color in HSV-like space,
    // then pick an accent color that contrasts.
    std::uint32_t h = 2166136261u;
    for (char c : t.name) { h ^= (std::uint32_t)(unsigned char)c; h *= 16777619u; }
    int hue_deg = (int)(h % 360u);
    auto hsv_to_rgb = [](int hue, double sat, double val, int& R, int& G, int& B) {
        double c = val * sat;
        double x = c * (1.0 - std::abs(std::fmod(hue / 60.0, 2.0) - 1.0));
        double m = val - c;
        double r=0, g=0, b=0;
        if      (hue <  60) { r=c; g=x; b=0; }
        else if (hue < 120) { r=x; g=c; b=0; }
        else if (hue < 180) { r=0; g=c; b=x; }
        else if (hue < 240) { r=0; g=x; b=c; }
        else if (hue < 300) { r=x; g=0; b=c; }
        else                { r=c; g=0; b=x; }
        R = clamp_v((int)((r + m) * 255.0), 0, 255);
        G = clamp_v((int)((g + m) * 255.0), 0, 255);
        B = clamp_v((int)((b + m) * 255.0), 0, 255);
    };
    hsv_to_rgb(hue_deg, 0.78, 0.85, t.color_primary_r, t.color_primary_g, t.color_primary_b);
    hsv_to_rgb((hue_deg + 60) % 360, 0.55, 0.95,
               t.color_accent_r, t.color_accent_g, t.color_accent_b);
}

double Team::ovr() const {
    if (roster.empty()) return 0.0;
    double s = 0.0;
    for (auto& p : roster) s += p->ovr();
    return s / roster.size();
}

void Team::sign_player(const PlayerPtr& p, int years, int current_year) {
    // 3-arg overload — default the contract_role to the player's natural
    // primary_role (no role shift, no penalty). 4-arg path can override.
    sign_player(p, years, current_year,
                p ? p->primary_role : Role::Count);
}

void Team::sign_player(const PlayerPtr& p, int years, int current_year,
                       Role intended_role) {
    if (!p) return;
    if (roster.size() >= 7) return;
    // Caller invariant: a contract must always be at least 1 year long. We
    // clamp defensively here so a stray `years = 0` doesn't yield a same-year
    // expiry (current_year + 0 - 1 = current_year - 1, instantly expired).
    if (years < 1) years = 1;
    roster.push_back(p);
    p->team_name = name;
    // `contract_years` is the FROZEN signed-duration snapshot — never
    // decremented at year-end. `contract.exp_year` is the live counter
    // (INCLUSIVE: the last season the player still plays under the deal).
    // For a 3-year deal signed in 2026, exp_year = 2028; the player plays
    // 2026/27/28 and is released at the end-of-2028 offseason via the
    // `exp_year <= year` gate in GameManager::run_end_of_year.
    p->contract_years = years;
    // Stamp the contract role. Role::Count means "use natural" — preserves
    // the legacy behavior so existing UI surfaces still work.
    p->contract_role = (intended_role == Role::Count)
                         ? p->primary_role : intended_role;
    if (current_year > 0) {
        p->contract.exp_year = current_year + years - 1;
        p->bump_mood(name, -p->mood_for(name));
    }
    // Strict invariant: every roster change must end with exactly one IGL
    // and one Flex among the starting 5 (when 5+ players are present).
    // This is the single point of enforcement so EVERY sign path stays
    // compliant. Order matters: IGL first (carries stat shift, anchors the
    // leadership slot), THEN Flex (positional wildcard).
    // The two flags are INDEPENDENT — a player can be both is_igl and
    // is_flex (the rare "Flex IGL"); no mutual exclusion enforced.
    enforce_one_igl();
    enforce_one_flex();
}

namespace {

// Mental-attribute composite, normalised to ~0..1 by dividing the 99-cap.
// Weights mirror Agent A's IGL-spawn function so promote / re-promote stays
// in sync with how IGLs are minted at world init.
//   GameSense       0.20
//   Communication   0.18
//   MidRoundCalling 0.18
//   Intelligence    0.15
//   Adaptability    0.12
//   Leadership      0.12
//   DecisionMaking  0.05
//                  ----
//                   1.00
double igl_mental_score(const Player& p) {
    const auto& a = p.attributes;
    return (at(a, Attr::GameSense)       * 0.20
          + at(a, Attr::Communication)   * 0.18
          + at(a, Attr::MidRoundCalling) * 0.18
          + at(a, Attr::Intelligence)    * 0.15
          + at(a, Attr::Adaptability)    * 0.12
          + at(a, Attr::Leadership)      * 0.12
          + at(a, Attr::DecisionMaking)  * 0.05) / 99.0;
}

// IGL-candidate score: mental composite × role bias. Controllers /
// Initiators are the canonical IGL roles, Sentinels OK, Duelists
// heavily penalised so DuelistIGLs only emerge when their MIND is
// elite. (Hard veto is applied separately in enforce_one_igl using
// the raw mental score before the role multiplier.)
double igl_candidate_score(const Player& p) {
    double mental = igl_mental_score(p);
    double role_mult = 1.0;
    switch (p.primary_role) {
        case Role::Controller: role_mult = 1.20; break;
        case Role::Initiator:  role_mult = 1.15; break;
        case Role::Sentinel:   role_mult = 1.00; break;
        case Role::Duelist:    role_mult = 0.35; break;
        default:               role_mult = 1.00; break;
    }
    return mental * role_mult;
}

// Apply the IGL stat shift to a player. Mirrors Player.cpp::generate_player
// for IGL-flagged spawns so promote/demote round-trips cleanly. `sign`
// = +1 on promotion, -1 on demotion (revert).
void apply_igl_stat_shift(Player& p, int sign) {
    p.apply_attribute_delta(Attr::Aim,            -12 * sign);
    p.apply_attribute_delta(Attr::Headshot,       -10 * sign);
    p.apply_attribute_delta(Attr::Entry,           -8 * sign);
    p.apply_attribute_delta(Attr::Aggressiveness,  -4 * sign);
    p.apply_attribute_delta(Attr::Leadership,     +14 * sign);
    p.apply_attribute_delta(Attr::Intelligence,   +10 * sign);
    p.apply_attribute_delta(Attr::Communication,  +10 * sign);
    p.apply_attribute_delta(Attr::GameSense,       +8 * sign);
    p.apply_attribute_delta(Attr::DecisionMaking,  +8 * sign);
    p.apply_attribute_delta(Attr::MidRoundCalling, +12 * sign);
    p.apply_attribute_delta(Attr::EconomyMgmt,    +12 * sign);
    p.apply_attribute_delta(Attr::AntiStrat,      +10 * sign);
}

}  // namespace

void Team::enforce_one_igl() {
    if (roster.size() < 5) return;
    const std::size_t starters = std::min<std::size_t>(5, roster.size());

    std::vector<std::size_t> igl_idxs;
    for (std::size_t i = 0; i < starters; ++i) {
        if (roster[i] && roster[i]->is_igl) igl_idxs.push_back(i);
    }

    // Pick best candidate among starters honoring the Duelist veto.
    // Returns starters (== "not found") if no acceptable candidate exists.
    auto pick_best_candidate = [&]() -> std::size_t {
        // Highest-score starter overall.
        std::size_t best = starters;
        double best_score = -1.0;
        for (std::size_t i = 0; i < starters; ++i) {
            if (!roster[i]) continue;
            double s = igl_candidate_score(*roster[i]);
            if (s > best_score) { best_score = s; best = i; }
        }
        if (best >= starters) return starters;  // empty / null roster

        // Duelist veto: a Duelist may ONLY be IGL if their pure mental
        // score clears 0.80 ("elite mind required"). Otherwise we walk
        // the candidate list and pick the next-best non-Duelist. If no
        // non-Duelist exists (degenerate all-Duelist starting 5) we
        // fall back to the highest Duelist regardless.
        const auto& champ = *roster[best];
        if (champ.primary_role == Role::Duelist && igl_mental_score(champ) < 0.80) {
            std::size_t alt = starters;
            double alt_score = -1.0;
            bool any_non_duelist = false;
            for (std::size_t i = 0; i < starters; ++i) {
                if (!roster[i]) continue;
                if (roster[i]->primary_role == Role::Duelist) continue;
                any_non_duelist = true;
                double s = igl_candidate_score(*roster[i]);
                if (s > alt_score) { alt_score = s; alt = i; }
            }
            if (any_non_duelist && alt < starters) return alt;
            // Fallback — all-Duelist starting 5. Promote the highest
            // Duelist anyway (rule explicitly allows this edge case).
        }
        return best;
    };

    if (igl_idxs.empty()) {
        std::size_t best = pick_best_candidate();
        if (best >= starters) return;
        auto& p = roster[best];
        if (!p) return;
        p->is_igl = true;
        // Promotion shift must mirror the spawn-time shift exactly so a
        // demote/repromote round-trip reverts cleanly.
        apply_igl_stat_shift(*p, +1);
        p->tend_play_aggressive = rng().irange(20, 80);
        p->tend_lurk_vs_execute = rng().irange(20, 80);
        p->tend_vocal           = rng().irange(40, 95);
        p->tend_adaptive        = rng().irange(30, 90);
        p->update_agent_pool();
        return;
    }

    if (igl_idxs.size() == 1) return;  // exactly one — no-op (user does not
                                       // want stat-thrash from re-electing).

    // 2+ IGLs in the starting 5 — keep the one with the highest
    // igl_candidate_score, demote the rest (revert stat shift).
    std::size_t keep = igl_idxs.front();
    double keep_score = igl_candidate_score(*roster[keep]);
    for (auto i : igl_idxs) {
        if (!roster[i]) continue;
        double s = igl_candidate_score(*roster[i]);
        if (s > keep_score) { keep_score = s; keep = i; }
    }
    for (auto i : igl_idxs) {
        if (i == keep) continue;
        auto& p = roster[i];
        if (!p) continue;
        p->is_igl = false;
        apply_igl_stat_shift(*p, -1);
        p->update_agent_pool();
    }
}

void Team::enforce_one_flex() {
    // At-least-one-Flex enforcement. Flex is an OVERLAY flag (like is_igl)
    // sitting on top of whatever primary_role the player has. NOT a Position.
    //
    // Insert-only: if any starter already has is_flex, this is a no-op.
    // Multiple Flex players in the starting 5 are allowed and never demoted
    // — Flex is a sub-role overlay, not a slot. INDEPENDENT of is_igl
    // (a player can carry both — the rare "Flex IGL").
    if (roster.size() < 5) return;
    const std::size_t starters = std::min<std::size_t>(5, roster.size());

    int flex_count = 0;
    for (std::size_t i = 0; i < starters; ++i) {
        if (roster[i] && roster[i]->is_flex) ++flex_count;
    }
    if (flex_count >= 1) return;  // already covered — done

    // No Flex among the starting 5. Promote the best candidate to is_flex.
    // Score: agent_pool_size·8 + Adaptability·2 + GameSense + Communication·0.5
    // (same heuristic as the old Position::Flex path, modulo it no longer
    // changes primary_role).
    auto flex_score = [](const Player& p) {
        return p.agent_pool_size * 8.0
             + at(p.attributes, Attr::Adaptability) * 2.0
             + at(p.attributes, Attr::GameSense)
             + at(p.attributes, Attr::Communication) * 0.5;
    };

    // First pass: prefer NON-IGL starters so we don't pile the Flex overlay
    // onto the IGL by default (Flex-IGLs should be uncommon, not auto-made).
    std::size_t best = starters;
    double best_s = -1.0;
    for (std::size_t i = 0; i < starters; ++i) {
        if (!roster[i]) continue;
        if (roster[i]->is_igl) continue;
        double s = flex_score(*roster[i]);
        if (s > best_s) { best_s = s; best = i; }
    }

    // Fallback: all starters are IGL (insane edge case — only possible via
    // god-mode). Allow Flex-IGL so the invariant still holds.
    if (best >= starters) {
        for (std::size_t i = 0; i < starters; ++i) {
            if (!roster[i]) continue;
            double s = flex_score(*roster[i]);
            if (s > best_s) { best_s = s; best = i; }
        }
    }

    if (best >= starters) return;
    roster[best]->is_flex = true;
    // No attribute shift — Flex is overlay-only, not stat-altering.
}

void Team::release_player(const PlayerPtr& p) {
    if (!p) return;
    auto it = std::find(roster.begin(), roster.end(), p);
    if (it != roster.end()) {
        roster.erase(it);
        p->team_name = "Free Agent";
        // Bump the cuts-this-year counter so update_org_memory can score
        // the team's stability_culture this year. Reset to 0 inside
        // update_org_memory after the metric delta has been applied.
        ++cuts_this_year_;
    }
}

int Team::total_payroll_k() const {
    int s = 0;
    for (auto& p : roster) s += p->contract.amount_k;
    if (head_coach) s += head_coach->salary_k;
    return s;
}
void Team::pay_payroll(int year) {
    long long pay = static_cast<long long>(total_payroll_k()) * 1000LL;
    budget -= pay;
    for (auto& p : roster) p->salary_log.emplace_back(year, p->contract.amount_k);
}

void Team::scout_top_fas(std::vector<PlayerPtr>& top_fas, int current_year) {
    std::array<int, static_cast<std::size_t>(Role::Count)> have{};
    for (auto& p : roster) have[static_cast<std::size_t>(p->primary_role)]++;
    std::vector<Role> missing;
    for (std::size_t i = 0; i < static_cast<std::size_t>(Role::Count); ++i) {
        int need = target_comp.need[i] - have[i];
        for (int k = 0; k < need; ++k) missing.push_back(static_cast<Role>(i));
    }

    while (!missing.empty() && !top_fas.empty() && roster.size() < 5) {
        Role target_role = missing.front();
        double best_score = -1e18;
        PlayerPtr best;
        for (auto& p : top_fas) {
            if (p->team_name != "Free Agent" || p->primary_role != target_role) continue;
            if (p->region != region) {
                int imports = 0;
                for (auto& x : roster) if (x->region != region) imports++;
                if (imports >= config().max_imports) continue;
            }
            double score = 0.0;
            score += scout_prefs.youth          * std::max(0, 25 - p->age);
            score += scout_prefs.experience     * (p->career_matches / 10.0);
            score += scout_prefs.mechanics      * ((at(p->attributes, Attr::Aim) + at(p->attributes, Attr::Headshot)) / 2.0);
            score += scout_prefs.smarts         * ((at(p->attributes, Attr::Intelligence) + at(p->attributes, Attr::GameSense)) / 2.0);
            score += scout_prefs.aggressiveness * at(p->attributes, Attr::Aggressiveness);
            score += scout_prefs.clutch         * at(p->attributes, Attr::Clutch);
            score += scout_prefs.potential      * p->potential;
            score += scout_prefs.flexibility    * static_cast<double>(p->agent_pool.size()) * 10.0;
            if (score > best_score && p->get_transfer_value() <= budget) {
                best_score = score;
                best = p;
            }
        }
        if (best) {
            budget -= best->get_transfer_value();
            sign_player(best, decide_contract_years(*best, current_year), current_year);
            top_fas.erase(std::remove(top_fas.begin(), top_fas.end(), best), top_fas.end());
            missing.erase(missing.begin());
        } else {
            break;
        }
    }
}

namespace {

// Filtered FA candidate pool: free, not retired, alive in `free_agents`.
std::vector<PlayerPtr> collect_free_agents(const std::vector<PlayerPtr>& free_agents) {
    std::vector<PlayerPtr> out;
    out.reserve(free_agents.size());
    for (const auto& p : free_agents) {
        if (!p) continue;
        if (p->team_name == "Free Agent" && !p->is_retired) out.push_back(p);
    }
    return out;
}

// Comp / strategy / personality blended ranking. 50% comp_fit + 35% strategy
// + 15% personality. Pulled out so the new per-slot fill helpers all run the
// SAME scoring as the legacy single-loop did.
struct CandidateRanker {
    CompTag             comp;
    Team::Strategy      strat;
    Personality         pers;

    double strat_score(const Player& p) const {
        double s = 0.0;
        switch (strat) {
            case Team::Strategy::Contender:
            case Team::Strategy::WinNow:
                s = p.ovr() * 1.4 + p.avg_match_rating() * 80.0
                  + p.career_matches * 0.20;
                break;
            case Team::Strategy::Rebuilding:
            case Team::Strategy::DevelopmentFocus:
                s = p.potential * 1.5 + std::max(0, 25 - p.age) * 8.0;
                break;
            case Team::Strategy::Bridge:
                s = p.ovr() * 1.0 + p.potential * 0.6;
                break;
            case Team::Strategy::BudgetRoster:
                s = p.ovr() * 0.8 - p.contract.amount_k * 0.5;
                break;
            case Team::Strategy::TalentFarm:
                s = (p.potential - p.ovr()) * 6.0 + std::max(0, 24 - p.age) * 6.0;
                break;
        }
        return s;
    }
    double pers_score(const Player& p) const {
        switch (pers) {
            case Personality::Aggressive:
                return at(p.attributes, Attr::Aim) + at(p.attributes, Attr::Entry);
            case Personality::Tactical:
                return at(p.attributes, Attr::Intelligence) + at(p.attributes, Attr::DecisionMaking);
            case Personality::Budget:
                return (p.potential - p.ovr()) * 8.0;
            case Personality::Balanced:
            default:
                return p.ovr();
        }
    }
    double total(const Player& p) const {
        return comp_fit_score(p, comp) * 0.50
             + strat_score(p)          * 0.35
             + pers_score(p)           * 0.15;
    }
};

}  // namespace

// ============================================================================
// auto_fill_roster — new D/C/I/S + flexible-5th model
//
// Fill order matches the user's spec for the new starting 5:
//   slot 1: Duelist
//   slot 2: Controller
//   slot 3: Initiator
//   slot 4: Sentinel
//   slot 5: flexible — preference cascade
//        a) a Flex player (is_flex == true) of any role
//        b) a secondary Initiator
//        c) a secondary Controller
//        d) best remaining FA
//
// Helpers below (has_role / try_fill_with_role / try_fill_with_flex_player /
// fill_best_remaining_fa) are file-static lambdas tied to *this so they can
// hit member state (region, budget, target_comp, strategy, personality) and
// call sign_player which carries the invariant enforcers.
// ============================================================================
void Team::auto_fill_roster(std::vector<PlayerPtr>& free_agents, int current_year) {

    // Shared scoring + signing primitive. Returns true if a signing happened.
    // Filters by role (or Role::Count == any), respects import appetite +
    // 2-import cap, requires affordability, and runs the comp/strat/pers
    // ranker before picking the top candidate.
    //
    // Captured by value into the per-helper closures so we don't allocate
    // a fresh vector each call.
    auto fill_one = [&](Role wanted_role,
                        bool require_flex_overlay) -> bool {
        if (roster.size() >= 5) return false;
        auto fas = collect_free_agents(free_agents);
        if (fas.empty()) return false;

        // === Import gate (same policy as the legacy loop) ====================
        int imports = 0;
        for (auto& x : roster) if (x && x->region != region) ++imports;
        double appetite = team_import_appetite(*this, wanted_role, &free_agents);
        bool import_attempt = (imports < config().max_imports)
                           && (appetite >= 0.20)
                           && rng().chance(appetite * 0.45);

        if (import_attempt) {
            std::vector<PlayerPtr> import_fas;
            for (auto& p : fas) {
                if (p->region != region && p->ovr() >= 60.0) import_fas.push_back(p);
            }
            if (!import_fas.empty()) fas.swap(import_fas);
            else                     import_attempt = false;
        }
        if (!import_attempt) {
            std::vector<PlayerPtr> regional;
            for (auto& p : fas) if (p->region == region) regional.push_back(p);
            if (!regional.empty())                       fas.swap(regional);
            else if (imports >= config().max_imports)    return false;
        }

        // === Quality + role + overlay filters ================================
        std::vector<PlayerPtr> valid;
        for (auto& p : fas) if (p->ovr() >= 40.0) valid.push_back(p);
        if (valid.empty()) valid = fas;

        if (wanted_role != Role::Count) {
            std::vector<PlayerPtr> role_fas;
            for (auto& p : valid) if (p->primary_role == wanted_role) role_fas.push_back(p);
            if (!role_fas.empty()) valid = std::move(role_fas);
        }
        if (require_flex_overlay) {
            std::vector<PlayerPtr> flex_fas;
            for (auto& p : valid) if (p->is_flex) flex_fas.push_back(p);
            // If no Flex-overlayed FA exists, the caller will fall through the
            // cascade — return false rather than silently signing a non-flex.
            if (flex_fas.empty()) return false;
            valid = std::move(flex_fas);
        }

        std::vector<PlayerPtr> affordable;
        for (auto& p : valid) {
            if (p->get_transfer_value() <= std::max<long long>(0, budget)) {
                affordable.push_back(p);
            }
        }

        PlayerPtr best;
        if (!affordable.empty()) {
            CandidateRanker rk{ comp_tag_of(target_comp), strategy, personality };
            // Composite score per candidate: base ranker + timeline fit
            // (multi-year coherence, weight 0.40) + risk_bias (Contenders
            // pay for consistency, Rebuilders gamble on volatile high
            // ceilings) + chemistry-reunite bonus (if this candidate has
            // prior positive synergy with any current roster member).
            // Precompute into a parallel vector then sort affordable by
            // it — keeps the rest of the desire-aware loop intact.
            std::vector<double> composite;
            composite.reserve(affordable.size());
            for (const auto& cand : affordable) {
                double s = rk.total(*cand);
                // Timeline fit (multi-year coherence nudge).
                s += 0.40 * timeline_fit_score(*this, *cand);

                // Risk tolerance via archetype consistency_mod.
                double cm = archetype_profile(cand->archetype).consistency_mod;
                double risk_bias = 0.0;
                if (strategy == Strategy::Contender ||
                    strategy == Strategy::WinNow) {
                    // Contenders pay for consistency: +cm scaled to ~±14
                    // score swing (cm itself is ±0.12).
                    risk_bias = cm * 120.0;
                } else if (strategy == Strategy::Rebuilding ||
                           strategy == Strategy::DevelopmentFocus ||
                           strategy == Strategy::TalentFarm) {
                    // Rebuilders gamble on volatile high-ceiling talent —
                    // reward NEGATIVE consistency_mod when potential is
                    // high enough to justify the boom/bust risk.
                    if (cand->potential >= 80) {
                        risk_bias = -cm * 80.0;
                    }
                }
                s += risk_bias;

                // Chemistry reunite bonus: if this FA has positive prior
                // chemistry (>= +0.5) with anyone currently on our roster,
                // reward bringing them back together. Almost always 0
                // unless they've previously played here / together.
                double total_pos_chem = 0.0;
                for (const auto& mate : roster) {
                    if (!mate) continue;
                    if (mate.get() == cand.get()) continue;
                    double c = chemistry_between(*mate, *cand);
                    if (c >= 0.5) total_pos_chem += c;
                }
                s += 8.0 * total_pos_chem;

                composite.push_back(s);
            }
            // Sort affordable by composite descending. Pair-sort approach
            // keeps `composite` in sync via an indirection vector.
            std::vector<std::size_t> idx(affordable.size());
            for (std::size_t i = 0; i < idx.size(); ++i) idx[i] = i;
            std::sort(idx.begin(), idx.end(),
                      [&](std::size_t x, std::size_t y) {
                          return composite[x] > composite[y];
                      });
            std::vector<PlayerPtr> reordered;
            reordered.reserve(affordable.size());
            for (auto i : idx) reordered.push_back(affordable[i]);
            affordable.swap(reordered);
            // Desire-aware affordability + acceptance pass: scan ranked
            // candidates and skip any whose Desire would either price
            // them out of THIS team specifically (salary_mult * baseline
            // exceeds 40% of our budget) OR whose Desire causes a flat
            // refusal of our offer. First candidate clearing both gates
            // is taken. If none clear, we fall back to the unconditional
            // top-ranked candidate (preserves legacy behaviour when no
            // desire-aware match exists).
            for (auto& cand : affordable) {
                if (!cand) continue;
                double mult       = cand->desire_salary_mult(*this);
                long long demand  = static_cast<long long>(
                                        cand->contract.amount_k * mult) * 1000LL;
                if (demand > static_cast<long long>(budget) * 4 / 10) continue;
                int adjusted_offer_k = static_cast<int>(cand->contract.amount_k * mult);
                if (!cand->desire_accepts(adjusted_offer_k, *this)) continue;
                best = cand;
                break;
            }
            if (!best) best = affordable.front();
        } else if (!fas.empty()) {
            // Nothing affordable — fall back to highest solo MMR FA in the
            // currently-filtered pool (matches the legacy behaviour).
            std::sort(fas.begin(), fas.end(),
                      [](const PlayerPtr& a, const PlayerPtr& b) { return a->solo_mmr > b->solo_mmr; });
            best = fas.front();
        }
        if (!best) return false;

        budget -= best->get_transfer_value();
        sign_player(best, decide_contract_years(*best, current_year), current_year);
        return true;
    };

    // Does any starter already carry this primary_role?
    auto has_role = [&](Role r) -> bool {
        const std::size_t starters = std::min<std::size_t>(5, roster.size());
        for (std::size_t i = 0; i < starters; ++i) {
            if (roster[i] && roster[i]->primary_role == r) return true;
        }
        return false;
    };

    auto try_fill_with_role        = [&](Role r) { return fill_one(r, /*flex=*/false); };
    auto try_fill_with_flex_player = [&]()       { return fill_one(Role::Count, /*flex=*/true); };
    auto fill_best_remaining_fa    = [&]()       { return fill_one(Role::Count, /*flex=*/false); };

    // --- Slots 1-4: one per core role -----------------------------------------
    if (roster.size() < 5 && !has_role(Role::Duelist))    try_fill_with_role(Role::Duelist);
    if (roster.size() < 5 && !has_role(Role::Controller)) try_fill_with_role(Role::Controller);
    if (roster.size() < 5 && !has_role(Role::Initiator))  try_fill_with_role(Role::Initiator);
    if (roster.size() < 5 && !has_role(Role::Sentinel))   try_fill_with_role(Role::Sentinel);

    // --- Slot 5: flexible wildcard -------------------------------------------
    if (roster.size() < 5) {
        if (!try_fill_with_flex_player()) {
            if (!try_fill_with_role(Role::Initiator)) {
                if (!try_fill_with_role(Role::Controller)) {
                    fill_best_remaining_fa();
                }
            }
        }
    }

    // --- Pathological recovery: any remaining vacancies (a missing core
    // role couldn't find a candidate at slot time but FAs have appeared
    // since via re-entry, OR an early try failed). Loop until we either hit
    // 5 starters or exhaust FAs. Matches the old "while (roster.size() < 5)"
    // resilience without re-introducing the legacy fill order.
    while (roster.size() < 5) {
        if (!fill_best_remaining_fa()) break;
    }

    // Strict invariants on the starting 5:
    //   * exactly 1 is_igl flag (enforce_one_igl) — overlays any role
    //   * >=1 is_flex flag       (enforce_one_flex) — overlay, insert-only
    // is_igl and is_flex are INDEPENDENT — a player can be both (rare
    // "Flex IGL"). IGL first (it carries the stat shift), then Flex.
    enforce_one_igl();
    enforce_one_flex();
    if (roster.size() >= 5) refresh_target_comp();
}

CompTag Team::pick_best_comp_for_roster() const {
    if (roster.empty()) return CompTag::DoubleInitiator;
    double agg = 0, entry = 0, aim = 0;
    double util = 0, comm = 0, gs = 0;
    double intel = 0, dec = 0, lead = 0;
    double pos = 0, clutch = 0, adapt = 0;
    int n = 0;
    for (auto& p : roster) {
        agg    += at(p->attributes, Attr::Aggressiveness);
        entry  += at(p->attributes, Attr::Entry);
        aim    += at(p->attributes, Attr::Aim);
        util   += at(p->attributes, Attr::Utility);
        comm   += at(p->attributes, Attr::Communication);
        gs     += at(p->attributes, Attr::GameSense);
        intel  += at(p->attributes, Attr::Intelligence);
        dec    += at(p->attributes, Attr::DecisionMaking);
        lead   += at(p->attributes, Attr::Leadership);
        pos    += at(p->attributes, Attr::Positioning);
        clutch += at(p->attributes, Attr::Clutch);
        adapt  += at(p->attributes, Attr::Adaptability);
        ++n;
    }
    if (n == 0) return CompTag::DoubleInitiator;
    double inv = 1.0 / n;
    double duelist_signal    = (agg + entry + aim) * inv;
    double initiator_signal  = (util + comm + gs)  * inv;
    double controller_signal = (intel + dec + lead) * inv;
    double sentinel_signal   = (pos + clutch + adapt) * inv;

    switch (personality) {
        case Personality::Aggressive: duelist_signal    += 6.0; break;
        case Personality::Tactical:   controller_signal += 6.0; break;
        case Personality::Budget:     sentinel_signal   += 4.0; break;
        case Personality::Balanced:   initiator_signal  += 3.0; break;
    }

    double best = duelist_signal;
    CompTag tag = CompTag::DoubleDuelist;
    if (initiator_signal  > best) { best = initiator_signal;  tag = CompTag::DoubleInitiator; }
    if (controller_signal > best) { best = controller_signal; tag = CompTag::DoubleController; }
    if (sentinel_signal   > best) { best = sentinel_signal;   tag = CompTag::DoubleSentinel; }
    return tag;
}

void Team::refresh_target_comp() { target_comp = comp_by_tag(pick_best_comp_for_roster()); }

namespace {

// Map a CompTag to a CompPlan + role-need array; small wrapper around the
// public comp_by_tag() so the per-map selection can swap target plans.
inline const CompPlan& plan_for(CompTag t) { return comp_by_tag(t); }

// Decide the comp tag this team will run on this map, factoring:
//   - the map's primary preference (Agent.cpp::map_pref)
//   - team-level CompIdentity overrides (DoubleDuelistTeam always wins)
//   - in-series adaptation: bias away from a tag we just lost on, force
//     a switch off a tag we've already used twice in the series.
//   - small "creative" reroll chance (FlexExperimental: 30%, else 12%).
CompTag decide_desired_tag(const Team& team,
                           const GameMap& map,
                           const std::vector<MapResultEntry>* prior,
                           bool is_team1) {
    const auto& pref = map_pref(map.name);
    CompTag desired = pref.primary;

    // Identity overrides.
    switch (team.comp_identity) {
        case CompIdentity::DoubleDuelistTeam:
            desired = CompTag::DoubleDuelist;
            break;
        case CompIdentity::AggressiveDive:
            // Prefer the secondary if it's a duelist comp, else nudge
            // primary toward DoubleDuelist when map's secondary isn't a duelist comp.
            if (pref.secondary == CompTag::DoubleDuelist) desired = pref.secondary;
            else if (pref.primary != CompTag::DoubleDuelist && rng().chance(0.45)) {
                desired = CompTag::DoubleDuelist;
            }
            break;
        case CompIdentity::UtilityHeavy:
            if (pref.primary != CompTag::DoubleInitiator &&
                pref.primary != CompTag::DoubleController) {
                desired = (pref.secondary == CompTag::DoubleInitiator ||
                           pref.secondary == CompTag::DoubleController)
                          ? pref.secondary
                          : CompTag::DoubleInitiator;
            }
            break;
        case CompIdentity::StructuredMacro:
            if (pref.primary == CompTag::DoubleSentinel) {
                desired = CompTag::DoubleSentinel;
            } else if (pref.secondary == CompTag::DoubleSentinel) {
                desired = pref.secondary;
            } else if (rng().chance(0.4)) {
                desired = CompTag::DoubleSentinel;
            }
            break;
        case CompIdentity::FlexExperimental:
            // Honour map but with extra creative noise downstream.
            break;
        case CompIdentity::Balanced:
        default:
            break;
    }

    // In-series adaptation. We only consider rows from the same `is_team1`
    // perspective for OUR comp & win record.
    if (prior && !prior->empty()) {
        // Track tag usage + last-loss tag.
        std::array<int, static_cast<std::size_t>(CompTag::Count)> used{};
        CompTag last_loss_tag = CompTag::Count;
        for (const auto& e : *prior) {
            CompTag our = is_team1 ? e.t1_comp : e.t2_comp;
            bool we_won = is_team1 ? e.t1_won
                                   : (!e.t1_won && e.t2_score > e.t1_score);
            used[static_cast<std::size_t>(our)]++;
            if (!we_won) last_loss_tag = our;
        }
        // If we just lost on `desired`, bias away from it.
        if (last_loss_tag == desired && rng().chance(0.50)) {
            desired = (pref.secondary != desired) ? pref.secondary
                                                  : pref.primary;
            // Final fallback: pick any other valid tag.
            if (desired == last_loss_tag) {
                for (int i = 0; i < static_cast<int>(CompTag::Count); ++i) {
                    if (static_cast<CompTag>(i) != last_loss_tag) {
                        desired = static_cast<CompTag>(i);
                        break;
                    }
                }
            }
        }
        // If we've already used the desired tag twice in this series,
        // force a switch.
        if (used[static_cast<std::size_t>(desired)] >= 2) {
            CompTag alt = (pref.secondary != desired) ? pref.secondary
                                                      : pref.primary;
            if (alt == desired) {
                for (int i = 0; i < static_cast<int>(CompTag::Count); ++i) {
                    if (static_cast<CompTag>(i) != desired) {
                        alt = static_cast<CompTag>(i);
                        break;
                    }
                }
            }
            desired = alt;
        }
    }

    // Creative reroll. FlexExperimental rolls more aggressively.
    double creative_p = (team.comp_identity == CompIdentity::FlexExperimental)
                        ? 0.30 : 0.12;
    if (rng().chance(creative_p)) {
        // Pick a different valid tag uniformly.
        std::vector<CompTag> options;
        for (int i = 0; i < static_cast<int>(CompTag::Count); ++i) {
            CompTag t = static_cast<CompTag>(i);
            if (t != desired) options.push_back(t);
        }
        if (!options.empty()) {
            desired = options[static_cast<std::size_t>(
                          rng().irange(0, static_cast<int>(options.size()) - 1))];
        }
    }

    return desired;
}

}  // namespace

Team::ResolvedTeam Team::get_team_overall_and_agents(const GameMap& map, bool is_solo_q) const {
    // Backwards-compat shim. Behaves like map-1-of-a-bo1: no series history
    // so adaptation logic is skipped.
    return build_round_selection(map, /*is_team1=*/true, /*prior_results=*/nullptr, is_solo_q);
}

Team::ResolvedTeam Team::build_round_selection(
        const GameMap& map,
        bool is_team1,
        const std::vector<MapResultEntry>* prior_results,
        bool /*is_solo_q*/) const {
    ResolvedTeam out;
    if (roster.size() < 5) return out;

    // === 1. Decide desired comp tag for THIS map ========================
    CompTag desired_tag = decide_desired_tag(*this, map, prior_results, is_team1);
    out.chosen_tag = desired_tag;
    const CompPlan& plan = plan_for(desired_tag);

    std::vector<PlayerPtr> players(roster.begin(), roster.begin() + 5);

    // Build role demand vector from the (possibly-overridden) plan.
    std::vector<Role> req;
    for (std::size_t i = 0; i < static_cast<std::size_t>(Role::Count); ++i) {
        for (int k = 0; k < plan.need[i]; ++k) req.push_back(static_cast<Role>(i));
    }

    std::unordered_map<Player*, Role> player_role_map;
    std::vector<PlayerPtr> unassigned_p = players;
    std::vector<Role> unassigned_r = req;

    auto take_role = [&](Role r) {
        auto it = std::find(unassigned_r.begin(), unassigned_r.end(), r);
        if (it != unassigned_r.end()) { unassigned_r.erase(it); return true; }
        return false;
    };

    // Phase A: assign players to their primary_role if the new plan still wants it.
    for (auto it = unassigned_p.begin(); it != unassigned_p.end();) {
        if (take_role((*it)->primary_role)) {
            player_role_map[(*it).get()] = (*it)->primary_role;
            it = unassigned_p.erase(it);
        } else { ++it; }
    }
    // Phase B: flex players use their pool's last entry as a hint.
    for (auto it = unassigned_p.begin(); it != unassigned_p.end();) {
        if (!(*it)->agent_pool.empty()) {
            Role flex = (*it)->agent_pool.back()->role;
            if (take_role(flex)) {
                player_role_map[(*it).get()] = flex;
                it = unassigned_p.erase(it);
                continue;
            }
        }
        ++it;
    }
    // Phase C: leftovers fill any remaining slot.
    for (auto& p : unassigned_p) {
        if (!unassigned_r.empty()) {
            player_role_map[p.get()] = unassigned_r.front();
            unassigned_r.erase(unassigned_r.begin());
        } else {
            player_role_map[p.get()] = Role::Duelist;
        }
    }

    // === 2. Per-player scoring with map-flavour + jitter ================
    const auto& pref = map_pref(map.name);

    std::unordered_set<std::string> picked_agents;
    double total = 0.0;
    for (auto& p : players) {
        Role tr = player_role_map[p.get()];
        std::vector<const Agent*> pool;
        bool off_role = false;
        for (auto* a : p->agent_pool) {
            if (a->role == tr && !picked_agents.count(a->name)) pool.push_back(a);
        }
        if (pool.empty()) {
            off_role = true;
            for (auto& a : agents()) if (a.role == tr && !picked_agents.count(a.name)) pool.push_back(&a);
        }
        if (pool.empty()) {
            for (auto& a : agents()) if (!picked_agents.count(a.name)) pool.push_back(&a);
        }
        if (pool.empty()) continue;

        // Score each candidate. Components (final weights, sum to ~1.0):
        //   0.65 * agent fit (get_rating)
        //   0.10 * pool priority (earlier in agent_pool = signature pick)
        //   0.10 * map-flavour fit (lurk/dive/anchor)
        //   0.10 * Player::map_mastery_bonus
        //   0.05 * uniform jitter (the bug-fix that breaks deterministic ties)
        //
        // Magnitudes are calibrated so a 4+ agent pool will swap picks
        // between maps in a series, but a clear stand-out will still win.
        const Agent* best_a = pool.front();
        double       best_score = -1e9;
        int          best_rating_int = 0;
        bool         best_off_role = off_role;
        for (auto* a : pool) {
            int rating_i = p->get_rating(*a, map);
            double rating_f = static_cast<double>(rating_i);
            // pool priority: position 0 = highest weight (signature),
            // tail = lower. Pool can be empty fallback to agents() in
            // which case priority is uniform 0.
            double pool_priority = 0.0;
            for (std::size_t k = 0; k < p->agent_pool.size(); ++k) {
                if (p->agent_pool[k] == a) {
                    pool_priority = static_cast<double>(p->agent_pool.size() - k);
                    break;
                }
            }
            double pool_norm = p->agent_pool.empty()
                               ? 0.0
                               : pool_priority / static_cast<double>(p->agent_pool.size());

            double flavor = agent_flavor_fit(*a, pref);  // 0..3
            // map_mastery_bonus is added by parallel agent B. Defaults to
            // 0.0 if they haven't shipped yet — call lives in Player.cpp.
            double mastery = p->map_mastery_bonus(map.name);

            double jitter = rng().drange(0.0, 1.0);

            double score =
                  rating_f * 0.65
                + pool_norm * 10.0 * 0.10      // scale pool 0..10 -> contribution
                + flavor * 5.0 * 0.10          // 0..3 -> 0..15 raw, weighted 10%
                + mastery * 0.10
                + jitter * (rating_f * 0.06);  // ~6% of rating as noise

            // Identity nudges at agent-level: AggressiveDive teams favour
            // dive-flagged picks an extra notch, StructuredMacro favours
            // anchor-flagged, etc. Keeps the team feel even after the
            // tag has been decided.
            switch (comp_identity) {
                case CompIdentity::AggressiveDive:
                    if (pref.favors_dive && flavor > 0.0) score += 2.5;
                    break;
                case CompIdentity::StructuredMacro:
                    if (pref.favors_anchor && flavor > 0.0) score += 2.5;
                    break;
                case CompIdentity::FlexExperimental:
                    // Extra jitter — encourages off-meta agent picks.
                    score += rng().drange(-3.0, 3.0);
                    break;
                default:
                    break;
            }

            if (score > best_score) {
                best_score = score;
                best_a = a;
                best_rating_int = rating_i;
                best_off_role = off_role;
            }
        }
        out.chosen_agents[p.get()] = best_a;
        picked_agents.insert(best_a->name);
        double rating = best_rating_int;
        if (best_off_role) rating *= 0.85;
        total += rating;
    }
    out.team_ovr = total / 5.0;
    return out;
}

void Team::save_history(int year) {
    history.push_back({year, wins, losses});
}

void Team::ai_manage_roster(std::vector<PlayerPtr>& free_agents, int current_year) {
    // Organizational-memory overlay: `stability_culture` scales the
    // cut-aggression threshold.
    //   * High stability (>= +40): MORE patient — players need to be
    //     even worse (rating < 0.80) before the team will cut them.
    //   * Low stability (<= -40): IMPATIENT — relaxes the threshold so
    //     mediocre players (< 0.90) are also cut.
    //   * Neutral: original < 0.85 threshold.
    double rating_threshold = 0.85;
    if      (memory.stability_culture >= 40) rating_threshold = 0.80;
    else if (memory.stability_culture <= -40) rating_threshold = 0.90;

    std::vector<PlayerPtr> drop;
    for (auto& p : roster) {
        bool poor = (p->career_matches > 10)
                 && (p->avg_match_rating() < rating_threshold)
                 && (p->potential < 65);
        // Contract expiry now lives on `Player::years_left` (which reads
        // `contract.exp_year`). `contract_years` is a frozen signed-duration
        // snapshot and is NOT decremented at year-end any more.
        int yrs_left = p->years_left(current_year);
        if (yrs_left <= 0 || poor) {
            // Chemistry retention boost: if this player has strong positive
            // synergy with the rest of the roster (sum >= +2.0), chemistry
            // trumps slight underperformance and we keep them — core synergy
            // preserved. Contract-expiration cuts (yrs_left <= 0) still
            // go through (the engine MUST release expired deals so the
            // re-sign loop can run).
            if (poor && yrs_left > 0) {
                double total_chem = 0.0;
                for (const auto& mate : roster) {
                    if (!mate || mate.get() == p.get()) continue;
                    total_chem += chemistry_between(*mate, *p);
                }
                if (total_chem >= 2.0) continue;  // protected
            }
            drop.push_back(p);
        }
    }
    for (auto& p : drop) release_player(p);
    auto_fill_roster(free_agents, current_year);
}

int Team::decide_contract_years(const Player& p, int current_year) const {
    (void)current_year;  // reserved for future budget-projection use

    // === Stage 1: player's preferred length (0..4 continuous) =========
    double lp = p.desire_length_pref(*this);
    double base = lp * 4.0;

    // === Stage 2: team window adjustments =============================
    // Closing — desperate but short flexibility for next rebuild.
    // Closed — minimum commitment, rebuilding.
    // Opening — building a long-term core, lock people in.
    // Open — prime contention, moderate-long commitments.
    switch (window) {
        case TeamWindow::Closing: base -= 1.0; break;
        case TeamWindow::Closed:  base -= 1.5; break;
        case TeamWindow::Opening: base += 0.8; break;
        case TeamWindow::Open:    base += 0.3; break;
    }

    // === Stage 3: strategy ============================================
    switch (strategy) {
        case Strategy::WinNow:           base -= 0.5; break;
        case Strategy::Rebuilding:       base += 0.7; break;
        case Strategy::DevelopmentFocus: base += 0.9; break;
        case Strategy::TalentFarm:       base += 0.4; break;
        case Strategy::Contender:        base += 0.2; break;
        case Strategy::BudgetRoster:     base -= 0.8; break;
        case Strategy::Bridge:           base -= 0.2; break;
    }

    // === Stage 4: player age ==========================================
    // 18-20 → big lock-in (dev project); 30+ → short ring-chase.
    if      (p.age <= 20) base += 1.2;
    else if (p.age <= 23) base += 0.6;
    else if (p.age <= 27) base += 0.0;
    else if (p.age <= 30) base -= 0.6;
    else                  base -= 1.6;

    // === Stage 5: potential gap (development bet) =====================
    int gap = p.potential - static_cast<int>(p.ovr());
    if (gap >= 12 && p.age <= 24)       base += 0.7;
    else if (gap >= 8 && p.age <= 26)   base += 0.4;
    else if (gap < 0)                   base -= 0.3;  // already declining

    // === Stage 6: organizational memory ===============================
    // Disciplined orgs prefer shorter, recoverable deals; loose orgs over-commit.
    if (memory.financial_discipline >= 30)  base -= 0.4;
    else if (memory.financial_discipline <= -30) base += 0.3;

    // === Stage 7: budget headroom =====================================
    // If the team is broke, can't realistically commit long. Use a rough
    // per-year-per-slot projection: budget / 5 slots / ~$50K typical = N
    // years of headroom. If headroom < 2 years, force short.
    long long headroom_years = budget / (50LL * 1000LL * 5LL);
    if (headroom_years <= 1) base -= 0.8;
    else if (headroom_years <= 2) base -= 0.3;
    // If the team is rich AND in a contention window, willing to commit.
    if (budget >= 1'200'000LL && (window == TeamWindow::Open || window == TeamWindow::Closing)) {
        base += 0.3;
    }

    // === Stage 8: clamp to [1, 4] =====================================
    int years = static_cast<int>(std::round(base));
    if (years < 1) years = 1;
    if (years > 4) years = 4;

    // === Stage 9: respect player's tolerance (never propose what they'd reject)
    int max_player_years;
    if      (lp <= 0.25) max_player_years = 2;   // bucket 1 player → tolerates up to 2
    else if (lp <= 0.50) max_player_years = 3;
    else if (lp <= 0.75) max_player_years = 4;
    else                 max_player_years = 5;
    if (years > max_player_years) years = max_player_years;

    return years;
}

void Team::ai_full_offseason_pass(std::vector<PlayerPtr>& free_agents,
                                  int current_year,
                                  std::vector<std::string>& log) {
    // ===== Stage 0: Header log =====
    {
        char buf[200];
        std::snprintf(buf, sizeof(buf),
            "[OFFSEASON GM] %s — running full offseason pass. Strategy: %s, "
            "Window: %s, Budget: $%lldK, Roster: %zu/5 starters.",
            name.c_str(),
            (strategy == Strategy::WinNow      ? "WinNow"
            :strategy == Strategy::Contender   ? "Contender"
            :strategy == Strategy::Bridge      ? "Bridge"
            :strategy == Strategy::Rebuilding  ? "Rebuilding"
            :strategy == Strategy::DevelopmentFocus ? "Development"
            :strategy == Strategy::BudgetRoster ? "BudgetRoster"
            :"TalentFarm"),
            team_window_name(window),
            budget / 1000LL,
            std::min<std::size_t>(5, roster.size()));
        log.emplace_back(buf);
    }

    // ===== Stage 1: Drop expired / clearly poor performers =====
    // Mirror ai_manage_roster's cut logic but log every drop explicitly so
    // the user can see what happened.
    double rating_threshold = 0.85;
    if      (memory.stability_culture >= 40)  rating_threshold = 0.80;
    else if (memory.stability_culture <= -40) rating_threshold = 0.90;

    std::vector<PlayerPtr> drop;
    for (auto& p : roster) {
        if (!p) continue;
        bool poor = (p->career_matches > 10)
                 && (p->avg_match_rating() < rating_threshold)
                 && (p->potential < 65);
        int yrs_left = p->years_left(current_year);
        if (yrs_left <= 0 || poor) {
            // Chemistry retention for poor (but not expired) — preserves
            // synergistic cores even when one member is slightly down.
            if (poor && yrs_left > 0) {
                double total_chem = 0.0;
                for (const auto& mate : roster) {
                    if (!mate || mate.get() == p.get()) continue;
                    total_chem += chemistry_between(*mate, *p);
                }
                if (total_chem >= 2.0) {
                    char buf[200];
                    std::snprintf(buf, sizeof(buf),
                        "[OFFSEASON GM] Kept %s — strong chemistry (+%.1f) overrides poor form.",
                        p->name.c_str(), total_chem);
                    log.emplace_back(buf);
                    continue;
                }
            }
            drop.push_back(p);
        }
    }
    for (auto& p : drop) {
        char buf[200];
        std::snprintf(buf, sizeof(buf),
            "[OFFSEASON GM] Released %s (%s, %d OVR — %s).",
            p->name.c_str(),
            role_name(p->primary_role),
            (int)std::lround(p->ovr()),
            (p->years_left(current_year) <= 0 ? "contract expired"
                                              : "underperformance"));
        log.emplace_back(buf);
        release_player(p);
    }

    // ===== Stage 2: Per-starter value assessment + targeted upgrade scan ==
    // For each remaining starter (positions 0..4 if filled), compute their
    // "current value score" and scan the FA pool for upgrades AT THE SAME
    // ROLE. If a free agent is meaningfully better, the team can afford,
    // and the candidate accepts the offer, execute a CUT-AND-REPLACE.
    //
    // "Meaningfully better" = upgrade OVR >= incumbent OVR + 4 (no lateral
    // moves). Also requires the incumbent's last-season rating < 1.10 (so
    // we don't churn a star purely because someone else is +5 OVR on paper).
    auto starter_value = [&](const Player& p) {
        double v = p.ovr();
        if (p.age <= 24)        v += (p.potential - p.ovr()) * 0.30;
        else if (p.age >= 30)   v -= 5.0;
        v += p.avg_match_rating() * 8.0;  // form premium
        return v;
    };

    int upgrades_attempted = 0;
    int upgrades_committed = 0;
    // 2026-05-28: dropped from 2 to 1 per pass. Over-signing audit feedback
    // noted that 2 cut-and-replace cycles per offseason for every team is
    // closer to "annual roster reset" than to GM behavior.
    constexpr int kMaxUpgradesPerPass = 1;

    for (std::size_t slot = 0; slot < std::min<std::size_t>(5, roster.size()); ++slot) {
        if (upgrades_committed >= kMaxUpgradesPerPass) break;
        auto& incumbent = roster[slot];
        if (!incumbent) continue;
        // Don't trade away protected pieces.
        if (incumbent->avg_match_rating() >= 1.10) continue;
        if (incumbent->age <= 22 && incumbent->potential >= 75) continue;
        double inc_val = starter_value(*incumbent);
        Role role = incumbent->primary_role;

        // Scan FAs for upgrades at this role.
        PlayerPtr best_upgrade;
        double best_delta = 4.0;  // minimum +4 OVR threshold
        for (auto& fa : free_agents) {
            if (!fa || fa->is_retired) continue;
            if (fa->team_name != "Free Agent") continue;
            if (fa->primary_role != role) continue;
            // Import cap respect
            if (fa->region != region) {
                int imp = 0;
                for (auto& x : roster) if (x && x->region != region) ++imp;
                if (imp >= config().max_imports) continue;
                if (fa->ovr() < 60.0) continue;
            }
            double up_val = starter_value(*fa);
            double delta = up_val - inc_val;
            if (delta < best_delta) continue;

            // Affordability check using player's actual ask via the
            // team-context overload of gen_contract.
            int ask_k = fa->gen_contract(current_year, false, false, this).amount_k;
            long long cost = static_cast<long long>(ask_k) * 1000LL;
            if (cost > budget * 4 / 10) continue;  // <= 40% of budget per upgrade

            // Will they actually accept? Use the multi-factor years decision
            // so the offered length is contextually reasoned, not arbitrary.
            int yrs = decide_contract_years(*fa, current_year);
            if (!fa->accepts_resign_offer(ask_k, yrs, *this)) continue;

            best_delta = delta;
            best_upgrade = fa;
        }

        if (best_upgrade) {
            ++upgrades_attempted;
            // Commit cut+replace
            char buf[260];
            std::snprintf(buf, sizeof(buf),
                "[OFFSEASON GM] UPGRADE: %s (%s %d OVR) -> %s (%d OVR, +%.1f value).",
                incumbent->name.c_str(), role_name(role), (int)std::lround(incumbent->ovr()),
                best_upgrade->name.c_str(), (int)std::lround(best_upgrade->ovr()),
                best_delta);
            log.emplace_back(buf);

            release_player(incumbent);  // mutates roster — careful below
            int ask_k = best_upgrade->gen_contract(current_year, false, false, this).amount_k;
            int yrs = decide_contract_years(*best_upgrade, current_year);
            budget -= static_cast<long long>(ask_k) * 1000LL;
            best_upgrade->contract.amount_k = ask_k;
            sign_player(best_upgrade, yrs, current_year);
            ++upgrades_committed;
            // After release+sign, roster indices may have shifted — restart
            // the scan from the slot we just touched.
            slot = 0;
            continue;
        }
    }
    if (upgrades_committed > 0) {
        char buf[200];
        std::snprintf(buf, sizeof(buf),
            "[OFFSEASON GM] Committed %d roster upgrade(s).", upgrades_committed);
        log.emplace_back(buf);
    }

    // ===== Stage 3: Backstop cascade-fill for any vacancies ================
    if (roster.size() < 5) {
        std::size_t before = roster.size();
        auto_fill_roster(free_agents, current_year);
        char buf[200];
        std::snprintf(buf, sizeof(buf),
            "[OFFSEASON GM] Cascade-filled %zu vacant slot(s) to reach %zu starters.",
            roster.size() - before, std::min<std::size_t>(5, roster.size()));
        log.emplace_back(buf);
    }

    // ===== Stage 4: Bench depth (single slot, optional) ====================
    // 2026-05-28 rebalance: capped at +1 bench piece (max 6 total, not 7),
    // raised budget floor from $200K -> $400K, added positional-stacking
    // guard (skip roles where the team already has 2+ players). User audit
    // feedback: bench-depth Stage 4 was inflating rosters to 7 every cycle
    // for every team, creating bloat + duplicate positional stacking.
    auto roster_role_count = [&](Role r) {
        int n = 0;
        for (auto& x : roster) if (x && x->primary_role == r) ++n;
        return n;
    };
    if (roster.size() == 5 && budget >= 400'000LL) {
        PlayerPtr bench_pick;
        double best_score = -1e9;
        for (auto& fa : free_agents) {
            if (!fa || fa->is_retired) continue;
            if (fa->team_name != "Free Agent") continue;
            bool on_roster = false;
            for (auto& r : roster) if (r.get() == fa.get()) { on_roster = true; break; }
            if (on_roster) continue;
            // Positional stacking guard — never bench-sign a 3rd at any role.
            if (roster_role_count(fa->primary_role) >= 2) continue;
            if (fa->region != region) {
                int imp = 0;
                for (auto& x : roster) if (x && x->region != region) ++imp;
                if (imp >= config().max_imports) continue;
            }
            int ask_k = fa->gen_contract(current_year, false, false, this).amount_k;
            // Bench piece must cost <= 20% of budget (was 25%).
            if (static_cast<long long>(ask_k) * 1000LL > budget / 5) continue;
            double score = fa->potential * 0.6 + fa->ovr() * 0.4 - ask_k * 0.5;
            if (fa->age <= 21 && fa->potential >= 75) score += 30;
            if (score > best_score) {
                best_score = score;
                bench_pick = fa;
            }
        }
        if (bench_pick) {
            int ask_k = bench_pick->gen_contract(current_year, false, false, this).amount_k;
            int yrs = std::max(1, decide_contract_years(*bench_pick, current_year) - 1);
            if (bench_pick->accepts_resign_offer(ask_k, yrs, *this)) {
                char buf[220];
                std::snprintf(buf, sizeof(buf),
                    "[OFFSEASON GM] Bench depth: signed %s (%s, %d OVR / %d POT) for %dY at $%dK.",
                    bench_pick->name.c_str(), role_name(bench_pick->primary_role),
                    (int)std::lround(bench_pick->ovr()), bench_pick->potential,
                    yrs, ask_k);
                log.emplace_back(buf);

                budget -= static_cast<long long>(ask_k) * 1000LL;
                bench_pick->contract.amount_k = ask_k;
                sign_player(bench_pick, yrs, current_year);
            }
        }
    }

    // ===== Stage 5: Final report =========================================
    {
        char buf[260];
        std::snprintf(buf, sizeof(buf),
            "[OFFSEASON GM] %s — pass complete. Roster: %zu (%zu starters), "
            "Budget: $%lldK remaining. Upgrades: %d, Bench depth: %zu.",
            name.c_str(), roster.size(), std::min<std::size_t>(5, roster.size()),
            budget / 1000LL, upgrades_committed,
            roster.size() > 5 ? (roster.size() - 5) : 0);
        log.emplace_back(buf);
    }
}

// === Trophy case + dynasty tier ============================================
//
// trophy_history_ is appended by record_trophy() (GameManager calls it when
// a tour finishes). finals_history_ is a forward-compat hook — left empty
// until Agent A wires it. The accessors degrade gracefully to zero/None.

void Team::record_trophy(int year, const std::string& event_name) {
    // Idempotent: skip if the exact (year, event_name) already exists. Lets
    // GameManager call this from both `play_tournament_round` AND
    // `force_finish_stale_tournaments` without risking a double-pin.
    for (const auto& tp : trophy_history_) {
        if (tp.first == year && tp.second == event_name) return;
    }
    trophy_history_.emplace_back(year, event_name);
}

Team::TrophyCase Team::trophy_case() const {
    TrophyCase tc;
    // Walk trophy_history_; classify each entry by substring match on the
    // event name. Tournament names emit "Regional ...", "Masters ...",
    // "Champions ..." per PROJECT_GUIDE §4.14.
    for (const auto& tp : trophy_history_) {
        const std::string& ev = tp.second;
        if (ev.find("Regional") != std::string::npos) {
            ++tc.regional_titles;
        } else if (ev.find("Masters") != std::string::npos) {
            ++tc.masters_titles;
        } else if (ev.find("Champions") != std::string::npos ||
                   ev.find("World")     != std::string::npos) {
            ++tc.world_titles;
        }
        tc.ordered.emplace_back(tp.first, ev);
    }
    // total_finals: 0 if finals_history_ isn't populated yet. When Agent A
    // wires the finalist-detection hook this lights up automatically.
    tc.total_finals = static_cast<int>(finals_history_.size());
    return tc;
}

// ============================================================================
// === Organizational memory year-end update =================================
// ============================================================================
//
// Walks the current roster and scores six rolling metrics based on this
// season's performance + last few seasons of signings. See OrgMemory in
// Team.h for the per-metric contract. Each delta is applied first, then a
// 3% decay toward zero (multiplicative 0.97), then clamped to [-100,+100].
// `cuts_this_year_` is consumed and reset.
void Team::update_org_memory(int current_year) {
    // Helper: tenure proxy = number of years the player has been with us.
    // We don't track team membership in salary_log, so we approximate by
    // bounding tenure to min(salary_log.size(), age - 16, 6). The 3-year
    // gates below use this only directionally — close enough to capture
    // "they came in as a rookie under our regime".
    auto tenure_proxy = [](const Player& p) -> int {
        int n = static_cast<int>(p.salary_log.size());
        // Players signed as rookies can't have a longer tenure than their
        // age minus a notional 16-year career start floor.
        int age_floor = std::max(0, p.age - 16);
        return std::min({n, age_floor, 6});
    };
    auto signed_within_3y = [&](const Player& p) -> bool {
        return tenure_proxy(p) <= 3;
    };
    auto age_at_signing = [&](const Player& p) -> int {
        return p.age - tenure_proxy(p);
    };
    auto rated_year_award = [](const Player& p, int yr) -> bool {
        std::string yr_str = std::to_string(yr);
        for (const auto& a : p.awards) {
            if ((a.find("[T]") != std::string::npos ||
                 a.find("[M]") != std::string::npos ||
                 a.find("[W]") != std::string::npos) &&
                a.find(yr_str) != std::string::npos) {
                return true;
            }
        }
        return false;
    };

    // -- rookie_success: young recent signings --------------------------------
    int rookie_delta = 0;
    for (const auto& p : roster) {
        if (!p) continue;
        if (!signed_within_3y(*p)) continue;
        if (age_at_signing(*p) > 22) continue;
        double r = p->avg_match_rating();
        if      (r >= 1.10) rookie_delta += 5;
        else if (r >= 1.00) rookie_delta += 2;
        else if (r <= 0.85) rookie_delta -= 4;
    }
    memory.rookie_success += rookie_delta;

    // -- import_success: cross-region recent signings -------------------------
    int import_delta = 0;
    for (const auto& p : roster) {
        if (!p) continue;
        if (!signed_within_3y(*p)) continue;
        if (p->region == region) continue;       // domestic, skip
        double r = p->avg_match_rating();
        if      (r >= 1.10) import_delta += 5;
        else if (r >= 1.00) import_delta += 2;
        else if (r <= 0.85) import_delta -= 4;
        if (rated_year_award(*p, current_year)) import_delta += 8;
        // Best-effort "cut mid-season" detection: a player whose team_name
        // currently == "Free Agent" but who we still scan via roster
        // membership shouldn't actually appear here (release_player drops
        // them). The heuristic instead penalises imports who clearly
        // underperformed this season ( <0.80 rating ) on the assumption
        // they're heading for the chopping block.
        if (r > 0.0 && r < 0.80) import_delta -= 6;
    }
    memory.import_success += import_delta;

    // -- veteran_success: 28+ at signing --------------------------------------
    int veteran_delta = 0;
    for (const auto& p : roster) {
        if (!p) continue;
        if (!signed_within_3y(*p)) continue;
        if (age_at_signing(*p) < 28) continue;
        double r = p->avg_match_rating();
        if      (r >= 1.10) veteran_delta += 5;
        else if (r >= 1.00) veteran_delta += 2;
        else if (r <= 0.85) veteran_delta -= 4;
        if (rated_year_award(*p, current_year)) veteran_delta += 6;
    }
    memory.veteran_success += veteran_delta;

    // -- financial_discipline: payroll vs strategy-tier baseline --------------
    // 2026-05 rebalance: baselines re-scaled to match the new salary cap
    // ($180K/player) + budget tiers ($250K-$2M). See classify_team_strategy
    // for the matching budget thresholds. Keeping these aligned matters —
    // a mismatch would push every team into "overpaying" territory.
    int fin_delta = 0;
    long long tier_baseline_k = 700; // default ~$700K
    switch (strategy) {
        case Strategy::Contender:        tier_baseline_k = 900;  break;
        case Strategy::WinNow:           tier_baseline_k = 1500; break;
        case Strategy::Bridge:           tier_baseline_k = 600;  break;
        case Strategy::BudgetRoster:     tier_baseline_k = 300;  break;
        case Strategy::Rebuilding:
        case Strategy::DevelopmentFocus:
        case Strategy::TalentFarm:
        default:                         tier_baseline_k = 700;  break;
    }
    long long actual_k = static_cast<long long>(total_payroll_k());
    double ratio = static_cast<double>(actual_k) / static_cast<double>(tier_baseline_k);
    if      (ratio <= 0.85) fin_delta += 3;       // >=15% under
    else if (ratio <= 0.95) fin_delta += 1;       // >=5% under
    else if (ratio >= 1.25) fin_delta -= 5;       // >=25% over
    else if (ratio >= 1.10) fin_delta -= 2;       // >=10% over
    memory.financial_discipline += fin_delta;

    // -- stability_culture: # cuts this year (cuts_this_year_) ----------------
    int stab_delta = 0;
    if      (cuts_this_year_ == 0) stab_delta += 4;
    else if (cuts_this_year_ <= 2) stab_delta += 1;
    else if (cuts_this_year_ <= 4) stab_delta -= 2;
    else                            stab_delta -= 5;
    memory.stability_culture += stab_delta;
    cuts_this_year_ = 0;

    // -- star_dependency: top-1 rating / avg-of-other-4 ----------------------
    int star_delta = 0;
    {
        std::vector<double> ratings;
        ratings.reserve(roster.size());
        for (const auto& p : roster) {
            if (!p) continue;
            ratings.push_back(p->avg_match_rating());
        }
        if (ratings.size() >= 5) {
            std::sort(ratings.begin(), ratings.end(), std::greater<double>());
            double top = ratings[0];
            double avg_others = (ratings[1] + ratings[2] + ratings[3] + ratings[4]) / 4.0;
            if (avg_others > 0.001) {
                double r = top / avg_others;
                if      (r >= 1.30) star_delta += 3;
                else if (r >= 1.20) star_delta += 1;
                else if (r <= 1.05) star_delta -= 2;
            }
        }
    }
    memory.star_dependency += star_delta;

    // -- Decay 3% toward 0 + clamp to [-100, +100] ----------------------------
    auto decay_and_clamp = [](int& m) {
        m = static_cast<int>(m * 0.97);
        if (m >  100) m =  100;
        if (m < -100) m = -100;
    };
    decay_and_clamp(memory.rookie_success);
    decay_and_clamp(memory.import_success);
    decay_and_clamp(memory.veteran_success);
    decay_and_clamp(memory.financial_discipline);
    decay_and_clamp(memory.stability_culture);
    decay_and_clamp(memory.star_dependency);

    // === Chemistry year-end decay + off-roster prune =========================
    //
    // Decay every chemistry edge by 5% (multiplicative). Then prune any
    // entry where NEITHER endpoint is currently on this team's roster —
    // chemistry between two players who've both moved on is dead weight in
    // the map and would balloon it across decades of save data. Keep edges
    // where at least one endpoint is still on the roster (mid-team trade
    // case — preserve the synergy memory for the player still here).
    //
    // Two-step: decay-in-place via iteration, then erase-by-key via a small
    // staging vector so we don't invalidate the iterator while erasing.
    if (!chemistry.empty()) {
        std::unordered_set<Player*> on_roster;
        on_roster.reserve(roster.size());
        for (const auto& p : roster) {
            if (p) on_roster.insert(p.get());
        }
        std::vector<ChemKey> to_erase;
        to_erase.reserve(chemistry.size() / 8 + 1);
        for (auto& kv : chemistry) {
            kv.second *= 0.95;
            if (on_roster.find(kv.first.a) == on_roster.end() &&
                on_roster.find(kv.first.b) == on_roster.end()) {
                to_erase.push_back(kv.first);
            }
        }
        for (const auto& k : to_erase) chemistry.erase(k);
    }
}

// ============================================================================
// === Strategy Inertia transition matrix ====================================
// ============================================================================
//
// Probabilistic gate on year-end strategy transitions. If classify suggests
// a strategy different from `t.previous_strategy`, roll the matrix entry;
// success -> commit to the new, failure -> stay on previous. Either way,
// previous_strategy is rewritten to the committed value.
Team::Strategy commit_strategy_with_inertia(Team& t,
                                            Team::Strategy suggested) {
    using S = Team::Strategy;
    // Fast-path: no change suggested -> no roll, just update bookkeeping
    // and return. Also covers the year-1 case where GameManager seeds
    // previous_strategy with the classify result so the first transition
    // matches legacy behaviour exactly.
    if (suggested == t.previous_strategy) {
        t.previous_strategy = suggested;
        return suggested;
    }

    auto idx_of = [](S s) -> int {
        switch (s) {
            case S::Contender:        return 0;
            case S::WinNow:           return 1;
            case S::Bridge:           return 2;
            case S::Rebuilding:       return 3;
            case S::DevelopmentFocus: return 4;
            case S::BudgetRoster:     return 5;
            case S::TalentFarm:       return 6;
        }
        return 2;
    };

    // Row = previous, col = suggested. Diagonal is unreachable here
    // (covered by fast-path), filled with 1.0 defensively.
    static const double M[7][7] = {
        /* from Contender    */ { 1.00, 0.55, 0.40, 0.20, 0.25, 0.30, 0.25 },
        /* from WinNow       */ { 0.60, 1.00, 0.45, 0.15, 0.20, 0.30, 0.20 },
        /* from Bridge       */ { 0.50, 0.40, 1.00, 0.55, 0.60, 0.50, 0.50 },
        /* from Rebuilding   */ { 0.20, 0.15, 0.45, 1.00, 0.65, 0.40, 0.55 },
        /* from Development  */ { 0.25, 0.20, 0.50, 0.55, 1.00, 0.45, 0.60 },
        /* from BudgetRoster */ { 0.30, 0.20, 0.45, 0.45, 0.50, 1.00, 0.55 },
        /* from TalentFarm   */ { 0.35, 0.25, 0.50, 0.55, 0.60, 0.45, 1.00 },
    };

    int from = idx_of(t.previous_strategy);
    int to   = idx_of(suggested);
    double accept = M[from][to];
    Team::Strategy committed = rng().chance(accept)
                                 ? suggested
                                 : t.previous_strategy;
    t.previous_strategy = committed;
    return committed;
}

// ============================================================================
// === Championship Window classification ====================================
// ============================================================================

const char* team_window_name(TeamWindow w) noexcept {
    switch (w) {
        case TeamWindow::Opening: return "Opening";
        case TeamWindow::Open:    return "Open";
        case TeamWindow::Closing: return "Closing";
        case TeamWindow::Closed:  return "Closed";
    }
    return "?";
}

TeamWindow compute_team_window(const Team& t, int current_year) {
    // Walk the starting 5 (or whatever's there if <5). Compute means.
    const std::size_t starters = std::min<std::size_t>(5, t.roster.size());
    if (starters == 0) return TeamWindow::Opening;

    double sum_age = 0.0, sum_pot = 0.0, sum_ovr = 0.0, sum_yrs = 0.0;
    int n = 0;
    for (std::size_t i = 0; i < starters; ++i) {
        const auto& p = t.roster[i];
        if (!p) continue;
        sum_age += p->age;
        sum_pot += p->potential;
        sum_ovr += p->ovr();
        // Contract years left via the single source-of-truth accessor —
        // matches Player::years_left semantics exactly (and skips FAs/Retired).
        int yrs_left = p->years_left(current_year);
        sum_yrs += yrs_left;
        ++n;
    }
    if (n == 0) return TeamWindow::Opening;
    double avg_age = sum_age / n;
    double avg_pot = sum_pot / n;
    double avg_ovr = sum_ovr / n;
    double avg_yrs = sum_yrs / n;

    // Cascade — first match wins.
    // Opening: young, rising, building.
    if (avg_age < 23.0 && avg_pot >= 75.0 && avg_ovr < 78.0) {
        return TeamWindow::Opening;
    }
    // Open: prime contender with stable contracts.
    if (avg_age >= 22.0 && avg_age <= 27.0 &&
        avg_ovr >= 75.0 && avg_yrs >= 1.5) {
        return TeamWindow::Open;
    }
    // Closing: old + good, OR great-but-expiring.
    if ((avg_age >= 28.0 && avg_ovr >= 75.0) ||
        (avg_age >= 26.0 && avg_ovr >= 78.0 && avg_yrs < 1.5)) {
        return TeamWindow::Closing;
    }
    // Closed: old + declining, OR a low-ceiling team that never broke through.
    if ((avg_age >= 28.0 && avg_ovr < 75.0) ||
        (avg_pot < 65.0 && avg_ovr < 72.0)) {
        return TeamWindow::Closed;
    }
    // Fallback: healthy mid-tier stays Open, otherwise Opening.
    if (avg_ovr >= 70.0) return TeamWindow::Open;
    return TeamWindow::Opening;
}

// ============================================================================
// === Multi-year timeline fit scoring ========================================
// ============================================================================
//
// Pure, O(1), no allocation. Used by run_initial_snake_draft::score_candidate
// (weight ~0.6) and auto_fill::fill_one (weight 0.4) to add multi-year
// coherence to the picker. See Team.h header comment for the component
// breakdown.
double timeline_fit_score(const Team& t, const Player& candidate) {
    const double ovr = candidate.ovr();
    const double pot = static_cast<double>(candidate.potential);
    const double age = static_cast<double>(candidate.age);

    // Youth factor: under-22 prospects get full FutureValue weight; over-25
    // get nothing. Linear ramp, clamped [0, 1.5].
    double youth_factor = (25.0 - age) / 8.0;
    if (youth_factor < 0.0) youth_factor = 0.0;
    if (youth_factor > 1.5) youth_factor = 1.5;

    // Perceived value: cheap proxy for what a "fair" salary would be in
    // gen_contract terms. (0.55 * ovr + 0.45 * pot) * 1.6 keeps it in the
    // same K-dollar magnitude as contract.amount_k.
    double perceived_value = (0.55 * ovr + 0.45 * pot) * 1.6;

    // TimelineFit: piecewise table by (window, age band, potential / ovr band).
    // The heart of "timeline coherence" — same candidate scores very
    // differently for an Opening rebuild vs a Closing contender.
    double timeline_fit = 0.0;
    enum class AgeBand { Young, Prime, Old };
    AgeBand ab = AgeBand::Old;
    if      (age <= 22.0) ab = AgeBand::Young;
    else if (age <= 26.0) ab = AgeBand::Prime;
    else                  ab = AgeBand::Old;

    const TeamWindow w = t.window;

    auto pick = [&](double opening, double open, double closing, double closed) {
        switch (w) {
            case TeamWindow::Opening: return opening;
            case TeamWindow::Open:    return open;
            case TeamWindow::Closing: return closing;
            case TeamWindow::Closed:  return closed;
        }
        return open;
    };

    // Per spec table:
    //                                 Opening | Open | Closing | Closed
    // Young (<=22) + High Pot (>=75)    +25    +5     -10        +20
    // Young + Mid Pot (60-74)           +15     0      -5        +15
    // Prime (23-26) + High OVR (>=78)    +5   +20     +15         -5
    // Prime + Mid OVR (70-77)           +10   +10      +5          0
    // Old (>=27) + High OVR (>=78)      -10   +15     +25        -15
    // Old + Mid OVR (70-77)              -5    +5      +5        -10
    // Old + Low OVR (<70)               -15   -10     -10         -5
    if (ab == AgeBand::Young) {
        if      (pot >= 75.0) timeline_fit = pick(+25, +5,  -10, +20);
        else if (pot >= 60.0) timeline_fit = pick(+15,  0,   -5, +15);
        else                  timeline_fit = pick( -5, -5,   -5,   0);
    } else if (ab == AgeBand::Prime) {
        if      (ovr >= 78.0) timeline_fit = pick( +5, +20, +15,  -5);
        else if (ovr >= 70.0) timeline_fit = pick(+10, +10,  +5,   0);
        else                  timeline_fit = pick( -5,  -5,  -5,  -5);
    } else { // Old
        if      (ovr >= 78.0) timeline_fit = pick(-10, +15, +25, -15);
        else if (ovr >= 70.0) timeline_fit = pick( -5,  +5,  +5, -10);
        else                  timeline_fit = pick(-15, -10, -10,  -5);
    }

    double current_value      = ovr;
    double future_value       = (pot - ovr) * youth_factor;
    double contract_efficiency = perceived_value
                               - static_cast<double>(candidate.contract.amount_k);

    return 1.00 * current_value
         + 0.50 * future_value
         + 0.40 * contract_efficiency
         + 0.80 * timeline_fit;
}

// ============================================================================
// === Chemistry storage ======================================================
// ============================================================================
//
// Canonicalize the pair so (a,b) and (b,a) collide. Stored EMA range is
// [-2.0, +2.0] with alpha = 0.15 (i.e. 85% retained per update). Skips
// self-pairs and null inputs.
void Team::record_chemistry(const Player& a, const Player& b, double delta) {
    Player* pa = const_cast<Player*>(&a);
    Player* pb = const_cast<Player*>(&b);
    if (!pa || !pb || pa == pb) return;
    ChemKey k;
    if (pa < pb) { k.a = pa; k.b = pb; } else { k.a = pb; k.b = pa; }
    auto it = chemistry.find(k);
    double prev = (it == chemistry.end()) ? 0.0 : it->second;
    double next = prev * 0.85 + delta * 0.15;
    if (next >  2.0) next =  2.0;
    if (next < -2.0) next = -2.0;
    chemistry[k] = next;
}

double Team::chemistry_between(const Player& a, const Player& b) const {
    Player* pa = const_cast<Player*>(&a);
    Player* pb = const_cast<Player*>(&b);
    if (!pa || !pb || pa == pb) return 0.0;
    ChemKey k;
    if (pa < pb) { k.a = pa; k.b = pb; } else { k.a = pb; k.b = pa; }
    auto it = chemistry.find(k);
    if (it == chemistry.end()) return 0.0;
    return it->second;
}

std::vector<std::tuple<Player*, Player*, double>>
Team::top_chemistry_pairs(int n) const {
    std::vector<std::tuple<Player*, Player*, double>> out;
    out.reserve(chemistry.size());
    for (const auto& kv : chemistry) {
        out.emplace_back(kv.first.a, kv.first.b, kv.second);
    }
    // Sort descending by value; ties broken by pointer address for
    // determinism.
    std::sort(out.begin(), out.end(),
        [](const std::tuple<Player*, Player*, double>& x,
           const std::tuple<Player*, Player*, double>& y) {
            if (std::get<2>(x) != std::get<2>(y)) {
                return std::get<2>(x) > std::get<2>(y);
            }
            if (std::get<0>(x) != std::get<0>(y)) {
                return std::get<0>(x) < std::get<0>(y);
            }
            return std::get<1>(x) < std::get<1>(y);
        });
    if (n >= 0 && static_cast<int>(out.size()) > n) {
        out.resize(static_cast<std::size_t>(n));
    }
    return out;
}

// ============================================================================
// === Re-sign + market-value helpers ========================================
// ============================================================================
//
// In-place contract extension. Differs from the release+sign round-trip in
// two important ways:
//   1. The player never leaves `roster`, so chemistry edges + IGL/Flex
//      overlays + agent pools survive unchanged.
//   2. The team's budget is debited by the deal value here (the GM no
//      longer needs to bookkeep it separately at the re-sign callsite).
//
// Validation: must be on THIS team, not retired, years >= 1, amount > 0.
// Returns false on any invariant violation without mutating state.
bool Team::resign_player(const PlayerPtr& p, int years, int amount_k,
                         int current_year) {
    // 4-arg overload — preserve existing contract_role (no role change).
    return resign_player(p, years, amount_k, current_year, Role::Count);
}

bool Team::resign_player(const PlayerPtr& p, int years, int amount_k,
                         int current_year, Role new_role) {
    if (!p) return false;
    if (p->is_retired) return false;
    if (years < 1) return false;
    if (amount_k <= 0) return false;

    // Must be a current roster member of THIS team.
    auto it = std::find(roster.begin(), roster.end(), p);
    if (it == roster.end()) return false;

    p->contract.amount_k = amount_k;
    p->contract.exp_year = current_year + years - 1;
    p->contract_years    = years;
    // Role::Count preserves prior contract_role (or seeds it from
    // primary_role if this is the first stamped deal). Any explicit role
    // override writes through.
    if (new_role != Role::Count) {
        p->contract_role = new_role;
    } else if (p->contract_role == Role::Count) {
        p->contract_role = p->primary_role;
    }
    // Clear any pending mood the player carried against THIS team — they
    // signed a new deal, the slate resets. mood_for + bump_mood compose to
    // a no-op if there was no mood, so this is harmless either way.
    p->bump_mood(name, -p->mood_for(name));

    budget -= static_cast<long long>(amount_k) * 1000LL;
    return true;
}

// Cheap market-value estimate. gen_contract with team_ctx folds in our
// desire_salary_mult (Player.cpp) on top of the baseline so the UI shows
// "what they'd ask US specifically", not a context-free MSRP.
int Team::market_value_estimate(const Player& p) const {
    Contract c = p.gen_contract(/*current_year=*/0,
                                /*randomize_amount=*/false,
                                /*randomize_exp=*/false,
                                this);
    return c.amount_k;
}

Team::DynastyTier Team::dynasty_tier(int current_year) const {
    // Rolling 5-year window: [current_year - 4, current_year] inclusive.
    int floor_year = current_year - 4;
    int titles_5yr = 0;
    for (const auto& tp : trophy_history_) {
        if (tp.first >= floor_year && tp.first <= current_year) ++titles_5yr;
    }
    int finals_5yr = 0;
    for (const auto& fp : finals_history_) {
        if (fp.first >= floor_year && fp.first <= current_year) ++finals_5yr;
    }
    // Tier ladder per spec — Dynasty trumps ChampionEra trumps Contender.
    if (titles_5yr >= 3)                            return DynastyTier::Dynasty;
    if (titles_5yr >= 1 && finals_5yr >= 2)         return DynastyTier::ChampionEra;
    if (titles_5yr == 0 && finals_5yr >= 2)         return DynastyTier::Contender;
    return DynastyTier::None;
}

}  // namespace vlr
