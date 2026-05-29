#pragma once

#include "Agent.h"
#include "Coach.h"
#include "Common.h"
#include "Player.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace vlr {

enum class Personality : std::uint8_t { Aggressive, Tactical, Budget, Balanced };

// Team-level comp identity. Distinct from `personality` (locker-room temper)
// and `strategy` (front-office direction). Biases agent picks across all
// maps — e.g. a DoubleDuelistTeam will lean DoubleDuelist even on maps
// where the meta wants DoubleSentinel. Set at team construction.
enum class CompIdentity : std::uint8_t {
    Balanced,           // default; follows the map's primary preference
    AggressiveDive,     // duelist-heavy, fast tempo
    UtilityHeavy,       // initiator/controller-heavy, methodical
    DoubleDuelistTeam,  // hyper aggressive, always DoubleDuelist
    StructuredMacro,    // sentinel-heavy, slow, biases DoubleSentinel
    FlexExperimental    // off-meta picks, weights player comfort over fit
};

const char* identity_name(CompIdentity ci) noexcept;
CompIdentity pick_comp_identity_from_personality(Personality p);

// Visual logo glyph for the team banner / bracket cards / scoreboard chip.
// Drawn procedurally by `LogoArt.cpp` (Agent B) — this enum is purely an
// identifier the renderer dispatches on. Order is stable; new entries MUST
// be added before `Count` so existing logo-hash assignments don't shift.
enum class LogoShape : std::uint8_t {
    Shield, Hexagon, Diamond, Circle, Triangle, Star, LightningBolt,
    Crown, Wolf, Eagle, Phoenix, Dragon, Wave, Mountain, Sun,
    Crosshair, Sword, Anchor, Flame, Skull, Compass, Tree,
    Count
};

struct ScoutPrefs {
    int youth = 5, mechanics = 5, smarts = 5, aggressiveness = 5;
    int flexibility = 5, clutch = 5, potential = 5, experience = 5;
};

// Where this team sits in its competitive lifecycle. Drives multi-year
// timeline coherence in candidate scoring (auto_fill_roster + snake draft)
// and tunes mid-season + offseason aggression / import appetite. Recomputed
// at year-end inside GameManager::run_end_of_year AFTER update_org_memory
// but BEFORE commit_strategy_with_inertia.
//
//   Opening : young, rising core — patient development, low cut churn
//   Open    : prime years — contend now, steady consistency premium
//   Closing : last 1-2y of a core's prime — desperate, willing to gamble
//   Closed  : rebuild — old + declining + expired contracts
enum class TeamWindow : std::uint8_t {
    Opening, Open, Closing, Closed
};

const char* team_window_name(TeamWindow w) noexcept;

// Chemistry pair key. Canonicalized via min/max pointer in record_chemistry
// so the unordered map key for (a,b) collides with (b,a) — eliminates the
// "do I look up (a,b) or (b,a)?" bug. Hashed by XOR of the canonical (a,b)
// pointer hashes.
//
// Value (stored as the unordered_map value type) is a rolling EMA centered
// on 0 with alpha = 0.15; clamped to [-2.0, +2.0]. Updated by Agent B from
// Match.cpp via Team::record_chemistry(); decayed 5% per year and pruned of
// stale (off-roster) entries inside Team::update_org_memory at year-end.
struct ChemKey {
    Player* a = nullptr;
    Player* b = nullptr;
    bool operator==(const ChemKey& o) const noexcept {
        return a == o.a && b == o.b;
    }
};
struct ChemKeyHash {
    std::size_t operator()(const ChemKey& k) const noexcept {
        return std::hash<Player*>{}(k.a) ^ (std::hash<Player*>{}(k.b) << 1);
    }
};

struct TeamHistoryEntry {
    int year;
    int wins;
    int losses;
};

class Team : public std::enable_shared_from_this<Team> {
public:
    Team(std::string name, long long budget, std::string region);

    std::string name;
    std::string tag;                // 3-letter ALL-CAPS abbreviation (e.g. "FNX")
    long long   budget;
    std::string region;
    std::string home_country;       // English country name
    std::string home_country_iso;   // 2-letter code, used for the team flag
    std::string home_city;          // city the org is based out of

