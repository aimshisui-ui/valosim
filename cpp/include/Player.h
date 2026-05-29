#pragma once

#include "Agent.h"
#include "Common.h"
#include "Country.h"

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace vlr {

enum class GrowthArchetype : std::uint8_t { Standard, EarlyPeaker, LateBloomer };

// 16 distinct rookie archetypes. Set ONCE at generation, persists for the
// player's career. Drives initial stat profile, growth curve, agent pool
// preference, contract personality, and consistency. The archetype is
// hidden from normal UI (revealed only in god mode) so scouting feels
// like discovery.
enum class RookieArchetype : std::uint8_t {
    None,                       // legacy spawns (pre-archetype world init)
    MechanicalProdigy,          // sky-high Aim/Headshot/Reaction, low GS
    RawAimer,                   // pure Aim/HS, low everything else
    TacticalIGL,                // high Intel/Decision/Lead/MidRound, IGL-ready
    ClutchSpecialist,           // huge Clutch + Anchor + Stamina
    AggressiveEntry,            // Entry/Aggro/Movement, mediocre Util
    SupportMastermind,          // Util/Comm/GS, low frag
    FlexibleUtility,            // balanced across all roles, deep agent pool
    SoloQueueDemon,             // high MMR potential, low team-play stats
    HighPotentialProject,       // low ovr but huge potential gap
    InconsistentSuperstar,      // very high ceiling, very low consistency
    VeteranStyleRookie,         // mature stats from day 1, low growth
    DefensiveAnchor,            // Anchor/Positioning/SpikeHandle Sentinel
    LANPerformer,               // big rating in pro matches, low solo MMR
    FragileConfidence,          // talented but high consistency variance
    HighIQStrategist,           // Intel/Decision/AntiStrat, low Aim
    TeamChemistryPlayer         // Comm/Adaptability/Lead, average mech
};

// Pretty-print + descriptions for the UI.
const char* rookie_archetype_name(RookieArchetype a) noexcept;
const char* rookie_archetype_blurb(RookieArchetype a) noexcept;

// =========================================================================
// === Desire — the FOURTH, NEGOTIATION-driving preference system ==========
// =========================================================================
//
// DISTINCT from the three pre-existing playstyle systems:
//   * `rookie_archetype` (16-value spawn growth template) — left as-is.
//   * `Archetype`        (32 sim-driving playstyle)      — left as-is.
//   * `playstyle_identity()` (derived display label)     — left as-is.
//
// `Desire` tags a player's CONTRACT/career-preference temperament. Assigned
// ONCE at generation (see pick_spawn_desire) from attributes + age +
// archetype, stable for the player's career. Read by the negotiation /
// signing path (Team.cpp Agent-A integration) via the three public accessors
// on Player: desire_salary_mult, desire_accepts, desire_length_pref. Goal:
// free agency stops being "highest affordable rating wins" and becomes a
// contest of human preferences.
enum class Desire : std::uint8_t {
    Greedy,          // max salary, short deals (re-evaluate every year)
    Loyal,           // stays with current team, prefers re-sign
    RingChaser,      // joins contenders for less; runs from rebuilders
    Mercenary,       // no team loyalty, short contracts, max ask everywhere
    StabilitySeeker, // long deals, avoids aggressive/winnow orgs
    Competitor,      // wants strong teammates, allergic to Rebuilding
    Count
};
const char* desire_name(Desire d) noexcept;
const char* desire_blurb(Desire d) noexcept;

// =========================================================================
// === Archetype — the THIRD, simulation-DRIVING playstyle system ==========
// =========================================================================
//
// This is DISTINCT from (and does NOT merge with) the two pre-existing
// systems:
//   * `rookie_archetype` (16-value spawn growth template) — left as-is.
//   * `playstyle_identity()` (derived display label) — left as-is.
//
// `Archetype` is a STABLE per-player tag, assigned ONCE at generation from
// role + attributes + rookie_archetype + a little rng (so two same-role
// players usually differ). It DRIVES the match simulation: Match.cpp reads
// `archetype_profile(p.archetype)` per duel and layers the modest multipliers
// /biases below ONTO its existing attribute-driven math — it never replaces
// it. Goal: NBA-2K-deep playstyles where two Jett players do not feel
// identical.
enum class Archetype : std::uint8_t {
    MechanicalDemon, HyperAggroEntry, SmartLurker, IceColdClutcher,
    UtilityGenius, PassiveAnchor, TempoController, MomentumFragger,
    LANDemon, OnlineFarmer, TacticalGenius, RiskyPlaymaker,
    ConsistencyMachine, ConfidencePlayer, SlowMethodical, HighCeilingLowFloor,
    AggressiveAWPer, SupportiveFacilitator, FlexGenius, AntiStratMaster,
    EcoSpecialist, EntrySacrifice, TeamFirstGlue, OverheatingSuperstar,
    RookieMechFreak, VeteranStabilizer, ClinicalCloser, ChaosAgent,
    ZoneController, RetakeSpecialist, DuelistDiva, QuietProfessional,
    Count
};   // 32 archetypes + Count

struct ArchetypeProfile {
    // Multipliers center on 1.0 (neutral); biases center on 0.0. Keep ranges
    // modest so no single archetype dominates — Match.cpp layers these onto
    // attribute-driven math, it does NOT replace it.
    float aggression_mult      = 1.0f; // 0.80..1.25  entry/peek frequency
    float fight_selection      = 0.0f; // -1..+1      <0 avoids duels, >0 seeks
    float utility_timing       = 0.0f; // -1..+1      <0 wasteful, >0 perfect
    float clutch_mult          = 1.0f; // 0.85..1.25  1vN performance
    float consistency_mod      = 0.0f; // -0.12..+0.12 round-to-round variance damping (+ = steadier)
    float tempo                = 0.0f; // -1..+1      <0 slow/methodical, >0 fast
    float risk_tolerance       = 0.0f; // -1..+1
    float lan_mod              = 0.0f; // -0.10..+0.10 perf delta at LAN/playoff events
    float momentum_sensitivity = 1.0f; // 0.70..1.40  how hard streaks swing them
    float economy_discipline   = 0.0f; // -1..+1      <0 force-happy, >0 disciplined
    float consistency_floor    = 0.0f; // 0..+0.12    raises floor of bad games
    float ceiling_boost        = 0.0f; // 0..+0.15    raises ceiling of good games
    float teamplay_mod         = 0.0f; // -1..+1      assist/trade/util-for-team lean
    float adapt_mod            = 0.0f; // -1..+1      in-series adaptation speed
    const char* name           = "";   // display name e.g. "Mechanical Demon"
};

// Pure, table-driven. O(1), no allocation, no rng — Match.cpp calls this
// per duel.
ArchetypeProfile archetype_profile(Archetype a);
const char*      archetype_name(Archetype a);     // == profile.name

struct MatchHistoryEntry {
    int    year = 0;
    int    age = 0;
    std::string team;
    double rating = 0.0;
    double kd = 0.0;
    double kast = 0.0;
    std::string placement;
    std::vector<std::string> awards;
    int    salary_k = 0;   // amount earned this season (in $K)
    // Per-season counters captured at year-end inside
    // save_history_and_progress. Default 0 so historical entries stamped
    // before these fields existed still load + behave sanely. Consumed by
    // Player::best_season() / top_n_seasons() — the 8-match qualification
    // gate keys off `matches`.
    int    matches = 0;
    int    kills   = 0;
    int    deaths  = 0;
    int    assists = 0;
};

struct ProMatchSnapshot {
    std::string event;
    std::string map_name;
    std::string blue_name;
    std::string red_name;
    std::string score;
    std::string mvp_name;
    int  k = 0;
    int  d = 0;
    int  a = 0;
    int  rating_x100 = 0;
};

// Forward declarations from Match.h. We can't include Match.h here because
// of header cycles (Match -> Team -> Player). The full type is forward-OK
// since RecordedMatch is held only by shared_ptr.
struct RoundLog;
struct PlayerMatchStats;
struct HistoryRecord;

}  // namespace vlr

