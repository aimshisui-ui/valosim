#pragma once

#include "Agent.h"
#include "Coach.h"
#include "Common.h"
#include "Player.h"
#include "Scout.h"
#include "Analyst.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace vlr {

class SaveGame;   // binary save/load (SaveGame.cpp) — friended on Team for private history

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

// WS-B: a club's BRAND TAG — the human label for its computed TeamIdentity (the
// (aggression, dev_youth) quadrant, with a dynasty flavour). 7 tags = the
// complete taxonomy. Derived from compute_team_identity, never authoritative.
enum class BrandTag : std::uint8_t {
    AggressiveEntryMerchants,
    TacticalMasterminds,
    AcademyPowerhouse,
    WinNowSpenders,
    MethodicalGrinders,
    DefensiveWall,
    BalancedContenders
};
const char* brand_tag_name(BrandTag b) noexcept;
const char* brand_tag_blurb(BrandTag b) noexcept;

// Visual logo glyph for the team banner / bracket cards / scoreboard chip.
// Drawn procedurally by `LogoArt.cpp` (Agent B) — this enum is purely an
// identifier the renderer dispatches on. Order is stable; new entries MUST
// be added before `Count` so existing logo-hash assignments don't shift.
enum class LogoShape : std::uint8_t {
    Shield, Hexagon, Diamond, Circle, Triangle, Star, LightningBolt,
    Crown, Wolf, Eagle, Phoenix, Dragon, Wave, Mountain, Sun,
    Crosshair, Sword, Anchor, Flame, Skull, Compass, Tree,
    // Added after the unified-backplate logo revamp. Order is stable —
    // new entries always append before Count so name-hash assignments for
    // existing teams stay deterministic across builds.
    Fangs, Trident, CrossedSwords, Gear, Talon, Eye, Cobra, Sunburst,
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

// Transient squad-need snapshot — how well each CORE role (Duelist=0,
// Initiator=1, Controller=2, Sentinel=3) is covered by the starting 5, and
// which role is the biggest hole. Recomputed on demand; holds NO PlayerPtr,
// so it never participates in the never-free invariant. Drives the need-gated
// scouting pipeline + sharpens auto_fill_roster's role targeting.
struct RoleNeed {
    int    count[4]    = {0, 0, 0, 0};   // starters covering each core role
    double best_ovr[4] = {0, 0, 0, 0};   // best starter OVR in each role
    double need[4]     = {0, 0, 0, 0};   // 0..1 hole severity per role
    Role   most_needed = Role::Count;    // single biggest hole (Count == none)
    double most_need_score = 0.0;
};

// Coach-archetype roster lean — how the head coach's personality tilts roster
// BUILDING (not just match-day). youth_lean>0 prefers young/high-ceiling, <0
// prefers proven veterans; risk_appetite>0 gambles on volatile high-ceiling,
// <0 prizes consistency. Both roughly [-1, +1]. Neutral {0,0} when no coach is
// bound. Until now coaches affected ONLY match synergy + player development —
// this is the hook that lets a DevelopmentCoach actually build differently
// from a Pragmatist.
struct CoachLean {
    double youth_lean    = 0.0;
    double risk_appetite = 0.0;
    // Finance lean (Workstream C4): >0 pushes the org to SPEND (win-now), <0
    // keeps it frugal. Development lean: >0 weights youth dev harder. Both ~[-1,1].
    double finance_lean  = 0.0;
    double dev_focus     = 0.0;
};

// WS-B TEAM IDENTITY — a club's playstyle fingerprint, CACHED on Team and
// refreshed yearly (NOT per-frame), parallel to `strategy`. Pure value data (no
// pointers) so it's never-free safe. Default is NEUTRAL (axes 0.5) so an
// unrefreshed team produces ZERO match tilt. aggression: 0 methodical .. 1
// aggressive; dev_youth: 0 win-now .. 1 youth-first. user_chosen marks a
// user-picked club philosophy (vs an emergent derivation).
struct TeamIdentity {
    double   aggression  = 0.5;
    double   dev_youth   = 0.5;
    CompTag  comp_lean   = CompTag::DoubleInitiator;
    BrandTag brand       = BrandTag::BalancedContenders;
    bool     user_chosen = false;
};

// Where an org sits on the rich<->poor spectrum. Recomputed at year-end from
// cash + income; drives the spend fork, fee headroom, and the finance-UI badge.
enum class WealthTier : std::uint8_t { Poor, Modest, Stable, Rich, SuperRich };
const char* wealth_tier_name(WealthTier w) noexcept;

// Season-start sponsor requirement (item 4). The user picks one of three
// offers at preseason; meeting the requirement credits a modest one-time lump
// to the club budget at year-end. Lives here (not GameManager.h) so Team can
// carry the chosen deal; GameManager's SponsorOffer struct reuses this enum.
enum class SponsorReqType : std::uint8_t {
    Placement,            // finish <= value in the league table
    WinCount,             // >= value regular-season wins
    TitleBerth,           // reach a regional playoff (placement <= cutoff)
    IndividualMilestone   // a rostered player ends the season >= 1.15 rating
};

// One completed transfer (or free signing) for the market log + finance UI.
// Mirrored onto BOTH clubs involved. from_team == "Free Agent" for a free
// signing (fee_k == 0). Never references a Player/Team pointer — pure record.
struct TransferRecord {
    int         year = 0;
    std::string player;
    std::string from_team;   // selling club, or "Free Agent"
    std::string to_team;     // buying club
    int         fee_k = 0;   // transfer fee paid buyer->seller ($K); 0 for an FA
    int         wage_k = 0;  // the new annual wage ($K)
};

class Team : public std::enable_shared_from_this<Team> {
public:
    // Save/load reads+writes trophy_history_/finals_history_ (private, no
    // setters) so a restored club keeps its full silverware record.
    friend class SaveGame;

