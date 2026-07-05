#pragma once

#include <array>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace vlr {

enum class Attr : std::uint8_t {
    Aim,
    Headshot,
    Entry,
    Utility,
    GameSense,
    Clutch,
    DecisionMaking,
    Intelligence,
    Aggressiveness,
    Positioning,
    Communication,
    Adaptability,
    Leadership,
    Reaction,
    SpikeHandle,
    Anchor,

    // === Micro-attributes (added round 9) ===========================
    // Movement quality: counter-strafe accuracy, peek timing, jiggle-peek
    // shots. Boosts duel power outside of pistol rounds.
    Movement,
    // Crosshair placement: where the crosshair sits while moving through
    // the map. Drives headshot chance and first-shot accuracy on entries.
    CrosshairPlacement,
    // Lurking: passive map control / info denial. Bumps team map control
    // gain even on rounds where the player isn't the one peeking.
    Lurking,
    // Awping: dedicated operator/sniper skill. When the team eco supports
    // an OP buy, the highest-Awping player on the side gets a power boost.
    Awping,
    // Stamina: maintaining the level late in a long series. Reduces variance
    // in matches that drag past 30 rounds.
    Stamina,
    // === IGL-specific (only meaningful for IGL-flagged players) =====
    // Mid-round calling: in-round adjustments and re-rotates.
    MidRoundCalling,
    // Economy management: when to force-buy vs save vs full-buy.
    EconomyMgmt,
    // Anti-strat: pre-game prep and reading opponent tendencies.
    AntiStrat,
    Count
};

constexpr std::size_t kAttrCount = static_cast<std::size_t>(Attr::Count);
using Attributes = std::array<int, kAttrCount>;

inline int& at(Attributes& a, Attr k) noexcept { return a[static_cast<std::size_t>(k)]; }
inline int  at(const Attributes& a, Attr k) noexcept { return a[static_cast<std::size_t>(k)]; }

const char* attr_name(Attr a) noexcept;
Attr attr_from_str(std::string_view s) noexcept;

class Rng {
public:
    Rng();
    explicit Rng(std::uint64_t seed);
    void seed(std::uint64_t seed);

    int    irange(int lo, int hi);
    double drange(double lo, double hi);
    double uniform();
    bool   chance(double p);

    template <typename T>
    const T& choice(const std::vector<T>& v) {
        return v[static_cast<std::size_t>(irange(0, static_cast<int>(v.size()) - 1))];
    }
    template <typename T>
    void shuffle(std::vector<T>& v) {
        std::shuffle(v.begin(), v.end(), engine_);
    }

    int weighted_index(const std::vector<int>& weights);
    int weighted_index(const std::vector<double>& weights);

    std::mt19937_64& engine() noexcept { return engine_; }

private:
    std::mt19937_64 engine_;
};

Rng& rng();

constexpr int kClampLo = 1;
constexpr int kClampHi = 99;

inline int clamp_attr(int v) noexcept {
    return v < kClampLo ? kClampLo : (v > kClampHi ? kClampHi : v);
}
template <typename T>
inline T clamp_v(T v, T lo, T hi) noexcept {
    return v < lo ? lo : (v > hi ? hi : v);
}

struct SimConfig {
    double pwr_aim       = 1.20;
    double pwr_headshot  = 0.80;
    double pwr_entry     = 0.50;
    double pwr_decision  = 0.60;
    double eco_advantage = 0.35;
    // Duel win-probability SHAPE. The resolver raises each duelist's power to
    // this exponent before normalising: P(p1)=p1^k/(p1^k+p2^k). k=1 is the old
    // linear model where a 1.5x skill edge won only ~60%; k>1 makes the clearly
    // better player win more decisively so high-skill players post a realistic
    // K/D/rating spread instead of every attribute washing into noise. Kept
    // MODERATE (1.5) because match/round outcomes are already favorite-dominated
    // by the momentum/map-control feedback loops (a steeper k mainly widens the
    // per-player stat spread, not who wins). See smoke test [27].
    double duel_exponent = 1.15;
    // Economy as a RELATIVE gun-tier matchup: per-tier (pistol<SMG<light<rifle<op)
    // power swing. full-buy vs full-eco (gap 4) -> ~+24%/-24%; full-vs-full -> 0.
    double gun_advantage = 0.06;
    double base_rating_offset = 0.12;
    double rw_kpr = 0.45;
    double rw_kd  = 0.30;
    double rw_adr = 0.0015;
    double rw_apr = 0.20;
    double aggro_feed_punishment = 1.5;
    double smart_clutch_bonus    = 0.1;
    int    max_imports           = 2;
    // === FA-market realism (WS-A INC-0) =================================
    // A cross-region ("international") free agent only belongs on the open
    // market if they're an ESTABLISHED player — a real international move is a
    // marquee event, not a flood of unknown foreign teenagers (who instead come
    // up through the soloq -> Tier-3 pipeline). A player clears the bar with
    // enough career matches OR a high enough OVR.
    int    intl_fa_min_matches   = 120;
    double intl_fa_min_ovr       = 70.0;
    // Cap displayed internationals to this fraction of the FA list so they stay
    // rare even when several clear the bar.
    double intl_fa_market_share  = 0.15;
    // === WS-B team identity / region meta (all bounded) ==================
    // A region's share of a team's aggression axis (region BIASES, never
    // DICTATES). And a hard ceiling on the region-attributable share of the
    // per-side match scalar (region actually rides through identity, but this
    // caps any direct region term).
    double region_identity_weight = 0.20;
    double region_match_tilt_cap  = 0.020;
};