#include <memory>

namespace vlr {

class Player;
class Team;
using TeamPtr = std::shared_ptr<Team>;

// Full match recording — enough data to replay round-by-round inside the
// Live Match viewer. Matches are immutable once written; multiple players
// (10 per pro match, 10 per solo Q match) reference the same instance.
struct RecordedMatch {
    std::string event;
    std::string map_name;
    std::string blue_name;
    std::string red_name;
    std::string score;
    std::string mvp_name;
    int  blue_score = 0;
    int  red_score  = 0;
    bool is_solo_q  = false;

    // Held as weak_ptr to break the shared_ptr ownership cycle:
    //     RecordedMatch -> Team -> Player -> pro_match_history -> RecordedMatch
    // The team is "live" — always re-pinned per call via .lock(). All current
    // consumers only read identity / color / roster fields, never mutate, so
    // a transient lock() is safe. nullptr-guard at every callsite.
    std::weak_ptr<Team> team1;
    std::weak_ptr<Team> team2;

    // Owned copies of the live-engine data so the replay survives even if
    // the Match object goes out of scope.
    std::shared_ptr<std::vector<RoundLog>> round_history;
    std::shared_ptr<HistoryRecord>         history_record;
    std::shared_ptr<std::unordered_map<Player*, PlayerMatchStats>> match_stats;
};

using RecordedMatchPtr = std::shared_ptr<RecordedMatch>;

struct MapStat {
    int    count = 0;
    double rating_total = 0.0;
};

// Snapshot of a player's career counters at a given progression-tick.
// Subtracting this from the live counters gives "performance over the last
// 30 days" without keeping a per-day timeline.
struct ProgressionSnapshot {
    int    career_kills = 0, career_deaths = 0, career_assists = 0;
    int    career_fb = 0, career_fd = 0, career_survivals = 0, career_trades = 0;
    int    career_rounds = 0;
    int    career_matches = 0;
    double career_rating_total = 0.0;
    int    solo_mmr = 1100;
    int    solo_wins = 0, solo_losses = 0;
    int    solo_kills = 0, solo_deaths = 0;
};

// Output of a single monthly progression pass; surfaced to the UI so the
// user can see exactly what changed and why.
struct ProgressionReport {
    std::string player_name;
    int  day = 0;
    int  year = 0;
    bool was_pro = false;
    int  matches_in_window  = 0;
    double avg_rating       = 0.0;
    int  ranked_played      = 0;
    int  mmr_delta          = 0;
    std::vector<std::pair<Attr, int>> changes;   // (attr, signed delta)
    std::string explanation;                      // human-readable summary
};

struct Contract {
    int amount_k = 25;
    int exp_year = 0;
};

// Per-agent mastery state. Tracks how much real pro experience a player has
// accumulated on each agent so signature picks emerge naturally from career
// performance rather than just instant attribute fit. Updated at end-of-match
// via Player::record_agent_performance() and decayed at year-end inside
// Player::save_history_and_progress().
struct AgentMastery {
    int    matches = 0;          // pro matches played on this agent
    double avg_rating = 0.0;     // exponential moving avg of VLR rating (alpha=0.15)
    double peak_rating = 0.0;    // best single-match rating ever
    int    last_played_year = 0; // for decay pruning
};

// Per-map mastery state. Tracks how much real pro experience a player has
// accumulated on each map. Drives "comfort map" identification and feeds
// per-map agent selection in Team.cpp via map_mastery_bonus().
struct MapMastery {
    int    matches = 0;          // pro matches played on this map
    double avg_rating = 0.0;     // EMA of VLR rating (alpha=0.15)
    double peak_rating = 0.0;    // best single-match rating
    int    last_played_year = 0; // for decay pruning
};

// Free helpers so Match.cpp (which has no GameManager pointer) can stamp the
// world year onto mastery records. GameManager bumps this at the top of each
// advance_day(); record_agent_performance reads it via current_world_year().
void set_current_world_year(int y) noexcept;
int  current_world_year() noexcept;

struct FreeAgentMood {
    std::unordered_map<std::string, double> per_team;
};

class Player : public std::enable_shared_from_this<Player> {
public:
    Player(std::string name, int age, Attributes attrs, std::string region,
           int potential = -1, int work_ethic = -1, int consistency = -1);