    Team(std::string name, long long budget, std::string region);

    std::uint64_t id = 0;   // stable, unique id (stamped in ctor; save handle)

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

    // === Anti-dynasty signals (POD) ===
    int core_wage_pressure_pct = 0;  // YoY % wage escalation on the starting 5 (finance UI)
    int titles_total_cached    = 0;  // trophy_case reg+mas+world total, stamped year-end

    // === Season sponsor (item 4) — the user's chosen deal + tracking state ===
    // Stamped by GameManager::choose_sponsor at preseason; evaluated + credited
    // at year-end by settle_sponsor. Strings/ints only (never-free safe).
    std::string    sponsor_name;
    SponsorReqType sponsor_req_type = SponsorReqType::Placement;
    int            sponsor_req_value = 0;
    int            sponsor_reward_k  = 0;
    bool           sponsor_active    = false;   // false = no/declined sponsor
    bool           sponsor_credited  = false;   // year-end payout already paid?

    // === Finance projection (FM-depth economy) =============================
    // Set at year-end by GameManager::project_team_finances — a READ-ONLY signal
    // for the wage envelope + the finance dashboard. All in $K. last_revenue_k
    // is the income actually booked last finance pass (real components, EMA'd in
    // B4); projected_income_k / committed_payroll_k / wage_envelope_k drive what
    // the AI can responsibly SPEND next season. wealth_tier buckets rich<->poor.
    int        last_revenue_k      = 0;
    int        projected_income_k  = 0;
    int        committed_payroll_k = 0;
    int        wage_envelope_k     = 0;
    int        net_transfer_k      = 0;   // net fees in(+)/out(-) this season, $K
    WealthTier wealth_tier         = WealthTier::Stable;
    // Club-to-club transfer history (most-recent-first, capped). Mirrored onto
    // both clubs by pay_transfer_fee. Pure records — no Player/Team pointers.
    std::vector<TransferRecord> transfer_log_;

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
    TeamIdentity identity{};   // WS-B: cached playstyle fingerprint (refreshed yearly)
    CompPlan    target_comp{};
    ScoutPrefs  scout_prefs{};