    // Visual identity used in scoreboards / brackets / banners. RGB ints
    // (0..255). Procedurally seeded from name hash at world init.
    int color_primary_r = 200, color_primary_g = 30,  color_primary_b = 30;
    int color_accent_r  = 240, color_accent_g  = 200, color_accent_b  = 80;

    // Logo glyph rendered on bracket cards / scoreboard chips. Assigned
    // deterministically from a hash of `name` at construction so a given
    // team always shows the same shape across runs (and so the snake-draft
    // re-order doesn't reshuffle visuals). Drawn by `LogoArt.cpp`.
    LogoShape logo_shape = LogoShape::Shield;

    // AI strategy classification — drives recruitment, contract offers,
    // mid-season decisions. Set in GameManager::initialize_world based on
    // budget / age profile / coach personality and refreshed yearly.
    enum class Strategy : std::uint8_t {
        Contender,         // wins-now, signs proven stars
        Rebuilding,        // young talent + potential
        Bridge,            // retooling between cycles
        BudgetRoster,      // cheap, opportunistic
        DevelopmentFocus,  // rookies + coaching priority
        WinNow,            // overpay for veterans
        TalentFarm         // grows talent to flip
    };
    Strategy strategy = Strategy::Bridge;
    // Previous-year strategy. Drives Strategy Inertia: a probabilistic
    // transition matrix decides whether the team actually adopts the new
    // strategy that `classify_team_strategy` suggests, or sticks with what
    // it had. Most strategic transitions are resisted — orgs don't pivot
    // their entire philosophy on a coin flip. See
    // `commit_strategy_with_inertia` for the transition probabilities.
    //
    // On a fresh world this is left as `Strategy::Bridge`; the first year
    // run_end_of_year seeds it with the classify result so year-1 behaviour
    // exactly matches the pre-inertia world.
    Strategy previous_strategy = Strategy::Bridge;

    // Championship-window classification. Recomputed at year-end (in
    // GameManager::run_end_of_year, AFTER update_org_memory and BEFORE
    // commit_strategy_with_inertia) via the free `compute_team_window`
    // helper. Drives timeline_fit_score (multi-year candidate scoring),
    // mid-season cut-chance multipliers, and import-appetite biases.
    // Defaults to Opening on fresh worlds — Year 1 GameManager seeds it
    // explicitly during init so behavior is deterministic.
    TeamWindow window = TeamWindow::Opening;

    int prestige = 50;       // 0..99, affects FA willingness to sign
    int sponsorship_k = 800; // annual sponsorship income, $K

    // === Organizational Memory =============================================
    //
    // Hidden rolling metrics that evolve year-over-year. Orgs LEARN from
    // outcomes: rookies that worked, imports that flopped, the cost of
    // overpaying, the price of impatience. Each metric is a signed int
    // clamped to [-100, +100]. Updated by `update_org_memory` at year-end
    // (called from GameManager::run_end_of_year AFTER awards have been
    // computed but BEFORE the roster cleanup pass, so per-player career
    // stats from THIS season are still observable on every roster slot).
    //
    // Decays toward 0 by 3% per year to keep the memory from accumulating
    // forever — orgs forget. Drives downstream signing + retention AI:
    //  * `team_import_appetite` reads `import_success`
    //  * `run_initial_snake_draft` candidate scoring reads
    //    rookie_success / import_success / veteran_success /
    //    financial_discipline
    //  * `ai_manage_roster` cut-aggression reads `stability_culture`
    //  * `run_mid_season_replacements` cut chance reads `stability_culture`
    //
    // Fresh worlds start with all metrics at 0 so year-1 simulation
    // matches pre-memory behaviour exactly.
    struct OrgMemory {
        int rookie_success       = 0; // rookies/young signings panned out?
        int import_success       = 0; // imports paid off?
        int veteran_success      = 0; // veteran signings paid off?
        int financial_discipline = 0; // history of overpaying or underpaying?
        int stability_culture    = 0; // hire/fire culture (high = patient)
        int star_dependency      = 0; // built around carries vs balanced cores
    };
    OrgMemory memory;

    // Cuts-this-year tally. Incremented inside `release_player`, consumed
    // and reset by `update_org_memory`. Drives `stability_culture` delta.
    int cuts_this_year_ = 0;