    std::string name;       // gamertag / handle — primary display name
    std::string first;      // legal first name
    std::string last;       // legal surname
    std::string country;    // English country name (e.g. "South Korea")
    std::string country_iso;// 2-letter ISO code (e.g. "kr")
    std::string region;     // VCT region affiliation: Americas / EMEA / Pacific

    int  age;
    int  potential;
    int  work_ethic;
    int  consistency;
    int  birth_year = 0;        // computed from age at generation
    std::string birth_city;     // hometown within `country`
    GrowthArchetype growth_archetype = GrowthArchetype::Standard;
    int  peak_age = 24;
    RookieArchetype rookie_archetype = RookieArchetype::None;

    // Stable per-player simulation-driving playstyle. Assigned ONCE at
    // generation (see generate_player) from role + attributes +
    // rookie_archetype + a little rng so same-role players usually differ.
    // Read by Match.cpp via archetype_profile(archetype) per duel. DISTINCT
    // from rookie_archetype (growth template) and playstyle_identity()
    // (derived display label) — the three coexist, none merged.
    Archetype archetype = Archetype::QuietProfessional;

    // Stable per-player NEGOTIATION temperament. Assigned ONCE at
    // generation from attrs + age + archetype, then read by the contract /
    // free-agency path via desire_salary_mult / desire_accepts /
    // desire_length_pref. Default Competitor matches the user spec's
    // "most pros want to win" intuition; the spawn roll overrides this.
    // DISTINCT from rookie_archetype (growth), Archetype (sim playstyle),
    // and playstyle_identity() (derived label) — none merged.
    Desire desire = Desire::Competitor;

    // How many agents this player can comfortably play. Most players: 3.
    // Solid flex: 4-5. True one-trick: 1-2. Ultra-flex (~0.1%): 6-7.
    // Rolled once at spawn, persists for career.
    int  agent_pool_size = 3;

    // IGL flag — set once at generation (or when promoted by team logic).
    // Players flagged as IGLs trade fragging power (-Aim/-HS/-Entry) for
    // mental ceiling (+Leadership/+Intelligence/+Communication/+GameSense
    // /+MidRoundCalling/+EconomyMgmt/+AntiStrat). Every team must have one.
    bool is_igl = false;

    // Flex player — the team's wildcard SUB-ROLE overlay. Plays whatever
    // agent the map + comp demand, rotating between 2-3 role categories per
    // map. Highest agent_pool_size + Adaptability on the roster.
    //
    // Set ONCE at team formation via Team::enforce_one_flex(). Flex is NOT
    // a Position value — it is a SUB-ROLE OVERLAY (analogous to is_igl)
    // layered on top of the player's actual core position. The 4 core
    // gameplay positions remain: Duelist / Controller / Sentinel / Initiator.
    // The canonical starting 5 keeps 1D + 1C + 1S + 1I composition, with one
    // of those starters additionally flagged is_flex as the wildcard.
    //
    // INDEPENDENT of is_igl. Both are overlays/modifiers, NOT standalone
    // role categories replacing core composition needs. A player can be
    // both is_igl and is_flex (the rare "Flex IGL"). No mutual exclusion is
    // enforced. No attribute stat shift (unlike is_igl) — Flex is just a
    // sub-role designation, not a stat-altering role.
    bool is_flex = false;

    // IGL tendencies — only meaningful when is_igl == true. 0 = passive
    // pole, 100 = aggressive pole. Drives the IGL profile tab.
    int  tend_play_aggressive = 50;     // aggressive vs passive system
    int  tend_lurk_vs_execute = 50;     // prefer lurking vs prefer executes
    int  tend_vocal           = 50;     // vocal leader vs quiet
    int  tend_adaptive        = 50;     // adaptive vs structured

    Attributes attributes{};