    // User-set per-map comp override (map name -> CompTag). A map absent from
    // this means "Auto" (the engine's decide_desired_tag picks). Coarse
    // archetype-level control; superseded by agent_override below when set.
    std::unordered_map<std::string, CompTag> comp_override;

    // User-set FULL per-map agent comp (map name -> exactly 5 agent names).
    // This is the granular control from the Strategy tab: the user picks the
    // exact 5 agents to field on a map and build_round_selection assigns the
    // starting 5 to them by best fit. Takes precedence over comp_override.
    // Only the user populates this; AI teams leave it empty.
    std::unordered_map<std::string, std::vector<std::string>> agent_override;

    // Per-map PREP (Scouting&Match-Prep increment E). level 0=None, 1=Standard,
    // 2=Heavy. Drives a BOUNDED, pure-upside per-side match tilt (prep_match_tilt
    // in Match.cpp, clamped [1.0,1.03], scaled by the user's analyst quality) that
    // applies ONLY to the USER's competitive matches (set via Match::set_prep_
    // context). AI teams leave this empty -> tilt 1.0 -> AI/dynasty sim unchanged.
    struct MapPrep { int level = 0; };
    std::unordered_map<std::string, MapPrep> map_prep;

    std::vector<PlayerPtr> roster;
    CoachPtr               head_coach;
    ScoutPtr               head_scout;   // WS-A: managed scout staff (parallels head_coach)
    AnalystPtr             head_analyst; // Scouting&Match-Prep: 3rd staff slot (opposition reports + per-map prep)
    int wins = 0, losses = 0;
    int phase_wins = 0, phase_losses = 0;
    // Rolling recent LEAGUE results (1 = win, 0 = loss), newest pushed to the
    // back, capped at 10. Drives the Standings "last-5 form" sparkline (Group F).
    std::vector<std::uint8_t> recent_results;
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

    // Benching: swap a starter (roster[0..4]) with a bench player (roster[5..])
    // by exchanging their vector positions. The benched player stays rostered
    // and PAID (still under contract) — they just stop playing. Re-runs the
    // IGL/Flex enforcers so the new starting 5 is legal. Returns false if
    // either player isn't found or both are on the same side of the bench line.
    bool swap_roster_slots(const PlayerPtr& benchee, const PlayerPtr& starter);

    // Merit-based starting 5: bench a slumping/weaker starter for a clearly
    // better same-role (or high-fit flex) reserve. Protects young prospects,
    // needs a form sample, and uses a hysteresis margin so it doesn't thrash.
    // Routes every move through swap_roster_slots (IGL/Flex stay legal). The AI
    // calls this each offseason (and mid-season) so it actually starts its best 5.
    void optimize_starting_five(int current_year);

    // === FM-depth roster intelligence =====================================
    // Squad-need snapshot over the starting 5 (transient; no PlayerPtr held).
    // A role with 0 coverage is a full hole (need 1.0); a covered-but-weak role
    // scales up to ~0.6. most_needed is the single biggest hole.
    RoleNeed compute_role_need() const;

    // Head-coach archetype -> roster-building lean (youth vs proven, risk vs
    // consistency). Neutral {0,0} when no coach is bound. Reads head_coach only.
    CoachLean coach_lean() const;

    // Need-gated, upgrade-gated valuation of a SCOUT TARGET (a tier-2 riser or
    // top solo-Q FA) for this team THIS year. Returns a positive score only
    // when the candidate (a) fills a genuine role hole, (b) clears role-fit,
    // (c) is a real upgrade over the weakest same-role incumbent, and (d) is
    // affordable. Returns 0.0 otherwise (do NOT sign). Strategy + coach lean +
    // window all tilt the score. Pure/read-only.
    // `sharpness` (P3.1, default 1.0 == strict no-op) = the AI manager's quality,
    // driven by world_difficulty(). >1.0 lowers the upgrade bar (harder AI poaches
    // more decisively); <1.0 raises it (easier AI passes marginal upgrades). At
    // 1.0 every term below is byte-identical to the legacy behaviour, so the
    // difficulty --dynasty (which runs at 1.0) is unaffected.
    // out_fill_role (optional): when the call returns a positive score, receives
    // the ROLE this candidate is being scouted to fill — the candidate's natural
    // role if that is the need, else the most-needed role for a cross-role flex.
    // The caller pins the signing to this role (4-arg sign_player) so the scouted
    // hole actually closes instead of the player reverting to their natural role.
    double score_scout_target(const Player& cand, const RoleNeed& need,
                              int current_year, double sharpness = 1.0,
                              Role* out_fill_role = nullptr) const;

