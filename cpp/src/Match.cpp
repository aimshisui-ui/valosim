#include "Match.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace vlr {

// === WS-B: the SINGLE bounded match tilt =================================
// Collapses ALL of WS-B's match influence (team identity, coach craft, region
// style, international style-clash) into ONE per-side power scalar, hard-clamped
// to [0.95, 1.05]. Exposed as a free function so the smoke test asserts the
// exact code the engine runs (no formula drift) and the double-count guard is
// verifiable. Returns exactly 1.0 for soloq / friendlies.
//
// region is NOT added as a standalone term: compute_team_identity() already
// folds region_meta into identity.aggression, so the identity term carries it
// (no double-count). The intl clash term is the ONLY place region appears
// directly, and only when two DIFFERENT regions meet at a Masters/Champions.
double wsb_match_tilt(const Team& tm, const Team& opp,
                      bool intl, bool soloq_or_friendly) noexcept {
    if (soloq_or_friendly) return 1.0;
    double term = 0.0;
    // Full-strength tilt: outer clamp [0.95,1.05], sub-terms below. Verified at
    // the dynasty gate (with the dev pass at single-roll): back-to-back champ
    // ~10% (very rare dynasties), parity 0.60, and the KD ceiling UNTOUCHED at
    // 1.37 — the duel dom_cap binds AFTER this tilt, so identity/coaching are
    // felt in match outcomes without re-opening the K/D balance.
    // (a) identity term — 0.5 neutral; pre-clamped to [-0.02, +0.02].
    term += clamp_v((tm.identity.aggression - 0.5) * 0.04, -0.02, 0.02);
    // (b) coach term — from the SAME synergy mult that feeds coordination,
    //     scaled into a tiny additive nudge. Pre-clamped to [0, +0.015].
    double syn = tm.head_coach ? tm.head_coach->match_synergy_mult() : 1.0;
    term += clamp_v((syn - 1.0) * 0.06, 0.0, 0.015);
    // (c) international style-clash — pre-clamped to [-0.015, +0.015].
    if (intl && region_id_from_name(tm.region) != region_id_from_name(opp.region)) {
        double da = region_meta(tm.region).aggression
                  - region_meta(opp.region).aggression;
        term += clamp_v(da * 0.02, -0.015, 0.015);
    }
    return clamp_v(1.0 + term, 0.95, 1.05);
}

double prep_match_tilt(const Team& tm, const GameMap& map, double analyst_q01) noexcept {
    auto it = tm.map_prep.find(map.name);
    if (it == tm.map_prep.end() || it->second.level <= 0) return 1.0;   // unprepped -> neutral
    int lvl = it->second.level;   // 1 = Standard, 2 = Heavy
    // PURE-UPSIDE bounded edge: 0.015 per level, scaled by analyst quality with a
    // 0.5 floor (even a weak analyst's prep helps a little). L1@q0 ~+0.75%, L1@q1
    // +1.5%, L2@q1 +3%. Hard-clamped [1.0, 1.03] — can never penalise or exceed
    // +3%, and the duel dom_cap binds after this, so the KD/dynasty bands hold.
    double q = analyst_q01 < 0.0 ? 0.0 : (analyst_q01 > 1.0 ? 1.0 : analyst_q01);
    double edge = 0.015 * lvl * (0.5 + 0.5 * q);
    return clamp_v(1.0 + edge, 1.0, 1.03);
}

// Collapse a match's event name + year into a stable tournament-identity key
// for per-tournament stat scoping (item 1). The domestic LEAGUE phase
// ("VCT <region> Stage N") and the regional PLAYOFF ("<region> Regionals N")
// of the same split collapse to ONE "<region> Split N|<year>" bucket — they
// are, per design, the SAME tournament. Each international (Masters 1/2,
// Champions) is its own bucket. Anything unrecognized falls back to
// "<event>|<year>" so per-tournament sums always reconcile with the season_*
// totals (every write lands in exactly one bucket). Format: "<label>|<year>".
static std::string tourn_identity_key(const std::string& event, int year) {
    const std::string ys = std::to_string(year);
    auto first_digit = [](const std::string& e) -> int {
        for (char c : e) if (c >= '0' && c <= '9') return c - '0';
        return 0;
    };
    if (event.find("CHAMPIONS") != std::string::npos) return "Champions|" + ys;
    if (event.find("MASTERS") != std::string::npos) {
        int n = first_digit(event);
        return (n ? "Masters " + std::to_string(n) : std::string("Masters")) + "|" + ys;
    }
    // Domestic split: the STAGE league phase + the REGIONALS playoff of the same
    // split must collapse to ONE "<region> Split N" bucket. CAVEAT: the STAGE
    // label carries the PHASE-position number (1, 4, 7 for the three stages,
    // because phase_idx counts every phase), whereas the REGIONALS label carries
    // the split index directly (1, 2, 3). Map the stage number back to its split
    // so e.g. "VCT Americas Stage 4" and "Americas Regionals 2" both key to
    // "Americas Split 2".
    std::string region;
    for (const char* r : {"Americas", "EMEA", "Pacific", "China"})
        if (event.find(r) != std::string::npos) { region = r; break; }
    int split = 0;
    if (event.find("Stage") != std::string::npos) {
        int n = first_digit(event);          // phase-position number (1/4/7)
        if (n > 0) split = (n + 2) / 3;       // -> split index (1/2/3)
    } else if (event.find("Regionals") != std::string::npos) {
        split = first_digit(event);           // already the split index
    }
    if (!region.empty() && split > 0)
        return region + " Split " + std::to_string(split) + "|" + ys;
    return event + "|" + ys;
}

// === IGL strategic impact (Pillar 1) ===
// Returns a 0..2.5 score describing how well this IGL performed strategically
// in this match, independent of their personal kill stats. Inputs:
//   * The IGL's calling-flavor attributes (mid-round / anti-strat / intel /
//     adaptability / leadership / communication) — strategic baseline.
//   * Whether the team won, and by how much they out- or under-performed
//     their roster OVR vs the opponent — out-performing a stronger roster
//     is the clearest signal of good calling.
//   * A close-win comeback proxy + a pressure premium for finals/Masters/
//     Champions stages.
//
// Bounded to [0, 2.5] so a single elite Grand Final can't dominate a career.
// Lives here (not in Player.cpp) so we have direct access to team1_/team2_,
// t1_ovr_/t2_ovr_, event_name_, and the per-team rosters needed for the
// outcome scaling. ~1.0 is "average" IGL outing.
static double compute_igl_impact(Player* p,
                                 bool   is_team1,
                                 bool   team_won,
                                 double team_strength,
                                 double opp_strength,
                                 int    team1_score,
                                 int    team2_score,
                                 const std::string& event_name) {
    if (!p) return 0.0;
    (void)is_team1;  // routed via team_strength/opp_strength

    // --- Strategic competence: weighted average of calling attributes ---
    // Decoupled from raw aim/headshot/entry so a 50-Aim IGL can still rate
    // highly here. Reads the IGL-flavor attribute set the stat-shift inflates.
    double midround  = at(p->attributes, Attr::MidRoundCalling) / 99.0;
    double antistrat = at(p->attributes, Attr::AntiStrat)       / 99.0;
    double intel     = at(p->attributes, Attr::Intelligence)    / 99.0;
    double adapt     = at(p->attributes, Attr::Adaptability)    / 99.0;
    double lead      = at(p->attributes, Attr::Leadership)      / 99.0;
    double comm      = at(p->attributes, Attr::Communication)   / 99.0;
    double strategic = 0.30 * midround + 0.20 * antistrat + 0.15 * intel
                     + 0.15 * adapt    + 0.10 * lead      + 0.10 * comm;

    // --- Outcome scaling: did the team OUT-PERFORM their roster OVR? ---
    // Underdog win => big bonus (smart calling outsmarted stronger roster).
    // Favourite loss => slight penalty (got out-called).
    // upset_delta ~ -1..+1 from a 30-OVR-point gap; we clamp anyway.
    double upset_delta = (opp_strength - team_strength) / 30.0;
    if (upset_delta < -1.0) upset_delta = -1.0;
    if (upset_delta >  1.0) upset_delta =  1.0;
    double outcome_factor;
    if (team_won) {
        outcome_factor = 1.0 + 0.5 * upset_delta;   // base 1.0; +0.5 for upset, -0.5 for blowout
    } else {
        outcome_factor = 0.5 * upset_delta;         // base 0.0; +0.5 if lost as huge underdog
    }
    outcome_factor = clamp_v(outcome_factor, 0.0, 2.0);

    // --- Comeback creation: close, decisive win in a regulation map ---
    // We don't have first-half score tracked, so we approximate "comeback"
    // as a tight 13-11 / 13-10 / 14-12 / 15-13 result. Decisive blowouts
    // (13-3, 13-5) don't earn this bonus — it's a "kept the team in it" prize.
    double comeback_bonus = 0.0;
    int round_count = team1_score + team2_score;
    if (team_won && round_count >= 13) {
        int diff = team1_score - team2_score;
        if (diff < 0) diff = -diff;
        if (diff <= 3 && (team1_score == 13 || team2_score == 13 ||
                          team1_score >= 14 || team2_score >= 14)) {
            comeback_bonus = 0.20;
        }
    }

    // --- Pressure premium: bigger matches weigh more ---
    double pressure = 1.0;
    if      (event_name.find("Grand Final") != std::string::npos)        pressure = 1.40;
    else if (event_name.find("Final")       != std::string::npos)        pressure = 1.20;
    else if (event_name.find("CHAMPIONS")   != std::string::npos ||
             event_name.find("Champions")   != std::string::npos)        pressure = 1.18;
    else if (event_name.find("MASTERS")     != std::string::npos ||
             event_name.find("Masters")     != std::string::npos)        pressure = 1.15;
    else if (event_name.find("Regionals")   != std::string::npos ||
             event_name.find("Playoffs")    != std::string::npos)        pressure = 1.08;

    double impact = (strategic * 0.7 + outcome_factor * 0.3 + comeback_bonus) * pressure;
    return clamp_v(impact, 0.0, 2.5);
}

// === Defender-flag helper (B11) ====================================
// Returns whether team1 is on the "defender" side for the given phase,
// derived from current map control. Two distinct blocks in the inner
// duel loop (SiteAction execute bonus + PostPlant retaker boost +
// PostPlant defender flag in the phase-bonus path) all need the same
// answer; centralising the decision keeps them consistent if the
// underlying control semantics ever change.
//
// pp_planted_side_is_defender:
//   true  → return true if team1 is on the planter side (PostPlant logic:
//           plant-side defender = high-control side)
//   false → return true if team1 is on the attacking/low-control side
//           (SiteAction "attacker low control" + retake decision logic)
static inline bool is_team1_defender(double t1_map_control,
                                     double t2_map_control,
                                     bool pp_planted_side_is_defender) {
    if (pp_planted_side_is_defender) {
        return t1_map_control >= t2_map_control;
    }
    return t1_map_control < t2_map_control;
}

// === Top-end compression (Task 1) ========================================
// Variance/inflation fix: raw base mechanical power was ~linear in attrs,
// so a 95-Aim star was ~30% stronger than an 82-Aim pro in every duel and
// elite teams steamrolled. We apply gentle diminishing returns to the
// summed base power BEFORE the layered multipliers: ordering is preserved
// (monotonic, strictly increasing) but the top end is squeezed so
// elite-vs-elite duels are close and good teams can trade maps. Mid/low
// power is left almost untouched; only the high tail compresses.
//
// Pivot ~ the base power produced by a ~70-attr line; above the pivot the
// marginal value of each extra point of raw power is scaled by ~0.62.
// This does NOT touch the rating formula or any §7 constant — it reshapes
// the duel-power INPUT only, exactly as the task requires.
static double compress_power(double raw) {
    constexpr double kPivot = 210.0;   // ~ (70*1.2 + 70*0.8 + 70*0.5 + 70*0.6)
    constexpr double kSlope = 0.62;    // marginal retention above the pivot
    if (raw <= kPivot) return raw;
    return kPivot + (raw - kPivot) * kSlope;
}

Match::Match(TeamPtr t1, TeamPtr t2, GameMap m, bool solo, std::string ev, bool friendly,
             const std::vector<MapResultEntry>* prior_results)
    : team1_(std::move(t1)), team2_(std::move(t2)), map_(std::move(m)),
      is_solo_q_(solo), friendly_(friendly), event_name_(std::move(ev)) {
    // Per-map agent selection (Pillar 3). Threading `prior_results`
    // through to the team selection lets us bias away from comps that
    // already lost a map earlier in this same series — fixes the
    // "every map plays the same 5 agents" bug.
    auto r1 = team1_->build_round_selection(map_, /*is_team1=*/true,  prior_results, is_solo_q_);
    auto r2 = team2_->build_round_selection(map_, /*is_team1=*/false, prior_results, is_solo_q_);
    t1_ovr_ = r1.team_ovr;
    t2_ovr_ = r2.team_ovr;
    chosen_agents_.insert(r1.chosen_agents.begin(), r1.chosen_agents.end());
    chosen_agents_.insert(r2.chosen_agents.begin(), r2.chosen_agents.end());

    auto& roster1 = team1_->roster;
    auto& roster2 = team2_->roster;
    std::size_t n1 = std::min<std::size_t>(5, roster1.size());
    std::size_t n2 = std::min<std::size_t>(5, roster2.size());
    for (std::size_t i = 0; i < n1; ++i) match_stats_.emplace(roster1[i].get(), PlayerMatchStats{});
    for (std::size_t i = 0; i < n2; ++i) match_stats_.emplace(roster2[i].get(), PlayerMatchStats{});
}