    std::string team_name = "Free Agent";
    // SNAPSHOT of the original duration agreed at signing. Set ONCE per
    // signing (see Team::sign_player) and NEVER decremented by year-end
    // progression — the live "years remaining" counter is derived from
    // `contract.exp_year` via `years_left(current_year)` below. Read this
    // ONLY when you specifically want "how long was the deal they signed"
    // (e.g. UI snapshots). For any "are they expiring soon?" / "how many
    // years are left?" question, use `years_left(year)` instead.
    int  contract_years = 1;
    // Contractual / signed role. Stamped at signing (or re-signing) by
    // Team::sign_player + Team::resign_player. Distinct from `primary_role`,
    // which is the spawn-time attribute-derived natural role. The contract
    // role is what the player was HIRED FOR — the role identity locked
    // into the deal — and is read by UI for "your contract role" surfaces
    // and by the negotiation evaluator (a player accepting the wrong role
    // takes a role-fit penalty). Defaults to Role::Count = "not stamped"
    // so legacy data and FA-pool players still behave correctly: callers
    // and the UI fall back to primary_role when contract_role == Count.
    // Role transitions during a contract must go through an explicit user
    // or AI decision — they're never auto-derived from attribute drift.
    Role contract_role = Role::Count;
    int  years_unsigned = 0;
    bool is_retired = false;

    Contract       contract{};

    // Years remaining on the current contract as of `current_year`. Returns 0
    // for FAs/Retired and for players whose contract has expired. Use this
    // EVERYWHERE you display "years left" or gate on "contract running out" —
    // the live counter is `exp_year`, NOT `contract_years` (which is the
    // signed-duration snapshot).
    int years_left(int current_year) const noexcept {
        if (team_name == "Free Agent" || team_name == "Retired") return 0;
        return std::max(0, contract.exp_year - current_year + 1);
    }

    FreeAgentMood  mood{};
    std::vector<std::pair<int,int>> salary_log;
    ProgressionSnapshot last_snapshot{};   // baseline for next monthly tick

    int    career_kills = 0, career_deaths = 0, career_assists = 0;
    int    career_fb = 0, career_fd = 0, career_survivals = 0, career_trades = 0, career_rounds = 0;
    int    career_matches = 0;
    double career_rating_total = 0.0;
    // League Leaders feeds — bumped in Match::play() on each match's
    // stat-transfer step.
    int    career_damage = 0;
    int    career_hs_hits = 0;
    int    career_rounds_with_kast = 0;
    int    career_mvps = 0;

    // === Hall of Fame milestone tracking ===
    // Bumped in Match::play stat-transfer for pro matches. Used by the
    // 4-of-9 HoF qualification check.
    int    career_max_match_kills = 0;       // most kills in a single pro match
    int    career_max_match_kd_x100 = 0;     // best K/D in a pro match (×100)
    int    career_grand_final_clutches = 0;  // last-round 1vN closures in a GF
    int    career_seasons_played = 0;        // bumped each year-end if season_matches >= 8
    bool   ever_top20_solo = false;          // marked when peak ladder rank hits top 20

    // Career-peak OVR tracker — updated during monthly progression. Drives the
    // two-stage retirement check's "former MVP refuses to retire at 90% of his
    // peak" catchnet: any 30+ player whose ovr() falls below 0.70 *
    // career_max_ovr force-retires regardless of the logistic stage-1 roll.
    int    career_max_ovr = 0;

    int    season_kills = 0, season_deaths = 0, season_assists = 0;
    int    season_fb = 0, season_fd = 0, season_survivals = 0, season_trades = 0, season_rounds = 0;
    int    season_rounds_with_kast = 0;  // binary per-round KAST flag, summed
    int    season_matches = 0;
    double season_rating_total = 0.0;

    // === IGL strategic impact ===
    // Accumulated across pro matches. Each pro match for which the player is
    // an IGL contributes a 0..2.5 igl_impact value derived from their team's
    // strategic performance vs their roster strength. Highly stat-decoupled
    // from raw frags so IGLs can win awards even when their K/D is mediocre.
    //
    // igl_impact_total accumulates over the entire career. igl_impact_season
    // resets at year-end inside save_history_and_progress (alongside the
    // existing season_* counters). igl_match_count is career-cumulative and
    // doubles as the denominator for per-match averages.
    double igl_impact_total       = 0.0;   // career total (never reset)
    double igl_impact_season      = 0.0;   // resets at year-end
    int    igl_match_count        = 0;     // pro matches as IGL (career)
    double igl_impact_season_peak = 0.0;   // best single-match impact this season

    // === MVP overhaul support ===
    // Pro matches played in playoff/LAN events this season — REGIONALS /
    // MASTERS / CHAMPIONS / "Final" / "Playoffs". Used by the new multi-
    // factor MVP score to reward players who show up in pressure moments.
    // Bumped at the Match.cpp end-of-match block (event-name keyed) and
    // reset at year-end alongside the other season_* counters.
    int    season_pressure_matches = 0;

    // Per-season clutch points (1vN closures). Aggregated at end-of-match
    // from PlayerMatchStats::clutch_pts so the MVP formula has a "shows up
    // in clutch moments" signal independent of raw rating. Reset year-end.
    int    season_clutch_pts = 0;

    int  solo_mmr = 1100;
    int  peak_mmr = 1100;
    int  solo_kills = 0, solo_deaths = 0, solo_wins = 0, solo_losses = 0;

    Role primary_role = Role::Duelist;
    std::vector<const Agent*> agent_pool;

    // Per-agent mastery — only populated for agents the player has actually
    // played in pro matches. Drives the "signature agent" identity and feeds
    // a bonus into update_agent_pool() so veterans stay on their main even
    // if attribute drift starts favouring something else.
    std::unordered_map<std::string, AgentMastery> agent_mastery;