    // Year-over-year memory update. Walks the current roster to score:
    //   * rookie_success      — players signed by THIS team within last 3
    //                            years and age <= 22 at signing
    //   * import_success      — same, but filtered to cross-region signings
    //   * veteran_success     — same, but age >= 28 at signing
    //   * financial_discipline — payroll vs tier baseline
    //   * stability_culture   — # of cuts this year (cuts_this_year_)
    //   * star_dependency     — top-1 player rating vs average of other 4
    //
    // Each metric is bumped/decremented per its score, decayed 3% toward
    // 0, then clamped to [-100, +100]. Called from
    // GameManager::run_end_of_year AFTER `compute_season_awards` (so the
    // [T]/[M]/[W] awards are pinned and visible on roster players) but
    // BEFORE the roster cleanup pass (so the players who were on the
    // roster THIS season are still attached and inspectable).
    void update_org_memory(int current_year);

    // === Chemistry storage ================================================
    //
    // Per-team unordered map of (Player*, Player*) -> rolling EMA chemistry
    // score. Key canonicalized inside record_chemistry so (a,b) collides
    // with (b,a) — only one map entry per pair. Value range roughly
    // [-2.0, +2.0]; positive = synergy, negative = clash.
    //
    // Cross-pack contract:
    //  * Agent B (Match.cpp) calls `record_chemistry(a, b, delta)` after
    //    each pro match — positive deltas for trades-saved, kills-assisted,
    //    won-clutches-together; negative for whiff-trades, abandoned-
    //    teammates, etc. EMA alpha = 0.15, clamp [-2, +2].
    //  * Agent C (gui_main.cpp) calls `chemistry_between(a, b)` for the
    //    UI "synergy line" between two players, and `top_chemistry_pairs(n)`
    //    for the team profile's "core synergy pairs" ribbon.
    //  * Year-end decay (5%) + off-roster prune happen inside
    //    `update_org_memory` so the map stays bounded.
    //
    // Stored as a member (not a free static) so reset_world's team-
    // destruction path naturally drops every chemistry edge.
    std::unordered_map<ChemKey, double, ChemKeyHash> chemistry;

    // Record (or update) the chemistry edge between players `a` and `b`.
    // Skips no-ops where a == b OR either is null. Canonicalizes the key
    // by ordering the pair so the SMALLER pointer is stored first; this
    // makes the map order-insensitive in (a,b) vs (b,a). Applies the
    // EMA update `prev * 0.85 + delta * 0.15` then clamps to [-2, +2].
    void record_chemistry(const Player& a, const Player& b, double delta);

    // Read-only chemistry lookup. Returns 0.0 if no edge exists or if
    // either arg is null / equal. Symmetric in (a, b).
    double chemistry_between(const Player& a, const Player& b) const;

    // Return the top-N positive-chemistry pairs as (a, b, value) tuples.
    // Sorted DESCENDING by value (positive synergy first). Deterministic
    // ordering — ties broken by pointer address. Caller is expected to
    // skip / filter entries whose value is below their own threshold.
    std::vector<std::tuple<Player*, Player*, double>>
        top_chemistry_pairs(int n) const;

    Personality personality = Personality::Balanced;
    CompIdentity comp_identity = CompIdentity::Balanced;
    CompPlan    target_comp{};
    ScoutPrefs  scout_prefs{};

    std::vector<PlayerPtr> roster;
    CoachPtr               head_coach;
    int wins = 0, losses = 0;
    int phase_wins = 0, phase_losses = 0;
    std::vector<TeamHistoryEntry> history;

    double ovr() const;

    void sign_player(const PlayerPtr& p, int years = 2, int current_year = 0);
    // Role-aware overload (2026-05-28). Stamps Player::contract_role so
    // downstream UI can show "your contract role: X" and so the negotiation
    // evaluator can apply the role-fit penalty consistently. The plain
    // 3-arg overload above keeps backward compat and defaults the contract
    // role to the player's `primary_role` at signing time (i.e. natural
    // fit). Pass `intended_role = Role::Count` to defer to primary_role.
    void sign_player(const PlayerPtr& p, int years, int current_year,
                     Role intended_role);
    void release_player(const PlayerPtr& p);