    // === FM-depth economy =================================================
    // Transfer FEE this club would owe to sign `p` from their current club.
    // 0 for a free agent (wages only); a contracted player ($K, [kMinFeeK,
    // kMaxFeeK]) sized off value, years left, and a scarcity premium. Pure.
    int transfer_fee_for(const Player& p, int current_year) const;

    // Execute the MONEY side of a transfer: debit this (buyer) by fee_k, credit
    // `seller` (may be null for a free agent), and mirror a TransferRecord onto
    // both clubs' transfer_log_. The caller does release_player + sign_player —
    // this only moves cash + records (never-free safe). Bumps net_transfer_k.
    void pay_transfer_fee(const TeamPtr& seller, const Player& p,
                          int current_year, int fee_k);

    // Can this club RESPONSIBLY add `wage_k` of new annual wage plus a `fee_k`
    // one-off, without breaching its wage envelope or the insolvency floors?
    // Always true for a club with a healthy envelope; the AI signing paths gate
    // on this so teams plan their future finances instead of spending to zero.
    bool within_wage_envelope(int wage_k, int fee_k) const;

    // === User Make-Offer support (Group D) ===============================
    // This club's asking price (a transfer-fee threshold) to part with one of
    // its OWN players `p`: the market fee scaled by the player's importance to
    // THIS roster (a protected top-2 core costs a big premium; bench depth is
    // cheap), the club's Strategy (a TalentFarm flips talent; a Contender guards
    // its core), and cash need. Pure. A bid >= this is accepted by will_sell.
    int  sell_threshold_k(const Player& p, int current_year) const;
    bool will_sell(const Player& p, int offered_fee_k, int current_year) const;

    // Bucket this org rich<->poor from cash + income. Drives the spend fork +
    // the finance-UI wealth badge. Recomputed each year-end.
    WealthTier compute_wealth_tier() const;

    // Strategy + wealth driven offseason SPEND behavior (Workstream A4): a rich
    // Contender BUYS proven upgrades (paying fees); a poor Rebuilder banks cash,
    // develops youth, and SELLS surplus risers. Falls back to ai_full_offseason_pass.
    void run_spend_fork(std::vector<PlayerPtr>& free_agents, int current_year,
                        std::vector<std::string>& log);

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

    // Append a FINALS APPEARANCE (champion OR runner-up reached the grand final)
    // to this team's history. Idempotent on (year, event_name) like
    // record_trophy, but does NOT bump titles_with_org (reaching a final isn't
    // winning it). Populating finals_history_ unlocks the previously-dead
    // ChampionEra / Contender branches of dynasty_tier().
    void record_finals_appearance(int year, const std::string& event_name);

    // How many trophies this club won in a given game year (reads trophy_history_).
    // Used by the coach market to evolve coach reputation from real silverware.
    int titles_in_year(int yr) const {
        int n = 0;
        for (const auto& tp : trophy_history_) if (tp.first == yr) ++n;
        return n;
    }

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

// WS-B: derive a club's TeamIdentity from its read-only state (personality,
// comp identity, head-coach archetype, coach-lean, roster shape, region,
// strategy, dynasty tier). Pure + rng-free — the single derivation point,
// recomputed at world-init and each year-end like the strategy.
TeamIdentity compute_team_identity(const Team& t);

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