    // === Import adaptation tracker ===
    // When a player signs with a team in a region OUTSIDE their nationality
    // region, this counter starts at 24 (months — i.e. 2 seasons) and ticks
    // down at year-end inside save_history_and_progress(). While > 0 the
    // player suffers a small chemistry/communication penalty in Match.cpp
    // (import_chemistry_penalty + per-duel consistency hit). Hitting 0 means
    // fully integrated. Resets to 24 if the player moves to ANOTHER non-native
    // region; resets to 0 the moment they return to their native region.
    int    adaptation_months_remaining = 0;

    // Tracked separately so save_history_and_progress() can detect roster
    // transitions year-over-year: which region was the player on at the last
    // tick? If their CURRENT region differs from native AND from this prev
    // value, we reset adaptation to 24.
    std::string last_seen_team_region;

    // Per-map mastery — populated only for maps the player has actually
    // played in pro matches. Drives "comfort map" identification and feeds
    // per-map agent selection in Team.cpp via map_mastery_bonus(map_name).
    std::unordered_map<std::string, MapMastery> map_mastery;

    std::vector<std::string> badges;
    std::vector<std::string> awards;
    std::vector<MatchHistoryEntry> history;
    // Replayable saved matches (last ~10 each). Stored as shared_ptrs so all
    // 10 participants reference one recording without dup memory.
    std::vector<RecordedMatchPtr>  pro_match_history;
    std::vector<RecordedMatchPtr>  solo_match_history;
    std::unordered_map<std::string, MapStat> map_stats;

    double ovr() const noexcept;
    double kd_ratio() const noexcept;
    double avg_match_rating() const noexcept;
    double solo_kd() const noexcept;
    std::string rank_name() const;
    double kast() const noexcept;            // season KAST %
    double entry_success() const noexcept;   // season entry-success %

    // Career rate helpers — feed the League Leaders leaderboards.
    double career_adr() const noexcept;       // damage / round
    double career_apr() const noexcept;       // assists / round
    double career_kpr() const noexcept;       // kills / round
    double career_kast_pct() const noexcept;  // 0..100
    double career_hs_pct() const noexcept;    // 0..100
    double career_entry_pct() const noexcept; // 0..100  (career fb / (fb+fd))

    int get_rating(const Agent& agent, const GameMap& map) const noexcept;
    int get_transfer_value() const noexcept;

    // === Role compatibility (2026-05-28 role-lock + contract-role work) ===
    //
    // role_fit_score(r) returns a 0.0..1.0 compatibility based on the
    // player's attribute profile (NOT on `primary_role` — a Duelist with
    // strong Comm/Intel/Util can still score 0.70 as an Initiator). The
    // player's own `primary_role` always scores 1.00 (natural fit).
    //
    // Used by:
    //  * sign / re-sign UI to surface which roles a player can plausibly
    //    play before offering them a contract at that role
    //  * evaluate_resign_offer to penalize bad role offers (a Duelist will
    //    refuse a Controller deal unless heavily overpaid)
    //  * AI signing paths can read this when considering off-role bids
    //
    // Pure read function, no rng, no state mutation.
    double role_fit_score(Role r) const;

    // Coarse band over role_fit_score, in plain English for UI chips:
    //   1.00       -> "Natural"     (primary_role exactly)
    //   >= 0.75    -> "Good fit"
    //   >= 0.55    -> "Possible"
    //   >= 0.35    -> "Stretch"
    //   <  0.35    -> "Mismatch"
    const char* role_fit_verdict(Role r) const;

    // 4-arg overload — when `team_ctx != nullptr` the player's Desire
    // multiplier (`desire_salary_mult`) is applied on top of the existing
    // player-only signals so the asking price reflects who's asking. Pass
    // nullptr for a context-free baseline (legacy call sites).
    Contract gen_contract(int current_year,
                          bool randomize_amount,
                          bool randomize_exp,
                          const Team* team_ctx) const;
    // 3-arg legacy overload — delegates to the 4-arg form with
    // team_ctx == nullptr so existing call sites keep working untouched.
    Contract gen_contract(int current_year,
                          bool randomize_amount = true,
                          bool randomize_exp = false) const;

    // === Re-sign offer API (Agent A, 2026-05 salary rebalance) ============
    //
    // The data the player would put on the table if their CURRENT team
    // approached them this offseason. Built once on call from team context
    // (strategy / window / memory / current roster ovr) + player Desire
    // archetype, then read by:
    //  * AI teams' resign loop (Team.cpp / GameManager.cpp) to decide
    //    re-sign vs let-walk based on `willingness` and `amount_k`.
    //  * UI re-sign modal (gui_main.cpp) to show the user what the player
    //    is asking before they accept / counter / walk.
    //
    // Fields:
    //  * `amount_k`            — the player's CURRENT asking salary.
    //  * `years`               — preferred contract length (1..4).
    //  * `willingness`         — 0..1 probability they re-sign at the
    //                            asked offer (composite of desire +
    //                            tenure + team success + prestige).
    //  * `min_acceptable_k`    — hard floor; offers below this are walked.
    //  * `max_acceptable_years`— hard ceiling on years they'll tolerate.
    //  * `explainer`           — one-line summary of WHY they want what
    //                            they want, surfaced in the UI.
    struct ResignOffer {
        int amount_k = 0;
        int years    = 0;
        double willingness = 0.0;
        int min_acceptable_k = 0;
        int max_acceptable_years = 0;
        std::string explainer;
    };