    // Extend an existing rostered player's contract in-place. Replaces their
    // current contract fields without going through release+sign (which would
    // drop them off the roster + reset chemistry). Returns true on success.
    //
    // Caller is responsible for affordability + acceptance checks; this just
    // commits the deal. The team's budget is debited by the new amount in $K.
    bool resign_player(const PlayerPtr& p, int years, int amount_k, int current_year);
    // 5-arg overload — also updates Player::contract_role. Used when an
    // extension explicitly re-negotiates the role too (e.g. the player
    // accepted a role shift during the new deal). Pass Role::Count to
    // leave the prior contract_role unchanged.
    bool resign_player(const PlayerPtr& p, int years, int amount_k,
                       int current_year, Role new_role);

    // Fair-market-value estimate for `p` if WE were the bidding team. Cheap
    // wrapper around Player::gen_contract with team context applied so the
    // amount reflects this team's desire_salary_mult and the league baseline.
    // Surfaced by the UI (Agent C) on the re-sign / negotiate panel so the
    // user can see what the player "should" cost on the open market.
    int market_value_estimate(const Player& p) const;

    // STRICT one-IGL-per-team enforcement. Call after ANY roster
    // mutation. If the starting 5 has 0 IGLs, promotes the player with
    // the highest `igl_candidate_score` (mental composite biased by
    // role — Controllers/Initiators preferred, Sentinels OK, Duelists
    // strongly penalised) and applies the IGL stat shift. If 2+ IGLs
    // are present, keeps the highest-score one and demotes the rest
    // (reverting the shift via the same helper).
    //
    // Hard Duelist veto: if the top-scored candidate is a Duelist, they
    // are only promoted when their pure mental_score >= 0.80 ("elite
    // mind" gate). Otherwise the next-best non-Duelist wins. If every
    // starter is a Duelist (degenerate edge case) the rule falls back
    // to "highest Duelist promoted regardless".
    //
    // Idempotent — safe to call repeatedly.
    void enforce_one_igl();

    // STRICT at-least-one-Flex-per-team enforcement. Flex is NOT a
    // Position any more — it's a sub-role overlay flag (like is_igl)
    // that sits on top of whatever primary_role the player has.
    // Canonical starting 5: 1 Duelist + 1 Controller + 1 Initiator +
    // 1 Sentinel + a flexible 5th (secondary I/C, a Flex player, or
    // utility-heavy role). Exactly one of those 5 carries the is_igl
    // overlay; at least one carries the is_flex overlay.
    //
    // INDEPENDENT of is_igl. A player can be both is_flex AND is_igl
    // (the rare "Flex IGL"). No mutual exclusion is enforced here.
    //
    // Insert-only behaviour: if the starting 5 already has >=1
    // is_flex, this is a no-op. If 0 is_flex, the best generalist is
    // promoted (is_flex = true) — preferring non-IGL candidates.
    // Multiple Flex players in the starting 5 are allowed and NEVER
    // demoted; Flex is an overlay, not a slot.
    //
    // No attribute shift on promotion (Flex is positional/identity-
    // only, not stat-altering — unlike is_igl).
    //
    // Idempotent — safe to call repeatedly.
    void enforce_one_flex();

    int  total_payroll_k() const;
    void pay_payroll(int year);

    // `current_year` is forwarded to sign_player so the new contract's
    // `exp_year` is set correctly. Pass 0 ONLY for the world-bootstrap path
    // where the year isn't known yet (Player ctor's initial gen_contract
    // baseline still applies). Every gameplay-time call MUST pass year.
    void scout_top_fas(std::vector<PlayerPtr>& top_fas, int current_year = 0);
    void auto_fill_roster(std::vector<PlayerPtr>& free_agents, int current_year = 0);

    CompTag pick_best_comp_for_roster() const;
    void    refresh_target_comp();

    struct ResolvedTeam {
        double team_ovr = 0.0;
        std::unordered_map<Player*, const Agent*> chosen_agents;
        // Tag of the comp this resolution actually produced. Used by
        // Series.cpp to record per-map history for in-series adaptation.
        CompTag chosen_tag = CompTag::DoubleInitiator;
    };

    // Backwards-compat single-map selection. Defers to build_round_selection
    // with no series context (so map-1-of-a-series behaviour matches a
    // standalone match).
    ResolvedTeam get_team_overall_and_agents(const GameMap& map, bool is_solo_q = false) const;

