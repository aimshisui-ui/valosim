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
    double base_rating_offset = 0.12;
    double rw_kpr = 0.45;
    double rw_kd  = 0.30;
    double rw_adr = 0.0015;
    double rw_apr = 0.20;
    double aggro_feed_punishment = 1.5;
    double smart_clutch_bonus    = 0.1;
    int    max_imports           = 2;
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

// 3-letter ALL-CAPS abbreviation derived from a team's display name.
// Used for scoreboards, brackets, and bracket flags.
std::string make_team_tag(const std::string& name);

inline constexpr std::array<const char*, 3> kRegions = {"Americas", "EMEA", "Pacific"};

// Absolute salary ceiling in $K (gen_contract output is clamped here even
// for legendary multipliers). Lowered from 999 to 180 in the 2026-05
// financial rebalance — see PROJECT_GUIDE.md §4.4 + §7. Keeps superstar
// salaries in believable range relative to team budgets (max ~$2M).
constexpr int kSalaryCapK   = 180;
constexpr int kSalaryFloorK = 10;

}  // namespace vlr