    // What this player would ask for if their current team approached them
    // for a new contract this year. Reads team context (strategy, window,
    // memory, current roster ovr) + player Desire archetype. O(1)-ish — only
    // walks the team's roster to compute avg ovr and scan award years.
    ResignOffer propose_resign_offer(const Team& team, int current_year) const;

    // Companion gate. Returns true iff:
    //   amount_k >= proposed min_acceptable_k
    //   AND years in [1, max_acceptable_years]
    //   AND !refuses_to_negotiate(amount_k, team.name)
    //   AND desire_accepts(amount_k, team)
    // Composes all existing gates so the AI / UI re-sign path doesn't have
    // to duplicate the rejection logic. Implementation now defers to
    // `evaluate_resign_offer().will_accept` — single source of truth so the
    // UI breakdown and the engine decision can never disagree.
    bool accepts_resign_offer(int amount_k, int years, const Team& team) const;

    // === Transparent negotiation breakdown ================================
    // Returns a per-factor score breakdown for an offer so the UI can show
    // the user WHY a deal is good/bad. Same calculation drives
    // `accepts_resign_offer` (score >= kResignAcceptThreshold).
    //
    // Score is monotonically NON-DECREASING in `amount_k` — increasing the
    // offer NEVER reduces acceptance probability. The only way a "higher"
    // offer can fail when a "lower" offer passed is via the contract LENGTH
    // (Mercenary refuses long deals) or an archetype hard-gate that's
    // independent of money (StabilitySeeker refuses Aggressive personality
    // teams; RingChaser refuses Rebuilders at 28+; Competitor refuses weak
    // teams in prime). Those flags are surfaced in `breakdown.labels`.
    struct ResignBreakdown {
        int base_score      = 50;  // baseline interest
        int salary_mod      = 0;   // delta vs player's ask (monotonic in offer)
        int prestige_mod    = 0;
        int contender_mod   = 0;   // window/strategy fit
        int loyalty_mod     = 0;   // tenure + Loyal archetype
        int years_mod       = 0;   // contract length vs preference
        int desire_mod      = 0;   // archetype hard gates / soft pressure
        int total           = 0;   // sum, clamped [0, 100] — acceptance probability proxy
        bool will_accept    = false;
        // One-of: "INSULTING", "WEAK", "FAIR", "STRONG", "OVERPAY".
        const char* verdict = "FAIR";
        // (label, value) factor list for UI display.
        std::vector<std::pair<std::string, int>> labels;
        // If !will_accept, the primary reason in one line. "" if accepted.
        std::string reject_reason;
    };
    ResignBreakdown evaluate_resign_offer(int amount_k, int years,
                                          const Team& team) const;

    // 4-arg overload — the offered role is part of the deal too. A Duelist
    // asked to switch to Controller will score the role fit penalty and
    // may reject regardless of money. Defaults to `primary_role` (no
    // penalty) which makes the 3-arg overload equivalent to "natural role
    // offer". See role_fit_score / role_fit_verdict.
    ResignBreakdown evaluate_resign_offer(int amount_k, int years,
                                          const Team& team,
                                          Role offered_role) const;

    // === Desire-driven negotiation modifiers ==============================
    // These three accessors are the ONLY surface the Team-side signing
    // integration (Agent A) calls on the desire system. The player's
    // `desire` field is the sole input. All three are pure read functions
    // — no state mutation, no rng.

    // Salary baseline multiplier in roughly [0.80, 1.30]. Applied by the
    // 4-arg gen_contract overload above when given a team context.
    double desire_salary_mult(const Team& team) const;

    // Hard rejection gate — returns false when the player's desire
    // archetype refuses this team at this price regardless of mood. Callers
    // should AND this with refuses_to_negotiate's mood check (note: that
    // function takes a name string and cannot reach the Team — callers must
    // invoke desire_accepts separately at signing sites).
    bool   desire_accepts(int offer_k, const Team& team) const;

    // Preferred contract length on a 0..1 scale: 0 = strongly short,
    // 1 = strongly long. Used by Agent-A's signing path to bias the
    // `years` parameter; ignored for re-sign locks.
    double desire_length_pref(const Team& team) const;

    int    amount_with_mood(int base_amount_k, std::string_view team_name) const;
    // Mood-only refusal gate. SEPARATE from desire_accepts — callers in
    // the signing path should invoke BOTH (refuses_to_negotiate &&
    // desire_accepts) and proceed only when neither rejects. The signature
    // takes a string (not a Team*), so no Team-aware logic lives here.
    bool   refuses_to_negotiate(int offer_k, std::string_view team_name) const;
    double mood_for(std::string_view team_name) const;
    void   bump_mood(std::string_view team_name, double delta);
    void   decay_mood(double delta = 0.015);
    void   decay_demands();

    void generate_badges();
    void update_agent_pool();
    void check_dynamic_badges();
    void save_history_and_progress(int year, std::string_view team_placement = "N/A");
    void apply_attribute_delta(Attr a, int delta);

    // Mastery hook — called by Match.cpp at the end-of-match stat-transfer
    // step (ONCE per (player, match), pro matches only — not solo Q, not
    // friendly). `match_rating` is the VLR-style rating the engine computes
    // for this player; `year` should be the current world year (Match.cpp
    // can use current_world_year() if no other source is handy).
    void record_agent_performance(const std::string& agent_name,
                                  double match_rating,
                                  int year);