    // Full per-map selection used by Match.cpp. `is_team1` flags whether we
    // are team1 in the prior MapResultEntry rows (drives which prior_*
    // column is "ours"). `prior_results` may be null for non-series matches
    // — in that case adaptation is skipped and we behave like a clean map 1.
    ResolvedTeam build_round_selection(const GameMap& map,
                                       bool is_team1,
                                       const std::vector<MapResultEntry>* prior_results,
                                       bool is_solo_q = false) const;

    void save_history(int year);
    // `current_year` lets the cut logic use `Player::years_left(year)` as
    // the authoritative "contract expired?" check (the legacy live counter
    // `contract_years` was removed in favour of an `exp_year`-derived view).
    void ai_manage_roster(std::vector<PlayerPtr>& free_agents, int current_year);

    // Decide contract length for `p` based on team context + player
    // attributes. THE single source of truth for "how many years should
    // we offer?" across every AI sign path (snake draft, auto_fill,
    // scout_top_fas, mid-season replacements, re-sign loop, market
    // competition, full offseason pass). Returns 1..4. Never returns a
    // value the player would reject (caps at the player's
    // max_acceptable_years derived from desire_length_pref).
    //
    // Factors layered into the decision:
    //   * Player desire length preference (base bucket 1..4)
    //   * Team window  — Closing/Closed shorter, Opening longer
    //   * Team strategy — WinNow/Bridge short, Rebuild/DevFocus long
    //   * Player age — young = long, 30+ = short
    //   * Potential gap (potential − ovr) — dev contracts get longer
    //   * Organizational memory financial_discipline
    //   * Budget headroom — broke teams can't commit long
    //
    // This replaces every prior `rng().irange(1, 3)` and `c.exp_year -
    // year + 1` callsite so every signing has explainable reasoning.
    int decide_contract_years(const Player& p, int current_year) const;

    // Deep offseason GM pass — multi-stage roster reconstruction that goes
    // BEYOND `ai_manage_roster`'s simple "drop bad + cascade-fill":
    //   1. Per-starter value assessment + per-role market scan
    //   2. Targeted CUT-AND-REPLACE if an FA upgrade exists at the same role
    //      that the candidate accepts and the team can afford
    //   3. Backstop auto_fill cascade for any remaining vacancies
    //   4. Bench depth signings (slot 6/7) if budget headroom > $200K
    //   5. Logs every move via `log` so the user can see what their front
    //      office did when they left OFFSEASON unattended.
    //
    // Used by the user-team OFFSEASON fallback (GameManager::advance_day)
    // and any other path that wants a complete GM pass. AI teams' yearly
    // `ai_manage_roster` call STILL runs the shallow path during year-end
    // because the re-sign loop + market competition + cascade-fill already
    // delivers their roster construction. This deeper variant is reserved
    // for the "build from scratch / rescue an incomplete roster" case.
    void ai_full_offseason_pass(std::vector<PlayerPtr>& free_agents,
                                int current_year,
                                std::vector<std::string>& log);

    // === Trophy case + dynasty tier (read-only aggregators) ================
    //
    // Storage is intentionally simple: GameManager calls record_trophy() when
    // a tour finishes (one append per championship). finals_history_ is left
    // as a forward-compat hook — GameManager Agent A may wire it later when a
    // clean finalist-detection point is available. Until then total_finals
    // returns 0 and the ChampionEra/Contender dynasty tiers are unreachable.

    struct TrophyCase {
        int regional_titles = 0;
        int masters_titles  = 0;
        int world_titles    = 0;
        int total_finals    = 0;   // 0 until finals_history_ is populated
        // Chronological (year, event_name) pairs for the trophy ribbon UI.
        std::vector<std::pair<int, std::string>> ordered;
    };
    TrophyCase trophy_case() const;

    // Append a trophy to this team's history. Idempotent — if the exact
    // (year, event_name) tuple already exists we silently skip so safe to
    // call from both `play_tournament_round` and `force_finish_stale_tournaments`
    // belt-and-braces.
    void record_trophy(int year, const std::string& event_name);

    enum class DynastyTier : std::uint8_t { None, Contender, ChampionEra, Dynasty };
    // Classifies a team's recent dominance in the trailing 5-year window
    // [current_year - 4, current_year]:
    //   Dynasty     : 3+ titles in that window
    //   ChampionEra : 1-2 titles + 2+ finals appearances in that window
    //   Contender   : 0 titles + 2+ finals appearances in that window
    //   None        : anything else
    // ChampionEra / Contender will be unreachable until finals_history_ is
    // populated by GameManager — Dynasty (titles-only) still works today.
    DynastyTier dynasty_tier(int current_year) const;

private:
    // (year, event_name) — appended by record_trophy().
    std::vector<std::pair<int, std::string>> trophy_history_;
    // (year, event_name) — appended by GameManager when a finals appearance
    // is detected. Empty until that hook is wired.
    std::vector<std::pair<int, std::string>> finals_history_;
};