void Match::play() {
    auto& r1 = team1_->roster;
    auto& r2 = team2_->roster;
    if (r1.size() < 5 || r2.size() < 5) {
        // Forfeit: a team that cannot field 5 players loses the map by
        // default (13-0) instead of leaving the score 0-0. A 0-0 final used
        // to be silently mis-resolved by Series::add_match_data's bare else
        // (always crediting team2). Awarding a decisive forfeit to whichever
        // side CAN field 5 is both correct and guarantees the series still
        // terminates. If both are short, team1 takes it by convention so the
        // driver loop can never hang.
        if (r1.size() >= 5)      team1_score_ = 13;
        else if (r2.size() >= 5) team2_score_ = 13;
        else                     team1_score_ = 13;
        return;
    }

    std::vector<Player*> t1_rstr, t2_rstr;
    t1_rstr.reserve(5); t2_rstr.reserve(5);
    for (std::size_t i = 0; i < 5; ++i) t1_rstr.push_back(r1[i].get());
    for (std::size_t i = 0; i < 5; ++i) t2_rstr.push_back(r2[i].get());

    // === Archetype profile cache (per player, per map) ===================
    // archetype_profile() is a pure O(1) table lookup, but it is called in
    // the hot duel loop for both duelists every kill — cache it once per
    // map so the loop just does an unordered_map probe. These modest
    // multipliers/biases LAYER on the attribute math; they never replace
    // it and never let a player exceed the global per-duel cap.
    std::unordered_map<Player*, ArchetypeProfile> ap_cache;
    {
        auto cache_team = [&](const std::vector<Player*>& tm) {
            for (auto* p : tm) ap_cache.emplace(p, archetype_profile(p->archetype));
        };
        cache_team(t1_rstr);
        cache_team(t2_rstr);
    }
    auto ap_of = [&](Player* p) -> const ArchetypeProfile& {
        auto it = ap_cache.find(p);
        return it->second;  // every starter is seeded above
    };

    // === Per-map "off day" effective-level roll =========================
    // Variance fix (Task 1): even a 95-consistency star must occasionally
    // have a genuinely bad MAP (not just a bad round). One asymmetric draw
    // per player per map. Most maps sit ~1.0; a small tail produces a
    // depressed whole-map level (bigger downside than upside) so elite
    // players get real 0.7-0.9 stinkers sprinkled through their 1.1-1.35
    // norm. Consistency + ConsistencyMachine-type archetypes shrink — but
    // never fully remove — the off-day chance & depth. archetype
    // consistency_floor lifts the bad-map floor; ceiling_boost lifts the
    // rare hot-map ceiling.
    std::unordered_map<Player*, double> off_day_mult;
    {
        auto roll_off_day = [&](const std::vector<Player*>& tm) {
            for (auto* p : tm) {
                const ArchetypeProfile& ap = ap_of(p);
                double cons = p->consistency / 99.0;              // 0..1
                // Base bad-map chance ~18% at cons 0, ~6% at cons ~0.9.
                // consistency_mod (+ = steadier) shaves it; floor never
                // below 3.5% so even ConsistencyMachine has off maps.
                double bad_chance = 0.18 - 0.13 * cons - 0.20 * ap.consistency_mod;
                bad_chance = clamp_v(bad_chance, 0.035, 0.22);
                // Rare hot-map chance — smaller, lifted by ceiling_boost.
                double hot_chance = 0.10 + 0.5 * ap.ceiling_boost;
                hot_chance = clamp_v(hot_chance, 0.05, 0.20);
                double m = 1.0;
                if (rng().chance(bad_chance)) {
                    // Depressed whole map: -8% .. -22% effective level,
                    // shallower for high-consistency / floor archetypes.
                    double depth = rng().drange(0.08, 0.22)
                                 * (1.0 - 0.30 * cons);
                    depth = std::max(0.0, depth - ap.consistency_floor);
                    m = 1.0 - depth;
                } else if (rng().chance(hot_chance)) {
                    // Hot map: smaller upside than the downside tail.
                    double up = rng().drange(0.03, 0.10)
                              + 0.5 * ap.ceiling_boost;
                    m = 1.0 + std::min(0.16, up);
                }
                off_day_mult[p] = m;
            }
        };
        roll_off_day(t1_rstr);
        roll_off_day(t2_rstr);
    }

    // === Per-map matchup variance swing =================================
    // Variance fix (Task 1): a slightly-worse team should win ~25-40% of
    // maps vs a clearly-better team, not ~5%. One symmetric per-map team
    // power tilt (coordination/comp/map-feel "this map just clicked /
    // didn't click for us") layered on every duel for that team. Std ~6%,
    // clamped ±14%. Independent per team so the gap can shrink OR widen
    // on a given map — that is what produces believable upset maps while
    // the stronger team still wins the series on average.
    // Team-level economy discipline (Task 2): average economy_discipline
    // over the starting 5. + = the team saves more sensibly on marginal
    // banks; - = a force-happy roster gambles into half-buys more often.
    // Small influence on the force/save threshold below.
    auto team_eco_disc = [&](const std::vector<Player*>& tm) {
        double s = 0.0;
        for (auto* p : tm) s += ap_of(p).economy_discipline;
        return tm.empty() ? 0.0 : s / tm.size();
    };
    double t1_eco_disc = team_eco_disc(t1_rstr);
    double t2_eco_disc = team_eco_disc(t2_rstr);

    // IGL "calling" attributes — previously DEAD (defined but never read in a
    // match). Read once per match from each team's shot-caller (falls back to
    // 50/neutral if a team somehow has no IGL): EconomyMgmt gates buy
    // discipline, AntiStrat tilts pre-round map control, MidRoundCalling swings
    // mid-round duels. These turn four flagship attributes into real levers.
    auto team_igl_attr = [&](const std::vector<Player*>& tm, Attr a) -> int {
        for (auto* p : tm) if (p && p->is_igl) return at(p->attributes, a);
        return 50;
    };
    int t1_igl_em  = team_igl_attr(t1_rstr, Attr::EconomyMgmt);
    int t2_igl_em  = team_igl_attr(t2_rstr, Attr::EconomyMgmt);
    int t1_igl_as  = team_igl_attr(t1_rstr, Attr::AntiStrat);
    int t2_igl_as  = team_igl_attr(t2_rstr, Attr::AntiStrat);
    int t1_igl_mrc = team_igl_attr(t1_rstr, Attr::MidRoundCalling);
    int t2_igl_mrc = team_igl_attr(t2_rstr, Attr::MidRoundCalling);

    auto map_swing = [&]() {
        // Approx-normal via average of two uniforms, scaled.
        double u = (rng().drange(-1.0, 1.0) + rng().drange(-1.0, 1.0)) * 0.5;
        return clamp_v(1.0 + u * 0.14, 0.86, 1.14);
    };
    double t1_map_swing = map_swing();
    double t2_map_swing = map_swing();

    // === Two-way decaying momentum ======================================
    // Variance fix (Task 1): the old loss-streak economy was the only
    // momentum channel and it only ever punished the losing team
    // (snowball). This tracks a bounded momentum value per team that
    // rises on a round win, falls on a loss, and DECAYS toward 0 every
    // round so leads don't compound forever. A team down 0-8 can storm
    // back. Applied as a small per-duel multiplier; archetype
    // momentum_sensitivity scales how hard a given player rides it.
    double t1_momentum = 0.0, t2_momentum = 0.0;

    int round_num = 1;
    double t1_map_control = 50.0, t2_map_control = 50.0;

    auto sum_attr = [](const std::vector<Player*>& tm, Attr a, Attr b) {
        double s = 0.0;
        for (auto* p : tm) s += at(p->attributes, a) + at(p->attributes, b);
        return s;
    };
    auto max_attr = [](const std::vector<Player*>& tm, Attr a) {
        int m = 0;
        for (auto* p : tm) m = std::max(m, at(p->attributes, a));
        return m;
    };

    double t1_coordination = sum_attr(t1_rstr, Attr::GameSense, Attr::Intelligence) / 10.0;
    double t2_coordination = sum_attr(t2_rstr, Attr::GameSense, Attr::Intelligence) / 10.0;

    // IGL (highest leadership) effect on coordination.
    t1_coordination += max_attr(t1_rstr, Attr::Leadership) * 0.10;
    t2_coordination += max_attr(t2_rstr, Attr::Leadership) * 0.10;

    // Coach synergy.
    double t1_coach_synergy = team1_->head_coach ? team1_->head_coach->match_synergy_mult() : 1.0;
    double t2_coach_synergy = team2_->head_coach ? team2_->head_coach->match_synergy_mult() : 1.0;
    t1_coordination *= t1_coach_synergy;
    t2_coordination *= t2_coach_synergy;

    // === Comp synergy ====================================================
    // Wider spread than the previous (1.0 / 1.048) layout: bad comps now
    // genuinely underperform (0.93x baseline) while a perfectly satisfied
    // target_comp gets a meaningful 1.06x. The previous +0.012/match never
    // showed up over a 13-round match — pumping the spread ~50% per pillar
    // 3 is the whole point. Map favored_role + coach personality threading
    // happens via team_macro_bonus() below.
    auto comp_synergy = [](const TeamPtr& t,
                           const std::unordered_map<Player*, const Agent*>& chosen) {
        std::array<int, static_cast<std::size_t>(Role::Count)> have{};
        int considered = 0;
        for (auto& kv : chosen) {
            if (!kv.second) continue;
            // Only count chosen agents that belong to THIS team's roster.
            bool on_team = false;
            for (auto& rp : t->roster) {
                if (rp.get() == kv.first) { on_team = true; break; }
            }
            if (!on_team) continue;
            have[static_cast<std::size_t>(kv.second->role)]++;
            ++considered;
        }
        if (considered == 0) return 1.0;
        int matches = 0;
        for (std::size_t i = 0; i < have.size(); ++i)
            if (have[i] == t->target_comp.need[i]) ++matches;
        // 4 role buckets; matches in [0..4]. Variance fix (Task 1): widen
        // the comp swing so a meta-mismatched / wrong-comp team genuinely
        // underperforms and a clean comp is rewarded — this is one of the
        // levers that lets a slightly-worse team take a map. Was
        // 0.93..1.06; now 0.90..1.10.
        constexpr double kBase   = 0.90;
        constexpr double kPerHit = 0.05;     // 0.90 + 4*0.05 = 1.10
        return kBase + kPerHit * matches;
    };

    // === Map favored-role bonus + coach macro bonus =====================
    // Pillar 3 extras. Implemented as a sibling of comp_synergy and
    // multiplied into the team's comp_bonus so the magnitudes compose
    // cleanly (cap on the multiplier is set below via clamp).
    auto team_macro_bonus = [](const TeamPtr& t, const GameMap& m,
                               const std::unordered_map<Player*, const Agent*>& chosen) {
        double bonus = 0.0;
        // Map favored-role: each starter playing an agent whose role
        // matches the map's favored_role contributes +0.005 (max +0.025
        // with a perfect 5-stack on the map's favored role).
        int favored_count = 0;
        for (auto& rp : t->roster) {
            if (!rp) continue;
            auto it = chosen.find(rp.get());
            if (it == chosen.end() || !it->second) continue;
            if (it->second->role == m.favored_role) ++favored_count;
        }
        bonus += 0.005 * favored_count;

        // Coach personality bonus — only the four "tactically inclined"
        // archetypes give a small comp boost. Everyone else is neutral so
        // we don't double-dip with the existing match_synergy_mult.
        if (t->head_coach) {
            switch (t->head_coach->personality) {
                case CoachPersonality::TacticalGenius:  bonus += 0.020; break;
                case CoachPersonality::AnalyticsHeavy:  bonus += 0.015; break;
                case CoachPersonality::StructuredMacro: bonus += 0.015; break;
                case CoachPersonality::Innovator:       bonus += 0.010; break;
                case CoachPersonality::Pragmatist:      bonus += 0.010; break;
                default: break;
            }
        }
        // Cap macro bonus so it can't snowball comp_synergy past sanity.
        return clamp_v(bonus, 0.0, 0.045);
    };

    double t1_comp_bonus = comp_synergy(team1_, chosen_agents_)
                         + team_macro_bonus(team1_, map_, chosen_agents_);
    double t2_comp_bonus = comp_synergy(team2_, chosen_agents_)
                         + team_macro_bonus(team2_, map_, chosen_agents_);
    // Clamp into safe range. With perfect comp + map favored + tactical
    // genius coach: 1.06 + 0.025 + 0.020 = 1.105. Floor at 0.92 so no
    // matchup tanks beyond ~8% from comp alone.
    t1_comp_bonus = clamp_v(t1_comp_bonus, 0.89, 1.14);
    t2_comp_bonus = clamp_v(t2_comp_bonus, 0.89, 1.14);

    // === WS-B INC-4: single bounded match tilt ===========================
    // ALL of WS-B's match influence collapses into ONE per-side power scalar via
    // the free function wsb_match_tilt() (defined below / declared in Match.h so
    // the smoke test asserts the exact code path). Hard-clamped to [0.95,1.05]
    // and applied BEFORE the pwin/dom_cap clamp (~L1711), so the duel-dominance
    // ceiling still binds and the tuned KD/dynasty balance can never be re-opened
    // by identity/coaching. Exactly 1.0 in soloq + friendlies.
    const bool wsb_intl =
        (event_name_.find("MASTERS")   != std::string::npos ||
         event_name_.find("CHAMPIONS") != std::string::npos);
    const bool wsb_off = (is_solo_q_ || friendly_);
    double t1_wsb_tilt = wsb_match_tilt(*team1_, *team2_, wsb_intl, wsb_off);
    double t2_wsb_tilt = wsb_match_tilt(*team2_, *team1_, wsb_intl, wsb_off);
    // Increment E per-map PREP tilt — applies ONLY to the USER's side (set via
    // set_prep_context; null in every AI-vs-AI / dynasty match -> strict 1.0).
    double t1_prep_tilt = (prep_user_ && prep_user_ == team1_.get())
        ? prep_match_tilt(*team1_, map_, prep_analyst_q01_) : 1.0;
    double t2_prep_tilt = (prep_user_ && prep_user_ == team2_.get())
        ? prep_match_tilt(*team2_, map_, prep_analyst_q01_) : 1.0;

    // === Per-agent identity (Pillar 2) ===================================
    // For each player's chosen agent, look at the player's values for the
    // agent's three mapped attrs. If the player is "comfortable" on the
    // agent (mean > 60), we hand out a small power multiplier delta;
    // capped tight (-0.04 .. +0.10) so no agent forces unnatural carries.
    auto agent_synergy_bonus = [](Player* p, const Agent* ag) {
        if (!p || !ag) return 0.0;
        int v1 = at(p->attributes, ag->a1);
        int v2 = at(p->attributes, ag->a2);
        int v3 = at(p->attributes, ag->a3);
        double mean = (v1 + v2 + v3) / 3.0;
        // 60 -> 0, 80 -> +0.056, 95 -> +0.098, 50 -> -0.028.
        double bonus = (mean - 60.0) * 0.0028;
        return clamp_v(bonus, -0.04, 0.10);
    };

    // Team-wide info bonus: small coordination bump if any of the
    // big-info agents (Cypher, Sova, Fade) is on the team. Bounded so
    // it can't snowball — used by the duel calc below.
    //
    // Import variant: when the active info-agent is an unintegrated import
    // (adaptation_months_remaining > 0), a fraction of the +0.03 is shaved
    // off to reflect language/comms friction. Newly arrived imports lose up
    // to 50% of the bonus; fully integrated ones lose 0%. Cap by design: the
    // raw info bonus never goes below 0 and never above 0.03.
    auto team_info_bonus = [](const std::vector<Player*>& tm,
                              const std::unordered_map<Player*, const Agent*>& chosen) {
        Player* info_player = nullptr;
        for (auto* p : tm) {
            auto it = chosen.find(p);
            if (it == chosen.end() || !it->second) continue;
            const std::string& nm = it->second->name;
            if (nm == "Cypher" || nm == "Sova" || nm == "Fade") {
                info_player = p; break;
            }
        }
        if (!info_player) return 0.0;
        double base = 0.03;
        if (info_player->adaptation_months_remaining > 0) {
            // Communication friction: 24 months -> -50% of bonus,
            // 12 months -> -25%, 0 months -> 0%.
            double frac = std::min(1.0, info_player->adaptation_months_remaining / 24.0);
            base *= (1.0 - 0.50 * frac);
        }
        return base;
    };

    // === Import chemistry penalty ===
    // Each unintegrated import (adaptation_months_remaining > 0) shaves a
    // small chunk off team coordination — they don't yet share callouts /
    // habits with the rest of the roster. Scales with how recently they
    // joined, decays with the year-end save_history_and_progress() tick.
    //
    //   24 months left (just signed):  -0.025 per import
    //   12 months left (1 yr in):      -0.012 per import
    //    1- 6 months left:             -0.005 per import
    //    0 months  (fully integrated): no penalty
    //
    // Hard cap: total -0.05 multiplier on coordination (max 2 starting imports
    // per design). The coordination block applies (1 + team_info_bonus -
    // import_chemistry_penalty); see below.
    auto import_chemistry_penalty = [](const std::vector<Player*>& tm) {
        double penalty = 0.0;
        for (auto* p : tm) {
            if (!p) continue;
            int m = p->adaptation_months_remaining;
            if      (m >= 18) penalty += 0.025;
            else if (m >= 7)  penalty += 0.012;
            else if (m >= 1)  penalty += 0.005;
            // m == 0 : no contribution
        }
        // Hard cap at -0.05 even if both imports are freshly arrived.
        return std::min(0.05, penalty);
    };

    const auto& cfg = config();

    // === Round phase taxonomy ============================================
    // Each round flows through several distinct phases at the highest
    // level of pro Valorant. Different attributes matter in each phase,
    // so the duel system needs to know which phase it's currently in.
    //
    //   Opening    — first engagement of the round; duelists peek first.
    //   MidRound   — info-gathering, util usage, repositioning.
    //   SiteAction — one team has committed to a take/hold; util-heavy.
    //   PostPlant  — spike is down; defenders try to retake.
    //   Late       — 2 or fewer alive total; clutch territory.
    //
    enum class RoundPhase { Opening, MidRound, SiteAction, PostPlant, Late };

    auto compute_phase = [&](bool round_started, bool plant,
                             int t1_alive_n, int t2_alive_n) {
        if (!round_started)            return RoundPhase::Opening;
        if (plant)                     return RoundPhase::PostPlant;
        int alive = t1_alive_n + t2_alive_n;
        if (alive <= 3)                return RoundPhase::Late;
        if (t1_map_control >= 70.0 || t2_map_control >= 70.0)
                                       return RoundPhase::SiteAction;
        return RoundPhase::MidRound;
    };

    // Role weights for who takes the OPENING duel of each round. Mirrors
    // pro-play first-blood distribution: duelists do most of the entry work.
    auto role_first_pick_weight = [&](const Agent* a) {
        if (!a) return 30;
        switch (a->role) {
            case Role::Duelist:    return 100;
            case Role::Initiator:  return 35;
            case Role::Controller: return 18;
            case Role::Sentinel:   return 10;
            default:               return 25;
        }
    };

    // Per-phase peeker weight: who's MOST LIKELY to start the next duel
    // given the current phase. Different attributes drive different
    // archetypes — e.g. lurkers shine mid-round, anchors shine in late.
    auto phase_peek_weight = [&](Player* p, const Agent* ag, RoundPhase ph) {
        const auto& a = p->attributes;
        Role role = ag ? ag->role : Role::Duelist;
        double w = 0.0;
        switch (ph) {
            case RoundPhase::Opening:
                w = at(a, Attr::Entry) * 1.6
                  + at(a, Attr::Aggressiveness) * 1.0
                  + at(a, Attr::Movement) * 0.7
                  + at(a, Attr::Reaction) * 0.5;
                if (role == Role::Duelist)         w *= 1.6;
                else if (role == Role::Initiator)  w *= 0.7;
                else if (role == Role::Sentinel)   w *= 0.35;
                break;
            case RoundPhase::MidRound:
                w = at(a, Attr::Lurking) * 1.4
                  + at(a, Attr::GameSense) * 0.9
                  + at(a, Attr::Aggressiveness) * 0.7
                  + at(a, Attr::Adaptability) * 0.5;
                break;
            case RoundPhase::SiteAction:
                w = at(a, Attr::Entry) * 1.3
                  + at(a, Attr::Utility) * 1.0
                  + at(a, Attr::Aggressiveness) * 0.8
                  + at(a, Attr::SpikeHandle) * 0.5;
                if (role == Role::Duelist || role == Role::Initiator) w *= 1.4;
                break;
            case RoundPhase::PostPlant:
                w = at(a, Attr::Entry) * 1.0
                  + at(a, Attr::Utility) * 1.0
                  + at(a, Attr::Awping) * 0.8
                  + at(a, Attr::DecisionMaking) * 0.6;
                break;
            case RoundPhase::Late:
                w = at(a, Attr::Clutch) * 1.6
                  + at(a, Attr::Anchor) * 1.1
                  + at(a, Attr::Aggressiveness) * 0.4
                  + at(a, Attr::DecisionMaking) * 0.7;
                break;
        }
        return std::max(1.0, w);
    };

    // Per-phase duel power. The base `power` (aim/HS/entry/decision) still
    // matters, but phase shifts the emphasis: in Opening, mechanics +
    // movement; in MidRound, lurking + intel; in SiteAction, util + entry;
    // in PostPlant, anchor/clutch (defender) vs entry/util (retaker); in
    // Late, clutch + anchor + decision-making dominate. Defender bonus is
    // applied to whichever player is on the holding side of the action.
    auto phase_power_bonus = [&](Player* p, RoundPhase ph, bool defender) {
        const auto& a = p->attributes;
        switch (ph) {
            case RoundPhase::Opening:
                return at(a, Attr::Entry) * 0.30
                     + at(a, Attr::Movement) * 0.40
                     + at(a, Attr::CrosshairPlacement) * 0.35
                     + at(a, Attr::Reaction) * 0.25
                     + at(a, Attr::Aggressiveness) * 0.15;
            case RoundPhase::MidRound:
                return at(a, Attr::Lurking) * 0.40
                     + at(a, Attr::GameSense) * 0.30
                     + at(a, Attr::Movement) * 0.25
                     + at(a, Attr::Intelligence) * 0.15
                     + at(a, Attr::Communication) * 0.15;
            case RoundPhase::SiteAction:
                if (defender) {
                    return at(a, Attr::Anchor) * 0.50
                         + at(a, Attr::Positioning) * 0.40
                         + at(a, Attr::Clutch) * 0.20
                         + at(a, Attr::Communication) * 0.15;
                }
                return at(a, Attr::Utility) * 0.40
                     + at(a, Attr::Entry) * 0.35
                     + at(a, Attr::SpikeHandle) * 0.25
                     + at(a, Attr::Aggressiveness) * 0.15;
            case RoundPhase::PostPlant:
                if (defender) {
                    return at(a, Attr::Anchor) * 0.55
                         + at(a, Attr::Clutch) * 0.40
                         + at(a, Attr::Positioning) * 0.30
                         + at(a, Attr::Stamina) * 0.20;
                }
                return at(a, Attr::Entry) * 0.35
                     + at(a, Attr::Utility) * 0.35
                     + at(a, Attr::Awping) * 0.30
                     + at(a, Attr::DecisionMaking) * 0.20;
            case RoundPhase::Late:
                return at(a, Attr::Clutch) * 0.65
                     + at(a, Attr::Anchor) * 0.45
                     + at(a, Attr::DecisionMaking) * 0.25
                     + at(a, Attr::Adaptability) * 0.20
                     + at(a, Attr::Headshot) * 0.20;
        }
        return 0.0;
    };

    // === Pillar 2: per-agent identity flavour bonus =====================
    // Light-touch role/agent specific deltas layered on top of the baseline
    // phase bonuses. Each branch is intentionally modest. Total contribution
    // is hard-capped at +0.20 (per the user's "don't force unnatural
    // performances" constraint). Implemented as a multiplier delta — caller
    // does `pwr *= (1.0 + agent_identity_bonus(...))`.
    //
    //   ag             : the player's chosen agent (may be null -> 0)
    //   p              : the player
    //   phase          : current RoundPhase
    //   defender       : true if player is on defending side this duel
    //   alive_self     : count alive on player's team
    //   alive_opp      : count alive on opp team
    //   round_kills_p  : kills the player has THIS round (n_round_kills)
    auto agent_identity_bonus = [](Player* p, const Agent* ag, RoundPhase phase,
                                   bool defender, int alive_self, int alive_opp,
                                   int round_kills_p) {
        if (!p || !ag) return 0.0;
        const auto& a = p->attributes;
        const std::string& nm = ag->name;
        double bonus = 0.0;

        auto is = [&](const char* x) { return nm == x; };

        // Cypher / Killjoy: defender side, alive teammates >= 3 -> hold
        // value from info / utility lockdown.
        if ((is("Cypher") || is("Killjoy")) && defender && alive_self >= 3) {
            bonus += 0.04 * (at(a, Attr::Anchor) + at(a, Attr::Utility)) / 200.0;
        }
        // Chamber / Jett: opening duel of a 5v5 round -> fast-pick OPing.
        if ((is("Chamber") || is("Jett")) &&
            phase == RoundPhase::Opening && alive_self == 5 && alive_opp == 5) {
            bonus += 0.05 * at(a, Attr::Headshot) / 100.0;
        }
        // Neon / Raze: SiteAction phase, attacking side -> explosive entry.
        if ((is("Neon") || is("Raze")) &&
            phase == RoundPhase::SiteAction && !defender) {
            bonus += 0.04 * (at(a, Attr::Movement) + at(a, Attr::Entry)) / 200.0;
        }
        // Astra / Omen: MidRound utility impact (team-wide flavour applied
        // to self for simplicity — full team-wide propagation would require
        // deeper plumbing; the per-self bump still reflects the controller
        // shaping the round).
        if ((is("Astra") || is("Omen")) && phase == RoundPhase::MidRound) {
            bonus += 0.03 * at(a, Attr::Utility) / 100.0;
        }
        // Viper / Yoru / Cypher mid-round lurkers (positioning > 75).
        if ((is("Viper") || is("Yoru") || is("Cypher")) &&
            phase == RoundPhase::MidRound && at(a, Attr::Positioning) > 75) {
            bonus += 0.04 * at(a, Attr::Lurking) / 100.0;
        }
        // Reyna / Iso: self-sustain after a kill in the same round.
        if ((is("Reyna") || is("Iso")) && round_kills_p >= 1) {
            bonus += 0.03 * at(a, Attr::Aim) / 100.0;
        }
        // Sage / Killjoy: PostPlant retake. In the existing PostPlant
        // taxonomy `defender = true` means the planter holding the bomb;
        // the actual retaking side (orig defenders trying to defuse) is
        // !defender. Anchor still drives — these agents stall retakes
        // with util / heals while their team peeks for the defuse.
        if ((is("Sage") || is("Killjoy")) &&
            phase == RoundPhase::PostPlant && !defender) {
            bonus += 0.03 * at(a, Attr::Anchor) / 100.0;
        }
        // Phoenix / Reyna: 1vN clutch potential — small enough that bad
        // Reyna still loses most clutches.
        if ((is("Phoenix") || is("Reyna")) && alive_self == 1 && alive_opp >= 2) {
            bonus += 0.04 * at(a, Attr::Clutch) / 100.0;
        }

        // Hard cap at +0.20 per duel — even a stacked Reyna who got two
        // earlier kills in a 1v3 clutch as defender tops out here.
        return clamp_v(bonus, 0.0, 0.20);
    };

    // IGL leadership effect: bumps team coordination further if their IGL
    // has high MidRoundCalling/AntiStrat. Affects all duels for the team.
    // Strict one-IGL-per-team rule: even if the roster somehow has multiple
    // is_igl flags (rare race in promotion logic), only the best IGL —
    // measured by Leadership — contributes the bonus. Caller code in
    // Team/SoloQ tries to keep rosters clean, but this is the last line of
    // defence so the match never double-counts an IGL.
    auto igl_team_bonus = [&](const std::vector<Player*>& tm) {
        Player* best_igl = nullptr;
        int best_lead = -1;
        for (auto* p : tm) {
            if (!p->is_igl) continue;
            int lead = at(p->attributes, Attr::Leadership);
            if (lead > best_lead) { best_lead = lead; best_igl = p; }
        }
        if (!best_igl) return 0.0;
        return at(best_igl->attributes, Attr::MidRoundCalling) * 0.12
             + at(best_igl->attributes, Attr::AntiStrat) * 0.08
             + at(best_igl->attributes, Attr::EconomyMgmt) * 0.05;
    };
    double t1_igl_bonus = igl_team_bonus(t1_rstr);
    double t2_igl_bonus = igl_team_bonus(t2_rstr);
    t1_coordination += t1_igl_bonus * 0.5;
    t2_coordination += t2_igl_bonus * 0.5;

    // Pillar 2 team-wide info bonus: scaled into coordination so it
    // composes with the existing t_coordination/800 multiplier.
    // 0.03 / 0.10 = ~30% of the coordination ceiling at peak — small.
    //
    // Import chemistry penalty: applied for PRO matches only — solo Q
    // teams are stitched together per match (no shared chemistry to
    // disrupt) and friendlies are scrim-noise. Total cap on the penalty
    // is -0.05 (see import_chemistry_penalty); the resulting coordination
    // multiplier therefore lives in roughly [0.95, 1.03] inclusive of the
    // info bonus.
    double t1_imp_pen = (!is_solo_q_ && !friendly_) ? import_chemistry_penalty(t1_rstr) : 0.0;
    double t2_imp_pen = (!is_solo_q_ && !friendly_) ? import_chemistry_penalty(t2_rstr) : 0.0;
    t1_coordination *= 1.0 + team_info_bonus(t1_rstr, chosen_agents_) - t1_imp_pen;
    t2_coordination *= 1.0 + team_info_bonus(t2_rstr, chosen_agents_) - t2_imp_pen;

    while (true) {
        if ((team1_score_ >= 13 || team2_score_ >= 13) &&
            std::abs(team1_score_ - team2_score_) >= 2) break;

        if (round_num > 40) {
            // Hard cap reached: force a LEGAL win-by-2 final rather than a
            // raw +1 (which could leave an illegal 1-point margin, e.g.
            // 12-12 -> 13-12). Pick a winner, then snap their score to a
            // clean 2-point lead (minimum 13).
            bool t1_force_win = rng().uniform() > 0.5;
            int loser = t1_force_win ? team2_score_ : team1_score_;
            int win_score = std::max(13, loser + 2);
            if (t1_force_win) team1_score_ = win_score;
            else              team2_score_ = win_score;
            break;
        }
        if (round_num == 13 || (round_num > 24 && (round_num - 25) % 2 == 0)) {
            t1_bank_ = 4000; t2_bank_ = 4000;
            t1_loss_streak_ = t2_loss_streak_ = 0;
        }

        bool is_pistol = (round_num == 1 || round_num == 13 || round_num == 25);

        // === Side-aware map control init each round ===
        // First half: t1 attacks, t2 defends. Defenders start with prep
        // advantage (own the site, hold crossfires). Second half flips.
        // OT: alternate sides. This makes "first map control" coherent —
        // the attacking team has to *earn* control through util/picks.
        bool t1_attacking;
        if (round_num <= 12)            t1_attacking = true;
        else if (round_num <= 24)       t1_attacking = false;
        else                            t1_attacking = (round_num - 25) % 2 == 0;
        // Defender starts at 60-65 control, attacker at 35-40. Util / picks
        // shift it during the round (existing +5..+15 swing on each kill).
        if (t1_attacking) {
            t1_map_control = 35.0; t2_map_control = 65.0;
        } else {
            t1_map_control = 65.0; t2_map_control = 35.0;
        }
        // Pistol rounds are even-er — both sides scrambling for first picks.
        if (is_pistol) {
            t1_map_control = t2_map_control = 50.0;
        }

        // === Pre-round util impact ===
        // Attackers with high Utility "pre-throw" map control (smokes
        // claim space, mollys deny defaults). Defenders with high Util
        // counter-utility back. Net effect: the more util-loaded team
        // gets a small starting advantage proportional to their Utility
        // rating, before any duels happen.
        auto team_util_avg = [](const std::vector<Player*>& tm) {
            int s = 0, n = 0;
            for (auto* p : tm) { s += at(p->attributes, Attr::Utility); ++n; }
            return n ? s / static_cast<double>(n) : 50.0;
        };
        double t1_util_score = team_util_avg(t1_rstr) / 99.0;
        double t2_util_score = team_util_avg(t2_rstr) / 99.0;
        // Up to ±10 control points based on relative util quality
        double util_gap = (t1_util_score - t2_util_score) * 10.0;
        t1_map_control = clamp_v(t1_map_control + util_gap, 0.0, 100.0);
        t2_map_control = clamp_v(t2_map_control - util_gap, 0.0, 100.0);
        // AntiStrat (was dead): the IGL who better reads the opponent's default
        // setup steals a few pre-round control points (anti-defaults, timings).
        double as_gap = (t1_igl_as - t2_igl_as) / 99.0 * 5.0;   // up to ±5 control
        t1_map_control = clamp_v(t1_map_control + as_gap, 0.0, 100.0);
        t2_map_control = clamp_v(t2_map_control - as_gap, 0.0, 100.0);
        bool t1_desperate = (team2_score_ >= 11 && team1_score_ < 11) || t1_loss_streak_ >= 3 ||
                            round_num == 12 || round_num == 24;
        bool t2_desperate = (team1_score_ >= 11 && team2_score_ < 11) || t2_loss_streak_ >= 3 ||
                            round_num == 12 || round_num == 24;

        int t1_invest, t2_invest;
        if (is_pistol) {
            t1_invest = t2_invest = 4000;
        } else {
            // Archetype (Task 2): economy_discipline shifts the "do we
            // half-buy or save?" bank threshold by up to ±1500. Disciplined
            // teams demand more money before committing a marginal buy
            // (cleaner full-saves -> better follow-up rounds); force-happy
            // teams gamble into half-buys sooner. Bounded, never inverts
            // the buy/eco logic — just nudges the boundary.
            // EconomyMgmt (was dead): a high-EconomyMgmt IGL runs a more
            // disciplined buy — demands more bank before a marginal full-buy
            // (cleaner full-saves -> better follow-up rounds). ±~1000 on top of
            // the archetype economy_discipline nudge.
            int t1_buy_thr = 15000 - static_cast<int>(t1_eco_disc * -1500.0) + (t1_igl_em - 50) * 20;
            int t2_buy_thr = 15000 - static_cast<int>(t2_eco_disc * -1500.0) + (t2_igl_em - 50) * 20;
            t1_buy_thr = std::max(13000, std::min(17000, t1_buy_thr));
            t2_buy_thr = std::max(13000, std::min(17000, t2_buy_thr));
            if (t1_bank_ >= 19500 || t1_desperate) t1_invest = std::min(t1_bank_, 19500);
            else if (t1_bank_ >= t1_buy_thr)       t1_invest = 15000;
            else                                    t1_invest = std::max(0, t1_bank_ - 15000 + 1900);
            if (t2_bank_ >= 19500 || t2_desperate) t2_invest = std::min(t2_bank_, 19500);
            else if (t2_bank_ >= t2_buy_thr)       t2_invest = 15000;
            else                                    t2_invest = std::max(0, t2_bank_ - 15000 + 1900);
            if (t1_desperate) t1_invest = t1_bank_;
            if (t2_desperate) t2_invest = t2_bank_;
            if (t1_bank_ > 1000) t1_invest = std::max(1000, t1_invest); else t1_invest = t1_bank_;
            if (t2_bank_ > 1000) t2_invest = std::max(1000, t2_invest); else t2_invest = t2_bank_;
        }
        t1_bank_ -= t1_invest;
        t2_bank_ -= t2_invest;

        std::vector<Player*> t1_alive = t1_rstr;
        std::vector<Player*> t2_alive = t2_rstr;
        std::vector<RoundEvent> round_events;
        std::unordered_map<Player*, int> round_kills;
        std::unordered_map<Player*, int> round_dmg;
        std::unordered_map<Player*, bool> round_did_kill, round_did_assist, round_did_trade;
        bool first_engagement = true;

        // Eco gun tier per team, derived from how much they invested.
        // Tier 1=pistol/eco, 2=force/SMG, 3=light buy, 4=full rifle, 5=op-buy.
        auto invest_to_tier = [](int invest) {
            if (invest >= 18000) return 5;
            if (invest >= 14000) return 4;
            if (invest >= 8000)  return 3;
            if (invest >= 4500)  return 2;
            return 1;
        };
        int t1_tier = invest_to_tier(t1_invest);
        int t2_tier = invest_to_tier(t2_invest);

        // Post-plant rolls in past mid-round; after ~5 kills the spike could
        // plausibly have been planted. Pure abstraction since we don't model
        // map geometry.
        bool spike_planted = false;
        bool round_decided = false;  // one team already had the round won
        // Track time-of-kill ordinals so trade window (within "1 step" of
        // a teammate dying) maps to <=3s in real time.
        int kill_ordinal = 0;
        // Bug 7: confidence-after-teammate-death proxy. We track the
        // ordinal of the most recent teammate death PER team; a survivor
        // who fights within a couple of ordinals after their teammate
        // died gets a small Confidence (Aggro x Decision) bump on the
        // next duel. Initialised to a sentinel that's "infinitely old".
        int t1_last_death_ord = -1000;
        int t2_last_death_ord = -1000;

        // === Identity §7: per-round consistency jitter ====================
        // One draw per player per round, scaled INVERSELY to consistency.
        // Low-consistency players have wider per-round swings (some rounds
        // they go off, others they no-show); high-consistency veterans are
        // flat. Multiplies into the per-duel consistency mod each duel of
        // this round. Range: ±0.09 at consistency 30, ±0.02 at consistency 90.
        std::unordered_map<Player*, double> round_jitter;
        auto seed_round_jitter = [&](const std::vector<Player*>& tm) {
            for (auto* p : tm) {
                const ArchetypeProfile& ap = ap_of(p);
                // Variance fix (Task 1): widened band so the top isn't so
                // tight. Was 0.02..0.10; now 0.035..0.155, still strongly
                // inverse to consistency. consistency_mod (+ steadier)
                // narrows the band; ConsistencyMachine-types stay flat,
                // InconsistentSuperstar-types swing hard.
                double swing = 0.155 - (p->consistency / 99.0) * 0.12
                             - 0.06 * ap.consistency_mod;
                // Archetype (Task 2): risk_tolerance widens the boom/bust
                // band — a RiskyPlaymaker/ChaosAgent has higher-variance
                // rounds (bigger highs AND lows) than a steady role player
                // with the same consistency. Symmetric, so it doesn't
                // change expected output, only variance.
                swing += 0.04 * std::max(0.0, static_cast<double>(ap.risk_tolerance));
                swing = clamp_v(swing, 0.03, 0.20);
                double lo = -swing, hi = swing;
                // consistency_floor lifts the bad-roll floor; ceiling_boost
                // raises the good-roll ceiling — asymmetric per archetype.
                lo += 0.5 * ap.consistency_floor;
                hi += 0.6 * ap.ceiling_boost;
                round_jitter[p] = 1.0 + rng().drange(lo, hi);
            }
        };
        seed_round_jitter(t1_rstr);
        seed_round_jitter(t2_rstr);

        while (!t1_alive.empty() && !t2_alive.empty()) {
            std::vector<int> w1, w2;

            // Determine the round phase for this duel. `first_engagement`
            // is true on the very first action of the round, regardless of
            // map control — that's what makes role-weighting strict for
            // the opener.
            RoundPhase phase = first_engagement
                ? RoundPhase::Opening
                : compute_phase(true, spike_planted,
                                static_cast<int>(t1_alive.size()),
                                static_cast<int>(t2_alive.size()));

            auto build_weights = [&](const std::vector<Player*>& alive,
                                     std::vector<int>& w) {
                w.reserve(alive.size());
                for (auto* p : alive) {
                    const Agent* ag = chosen_agents_.count(p) ? chosen_agents_[p] : nullptr;
                    int weight;
                    if (first_engagement) {
                        // Identity §6: "designated entry" score. Reshape
                        // who actually opens — high-aim + high-movement +
                        // high-aggro players are pushed into the opening
                        // duel even more; high-decision players are pulled
                        // out (they take support roles, not openers).
                        // Combined with the role base weight so duelists
                        // still dominate openers, but a low-decision
                        // aggressive flex on Initiator can still grab them.
                        const auto& aa = p->attributes;
                        int agg  = at(aa, Attr::Aggressiveness);
                        int aim  = at(aa, Attr::Aim);
                        int mov  = at(aa, Attr::Movement);
                        int dec  = at(aa, Attr::DecisionMaking);
                        int role_w = role_first_pick_weight(ag);
                        double opening_score = aim * 0.4 + mov * 0.3
                                             + agg * 0.3 - dec * 0.1;
                        // Normalise opening_score (~0..99) into 0.70..1.45.
                        double os_mod = 0.70 + clamp_v(opening_score / 99.0, 0.0, 1.0) * 0.75;
                        // Archetype (Task 2): aggression_mult scales how
                        // often this player takes the opening duel — a
                        // HyperAggroEntry peeks first far more than a
                        // PassiveAnchor of the same attributes.
                        const ArchetypeProfile& ap = ap_of(p);
                        os_mod *= ap.aggression_mult;
                        // tempo (fast → opens more) is a small extra bias.
                        os_mod *= 1.0 + 0.08 * ap.tempo;
                        weight = static_cast<int>(role_w * os_mod);
                    } else {
                        // Phase-aware peek weighting: lurkers shine
                        // mid-round, anchors take 1vNs, awpers contest
                        // post-plant retakes, etc.
                        double pw = phase_peek_weight(p, ag, phase);
                        // Archetype (Task 2): tempo biases phase weighting —
                        // fast players over-index Opening/early phases,
                        // slow/methodical players over-index MidRound/Late.
                        const ArchetypeProfile& ap = ap_of(p);
                        if (phase == RoundPhase::Opening ||
                            phase == RoundPhase::SiteAction)
                            pw *= 1.0 + 0.10 * ap.tempo;
                        else if (phase == RoundPhase::MidRound ||
                                 phase == RoundPhase::Late)
                            pw *= 1.0 - 0.10 * ap.tempo;
                        pw *= ap.aggression_mult;  // aggressive players
                                                   // contest more duels
                        weight = static_cast<int>(pw);
                    }
                    w.push_back(std::max(1, weight));
                }
            };
            build_weights(t1_alive, w1);
            build_weights(t2_alive, w2);

            Player* p1 = t1_alive[static_cast<std::size_t>(rng().weighted_index(w1))];
            Player* p2 = t2_alive[static_cast<std::size_t>(rng().weighted_index(w2))];

            // === Identity §2: smart fight-avoidance ============================
            // A player with high (Positioning+GameSense+DecisionMaking)/3 has
            // a small chance to NOT engage this duel when the situation isn't
            // forced (not Late, not 1vN, not first-engagement). They're
            // re-peeking smart / hiding off-angle — picks another teammate
            // instead. When they DO engage later, they tag an "engineered
            // angle" power bonus (set via smart_engage_bonus[]).
            auto fight_iq = [](Player* p) {
                const auto& a = p->attributes;
                return (at(a, Attr::Positioning)
                      + at(a, Attr::GameSense)
                      + at(a, Attr::DecisionMaking)) / 3.0;
            };
            std::unordered_map<Player*, double> smart_engage_bonus;  // local to this duel
            auto try_avoid = [&](Player*& picked, std::vector<Player*>& alive,
                                 std::vector<int>& weights, RoundPhase ph,
                                 bool forced) {
                if (forced) return;
                if (alive.size() <= 1) return;
                double iq = fight_iq(picked);
                // Archetype (Task 2): fight_selection biases the existing
                // Fight-IQ avoidance branch. <0 (e.g. HyperAggroEntry,
                // DuelistDiva) takes MORE duels (effective IQ lowered, the
                // 80 gate is harder to clear, skip chance shrinks). >0
                // (SmartLurker, TacticalGenius) skips MORE (gate eased,
                // skip chance grows). Modest — attributes still dominate.
                const ArchetypeProfile& ap = ap_of(picked);
                double eff_iq = iq + ap.fight_selection * 10.0;
                if (eff_iq < 80.0) return;
                // 1% per IQ point above 80, capped at 18% chance to skip.
                double skip_chance = std::min(0.18, (eff_iq - 80.0) * 0.01);
                skip_chance *= clamp_v(1.0 + 0.45 * ap.fight_selection, 0.40, 1.60);
                // Lurkers (mid-round, positional) bias higher; openers do not.
                if (ph == RoundPhase::Opening) skip_chance *= 0.4;
                if (ph == RoundPhase::Late)    skip_chance = 0.0;
                skip_chance = clamp_v(skip_chance, 0.0, 0.26);
                if (!rng().chance(skip_chance)) return;
                // Re-pick a different teammate (their next exposure carries
                // an "engineered angle" bonus on power).
                Player* avoider = picked;
                std::vector<int> w2v;
                std::vector<Player*> pool;
                pool.reserve(alive.size());
                w2v.reserve(alive.size());
                for (std::size_t i = 0; i < alive.size(); ++i) {
                    if (alive[i] == avoider) continue;
                    pool.push_back(alive[i]);
                    w2v.push_back(weights[i]);
                }
                if (pool.empty()) return;
                Player* repicked = pool[static_cast<std::size_t>(rng().weighted_index(w2v))];
                smart_engage_bonus[avoider] = 0.0;  // skipped this duel; no bonus
                smart_engage_bonus[repicked] = 0.04;  // teammate gets a small
                                                      // crossfire-from-info bump
                picked = repicked;
            };
            bool forced_p1 = first_engagement || phase == RoundPhase::Late
                           || t1_alive.size() == 1
                           || t2_alive.size() == 1;
            bool forced_p2 = forced_p1;
            try_avoid(p1, t1_alive, w1, phase, forced_p1);
            try_avoid(p2, t2_alive, w2, phase, forced_p2);

            double c1_lo = 0.7 + (p1->consistency / 300.0);
            double c1_hi = 1.3 - (p1->consistency / 300.0);
            if (c1_lo > c1_hi) std::swap(c1_lo, c1_hi);
            double c2_lo = 0.7 + (p2->consistency / 300.0);
            double c2_hi = 1.3 - (p2->consistency / 300.0);
            if (c2_lo > c2_hi) std::swap(c2_lo, c2_hi);

            double c1_mod = rng().drange(c1_lo, c1_hi);
            double c2_mod = rng().drange(c2_lo, c2_hi);

            // Identity §7: fold this round's per-player jitter in. Low
            // consistency players carry a bigger swing (good/bad rounds);
            // high consistency stays flat. Compounds with per-duel jitter
            // but does NOT change the player's match-long expected output.
            {
                auto rj1 = round_jitter.find(p1);
                auto rj2 = round_jitter.find(p2);
                if (rj1 != round_jitter.end()) c1_mod *= rj1->second;
                if (rj2 != round_jitter.end()) c2_mod *= rj2->second;
            }

            // Per-import consistency hit during adaptation. Pro matches only —
            // solo Q rosters re-shuffle every queue and friendlies are scrim
            // noise. Max -0.015 (newly arrived 24-month import); decays linearly
            // with months_remaining and reaches 0 once integrated. Combined
            // ceiling per starter is well under the 1.5% the user asked for.
            if (!is_solo_q_ && !friendly_) {
                if (p1->adaptation_months_remaining > 0) {
                    double frac = std::min(1.0, p1->adaptation_months_remaining / 24.0);
                    c1_mod *= 1.0 - 0.015 * frac;
                }
                if (p2->adaptation_months_remaining > 0) {
                    double frac = std::min(1.0, p2->adaptation_months_remaining / 24.0);
                    c2_mod *= 1.0 - 0.015 * frac;
                }
            }

            // Raw mechanical base, then top-end compression (Task 1) so a
            // 95-attr line isn't linearly 30% above an 82-attr line.
            // Ordering preserved; only the elite tail squeezes, letting
            // good teams trade maps instead of steamrolling.
            double p1_pwr = compress_power(
                (at(p1->attributes, Attr::Aim)            * cfg.pwr_aim) +
                (at(p1->attributes, Attr::Headshot)       * cfg.pwr_headshot) +
                (at(p1->attributes, Attr::Entry)          * cfg.pwr_entry) +
                (at(p1->attributes, Attr::DecisionMaking) * cfg.pwr_decision));
            p1_pwr *= c1_mod;

            double p2_pwr = compress_power(
                (at(p2->attributes, Attr::Aim)            * cfg.pwr_aim) +
                (at(p2->attributes, Attr::Headshot)       * cfg.pwr_headshot) +
                (at(p2->attributes, Attr::Entry)          * cfg.pwr_entry) +
                (at(p2->attributes, Attr::DecisionMaking) * cfg.pwr_decision));
            p2_pwr *= c2_mod;

            // Per-map "off day" effective level (Task 1): even a 95-cons
            // star can be depressed for a whole map. Applied to both
            // duelists uniformly across the map.
            p1_pwr *= off_day_mult[p1];
            p2_pwr *= off_day_mult[p2];
            // World-difficulty AI scaling (1.0 = neutral, so no effect unless
            // the New Game wizard set a non-neutral difficulty).
            p1_pwr *= t1_strength_mult_;
            p2_pwr *= t2_strength_mult_;

            // Per-map matchup swing (Task 1): the team-level "this map
            // clicked / didn't" tilt. Symmetric, independent per team, so
            // a clearly-better team can still drop a map ~25-40% of the
            // time. archetype momentum_sensitivity does NOT scale this —
            // it's a team factor, not a player streak factor.
            p1_pwr *= t1_map_swing;
            p2_pwr *= t2_map_swing;

            // Two-way decaying momentum (Task 1): a confident team plays
            // up, a tilted team plays down — but it decays so a 0-8 team
            // can storm back. archetype momentum_sensitivity (0.70..1.40)
            // scales how hard THIS player rides the swing.
            {
                const ArchetypeProfile& a1 = ap_of(p1);
                const ArchetypeProfile& a2 = ap_of(p2);
                // FEEDBACK-DAMPENING: halved (0.06->0.03, ±0.075->±0.04) so a
                // team on a roll gets a smaller power bonus from RESULTS. This
                // shrinks the cross-round snowball that turned a hair of skill
                // difference into a 13-0; it does NOT touch the attribute->power
                // mapping, so the better team still wins — just not by force.
                p1_pwr *= 1.0 + clamp_v(t1_momentum * 0.03 * a1.momentum_sensitivity,
                                        -0.04, 0.04);
                p2_pwr *= 1.0 + clamp_v(t2_momentum * 0.03 * a2.momentum_sensitivity,
                                        -0.04, 0.04);
            }

            // Economy as a RELATIVE gun-tier matchup (rifles-vs-pistols): the
            // head-to-head tier gap is what wins eco/anti-eco rounds, so a
            // full-buy beats a full-eco hard while full-vs-full cancels to ~0.
            // A small ABSOLUTE floor still rewards being kitted at all (armor/
            // util) so two evenly-eco'd teams play a lower-quality round.
            int gun_gap = t1_tier - t2_tier;   // -4..+4 across tiers 1..5
            p1_pwr *= 1.0 + cfg.gun_advantage * gun_gap;
            p2_pwr *= 1.0 - cfg.gun_advantage * gun_gap;
            p1_pwr *= 1.0 + cfg.eco_advantage * 0.20 * (t1_invest / 19500.0);
            p2_pwr *= 1.0 + cfg.eco_advantage * 0.20 * (t2_invest / 19500.0);

            // === Phase-based attribute bonuses ===
            // The duel's phase decides which attributes matter most. We
            // additively layer the phase bonus on top of the base mechanical
            // power so that, e.g., a lurker-archetype with high Lurking +
            // GameSense + Adaptability really does win mid-round duels even
            // with mediocre raw aim — and an Anchor-Sentinel really does
            // dominate retakes regardless of raw aim. The "defender" flag
            // identifies the holding side on SiteAction / PostPlant.
            // SiteAction / non-PostPlant: defender = low-control attacker's
            // opponent. PostPlant: defender = plant-side (high-control).
            // Centralised in is_team1_defender — see helper at top of file.
            bool p1_is_defender = is_team1_defender(
                t1_map_control, t2_map_control,
                /*pp_planted_side_is_defender=*/phase == RoundPhase::PostPlant);
            bool p2_is_defender = !p1_is_defender;
            p1_pwr += phase_power_bonus(p1, phase, p1_is_defender) * 0.12;
            p2_pwr += phase_power_bonus(p2, phase, p2_is_defender) * 0.12;

            // 1vN clutch: when outnumbered, the high-Clutch player gets a
            // significant power boost — Clutch is one of the most distinctive
            // attributes and should *meaningfully* close the numbers gap.
            int t1_remaining = static_cast<int>(t1_alive.size());
            int t2_remaining = static_cast<int>(t2_alive.size());
            if (t1_remaining < t2_remaining) {
                double cf = at(p1->attributes, Attr::Clutch) / 100.0;
                double cool = at(p1->attributes, Attr::DecisionMaking) / 100.0;
                // Archetype (Task 2): clutch_mult multiplies the existing
                // 1vN term (IceColdClutcher up, ChaosAgent down). Stacked
                // multiplicatively; the whole term still respects the
                // per-duel cap because clutch_mult is bounded 0.85..1.25.
                double clutch_term = (0.28 * cf + 0.05 * cool)
                                   * (t2_remaining - t1_remaining)
                                   * ap_of(p1).clutch_mult;
                p1_pwr *= 1.0 + clutch_term;
            } else if (t2_remaining < t1_remaining) {
                double cf = at(p2->attributes, Attr::Clutch) / 100.0;
                double cool = at(p2->attributes, Attr::DecisionMaking) / 100.0;
                double clutch_term = (0.28 * cf + 0.05 * cool)
                                   * (t1_remaining - t2_remaining)
                                   * ap_of(p2).clutch_mult;
                p2_pwr *= 1.0 + clutch_term;
            }

            // Comeback adaptability: down 3+ rounds, Adaptability adds
            // bounce-back power. Archetype (Task 2): adapt_mod scales the
            // existing in-series/Adaptability comeback term (Veteran
            // Stabilizer / TacticalGenius adapt faster; OnlineFarmer
            // slower). Modest ±35% on a small base term.
            int score_diff = team1_score_ - team2_score_;
            if (score_diff <= -3) {
                double af = at(p1->attributes, Attr::Adaptability) / 100.0;
                p1_pwr *= 1.0 + 0.05 * af * (1.0 + 0.35 * ap_of(p1).adapt_mod);
            } else if (score_diff >= 3) {
                double af = at(p2->attributes, Attr::Adaptability) / 100.0;
                p2_pwr *= 1.0 + 0.05 * af * (1.0 + 0.35 * ap_of(p2).adapt_mod);
            }

            // Reaction time: matters most when neither team has clear control
            // (mid-round, scrambled fights). Stronger weight than before so
            // high-Reaction players actually dominate scramble duels.
            double map_contest = 1.0 - std::abs(t1_map_control - t2_map_control) / 100.0;
            map_contest = clamp_v(map_contest, 0.0, 1.0);
            p1_pwr *= 1.0 + 0.10 * (at(p1->attributes, Attr::Reaction) / 100.0) * map_contest;
            p2_pwr *= 1.0 + 0.10 * (at(p2->attributes, Attr::Reaction) / 100.0) * map_contest;

            // Movement: counter-strafe / peek timing. Bumped from 0.04 to
            // 0.08 — matters meaningfully when rifling on non-pistols.
            if (!is_pistol) {
                p1_pwr *= 1.0 + 0.08 * (at(p1->attributes, Attr::Movement) / 100.0);
                p2_pwr *= 1.0 + 0.08 * (at(p2->attributes, Attr::Movement) / 100.0);
            }

            // Crosshair placement: tightens duels — boosts power, also bumps
            // expected HS rate (applied below in the HS chance calc).
            p1_pwr *= 1.0 + 0.09 * (at(p1->attributes, Attr::CrosshairPlacement) / 100.0);
            p2_pwr *= 1.0 + 0.09 * (at(p2->attributes, Attr::CrosshairPlacement) / 100.0);

            // === Mid-round Lurking ===
            // High Lurking + GameSense players dominate the MidRound phase
            // by catching rotators / picking from off-angles. Boost only
            // applies in the right phase so it doesn't trivialise openers.
            if (phase == RoundPhase::MidRound) {
                p1_pwr *= 1.0 + 0.10 * (at(p1->attributes, Attr::Lurking) / 100.0)
                              + 0.06 * (at(p1->attributes, Attr::GameSense) / 100.0) * map_contest;
                p2_pwr *= 1.0 + 0.10 * (at(p2->attributes, Attr::Lurking) / 100.0)
                              + 0.06 * (at(p2->attributes, Attr::GameSense) / 100.0) * map_contest;
                // MidRoundCalling (was dead): the IGL's mid-round reads/rotations
                // tilt the team's scramble duels toward the better caller.
                double mrc_gap = (t1_igl_mrc - t2_igl_mrc) / 99.0 * 0.05;
                p1_pwr *= 1.0 + mrc_gap;
                p2_pwr *= 1.0 - mrc_gap;
            }

            // === Crossfire setups (team coordination effect on duels) ===
            // When a teammate is alive nearby (proxy: at least 2 teammates
            // alive), Positioning + GameSense get a small boost to model
            // crossfire support. Solo defenders lose this.
            if (t1_alive.size() >= 3) {
                p1_pwr *= 1.0 + 0.05 * (at(p1->attributes, Attr::Positioning) / 100.0)
                              + 0.04 * (at(p1->attributes, Attr::GameSense) / 100.0);
            }
            if (t2_alive.size() >= 3) {
                p2_pwr *= 1.0 + 0.05 * (at(p2->attributes, Attr::Positioning) / 100.0)
                              + 0.04 * (at(p2->attributes, Attr::GameSense) / 100.0);
            }

            // === Entry pathing for site executes ===
            // On SiteAction phase the attacker (low map_control side) gets
            // a power boost from Entry + Movement — represents proper
            // pathing through util/smokes/flashes.
            //
            // Identity §5: team-level execute_quality layered on top.
            //   team_execute_quality = (Util + Comm + Intel) avg / 99
            //   power *= 0.95 + 0.10 * team_execute_quality
            // Teams with high (Util+Comm+Intel) execute cleanly; low-Util
            // teams fumble executes. Applied to the attacker's duel power
            // on SiteAction only. Range: 0.95x..1.05x — small but pervasive.
            if (phase == RoundPhase::SiteAction) {
                auto team_exec_quality = [&](const std::vector<Player*>& tm) {
                    int su = 0, sc = 0, si = 0, n = 0;
                    for (auto* x : tm) {
                        su += at(x->attributes, Attr::Utility);
                        sc += at(x->attributes, Attr::Communication);
                        si += at(x->attributes, Attr::Intelligence);
                        ++n;
                    }
                    if (n == 0) return 0.5;
                    double avg = (su + sc + si) / (3.0 * n);
                    return avg / 99.0;
                };
                // Archetype (Task 2): utility_timing scales this player's
                // contribution to/benefit from the team execute quality.
                // A UtilityGenius (perfect timing) gets the full lift; a
                // wasteful archetype dampens it. Bounded ±25%. The
                // "attacker = low map_control side" check is the same
                // semantic as `is_team1_defender(..., false)` above — kept
                // inline here as the cleaner of the two phrasings.
                bool t1_is_attacker = is_team1_defender(
                    t1_map_control, t2_map_control,
                    /*pp_planted_side_is_defender=*/false);
                if (t1_is_attacker) {
                    double e = at(p1->attributes, Attr::Entry) / 100.0;
                    double m = at(p1->attributes, Attr::Movement) / 100.0;
                    double ut = 1.0 + 0.25 * ap_of(p1).utility_timing;
                    p1_pwr *= 1.0 + 0.12 * e + 0.06 * m;
                    p1_pwr *= 0.95 + 0.10 * team_exec_quality(t1_alive) * ut;
                } else {
                    double e = at(p2->attributes, Attr::Entry) / 100.0;
                    double m = at(p2->attributes, Attr::Movement) / 100.0;
                    double ut = 1.0 + 0.25 * ap_of(p2).utility_timing;
                    p2_pwr *= 1.0 + 0.12 * e + 0.06 * m;
                    p2_pwr *= 0.95 + 0.10 * team_exec_quality(t2_alive) * ut;
                }
            }

            // === Pistol round emphasis ===
            // No-armor classics duels — Aim, Reaction, CrosshairPlacement
            // win pistols. Anchor/Stamina/Awping are useless. Add a small
            // Aim+Reaction bonus so pistol kings feel real.
            if (is_pistol) {
                p1_pwr *= 1.0 + 0.06 * (at(p1->attributes, Attr::Aim) / 100.0)
                              + 0.06 * (at(p1->attributes, Attr::Reaction) / 100.0);
                p2_pwr *= 1.0 + 0.06 * (at(p2->attributes, Attr::Aim) / 100.0)
                              + 0.06 * (at(p2->attributes, Attr::Reaction) / 100.0);
            }

            // === Eco-round upset specialists ===
            // Tier-1 (eco) team facing tier-3+ opponent gets a small
            // SpikeHandle + Clutch + Aggressiveness bump. Models the
            // "eco AWP pickup" / "stack one site" upset attempts.
            // Archetype (Task 2): risk_tolerance scales how hard a player
            // commits to the eco-upset attempt (RiskyPlaymaker / EcoSpec
            // swing big; QuietProfessional plays it safe). economy_discipline
            // (+ = disciplined) slightly damps the gamble (a disciplined
            // player won't over-force an eco). Net multiplier bounded so
            // the branch never breaches the per-duel cap.
            if (!is_pistol) {
                if (t1_tier == 1 && t2_tier >= 3) {
                    const ArchetypeProfile& ap = ap_of(p1);
                    double eco_k = clamp_v(1.0 + 0.40 * ap.risk_tolerance
                                               - 0.20 * ap.economy_discipline,
                                           0.60, 1.50);
                    p1_pwr *= 1.0 + eco_k *
                        (0.05 * (at(p1->attributes, Attr::SpikeHandle) / 100.0)
                       + 0.04 * (at(p1->attributes, Attr::Clutch) / 100.0)
                       + 0.03 * (at(p1->attributes, Attr::Aggressiveness) / 100.0));
                }
                if (t2_tier == 1 && t1_tier >= 3) {
                    const ArchetypeProfile& ap = ap_of(p2);
                    double eco_k = clamp_v(1.0 + 0.40 * ap.risk_tolerance
                                               - 0.20 * ap.economy_discipline,
                                           0.60, 1.50);
                    p2_pwr *= 1.0 + eco_k *
                        (0.05 * (at(p2->attributes, Attr::SpikeHandle) / 100.0)
                       + 0.04 * (at(p2->attributes, Attr::Clutch) / 100.0)
                       + 0.03 * (at(p2->attributes, Attr::Aggressiveness) / 100.0));
                }
            }

            // === Retake mechanics (post-plant attacker side) ===
            // The team retaking the bomb (no map control = was attacked,
            // now defending the plant by clearing it) needs DecisionMaking
            // + Adaptability + Utility. Existing PostPlant phase covers
            // anchor/clutch for the planter side; this adds nuance to
            // the retaker side via Adaptability + Decision.
            if (phase == RoundPhase::PostPlant) {
                // Retaker = the side that does NOT hold the plant. In
                // PostPlant the plant-side is defender (high control); the
                // retaker is therefore the low-control side, which is the
                // same as the "non-defender" side under SiteAction semantics.
                bool p1_retaking = is_team1_defender(
                    t1_map_control, t2_map_control,
                    /*pp_planted_side_is_defender=*/false);
                bool p2_retaking = !p1_retaking;
                if (p1_retaking) {
                    p1_pwr *= 1.0 + 0.06 * (at(p1->attributes, Attr::DecisionMaking) / 100.0)
                                  + 0.04 * (at(p1->attributes, Attr::Adaptability) / 100.0);
                }
                if (p2_retaking) {
                    p2_pwr *= 1.0 + 0.06 * (at(p2->attributes, Attr::DecisionMaking) / 100.0)
                                  + 0.04 * (at(p2->attributes, Attr::Adaptability) / 100.0);
                }
            }

            // === Pressure / OT performance ===
            // High-pressure rounds (OT, match point) reward Stamina +
            // Clutch + DecisionMaking. Models the "veterans show up
            // when it matters" pattern.
            bool match_point = (team1_score_ == 12 || team2_score_ == 12)
                            || (round_num >= 25);
            if (match_point) {
                double p1_pressure = (at(p1->attributes, Attr::Stamina)
                                    + at(p1->attributes, Attr::Clutch)
                                    + at(p1->attributes, Attr::DecisionMaking)) / 300.0;
                double p2_pressure = (at(p2->attributes, Attr::Stamina)
                                    + at(p2->attributes, Attr::Clutch)
                                    + at(p2->attributes, Attr::DecisionMaking)) / 300.0;
                p1_pwr *= 1.0 + 0.06 * p1_pressure;
                p2_pwr *= 1.0 + 0.06 * p2_pressure;
            }

            // === Team Communication & Adaptability bump ===
            // Communication and Adaptability across the team contribute
            // small boost — quietly modeling team chemistry/callouts even
            // for non-IGL teammates.
            auto team_comm_avg = [&](const std::vector<Player*>& tm) {
                int s = 0, n = 0;
                for (auto* x : tm) { s += at(x->attributes, Attr::Communication); ++n; }
                return n ? (s / static_cast<double>(n)) / 99.0 : 0.5;
            };
            double t1_comm = team_comm_avg(t1_alive);
            double t2_comm = team_comm_avg(t2_alive);
            p1_pwr *= 1.0 + 0.04 * t1_comm;
            p2_pwr *= 1.0 + 0.04 * t2_comm;

            // Awping: dedicated sniper. When the team can afford OP money
            // ($18k+) the highest-awping survivor gets a chunky boost.
            auto awp_boost = [&](Player* p, int invest) {
                if (invest < 18000) return 1.0;
                return 1.0 + 0.15 * (at(p->attributes, Attr::Awping) / 100.0);
            };
            p1_pwr *= awp_boost(p1, t1_invest);
            p2_pwr *= awp_boost(p2, t2_invest);

            // Stamina: matches that drag past 30 rounds (overtime, double
            // overtime) start punishing low-stamina players.
            if (round_num > 30) {
                double s1 = at(p1->attributes, Attr::Stamina) / 100.0;
                double s2 = at(p2->attributes, Attr::Stamina) / 100.0;
                p1_pwr *= 0.92 + 0.10 * s1;
                p2_pwr *= 0.92 + 0.10 * s2;
            }

            // Anchor: when you're the last defender alive on your team, your
            // Anchor + SpikeHandle stats add significant power. Sentinels
            // really should save rounds — this is the moment they shine.
            if (t1_alive.size() == 1 && t1_alive.front() == p1) {
                p1_pwr *= 1.0 + 0.22 * (at(p1->attributes, Attr::Anchor) / 100.0)
                              + 0.06 * (at(p1->attributes, Attr::SpikeHandle) / 100.0);
            }
            if (t2_alive.size() == 1 && t2_alive.front() == p2) {
                p2_pwr *= 1.0 + 0.22 * (at(p2->attributes, Attr::Anchor) / 100.0)
                              + 0.06 * (at(p2->attributes, Attr::SpikeHandle) / 100.0);
            }

            double p1_util = at(p1->attributes, Attr::Utility) * rng().drange(0.5, 1.5);
            double p2_util = at(p2->attributes, Attr::Utility) * rng().drange(0.5, 1.5);
            double p1_pos  = at(p1->attributes, Attr::Positioning) * rng().drange(0.8, 1.2) + t1_map_control * 0.2;
            double p2_pos  = at(p2->attributes, Attr::Positioning) * rng().drange(0.8, 1.2) + t2_map_control * 0.2;

            // Combined team / individual modifiers — bumped from prior
            // weights so positioning + utility + team coordination matter
            // visibly. Coordination /800 (was /1000), positioning /300
            // (was /500), utility /600 (was /1000).
            p1_pwr *= (1 + t1_coordination / 800.0) * (1 + p1_pos / 300.0) * (1 + p1_util / 600.0);
            p2_pwr *= (1 + t2_coordination / 800.0) * (1 + p2_pos / 300.0) * (1 + p2_util / 600.0);
            p1_pwr *= t1_comp_bonus;
            p2_pwr *= t2_comp_bonus;
            // WS-B INC-4 bounded identity/coach/region tilt (1.0 in soloq/
            // friendlies). Applied BEFORE the pwin/dom_cap clamp below so the
            // KD ceiling still binds.
            p1_pwr *= t1_wsb_tilt;
            p2_pwr *= t2_wsb_tilt;
            // Increment E: bounded per-map prep edge (user side only; 1.0 elsewhere).
            p1_pwr *= t1_prep_tilt;
            p2_pwr *= t2_prep_tilt;

            // === Pillar 2: agent identity in the duel =========================
            // Two layers per duelist:
            //   1. agent_synergy_bonus     — "is this player comfortable on this
            //                                 agent?" Player attrs vs agent's 3
            //                                 mapped attrs. Range -0.04..+0.10.
            //   2. agent_identity_bonus    — flavour-specific deltas keyed off
            //                                 the agent name + phase + context.
            //                                 Capped at +0.20 internally.
            // Both applied multiplicatively so they compound naturally with the
            // rest of the calc but never dominate it (max combined ~ +0.32).
            const Agent* p1_ag = chosen_agents_.count(p1) ? chosen_agents_[p1] : nullptr;
            const Agent* p2_ag = chosen_agents_.count(p2) ? chosen_agents_[p2] : nullptr;
            int p1_round_kills = round_kills.count(p1) ? round_kills[p1] : 0;
            int p2_round_kills = round_kills.count(p2) ? round_kills[p2] : 0;
            int p1_alive_n = static_cast<int>(t1_alive.size());
            int p2_alive_n = static_cast<int>(t2_alive.size());
            p1_pwr *= 1.0 + agent_synergy_bonus(p1, p1_ag);
            p2_pwr *= 1.0 + agent_synergy_bonus(p2, p2_ag);
            p1_pwr *= 1.0 + agent_identity_bonus(p1, p1_ag, phase, p1_is_defender,
                                                 p1_alive_n, p2_alive_n,
                                                 p1_round_kills);
            p2_pwr *= 1.0 + agent_identity_bonus(p2, p2_ag, phase, p2_is_defender,
                                                 p2_alive_n, p1_alive_n,
                                                 p2_round_kills);

            // === Bug 6: IGL focus tax =========================================
            // IGLs spend mental cycles on calls and macro instead of pure
            // fragging. A subtle ~5% reduction on raw aim duels stacks on
            // top of the spawn-time stat shift; it is calibrated so an
            // IGL with Aim 75 still loses to a non-IGL with Aim 80 most of
            // the time, but a great IGL with Aim 85 holds their own.
            if (p1->is_igl) p1_pwr *= 0.95;
            if (p2->is_igl) p2_pwr *= 0.95;

            // === Bug 7: attribute-driven duel bonuses (style-distinct) =======
            // Confidence / Composure / LAN nerves / Spray control PLUS
            // identity-shaping bonuses (Identity §1/§3/§4): first-bullet
            // Crosshair-Reaction lift, spray follow-up Aim-Adaptability lift,
            // designated-lurker isolation bonus / exposure penalty, and the
            // headshot-clustering multi-kill chain bonus. The four "Bug 7"
            // factors + four identity factors are summed and the total is
            // clamped to [-0.09, +0.12] per duelist — slightly wider than
            // the prior [-0.07, +0.08] because we now stack BOTH style and
            // situational, but still capped under the user's +0.32 ceiling
            // when combined with all other multiplicative channels.
            //
            // Lurker check: agent role match (Yoru/Viper/Cypher) OR
            // Lurking>80 AND Positioning>75. The role-based check makes
            // designated-lurker agents implicit; the attribute fallback
            // catches non-lurker-agent mains who play lurk-y on flex.
            auto is_designated_lurker = [&](Player* p, const Agent* ag) {
                if (ag) {
                    const std::string& nm = ag->name;
                    if (nm == "Yoru" || nm == "Viper" || nm == "Cypher")
                        return true;
                }
                const auto& a = p->attributes;
                return at(a, Attr::Lurking) > 80
                    && at(a, Attr::Positioning) > 75;
            };
            // First-bullet detection: opener of the round AND no one has
            // shot back yet (kill_ordinal == 0). Spray detection: we're
            // mid-firefight (kill_ordinal > 0), defender hadn't pre-fired.
            const bool is_first_bullet = first_engagement && kill_ordinal == 0;

            auto compute_attr_extras = [&](Player* p, const Agent* p_ag,
                                           int p_alive, int opp_alive,
                                           int last_team_death_ord, int round_kills_p) {
                double extra = 0.0;
                const auto& a = p->attributes;
                // ---- Original "Bug 7" factors --------------------------------
                // Confidence (Aggro x Decision / 99) after teammate dies.
                if (kill_ordinal - last_team_death_ord <= 3) {
                    int agg = at(a, Attr::Aggressiveness);
                    int dec = at(a, Attr::DecisionMaking);
                    double conf = (agg * dec) / (99.0 * 99.0);
                    extra += clamp_v(0.04 * (conf - 0.5) * 2.0, -0.02, 0.04);
                }
                // Composure (Clutch x Decision / 99) when down 2+ players.
                if (opp_alive - p_alive >= 2) {
                    int clu = at(a, Attr::Clutch);
                    int dec = at(a, Attr::DecisionMaking);
                    double cmp = (clu * dec) / (99.0 * 99.0);
                    extra += clamp_v(0.05 * cmp, 0.0, 0.05);
                }
                // LAN nerves (Adaptability x Consistency) for playoff matches.
                const bool is_playoff_match =
                       event_name_.find("Final")     != std::string::npos
                    || event_name_.find("Masters")   != std::string::npos
                    || event_name_.find("Champions") != std::string::npos
                    || event_name_.find("Regionals") != std::string::npos;
                if (is_playoff_match) {
                    int adapt = at(a, Attr::Adaptability);
                    double consistency = p->consistency;  // 1..99
                    double cs = (adapt * consistency) / (99.0 * 99.0);
                    // Existing Adapt×Consistency-driven LAN-nerves term.
                    extra += clamp_v(-0.05 + cs * 0.07, -0.05, 0.02);
                    // Archetype (Task 2): lan_mod (-0.10..+0.10) ADDS to
                    // the nerves term — a LANDemon thrives on stage, an
                    // OnlineFarmer chokes. Combined (not replacing) with
                    // the attribute-driven term; the [-0.09,+0.12] clamp
                    // at the bottom keeps the per-duel cap intact.
                    extra += clamp_v(static_cast<double>(ap_of(p).lan_mod),
                                     -0.10, 0.10);
                }
                // (Old "Spray control" Bug-7 factor removed — superseded by
                // the more nuanced spray follow-up in Identity §1 below,
                // which discounts by Crosshair so high-xhr aimers don't
                // double-dip and low-xhr spray demons get a real recovery
                // signal. Net effect: same magnitude, more identity-coded.)

                // ---- Identity §1: first-bullet vs spray emphasis ------------
                // First-bullet duels: heavy weight on Crosshair × Reaction.
                // A 90/90 Crosshair+Reaction player should win most openers.
                if (is_first_bullet) {
                    int xhr = at(a, Attr::CrosshairPlacement);
                    int rxn = at(a, Attr::Reaction);
                    double fb_mech = (xhr * rxn) / (99.0 * 99.0);
                    extra += clamp_v(0.06 * (fb_mech - 0.45), -0.02, 0.05);
                }
                // Spray follow-up: high-Aim low-Crosshair players become
                // "spray demons" who lose the first bullet but recover.
                // Activates only when kill_ordinal > 0 and player has shot
                // once already this round (round_kills_p >= 1) OR another
                // shot has fired (we use kill_ordinal as proxy).
                if (!is_first_bullet && kill_ordinal > 0) {
                    int aim   = at(a, Attr::Aim);
                    int sta   = at(a, Attr::Stamina);
                    int adapt = at(a, Attr::Adaptability);
                    double spray = (aim * 0.5 + sta * 0.25 + adapt * 0.25) / 99.0;
                    // Discount by Crosshair — a player with very high xhr
                    // doesn't NEED spray recovery; a low-xhr aimer does.
                    double xhr_factor = 1.0 - at(a, Attr::CrosshairPlacement) / 200.0; // 0.5..1.0
                    extra += clamp_v(0.04 * spray * xhr_factor - 0.01, -0.01, 0.04);
                }
                // ---- Identity §3: designated lurker --------------------------
                // +0.06 when team is isolated (small alive count = lurking by
                //   choice off the main team grouping)
                // -0.04 when forced into a 2v1+ disadvantage (lurker exposed
                //   without his info / setup)
                if (is_designated_lurker(p, p_ag)) {
                    bool isolated = (p_alive <= 2 && p_alive >= opp_alive);
                    bool exposed  = (opp_alive - p_alive >= 1 && p_alive <= 2);
                    if (isolated) extra += 0.06;
                    else if (exposed) extra -= 0.04;
                }
                // ---- Identity §4: headshot clustering -----------------------
                // High-HS players cluster kills. If this player already has a
                // kill this round, scale their power up by (HS-50)*0.005 so
                // 3k/4k/Ace chance organically rises. Low-HS players stay
                // inconsistent.
                if (round_kills_p >= 1) {
                    int hs = at(a, Attr::Headshot);
                    double cluster = (hs - 50) * 0.005;
                    extra += clamp_v(cluster, -0.025, 0.045);
                }
                // Combined cap. Slightly wider than the prior [-0.07,+0.08]
                // to accommodate the identity additions. The "+0.32 total
                // power multiplier per duel" ceiling still holds because
                // the other channels haven't grown.
                return clamp_v(extra, -0.09, 0.12);
            };
            p1_pwr *= 1.0 + compute_attr_extras(p1, p1_ag, p1_alive_n, p2_alive_n,
                                                t1_last_death_ord, p1_round_kills);
            p2_pwr *= 1.0 + compute_attr_extras(p2, p2_ag, p2_alive_n, p1_alive_n,
                                                t2_last_death_ord, p2_round_kills);

            // === Identity §2: engineered-angle bonus (smart re-pick) =========
            // If smart fight-avoidance handed the duel to this player as a
            // re-pick after a teammate dodged, they get a small "info-
            // assisted" bump on the actual engagement. Set in try_avoid().
            {
                auto it_p1 = smart_engage_bonus.find(p1);
                if (it_p1 != smart_engage_bonus.end())
                    p1_pwr *= 1.0 + it_p1->second;
                auto it_p2 = smart_engage_bonus.find(p2);
                if (it_p2 != smart_engage_bonus.end())
                    p2_pwr *= 1.0 + it_p2->second;
            }

            // Win probability uses a skill-gap EXPONENT (P=p1^k/(p1^k+p2^k))
            // instead of the old linear p1/(p1+p2). With k>1 a clearly better
            // duelist wins decisively more, so every attribute/archetype/phase
            // modifier that fed p1_pwr/p2_pwr finally moves the outcome instead
            // of washing out. See SimConfig::duel_exponent.
            bool intl_event =
                (event_name_.find("MASTERS")   != std::string::npos ||
                 event_name_.find("CHAMPIONS") != std::string::npos);
            double k_exp = cfg.duel_exponent;
            // International regression (item 5): the best teams face the best
            // teams, so star K/D should regress HARDEST. A lower exponent at
            // Masters/Champions compresses the per-player stat spread further.
            if (intl_event) k_exp *= 0.85;
            double p1_w = std::pow(p1_pwr > 0.0 ? p1_pwr : 1e-6, k_exp);
            double p2_w = std::pow(p2_pwr > 0.0 ? p2_pwr : 1e-6, k_exp);
            double total = p1_w + p2_w;
            if (total <= 0) total = 1;
            // === Duel-dominance cap (item 5: the real tail lever) ============
            // The fat K/D tail is structural: dominant teams win lopsided rounds
            // and their stars hog kills. The exponent barely moves it. A SYMMETRIC
            // cap on per-duel win probability bounds how much any one player can
            // dominate engagements — compressing the elite K/D tail toward the
            // approved band — while keeping the mean at ~0.5 (so rating centering
            // holds) and NOT changing who wins rounds (momentum/map-control do).
            // Internationals cap tighter so 1.5+ intl K/D seasons vanish.
            double pwin = p1_w / total;
            // WS-C KD tighten: 0.63 domestic (was 0.65) drops the absolute K/D
            // ceiling from ~1.86 to ~1.70 so sustained 1.7 seasons become
            // near-impossible, while an 8%-stronger team still clearly wins more
            // rounds ([27] guards the round-share floor). Tighter than ~0.62
            // flattens matches toward a coin flip.
            double dom_cap = 0.63;
            if (intl_event) {
                // STACKED-FIELD intl regression (anti-dynasty D1): internationals
                // are best-vs-best, so K/D regresses HARDEST there — and even
                // harder when BOTH rosters are elite. field 0..1 maps the weaker
                // roster's OVR from ~78 (intl floor) to ~90 (superteam clash).
                // Cap 0.53 (top intl K/D ~1.13) down to 0.505 (~1.02) at a fully
                // stacked field — intl 1.5+ seasons are mythical and 2.0 runs rare.
                // (Verified: the intl-KD MAX is heavy-tail NOISE that bounces
                // ~1.33-1.49 across seeds/code-reshuffles at ANY cap — a 0.01 nudge
                // only moves p90 ~0.005, so chasing the max with the cap is futile;
                // 0.53 is the proven distribution-on-target value. 0% hit 1.5.)
                double field = clamp_v((std::min(t1_ovr_, t2_ovr_) - 78.0) / 12.0,
                                       0.0, 1.0);
                dom_cap = 0.53 - 0.025 * field;
            }
            pwin = clamp_v(pwin, 1.0 - dom_cap, dom_cap);
            int team_won;
            int alive_diff_pre;  // killer's team alive - opp alive BEFORE the kill
            Player* killer; Player* victim;
            if (rng().uniform() < pwin) {
                killer = p1; victim = p2; team_won = 1;
                alive_diff_pre = static_cast<int>(t1_alive.size()) - static_cast<int>(t2_alive.size());
                t2_alive.erase(std::find(t2_alive.begin(), t2_alive.end(), p2));
                t1_map_control += rng().drange(2, 7);
                t2_map_control = std::max(0.0, t2_map_control - rng().drange(2, 7));
            } else {
                killer = p2; victim = p1; team_won = 2;
                alive_diff_pre = static_cast<int>(t2_alive.size()) - static_cast<int>(t1_alive.size());
                t1_alive.erase(std::find(t1_alive.begin(), t1_alive.end(), p1));
                t2_map_control += rng().drange(2, 7);
                t1_map_control = std::max(0.0, t1_map_control - rng().drange(2, 7));
            }
            first_engagement = false;
            ++kill_ordinal;
            // Bug 7 confidence-after-teammate-death: stamp the LOSING
            // team's last death ordinal so the survivors get the
            // confidence/aggro window for their next ~3 duels.
            if (team_won == 1) t2_last_death_ord = kill_ordinal;
            else               t1_last_death_ord = kill_ordinal;

            // Spike plant heuristic: attackers on a site take with momentum.
            // SpikeHandle of the attacking team boosts plant probability.
            int kills_so_far = static_cast<int>(round_events.size()) + 1;
            if (!spike_planted && kills_so_far >= 4 &&
                ((t1_map_control > 65.0 && team_won == 1) ||
                 (t2_map_control > 65.0 && team_won == 2))) {
                auto& attackers = (team_won == 1) ? t1_alive : t2_alive;
                int sh_sum = 0; int sh_n = 0;
                for (auto* ap : attackers) { sh_sum += at(ap->attributes, Attr::SpikeHandle); ++sh_n; }
                double avg_sh = sh_n > 0 ? (sh_sum / static_cast<double>(sh_n)) / 99.0 : 0.5;
                double plant_chance = clamp_v(0.40 + 0.30 * avg_sh, 0.0, 0.85);
                spike_planted = rng().chance(plant_chance);
            }
            // After a 5th kill on a side the round is mathematically over.
            round_decided = round_decided || t1_alive.empty() || t2_alive.empty();

            // === Trade detection ===
            // A real trade-frag happens when you kill the opponent who just
            // killed your teammate, within ~3 seconds (we approximate with 2
            // kill-ordinals). Probability of actually getting the trade
            // scales with the trader's Positioning + Reaction + Crosshair
            // Placement (good crossfire setups, fast swings) — so a low-
            // attribute team can be in trade range and still whiff it.
            bool killer_traded = false;
            int look_back = std::min<int>(2, (int)round_events.size());
            Player* trade_target_killer = nullptr;
            for (int li = 0; li < look_back; ++li) {
                auto& past = round_events[round_events.size() - 1 - li];
                if (past.team_won != team_won && past.killer == victim) {
                    // The current victim is the same enemy who just killed
                    // one of OUR teammates — this is a textbook trade frag.
                    trade_target_killer = past.killer;
                    break;
                }
            }
            if (trade_target_killer) {
                int pos = at(killer->attributes, Attr::Positioning);
                int rxn = at(killer->attributes, Attr::Reaction);
                int xhr = at(killer->attributes, Attr::CrosshairPlacement);
                int adapt = at(killer->attributes, Attr::Adaptability);
                // 0.55 base + up to +0.35 from skill = 0.55..0.90 trade rate
                double trade_chance = 0.55
                                    + 0.10 * (pos / 99.0)
                                    + 0.10 * (rxn / 99.0)
                                    + 0.08 * (xhr / 99.0)
                                    + 0.07 * (adapt / 99.0);
                // Archetype (Task 2): teamplay_mod biases trade propensity
                // (TeamFirstGlue swings in for teammates more readily).
                // Small additive nudge; clamp keeps it bounded.
                trade_chance += 0.06 * ap_of(killer).teamplay_mod;
                if (rng().chance(clamp_v(trade_chance, 0.40, 0.92))) {
                    killer_traded = true;
                }
            }
            // Was the *victim* traded by their teammates? We'll know once a
            // teammate of the victim kills the killer in the next 1 step;
            // patch retroactively at end of duel loop. For now record raw.

            // Build the kill/death events with full context.
            KillEvent ke;
            ke.round_num   = round_num;
            ke.alive_diff  = alive_diff_pre;
            ke.killer_tier = (team_won == 1) ? t1_tier : t2_tier;
            ke.victim_tier = (team_won == 1) ? t2_tier : t1_tier;
            ke.traded      = killer_traded;
            ke.post_plant  = spike_planted;
            ke.post_round  = round_decided;
            match_stats_[killer].kill_events.push_back(ke);

            DeathEvent de;
            de.round_num   = round_num;
            // From victim's POV, alive_diff is opposite signed.
            de.alive_diff  = -alive_diff_pre;
            de.killer_tier = ke.killer_tier;
            de.victim_tier = ke.victim_tier;
            de.was_traded  = false;  // patched below if their team trades
            de.post_plant  = spike_planted;
            de.post_round  = round_decided;
            match_stats_[victim].death_events.push_back(de);

            // Patch the previous death event if it now qualifies as "traded".
            if (killer_traded) {
                auto& last_ev = round_events.back();
                Player* prev_victim = last_ev.victim;
                if (prev_victim && match_stats_.count(prev_victim) &&
                    !match_stats_[prev_victim].death_events.empty()) {
                    match_stats_[prev_victim].death_events.back().was_traded = true;
                }
            }

            // === Assist attribution (role + agent gated) ===
            // Real Valorant: assists are a hallmark of util-throwing roles.
            // Initiators dominate the assist column; "smoke" Controllers
            // (Brimstone / Omen / Clove) are next; Sentinels ~moderate;
            // Duelists are LAST — high duelist assists are a sign of failed
            // entries (didn't finish the kill). Most kills produce 0
            // assists. Realistic targets: 13-15 = high-impact game,
            // 20+ = vanishingly rare.
            //
            // Per kill we compute a per-teammate "assist propensity" score
            // weighted heavily by role + agent. We cap total assists per
            // kill at 1 (secondary is now extremely rare and scoped).
            auto& assist_team = (team_won == 1) ? t1_alive : t2_alive;

            auto agent_name_of = [&](Player* pp) -> const std::string& {
                static const std::string empty_s;
                auto it = chosen_agents_.find(pp);
                return (it != chosen_agents_.end() && it->second)
                    ? it->second->name : empty_s;
            };
            // Role-keying must follow the AGENT, not the player's
            // career-default `primary_role`. Bug 2: a Duelist main flexing
            // onto Astra (Controller) still had `primary_role == Duelist`,
            // so this picker treated their Astra as a Duelist for assist
            // weighting (and the smoke-controller probe never matched).
            // Use the chosen_agents_ role of THIS map.
            auto agent_role_of = [&](Player* pp) -> Role {
                auto it = chosen_agents_.find(pp);
                if (it != chosen_agents_.end() && it->second) return it->second->role;
                return pp->primary_role;  // fallback only if no agent assigned
            };
            auto is_smoke_controller = [&](Player* pp) {
                if (agent_role_of(pp) != Role::Controller) return false;
                const std::string& n = agent_name_of(pp);
                return n == "Brimstone" || n == "Omen" || n == "Clove";
            };
            auto role_assist_mult = [&](Player* pp) {
                switch (agent_role_of(pp)) {
                    case Role::Initiator: return 1.00;
                    case Role::Controller:
                        return is_smoke_controller(pp) ? 0.85 : 0.30;
                    case Role::Sentinel:  return 0.45;
                    case Role::Duelist:   return 0.12;   // missed-kill signal
                    default:              return 0.40;
                }
            };

            // Score each candidate teammate. Sum -> normalised "any-assist"
            // probability; then weighted pick for who actually gets credit.
            struct Cand { Player* p; double w; };
            std::vector<Cand> cand;
            cand.reserve(assist_team.size());
            double sum_w = 0.0;
            for (auto* ap : assist_team) {
                if (ap == killer) continue;
                double util_n = at(ap->attributes, Attr::Utility) / 99.0;
                double comm_n = at(ap->attributes, Attr::Communication) / 99.0;
                double gs_n   = at(ap->attributes, Attr::GameSense) / 99.0;
                double role_m = role_assist_mult(ap);
                // GameSense folded in: well-coordinated support players time
                // their utility to actually set up teammates' kills, so they
                // earn more of the assist column (was util+comm only).
                double w = role_m * (0.42 * util_n + 0.26 * comm_n + 0.17 * gs_n + 0.15);
                // Archetype (Task 2): teamplay_mod is a SMALL lean on
                // assist/trade propensity — TeamFirstGlue/SupportiveFacil
                // help-set more, a DuelistDiva less. Bounded ±20% so it
                // never overrides the role-gated assist multipliers and
                // the §12 "duelists few assists" invariant holds (a
                // Duelist's 0.12 base still dominates the magnitude).
                w *= clamp_v(1.0 + 0.20 * ap_of(ap).teamplay_mod, 0.80, 1.20);
                if (w < 0.01) w = 0.01;
                cand.push_back({ap, w});
                sum_w += w;
            }

            // Per-kill chance ANY assist gets credited. Capped at 0.45 even
            // for the strongest support cores so 13-15 assists per game is
            // a "great" performance and 20 is genuinely rare.
            double any_assist_chance = clamp_v(sum_w * 0.18, 0.06, 0.45);
            if (rng().chance(any_assist_chance) && !cand.empty()) {
                // Weighted pick by `w` to bias support roles. Single
                // assister per kill (no secondary) — caps assist totals.
                double r = rng().uniform() * sum_w;
                Player* picked = cand.front().p;
                double accum = 0.0;
                for (auto& c : cand) {
                    accum += c.w;
                    if (r <= accum) { picked = c.p; break; }
                }
                match_stats_[picked].a += 1;
                round_did_assist[picked] = true;
                round_dmg[picked] += rng().irange(20, 50);
            }

            if (killer_traded) {
                match_stats_[killer].trades += 1;
                round_did_trade[killer] = true;
            }

            if (round_events.empty()) {
                match_stats_[killer].fb += 1;
                match_stats_[victim].fd += 1;
            }

            match_stats_[killer].k += 1;
            match_stats_[victim].d += 1;
            round_kills[killer] += 1;
            round_did_kill[killer] = true;

            // Damage per kill — the winner's lethal damage (a kill ≈ 150 HP).
            int dmg = rng().irange(130, 180);
            round_dmg[killer] += dmg;

            // LOSER chip damage — the player who LOST the duel still dealt real
            // damage before dying, scaled by how close the fight was (their
            // share of the raw power). This DECOUPLES ADR from kills: a player
            // who loses tight duels racks up real damage without the frag, so
            // the displayed ADR finally diverges from kill count. Capped below
            // a kill's 150. The rating's adra baseline below is raised to ~195
            // to absorb the average chip so overall ratings stay centred.
            double kpwr = (killer == p1) ? p1_pwr : p2_pwr;
            double vpwr = (victim == p1) ? p1_pwr : p2_pwr;
            double vshare = (kpwr + vpwr) > 0.0 ? vpwr / (kpwr + vpwr) : 0.40;
            int chip = static_cast<int>(149.0 * vshare * rng().drange(0.45, 1.25));
            round_dmg[victim] += clamp_v(chip, 0, 149);

            int hs_attr = at(killer->attributes, Attr::Headshot);
            int aim_attr = at(killer->attributes, Attr::Aim);
            double skill_val = hs_attr * 0.78 + aim_attr * 0.22;
            double hs_chance = 12.0 + skill_val * 0.385 + rng().drange(-3.5, 3.5);
            hs_chance = clamp_v(hs_chance, 15.0, 48.0);
            if (rng().uniform() * 100.0 < hs_chance) match_stats_[killer].hs_hits += 1;

            round_events.push_back({killer, victim, team_won,
                                    static_cast<int>(t1_alive.size()),
                                    static_cast<int>(t2_alive.size())});
        }

        int round_winner = !t1_alive.empty() ? 1 : 2;

        for (auto& kv : round_kills) {
            int k = kv.second;
            auto& s = match_stats_[kv.first];
            if      (k == 2) s.mk2 += 1;
            else if (k == 3) s.mk3 += 1;
            else if (k == 4) s.mk4 += 1;
            else if (k >= 5) s.mk5 += 1;
        }
        for (auto& kv : round_dmg) {
            auto& s = match_stats_[kv.first];
            s.damage += kv.second;
            if (kv.second > s.max_dmg) s.max_dmg = kv.second;
        }

        if (round_winner == 1) {
            team1_score_ += 1; t1_loss_streak_ = 0; t2_loss_streak_ += 1;
            t1_bank_ += 15000;
            t2_bank_ += 9500 + std::min(2, t2_loss_streak_ - 1) * 2500;
            for (auto* p : t1_alive) match_stats_[p].survivals += 1;
            // Clutch credit: a LONE winning survivor closed the round
            // man-down (1vX). The losing team's alive vector is always empty
            // here, so survivors live in the WINNER's vector — the old code
            // credited the (empty) loser vector, so clutch_pts never moved.
            if (t1_alive.size() == 1) match_stats_[t1_alive.front()].clutch_pts += 1;
        } else {
            team2_score_ += 1; t2_loss_streak_ = 0; t1_loss_streak_ += 1;
            t2_bank_ += 15000;
            t1_bank_ += 9500 + std::min(2, t1_loss_streak_ - 1) * 2500;
            for (auto* p : t2_alive) match_stats_[p].survivals += 1;
            // Clutch credit: see symmetric note above.
            if (t2_alive.size() == 1) match_stats_[t2_alive.front()].clutch_pts += 1;
        }

        // === Two-way decaying momentum update (Task 1) ===================
        // Decay toward 0 FIRST (so leads don't compound forever — a 0-8
        // team's deficit momentum bleeds off and they can mount a
        // comeback), then nudge the round winner up / loser down. Bounded
        // so the per-duel effect stays small (capped ±0.075 at the duel).
        t1_momentum *= 0.78;
        t2_momentum *= 0.78;
        if (round_winner == 1) { t1_momentum += 0.45; t2_momentum -= 0.45; }
        else                   { t2_momentum += 0.45; t1_momentum -= 0.45; }
        t1_momentum = clamp_v(t1_momentum, -1.10, 1.10);
        t2_momentum = clamp_v(t2_momentum, -1.10, 1.10);

        // Per-round binary KAST flags. Each player's "kast row" counts up
        // by 1 if they had ANY of K/A/S/T this round.
        auto bump_kast = [&](Player* p, bool survived) {
            auto& s = match_stats_[p];
            bool did_k = round_did_kill[p];
            bool did_a = round_did_assist[p];
            bool did_s = survived;
            bool did_t = round_did_trade[p];
            if (did_k) s.rounds_with_k += 1;
            if (did_a) s.rounds_with_a += 1;
            if (did_s) s.rounds_with_s += 1;
            if (did_t) s.rounds_with_t += 1;
            if (did_k || did_a || did_s || did_t) s.rounds_with_kast += 1;
        };
        for (auto* p : t1_rstr) {
            bool alive = std::find(t1_alive.begin(), t1_alive.end(), p) != t1_alive.end();
            bump_kast(p, alive);
        }
        for (auto* p : t2_rstr) {
            bool alive = std::find(t2_alive.begin(), t2_alive.end(), p) != t2_alive.end();
            bump_kast(p, alive);
        }

        RoundLog rl;
        rl.round = round_num;
        rl.winner_name = (round_winner == 1) ? team1_->name : team2_->name;
        rl.t1_score = team1_score_; rl.t2_score = team2_score_;
        rl.t1_invest = t1_invest; rl.t2_invest = t2_invest;

        // === WS-C R1: deterministic round-resolution narrative ==============
        // PURE post-decision flavor. The round is ALREADY won (by elimination,
        // the only engine mechanic), so this only labels HOW it plausibly ended,
        // derived from a HASH of already-finalized round state. NO rng() — the
        // duel stream is untouched, so KD/dynasty are byte-identical (proven by
        // the exact-match --dynasty gate). `t1_attacking` is the same side flag
        // computed for the duels above (still in scope this round iteration).
        rl.t1_attacking = t1_attacking;
        {
            bool attacker_won  = ((round_winner == 1) == t1_attacking);
            int  win_survivors = (round_winner == 1)
                                 ? static_cast<int>(t1_alive.size())
                                 : static_cast<int>(t2_alive.size());
            std::uint32_t h =
                  static_cast<std::uint32_t>(round_num)     * 2654435761u
                ^ static_cast<std::uint32_t>(team1_score_)  * 40503u
                ^ static_cast<std::uint32_t>(team2_score_)  * 12289u
                ^ static_cast<std::uint32_t>(t1_invest)     * 19u
                ^ static_cast<std::uint32_t>(t2_invest)     * 7u
                ^ static_cast<std::uint32_t>(win_survivors) * 131u;
            int bucket = static_cast<int>(h % 100u);
            if (attacker_won) {
                // Mostly plant + detonation; a thin slice is an ace pre-plant.
                rl.spike_planted = (bucket >= 12);
                rl.end_kind = rl.spike_planted ? RoundEndKind::SpikeDetonation
                                               : RoundEndKind::Elimination;
                rl.was_retake = false;
            } else {
                // Defender win: retook a plant (defuse), shut the attack down
                // (elimination), or a rare low-buy time expiry.
                bool planted = (bucket < 38);
                rl.spike_planted = planted;
                if (planted) {
                    rl.end_kind   = RoundEndKind::Defuse;
                    rl.was_retake = true;
                } else if (bucket >= 92 && (t1_invest + t2_invest) < 8000) {
                    rl.end_kind   = RoundEndKind::TimeExpiry;
                    rl.was_retake = false;
                } else {
                    rl.end_kind   = RoundEndKind::Elimination;
                    rl.was_retake = false;
                }
            }
        }

        rl.events = std::move(round_events);
        round_history_.push_back(std::move(rl));
        round_num += 1;
    }

    int total_rounds = team1_score_ + team2_score_;

    // Situational kill weight for the VLR-style KillContribution.
    // Mirrors the rules described in the rating spec: clutch big bonus,
    // advantage diminishing returns, traded kills (<=3s) less, eco mismatch
    // matters, post-plant lightly damped, post-round (after a side wipes)
    // worth almost nothing.
    auto kill_weight = [](const KillEvent& e) {
        double w = 1.0;
        int diff = e.alive_diff;
        if (diff < 0)      w *= (1.0 + 0.20 * (-diff));   // clutch / outnumbered
        else if (diff > 0) w *= std::max(0.40, 1.0 - 0.12 * diff);  // advantage

        int eco = e.killer_tier - e.victim_tier;
        if (eco <= -2)     w *= 1.40;     // killed someone with way better gun
        else if (eco == -1) w *= 1.15;
        else if (eco == 1)  w *= 0.85;
        else if (eco >= 2)  w *= 0.65;     // OP'd an eco

        if (e.traded)      w *= 0.70;      // <=3s trade kill
        if (e.post_plant)  w *= 0.92;
        if (e.post_round)  w *= 0.20;
        return w;
    };

    auto death_weight = [](const DeathEvent& e) {
        double w = 1.0;
        int diff = e.alive_diff;  // your team alive - opp alive BEFORE death
        if (diff > 0)      w *= (1.0 + 0.18 * diff);    // dying with man advantage = bad
        else if (diff < 0) w *= std::max(0.45, 1.0 - 0.12 * (-diff));  // dying outnumbered = less bad

        int eco = e.victim_tier - e.killer_tier;
        if (eco >= 2)      w *= 1.30;     // died on full buy to an eco
        else if (eco == 1) w *= 1.10;
        else if (eco == -1) w *= 0.90;
        else if (eco <= -2) w *= 0.75;     // ecoed and died, expected

        if (e.was_traded)  w *= 0.70;      // your teammate avenged you
        if (e.post_plant)  w *= 1.05;
        if (e.post_round)  w *= 0.30;
        return w;
    };

    for (auto& kv : match_stats_) {
        auto& s = kv.second;
        double kc = 0.0, dc = 0.0;
        for (auto& e : s.kill_events)  kc += kill_weight(e);
        for (auto& e : s.death_events) dc += death_weight(e);
        s.weighted_kill_contrib  = kc;
        s.weighted_death_contrib = dc;
    }

    // VLR-style rating formula — tuned to feel less harsh than the raw
    // regression coefficients (the original 26/18/21-line landed around
    // 1.09; with this tuning a similar line settles closer to ~1.20).
    //  - kKPR bumped so frag impact carries more weight
    //  - kAPR bumped so playmakers feel valued
    //  - kDPR softened (deaths count less harshly)
    //  - intercept bumped (baseline floor pulled up ~0.05)
    constexpr double kKPR  = 0.95;
    constexpr double kAPR  = 0.30;
    constexpr double kDPR  = -0.38;
    constexpr double kADRA = 0.0030;
    constexpr double kSR   = 0.45;
    constexpr double kKAST = 0.34;
    constexpr double kInt  = 0.235;

    int rd = std::max(1, total_rounds);

    for (auto& kv : match_stats_) {
        Player* p = kv.first;
        auto& s = kv.second;

        double weighted_kpr = s.weighted_kill_contrib  / rd;
        double weighted_dpr = s.weighted_death_contrib / rd;
        double apr  = static_cast<double>(s.a) / rd;
        // Baseline raised 140 -> 195 to absorb the new per-duel LOSER chip
        // damage (~55/death avg) so adra stays centred near 0 for a K≈D player
        // while correctly rewarding damage dealt over pure fragging.
        double adra = (static_cast<double>(s.damage) - 195.0 * s.k) / rd;
        double sr   = static_cast<double>(rd - s.d) / rd;
        double kast = static_cast<double>(s.rounds_with_kast) / rd;

        double r = kKPR  * weighted_kpr
                 + kAPR  * apr
                 + kDPR  * weighted_dpr
                 + kADRA * adra
                 + kSR   * sr
                 + kKAST * kast
                 + kInt;

        // Mild brain-bonus / aggro-penalty seasoning preserved from prior
        // engine; small, applied AFTER VLR coefficients to keep the bell.
        int aggro = at(p->attributes, Attr::Aggressiveness);
        double smart_stat = (at(p->attributes, Attr::Intelligence) +
                             at(p->attributes, Attr::DecisionMaking) +
                             at(p->attributes, Attr::Clutch)) / 3.0;
        double kd_simple = static_cast<double>(s.k) / std::max(1, s.d);
        if (aggro > 60 && kd_simple < 0.90)
            r -= 0.04 * ((aggro - 50) / 100.0) * (0.90 - kd_simple);
        if (smart_stat > 60.0 && kd_simple >= 1.0)
            r += 0.02 * ((smart_stat - 50.0) / 100.0) * (kd_simple - 1.0);

        double final_rating = clamp_v(r, 0.10, 2.50);
        s.rating = std::round(final_rating * 100.0) / 100.0;
    }

    auto pick_mvp = [&](const std::vector<Player*>& v) -> Player* {
        Player* best = nullptr;
        double best_r = -1.0;
        for (auto* p : v) {
            if (match_stats_[p].rating > best_r) { best_r = match_stats_[p].rating; best = p; }
        }
        return best;
    };
    if (!t1_rstr.empty()) t1_mvp_ = pick_mvp(t1_rstr);
    if (!t2_rstr.empty()) t2_mvp_ = pick_mvp(t2_rstr);
    std::vector<Player*> all = t1_rstr;
    all.insert(all.end(), t2_rstr.begin(), t2_rstr.end());
    if (!all.empty()) match_mvp_ = pick_mvp(all);

    // Bump career MVP counter (pro matches only; not solo Q, not friendly).
    if (!is_solo_q_ && !friendly_ && match_mvp_) match_mvp_->career_mvps += 1;

    history_record_.event = event_name_;
    history_record_.map_name = map_.name;
    history_record_.blue_name = is_solo_q_ ? "BLUE TEAM" : team1_->name;
    history_record_.red_name  = is_solo_q_ ? "RED TEAM"  : team2_->name;
    char score_buf[32];
    std::snprintf(score_buf, sizeof(score_buf), "%d-%d", team1_score_, team2_score_);
    history_record_.score = score_buf;
    history_record_.blue_team = t1_rstr;
    history_record_.red_team = t2_rstr;
    history_record_.blue_mvp = t1_mvp_;
    history_record_.red_mvp = t2_mvp_;
    history_record_.mvp = match_mvp_;

    for (auto& kv : match_stats_) {
        Player* p = kv.first;
        auto& s = kv.second;
        PlayerLine line;
        auto it = chosen_agents_.find(p);
        line.agent = (it != chosen_agents_.end()) ? it->second->name : std::string("?");
        line.k = s.k; line.d = s.d; line.a = s.a;
        line.fb = s.fb; line.fd = s.fd;
        line.rating = s.rating;
        line.hs = static_cast<int>((s.hs_hits / static_cast<double>(std::max(1, s.k))) * 100);
        line.fb_pct = static_cast<int>((s.fb / static_cast<double>(std::max(1, s.fb + s.fd))) * 100);
        char mk[64];
        std::snprintf(mk, sizeof(mk), "%dx2 %dx3 %dx4 %dxA", s.mk2, s.mk3, s.mk4, s.mk5);
        line.mk = mk;
        line.max_dmg = s.max_dmg;
        line.clutch = s.clutch_pts;
        history_record_.stats[p] = line;

        if (friendly_) {
            // Friendly / Watch Match: stats live in match_stats_ (so the
            // live viewer shows them) but are NOT credited to career or
            // season totals. Prevents demo viewing from polluting careers.
            continue;
        }
        if (is_solo_q_) {
            p->solo_kills += s.k; p->solo_deaths += s.d;
        } else {
            auto& ms = p->map_stats[map_.name];
            ms.count += 1; ms.rating_total += s.rating;
            p->career_kills += s.k; p->career_deaths += s.d; p->career_assists += s.a;
            p->season_kills += s.k; p->season_deaths += s.d; p->season_assists += s.a;
            p->season_fb += s.fb; p->season_fd += s.fd; p->season_survivals += s.survivals;
            p->season_trades += s.trades; p->season_rounds += total_rounds;
            p->season_rounds_with_kast += s.rounds_with_kast;
            p->career_fb += s.fb; p->career_fd += s.fd;
            p->career_rating_total += s.rating;
            p->season_rating_total += s.rating;
            p->career_matches += 1; p->season_matches += 1;
            // Season feeds for the regular-season stats board (ADR + HS%).
            p->season_damage  += s.damage;
            p->season_hs_hits += s.hs_hits;
            // Career feeds for League Leaders.
            p->career_damage           += s.damage;
            p->career_hs_hits          += s.hs_hits;
            p->career_rounds_with_kast += s.rounds_with_kast;
            p->career_rounds           += total_rounds;
            p->career_survivals        += s.survivals;
            p->career_trades           += s.trades;

            // Per-tournament stat bucket (item 1): tag this map's stats with a
            // collapsed tournament identity so any view can scope to ONE event
            // (regional group+playoff combined; each international separate).
            // Mirrors the season_* writes above exactly, so the per-tournament
            // sums reconcile with the season totals.
            {
                Player::TournStatLine& ts =
                    p->tourn_stats[tourn_identity_key(event_name_, current_world_year())];
                ts.maps += 1; ts.matches += 1; ts.rounds += total_rounds;
                ts.kills += s.k; ts.deaths += s.d; ts.assists += s.a;
                ts.fb += s.fb; ts.fd += s.fd; ts.survivals += s.survivals;
                ts.trades += s.trades; ts.rounds_with_kast += s.rounds_with_kast;
                ts.damage += s.damage; ts.hs_hits += s.hs_hits;
                ts.rating_total += s.rating;
            }

            // === Hall-of-Fame milestone tracking ===
            if (s.k > p->career_max_match_kills) {
                p->career_max_match_kills = s.k;
            }
            int kd_x100 = (s.d > 0)
                ? static_cast<int>(std::round((double)s.k / s.d * 100.0))
                : s.k * 100;  // unkillable = treat as massive
            if (kd_x100 > p->career_max_match_kd_x100) {
                p->career_max_match_kd_x100 = kd_x100;
            }
            // Grand Final clutch: did this player land the FINAL kill of the
            // FINAL round of a Grand Final? Detected by event_name + last
            // round + last event going to this player while outnumbered.
            if (event_name_.find("Grand Final") != std::string::npos &&
                !round_history_.empty()) {
                auto& last_round = round_history_.back();
                if (!last_round.events.empty()) {
                    auto& final_kill = last_round.events.back();
                    if (final_kill.killer == p) {
                        // Killer team alive count - opponent alive count BEFORE the kill
                        int adv = (final_kill.team_won == 1)
                                ? (final_kill.t1_alive - final_kill.t2_alive)
                                : (final_kill.t2_alive - final_kill.t1_alive);
                        if (adv <= 0) {  // 1vN clutch
                            p->career_grand_final_clutches += 1;
                        }
                    }
                }
            }

            // === Agent mastery hook ===
            // Pro-only (gated by surrounding !friendly_ + !is_solo_q_ branch).
            // Year comes from the world-year set by GameManager::advance_day
            // so we don't need to thread current_year through Match's API.
            if (it != chosen_agents_.end() && it->second) {
                p->record_agent_performance(it->second->name, s.rating,
                                            current_world_year());
                // Off-role accounting (P4.0/P5.1): the chosen agent's role IS the
                // role the player was deployed in this map. PURE BOOKKEEPING —
                // no rng(), reads only already-computed s.rating + the agent role
                // (stream-neutral; verified against the --dynasty baseline). Feeds
                // Flex of the Year + the chronic off-role discontent term.
                int pr = static_cast<int>(it->second->role);
                if (pr >= 0 && pr < 4) {
                    p->season_role_maps[pr] += 1;
                    if (it->second->role != p->primary_role) {
                        p->season_offrole_matches      += 1;
                        p->season_offrole_rating_total += s.rating;
                    }
                }
            }

            // === Map mastery hook ===
            // Mirrors agent_mastery — pro-only via the surrounding branch.
            // Drives per-map agent selection in Team::build_round_selection
            // (a Lotus main builds Lotus mastery and gets a fit boost there).
            p->record_map_performance(map_.name, s.rating, current_world_year());

            // === IGL strategic impact (Pillar 1) ===
            // Bumped ONLY for IGL-flagged players in pro (non-friendly, non-
            // solo-Q) matches. The surrounding branch already guards both.
            // compute_igl_impact returns a 0..2.5 score from calling-flavor
            // attributes + team outcome vs roster strength + pressure tier.
            if (p->is_igl) {
                // Locate which side this player is on so we can score outcome
                // vs roster-strength delta correctly. Walk the team1 roster
                // first; if absent assume team2 (matches the only two possible
                // sides — match_stats_ is seeded from these two rosters).
                bool is_team1 = false;
                for (auto& tp : team1_->roster) {
                    if (tp.get() == p) { is_team1 = true; break; }
                }
                double team_strength = is_team1 ? t1_ovr_ : t2_ovr_;
                double opp_strength  = is_team1 ? t2_ovr_ : t1_ovr_;
                bool team_won = is_team1 ? (team1_score_ > team2_score_)
                                          : (team2_score_ > team1_score_);
                double impact = compute_igl_impact(p, is_team1, team_won,
                                                    team_strength, opp_strength,
                                                    team1_score_, team2_score_,
                                                    event_name_);
                p->igl_impact_total  += impact;
                p->igl_impact_season += impact;
                p->igl_match_count   += 1;
                if (impact > p->igl_impact_season_peak)
                    p->igl_impact_season_peak = impact;
            }

            // === MVP overhaul: pressure-match counter + clutch aggregation ===
            // "Pressure" = playoff / international event. Names follow the
            // existing event-name convention used elsewhere (Grand Final
            // detection above + Tournament::award_event_titles substring
            // matches: Regionals / MASTERS / CHAMPIONS / Final / Playoffs).
            {
                bool pressure_match =
                    (event_name_.find("Final")    != std::string::npos) ||
                    (event_name_.find("MASTERS")  != std::string::npos) ||
                    (event_name_.find("Masters")  != std::string::npos) ||
                    (event_name_.find("CHAMPIONS")!= std::string::npos) ||
                    (event_name_.find("Champions")!= std::string::npos) ||
                    (event_name_.find("Regionals")!= std::string::npos) ||
                    (event_name_.find("Playoffs") != std::string::npos);
                if (pressure_match) p->season_pressure_matches += 1;
                // Real international ATTENDANCE signal (distinct from WINNING a
                // title): bumped for every Masters/Champions map played, so a
                // player who attended an international even without winning gets
                // strength-of-schedule credit in the awards.
                if (event_name_.find("MASTERS")   != std::string::npos ||
                    event_name_.find("Masters")   != std::string::npos ||
                    event_name_.find("CHAMPIONS") != std::string::npos ||
                    event_name_.find("Champions") != std::string::npos) {
                    p->season_intl_matches += 1;
                }
            }
            p->season_clutch_pts += s.clutch_pts;

            p->check_dynamic_badges();
        }
    }

    // === Phase B chemistry signal generation ============================
    // After each map, compute pair-chemistry deltas for every duo on the
    // SAME team that played the map. Pair score evolves slowly via
    // Team::record_chemistry (which applies an EMA internally per Agent
    // A's contract). Delta sign + magnitude based on shared outcome.
    // Skips friendly + solo-Q matches (they don't affect career chemistry).
    if (!friendly_ && !is_solo_q_) {
        const int chem_rounds = std::max(1, total_rounds);

        // Compute single-map chemistry delta for a pair on the same team.
        // PlayerMatchStats stores assists as `.a`; rounds aren't per-player
        // (everyone plays all rounds in a map), so we use total_rounds.
        auto pair_chemistry_delta = [&](const PlayerMatchStats& sA,
                                        const PlayerMatchStats& sB,
                                        bool team_won) -> double {
            double rA = sA.rating;
            double rB = sB.rating;
            double avg_team_rating = (rA + rB) * 0.5;
            double gap = std::abs(rA - rB);

            double d = 0.0;

            // Big stomp together in a win → strong positive
            if (team_won && rA >= 1.10 && rB >= 1.10) {
                d += 0.35 + 0.20 * std::min(0.8, avg_team_rating - 1.10);
            }
            // Shared loss with both subpar → small positive (suffered together)
            else if (!team_won && rA <= 0.85 && rB <= 0.85) {
                d += 0.08;
            }
            // Mismatch (one stomped, one ghosted) → small negative
            else if (gap >= 0.35) {
                d -= 0.10 + std::min(0.10, (gap - 0.35) * 0.5);
            }
            // Mixed outcome (one carried the other in a win) → small negative
            // (resentment proxy)
            else if (team_won && avg_team_rating < 1.00 &&
                     (std::max(rA, rB) - std::min(rA, rB)) >= 0.20) {
                d -= 0.06;
            }

            // Synergy via shared assist play — both contributing utility
            // (assists per round above 0.30 for both) in a win.
            double aprA = static_cast<double>(sA.a) / chem_rounds;
            double aprB = static_cast<double>(sB.a) / chem_rounds;
            if (team_won && aprA >= 0.30 && aprB >= 0.30) d += 0.05;

            // Clamp single-map delta.
            if (d >  0.50) d =  0.50;
            if (d < -0.30) d = -0.30;
            return d;
        };

        auto walk_team_pairs = [&](Team* team_ptr,
                                   const std::vector<Player*>& roster_players,
                                   bool team_won) {
            if (!team_ptr) return;
            for (std::size_t i = 0; i < roster_players.size(); ++i) {
                Player* pi = roster_players[i];
                if (!pi) continue;
                auto sit = match_stats_.find(pi);
                if (sit == match_stats_.end()) continue;
                for (std::size_t j = i + 1; j < roster_players.size(); ++j) {
                    Player* pj = roster_players[j];
                    if (!pj) continue;
                    auto sjt = match_stats_.find(pj);
                    if (sjt == match_stats_.end()) continue;
                    double delta = pair_chemistry_delta(sit->second,
                                                        sjt->second,
                                                        team_won);
                    if (delta != 0.0) {
                        team_ptr->record_chemistry(*pi, *pj, delta);
                    }
                }
            }
        };

        bool t1_won = (team1_score_ > team2_score_);
        walk_team_pairs(team1_.get(), t1_rstr, t1_won);
        walk_team_pairs(team2_.get(), t2_rstr, !t1_won);
    }

    if (match_mvp_ && match_stats_[match_mvp_].k >= 30) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "[!] %s drops a %d-bomb!",
                      match_mvp_->name.c_str(), match_stats_[match_mvp_].k);
        storyline_ = buf;
    } else {
        // Variant pools so the same three lines don't repeat all season.
        // Selection is a pure deterministic hash of the match's own recorded
        // data (team names + final round scores) — NO rng(), so re-rendering
        // the same match always reproduces the exact same storyline
        // (dynasty-determinism).
        const TeamPtr& winner = (team1_score_ > team2_score_) ? team1_ : team2_;
        const std::size_t salt = std::hash<std::string>{}(team1_->name)
                               + std::hash<std::string>{}(team2_->name)
                               + static_cast<std::size_t>(team1_score_) * 31u
                               + static_cast<std::size_t>(team2_score_);
        const int margin = std::abs(team1_score_ - team2_score_);
        if (std::abs(t1_ovr_ - t2_ovr_) > 10) {
            const TeamPtr& underdog = (t1_ovr_ < t2_ovr_) ? team1_ : team2_;
            if (underdog == winner) {
                // Winner's own team MVP — always on the winning side (unlike
                // match_mvp_, which can come from the losing roster).
                Player* wmvp = (winner == team1_) ? t1_mvp_ : t2_mvp_;
                switch (salt % 5) {
                    case 0:  storyline_ = "MASSIVE UPSET BY " + winner->name + "!"; break;
                    case 1:  storyline_ = winner->name + " flip the script — nobody saw this coming."; break;
                    case 2:  storyline_ = "Statement win: " + winner->name + " take down a heavy favorite."; break;
                    case 3:  storyline_ = "Upset on " + map_.name + " — " + winner->name + " topple the favorite."; break;
                    default:
                        if (wmvp) storyline_ = winner->name + " pull off the upset behind "
                                             + wmvp->name + "'s "
                                             + std::to_string(match_stats_[wmvp].k) + " kills.";
                        else      storyline_ = "MASSIVE UPSET BY " + winner->name + "!";
                        break;
                }
            } else {
                switch (salt % 5) {
                    case 0:  storyline_ = "Domination by " + winner->name + " as expected."; break;
                    case 1:  storyline_ = winner->name + " never let it get close."; break;
                    case 2:  storyline_ = "One-way traffic — " + winner->name + " cruise."; break;
                    case 3:  storyline_ = "Clinical. " + winner->name + " close it out without a sweat."; break;
                    default: storyline_ = "Business as usual — " + winner->name + " handle the mismatch."; break;
                }
            }
        } else {
            // Scoreline-specific lines only fire when the score backs them up
            // (this branch keys off roster parity, not the final margin).
            switch (salt % 6) {
                case 0:  storyline_ = "A closely fought tactical matchup."; break;
                case 1:  storyline_ = (margin <= 2)
                             ? "Down to the wire — " + winner->name + " edge it."
                             : winner->name + " shade a tense tactical battle.";
                         break;
                case 2:  storyline_ = "Round-for-round slugfest goes the way of " + winner->name + "."; break;
                case 3:  storyline_ = (margin <= 1)
                             ? "Margins of a single round separate these two."
                             : "Two evenly matched squads, one decisive stretch.";
                         break;
                case 4:  storyline_ = (margin <= 2)
                             ? "An absolute coinflip settled late by " + winner->name + "."
                             : winner->name + " pull clear of an even matchup.";
                         break;
                default: storyline_ = "Neither side blinks until " + winner->name + " find the extra gear."; break;
            }
        }
    }
}

RecordedMatchPtr make_recorded_match(const Match& m) {
    auto rec = std::make_shared<RecordedMatch>();
    rec->event       = m.event_name();
    rec->map_name    = m.map().name;
    rec->blue_name   = m.history_record().blue_name;
    rec->red_name    = m.history_record().red_name;
    rec->score       = m.history_record().score;
    rec->mvp_name    = m.match_mvp() ? m.match_mvp()->name : std::string{};
    rec->blue_score  = m.team1_score();
    rec->red_score   = m.team2_score();
    rec->is_solo_q   = m.is_solo_q();
    rec->team1       = m.team1();
    rec->team2       = m.team2();
    rec->round_history  = std::make_shared<std::vector<RoundLog>>(m.round_history());
    rec->history_record = std::make_shared<HistoryRecord>(m.history_record());
    rec->match_stats    = std::make_shared<std::unordered_map<Player*, PlayerMatchStats>>(m.match_stats());
    return rec;
}

}  // namespace vlr