    // Returns the player's "signature" agent — highest mastery score
    // weighted by both match count and avg rating. Empty string if the
    // player has no agent with at least 5 pro matches logged.
    std::string signature_agent() const;

    // Mastery -> pool-score bonus (capped). Public so the GUI can show
    // "Mastery: +X.X" when inspecting a player's pool. Returns 0 if no
    // history with the named agent.
    double mastery_bonus_for(const std::string& agent_name) const;

    // Update map mastery from a finished match. Called by Match.cpp once per
    // pro match (skipped for solo Q + friendly).
    void   record_map_performance(const std::string& map_name,
                                  double match_rating,
                                  int    year);

    // Mastery bonus for use in per-map agent selection. Cap ~+12. The new
    // per-map selection logic in Team.cpp consumes this so a Cypher main on
    // Lotus actually plays better than a Cypher main on Bind.
    double map_mastery_bonus(const std::string& map_name) const;

    // Highest-mastery map name. Empty string if no map has 5+ matches.
    std::string comfort_map() const;

    // Emergent playstyle label derived from attribute profile + career stats.
    // Returns one of: "Mechanical Demon", "Smart Lurker", "Passive Anchor",
    // "Aggressive Entry", "Utility Support", "Clutch Specialist",
    // "Hyper-consistent Veteran", "Momentum Player", "LAN Choker",
    // "Ice-cold Closer", "Generalist" (fallback).
    //
    // Computed live from `attributes` + `consistency` + relevant career
    // counters (career_grand_final_clutches, career_max_match_kills, etc.).
    // Pure function — no state mutation. Cheap (a few attr reads + compares).
    std::string playstyle_identity() const;

    // === Awards integrity + query helpers ===
    //
    // Hash-deduped award add. Idempotent — already-known awards are dropped at
    // the source so the year-end dedupe_awards() pass becomes belt-and-braces.
    // Maintains the awards_seen_ hash mirror.
    void add_award(const std::string& award);

    // Rebuild the awards_seen_ hash mirror from the current awards vector.
    // Call this after any EXTERNAL mutation of awards (e.g. god-mode editor)
    // so add_award() keeps dedup correctly.
    void rebuild_awards_seen();

    // Remove duplicate award strings from this player's awards list. The
    // existing pin logic in Tournament::award_event_titles already checks
    // for duplicates, but a now-fixed pre-existing bug may have caused
    // duplicates to land. This is the one-shot cleanup hook. Returns the
    // number of duplicates removed for logging.
    //
    // Stable: the first occurrence of each unique string is preserved in
    // its original position; only later duplicates are dropped.
    int dedupe_awards();

    // Robust query — string-matches on the award prefix character. Returns
    // the count of awards in each category across the player's career
    // (e.g. "[W] " = world championships). `prefix` should be the literal
    // 4-char string the pin logic emits ("[T] ", "[M] ", "[W] ", "[A] ").
    int award_count_by_prefix(const std::string& prefix) const;

    // True if the player has at least one award matching `prefix`.
    bool has_award_with_prefix(const std::string& prefix) const noexcept;

    // === Hall of Fame criterion helpers ===
    // Robust against malformed entries (use prefix matching, never raw
    // substring). Used by GameManager's HoF qualification check.
    bool ever_won_international() const noexcept;   // [M] or [W]
    bool ever_won_role_award()    const noexcept;   // [A] *of the Year* excluding MVP/IGL
    bool ever_won_mvp()           const noexcept;   // [A] MVP YYYY

    // === Attribute aging classes (drives per-attr progression curves) =====
    //
    // Each Attr belongs to exactly one of three aging classes. Mechanical
    // attrs peak early and decline sharply; Game-IQ attrs climb late and age
    // gracefully; Athletic attrs sit in the middle. See attr_aging_class()
    // for the mapping table.
    enum class AttrAgingClass : std::uint8_t { Mechanical, GameIQ, Athletic };
    static AttrAgingClass attr_aging_class(Attr a) noexcept;

    // Two-stage retirement check.
    //   Stage 1: logistic on (age, potential, recent rating, seasons played).
    //   Stage 2: force-retire if 30+ and ovr() < 0.70 * career_max_ovr.
    // years_unsigned >= 4 still hard-retires elsewhere; this returns true if
    // the player SHOULD retire this off-season per the new probabilistic rule.
    bool should_retire(int current_year) const;

    // Run a 30-day progression pass. Inputs:
    //   coach: optional team coach (provides dev_chance_mult)
    //   year/day: world-time at the tick (recorded into the report)
    // Returns a summary of the changes for UI display.
    ProgressionReport apply_monthly_progression(const class Coach* coach,
                                                 int year, int day);

    // === Read-only career aggregators (UI + GameManager) ===================
    //
    // These are pure inspectors — no mutation, no I/O. Safe to call from any
    // thread that has a stable read-only view of the Player. Empty awards /
    // history / pro_match_history => sensible zero-defaults (no UB).

    // Trophy + accolade rollup. Counts derive from `awards` via prefix match
    // (so the existing tournament/award pinning format `[X] ... YYYY` is
    // honoured exactly). `all_titles` is the deduplicated, ordered list of
    // every award string — convenient for the player profile "Trophy Case"
    // section in the UI.
    struct TrophySummary {
        int regional    = 0;   // count of "[T] Regional Champ YYYY"
        int masters     = 0;   // count of "[M] Masters Champ YYYY"
        int worlds      = 0;   // count of "[W] World Champion YYYY"
        int role_awards = 0;   // [A] Duelist/Initiator/Controller/Sentinel oTY
        int mvps        = 0;   // [A] MVP YYYY
        int igl_oty     = 0;   // [A] IGL of the Year YYYY
        int total_trophies() const noexcept { return regional + masters + worlds; }
        std::vector<std::string> all_titles;
    };
    TrophySummary trophy_summary() const;