using TeamPtr = std::shared_ptr<Team>;

const char* team_strategy_name(Team::Strategy s) noexcept;
const char* team_strategy_blurb(Team::Strategy s) noexcept;

// Pick the best initial strategy for a team given its budget profile,
// average roster age, coach personality, and prestige. Called at world
// init and again at each year-end so teams adapt to roster changes.
Team::Strategy classify_team_strategy(const Team& t) noexcept;

// === Strategy Inertia =====================================================
//
// Probabilistic strategy transition. Replaces the direct write of
// classify_team_strategy() at year-end. Reads `t.previous_strategy` and
// the `suggested` new strategy and returns the strategy the team actually
// commits to this year. Most transitions are inertial — orgs resist
// snapping into a new philosophy on a single year's evidence.
//
// If `suggested == t.previous_strategy` the function is a no-op (returns
// it unchanged). Otherwise a roll against the per-pair transition matrix
// decides: success -> commit to `suggested`, failure -> commit to
// `t.previous_strategy` (resist the change). Either way
// `t.previous_strategy` is updated to the committed value before return.
//
// Year-1 compatibility: GameManager seeds `previous_strategy` to the
// classify result on the first end-of-year tick (see run_end_of_year), so
// the first call always passes the "suggested == previous_strategy"
// fast-path and the world behaves identically to the pre-inertia build.
Team::Strategy commit_strategy_with_inertia(Team& t,
                                            Team::Strategy suggested);

// Returns a 0.0..~0.30 "import appetite" score: how aggressively this
// team should pursue foreign signings on a given decision. Heavy-tailed
// — most teams land <0.10, only a handful per region clear 0.25.
//
// Factors weighted: budget tier, prestige, strategy (Contender/WinNow
// boost; Rebuilding-with-trophy boost), recent international award on a
// rostered player, role scarcity in own region (when target_role given
// + a regional FA pool is supplied), TacticalGenius/Innovator coach,
// region multiplier (Americas 1.10 / EMEA 1.00 / Pacific 0.85).
//
// `target_role` may be `Role::Count` to skip the role-scarcity factor.
// `regional_fa_pool` may be null — same effect as Role::Count.
double team_import_appetite(const Team& t,
                            Role target_role,
                            const std::vector<PlayerPtr>* regional_fa_pool = nullptr);

// Hash a team's name to deterministic primary/accent RGB colors. Idempotent.
void seed_team_colors(Team& t);

// Compute the championship-window classification for `t` as of
// `current_year`. Pure, read-only. Cascade (first match wins):
//   Opening : avg_age < 23 AND avg_potential >= 75 AND avg_ovr < 78
//   Open    : avg_age in [22, 27] AND avg_ovr >= 75 AND avg_contract_years_left >= 1.5
//   Closing : (avg_age >= 28 AND avg_ovr >= 75)
//             OR (avg_age >= 26 AND avg_ovr >= 78 AND avg_contract_years_left < 1.5)
//   Closed  : avg_age >= 28 AND avg_ovr < 75
//             OR (avg_potential < 65 AND avg_ovr < 72)
//   else    : Open if healthy mid-tier (avg_ovr >= 70), else Opening
//
// Empty / sub-5 rosters fall through to Opening.
TeamWindow compute_team_window(const Team& t, int current_year);

// Multi-year candidate scoring. Returns a composite designed to be ADDED
// to existing strategy-based draft / auto-fill scores. Pure, O(1),
// read-only. See Team.cpp implementation for the per-window scoring
// table.
//
// Components:
//   CurrentValue       = candidate.ovr()                              w 1.00
//   FutureValue        = (potential - ovr) * youth_factor             w 0.50
//   ContractEfficiency = (perceived_value - contract.amount_k)         w 0.40
//   TimelineFit        = piecewise window × age band × potential band w 0.80
double timeline_fit_score(const Team& t, const Player& candidate);

}  // namespace vlr