const SimConfig& config();
SimConfig&       mutable_config();

std::string get_rank_from_mmr(int mmr);

std::vector<std::string> load_lines(const std::string& path, std::size_t cap = 10000);
const std::vector<std::string>& global_names();
const std::vector<std::string>& global_teams();
std::string take_unused_name();
std::string take_team_name();

// Reset name + team-name de-dup tracking so a fresh world can be booted
// without re-launching the exe (called from `GameManager::reset_world`).
// Clears both the player-name and team-name cursors / taken sets. The
// underlying name pools themselves stay loaded — only the "already
// handed out" bookkeeping is wiped.
void reset_name_caches();

// Save/load: snapshot + restore the name-uniqueness registries. The SETS are
// authoritative (take_unused_name / take_team_name re-check them on every
// draw), so restore leaves the pool cursors wherever the post-reset state put
// them — a cursor pointing at an already-used name just skips forward.
std::vector<std::string> snapshot_used_names();
std::vector<std::string> snapshot_taken_team_names();
void restore_name_registries(std::vector<std::string> used, std::vector<std::string> taken);

// === Stable entity IDs (save/load foundation) ============================
// Every persistent entity (Player/Team/Coach) stamps a unique, monotonically
// increasing id from this single global source at construction. IDs are never
// reused within a run, so they're safe as stable cross-references for the
// (future) save system AND as robust deep-link handles today. On load, the
// world is rebuilt and reset_entity_id_counter() is set past the max restored
// id so freshly-generated entities never collide with loaded ones.
std::uint64_t next_entity_id();
void reset_entity_id_counter(std::uint64_t at_least);

// 3-letter ALL-CAPS abbreviation derived from a team's display name.
// Used for scoreboards, brackets, and bracket flags.
std::string make_team_tag(const std::string& name);

inline constexpr std::array<const char*, 3> kRegions = {"Americas", "EMEA", "Pacific"};

// === WS-B region meta (B3) — per-region playstyle profile (pure const data) ==
// A small set of RELATIVE tilts per region. `aggression` is the relative
// methodical(-)/aggressive(+) lean fed (weighted) into a team's aggression axis;
// the rest flavour the comp/agent meta + the readouts. Indexed by RegionId.
enum class RegionId : std::uint8_t { Americas, EMEA, Pacific, Count };
struct RegionMeta {
    const char* brand;        // short style label, e.g. "Aggressive Duelist"
    double      aggression;   // relative tilt ~[-0.5, +0.5]
    double      utility;      // utility/methodical lean
    double      duelist_bias; // duelist-heavy meta
    double      flash_indiv;  // individual-flash lean
};
inline constexpr std::array<RegionMeta, 3> kRegionMeta = {{
    /* Americas */ { "Individual Flash",    0.10, 0.00,  0.05,  0.30 },
    /* EMEA     */ { "Methodical Utility", -0.35, 0.30, -0.10, -0.05 },
    /* Pacific  */ { "Aggressive Duelist",   0.45, -0.10, 0.30,  0.05 },
}};
RegionId region_id_from_name(const std::string& region) noexcept;   // unknown -> Americas
const RegionMeta& region_meta(const std::string& region) noexcept;
// Human "style clash" note when two DIFFERENT regions meet; "" if same region.
std::string region_clash_note(const std::string& rA, const std::string& rB);

// Absolute salary ceiling in $K (gen_contract output is clamped here even
// for legendary multipliers). Lowered from 999 to 180 in the 2026-05
// financial rebalance — see PROJECT_GUIDE.md §4.4 + §7. Keeps superstar
// salaries in believable range relative to team budgets (max ~$2M).
constexpr int kSalaryCapK   = 180;
constexpr int kSalaryFloorK = 10;

// === FM-depth economy tuning (transfer market + finance projection) ========
// A player UNDER CONTRACT never moves club-to-club for free: the buyer pays a
// fee to the selling club, clamped to this band ($K). Free agents cost wages
// only (no fee). See Team::transfer_fee_for / pay_transfer_fee.
constexpr int kMinFeeK = 10;
constexpr int kMaxFeeK = 600;
// AI insolvency guards: an AI signing is refused if it would push cash below
// kInsolvencyFloorBudget ($) OR the projected next-season balance below
// kInsolvencyFloorK ($K). The USER's team is never auto-gated (it manages its
// own finances unless user_auto_manage).
constexpr long long kInsolvencyFloorBudget = -100000LL;
constexpr int       kInsolvencyFloorK      = -100;
// Dynamic-sponsorship clamps ($K/yr): sponsorship drifts (EMA) with prestige +
// results + buzz but stays inside this band so rich/poor diverge WITHOUT a
// runaway or a death spiral.
constexpr int kSponsorFloorK = 150;
constexpr int kSponsorCeilK  = 1400;

}  // namespace vlr