    // Chronological narrative entries — debut / transfers / award wins /
    // standout seasons / retirement. Used by the Player Profile timeline.
    struct CareerEvent {
        int         year = 0;
        std::string label;
        // One of: "debut", "transfer", "milestone", "award", "trophy",
        // "retirement". Determines the icon/color the UI picks.
        std::string category;
    };
    std::vector<CareerEvent> career_timeline() const;

    // One row of "best season" data for the season-history highlight reel.
    // matches < 8 means "didn't play enough to qualify" — best_season() and
    // top_n_seasons() filter on the 8-match gate before ranking.
    struct SeasonHighlight {
        int    year     = 0;
        double rating   = 0.0;
        int    matches  = 0;
        int    kills    = 0;
        int    deaths   = 0;
        int    assists  = 0;
        std::string team_name;
        std::vector<std::string> awards_that_year;
    };
    // Highest-rating qualifying season. Returns SeasonHighlight{year=0,...}
    // if no season meets the 8-match threshold.
    SeasonHighlight best_season() const;
    // Top-N qualifying seasons sorted by rating desc. n <= 0 => empty.
    std::vector<SeasonHighlight> top_n_seasons(int n) const;

    // Head-to-head record vs another player. Walks pro_match_history and
    // tallies only matches where this player was on the OPPOSING team from
    // `other` (teammate matches are excluded). Self vs self => empty record.
    struct HeadToHeadRecord {
        int    wins                = 0;
        int    losses              = 0;
        int    matches             = 0;
        int    kills_for           = 0;   // this player's kills, summed
        int    kills_against       = 0;   // other player's kills, summed
        double avg_rating_when_facing = 0.0;
    };
    HeadToHeadRecord head_to_head(const Player& other) const;

private:
    void apply_badge(std::string_view name);

    // Hash mirror of `awards` for O(1) dedup inside add_award(). Synced via
    // add_award + rebuild_awards_seen + dedupe_awards. Skipped during direct
    // .push_back / .emplace_back — those paths should be migrated to
    // add_award() to keep the mirror live.
    std::unordered_set<std::string> awards_seen_;

    // Internal helpers for monthly progression — kept here so the
    // implementation file can describe the deltas to the public API.
    struct PerfBuckets {
        // Normalised [0,1] signals indicating which attributes the player
        // was *actually using* during the window. Drives targeted growth.
        double aim_signal       = 0.0;
        double headshot_signal  = 0.0;
        double entry_signal     = 0.0;
        double clutch_signal    = 0.0;
        double utility_signal   = 0.0;
        double survival_signal  = 0.0;
        double comm_signal      = 0.0;
    };
    PerfBuckets compute_perf_buckets(int new_kills,  int new_deaths,
                                     int new_fb,     int new_fd,
                                     int new_assists,int new_survivals,
                                     int new_rounds, int new_trades,
                                     double avg_rating) const;
};

using PlayerPtr = std::shared_ptr<Player>;

// `deep_potential` (default true): run the 20-sim Monte-Carlo to set
// `potential`. Pass false for low-stakes filler (solo Q ladder fill, mass
// background population) — saves ~2400 progression ticks per player. The
// player's `potential` falls back to a cheap heuristic when false.
PlayerPtr generate_player(int min_age, int max_age, std::string_view region,
                          bool deep_potential = true);
PlayerPtr generate_rookie(std::string_view region);

// === Canonical roster position ===========================================
// The 4 CORE gameplay positions on a Valorant team:
//   Duelist / Controller / Sentinel / Initiator.
//
// IGL is NOT a position — it's a LEADERSHIP designation (is_igl flag) that
// overlays whichever of these 4 the player is on. So a team has e.g. a
// "Controller IGL" or an "Initiator IGL" (most common), with the IGL flag
// orthogonal to the Position enum.
//
// Flex is ALSO NOT a position — it's a SUB-ROLE OVERLAY flag (is_flex)
// similar to IGL. A Flex player is whichever of the 4 core positions their
// primary_role dictates, with a wildcard agent-pool overlay. Core
// composition needs (1D + 1C + 1S + 1I) are unchanged by is_flex.
//
// The Position is derived from the Player's primary_role ONLY, not stored
// directly. is_flex and is_igl are INDEPENDENT overlays consulted nowhere
// inside position_of(). See position_of() for the derivation rules.
enum class Position : std::uint8_t {
    Duelist = 0,
    Controller,
    Sentinel,
    Initiator,
    Count
};

const char* position_name(Position p) noexcept;

// Derive the player's POSITION from primary_role alone.
//   primary_role == Duelist               -> Position::Duelist
//   primary_role == Controller            -> Position::Controller
//   primary_role == Sentinel              -> Position::Sentinel
//   primary_role == Initiator             -> Position::Initiator
//   fallback                              -> Position::Initiator
//
// is_igl is NOT consulted — IGL is an orthogonal LEADERSHIP overlay.
// is_flex is NOT consulted — Flex is an orthogonal SUB-ROLE overlay.
// Pure read function, no state mutation.
Position position_of(const Player& p) noexcept;

}  // namespace vlr
