#include "SaveGame.h"

#include "GameManager.h"   // pulls League/SoloQ/Team/Player/Coach/Scout/Analyst/Common
#include "Names.h"         // snapshot_used_handles / restore_used_handles

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace vlr {

namespace {

// === Format constants =====================================================
constexpr char          kMagic[4]       = {'V', 'L', 'R', 'S'};
constexpr std::uint32_t kVersion        = 1;
// Trailing canary — a partially written / truncated file fails the load
// instead of silently restoring half a world.
constexpr std::uint32_t kFooterSentinel = 0xF00DF001u;
// Sanity cap on any single count/length read from disk. A corrupt length
// must throw, not attempt a multi-GB allocation.
constexpr std::uint32_t kMaxCount       = 0x04000000u;   // 64M

// === Binary writer ========================================================
// Little-endian fixed-size primitives written raw (MSVC x64 native layout).
// No error checking per write — ofstream badbit is sticky, so one .good()
// check after the final flush covers the whole file.
struct BinW {
    std::ofstream out;
    explicit BinW(const std::string& path)
        : out(path, std::ios::binary | std::ios::trunc) {}

    void raw(const void* p, std::size_t n) {
        out.write(static_cast<const char*>(p), static_cast<std::streamsize>(n));
    }
    void u8(std::uint8_t v)   { raw(&v, sizeof v); }
    void b(bool v)            { u8(v ? std::uint8_t{1} : std::uint8_t{0}); }
    void i32(std::int32_t v)  { raw(&v, sizeof v); }
    void u32(std::uint32_t v) { raw(&v, sizeof v); }
    void i64(std::int64_t v)  { raw(&v, sizeof v); }
    void u64(std::uint64_t v) { raw(&v, sizeof v); }
    void f64(double v)        { raw(&v, sizeof v); }
    void str(const std::string& s) {
        u32(static_cast<std::uint32_t>(s.size()));
        if (!s.empty()) raw(s.data(), s.size());
    }
    void vec_str(const std::vector<std::string>& v) {
        u32(static_cast<std::uint32_t>(v.size()));
        for (const auto& s : v) str(s);
    }
    void set_str(const std::unordered_set<std::string>& s) {
        u32(static_cast<std::uint32_t>(s.size()));
        for (const auto& e : s) str(e);
    }
    void map_str_f64(const std::unordered_map<std::string, double>& m) {
        u32(static_cast<std::uint32_t>(m.size()));
        for (const auto& kv : m) { str(kv.first); f64(kv.second); }
    }
};

// === Binary reader ========================================================
// Every primitive read validates the stream and THROWS on a short read, so
// SaveGame::load's single try/catch turns any truncation/corruption into a
// clean `false` instead of propagating garbage into the world.
struct BinR {
    std::ifstream in;
    explicit BinR(const std::string& path) : in(path, std::ios::binary) {}

    void raw(void* p, std::size_t n) {
        in.read(static_cast<char*>(p), static_cast<std::streamsize>(n));
        if (!in) throw std::runtime_error("save file truncated or unreadable");
    }
    std::uint8_t u8()   { std::uint8_t v;  raw(&v, sizeof v); return v; }
    bool b()            { return u8() != 0; }
    std::int32_t i32()  { std::int32_t v;  raw(&v, sizeof v); return v; }
    std::uint32_t u32() { std::uint32_t v; raw(&v, sizeof v); return v; }
    std::int64_t i64()  { std::int64_t v;  raw(&v, sizeof v); return v; }
    std::uint64_t u64() { std::uint64_t v; raw(&v, sizeof v); return v; }
    double f64()        { double v;        raw(&v, sizeof v); return v; }
    // Count/length with a hard sanity ceiling (corruption guard).
    std::uint32_t count() {
        std::uint32_t n = u32();
        if (n > kMaxCount) throw std::runtime_error("save file corrupt (absurd count)");
        return n;
    }
    std::string str() {
        std::uint32_t n = count();
        std::string s(n, '\0');
        if (n) raw(&s[0], n);
        return s;
    }
    std::vector<std::string> vec_str() {
        std::uint32_t n = count();
        std::vector<std::string> v;
        v.reserve(n);
        for (std::uint32_t i = 0; i < n; ++i) v.push_back(str());
        return v;
    }
    std::unordered_set<std::string> set_str() {
        std::uint32_t n = count();
        std::unordered_set<std::string> s;
        s.reserve(n);
        for (std::uint32_t i = 0; i < n; ++i) s.insert(str());
        return s;
    }
    std::unordered_map<std::string, double> map_str_f64() {
        std::uint32_t n = count();
        std::unordered_map<std::string, double> m;
        m.reserve(n);
        for (std::uint32_t i = 0; i < n; ++i) {
            std::string k = str();
            m[k] = f64();
        }
        return m;
    }
};

// === Cross-reference helpers ==============================================
using PlayerMap  = std::unordered_map<std::uint64_t, PlayerPtr>;
using TeamMap    = std::unordered_map<std::uint64_t, TeamPtr>;
using CoachMap   = std::unordered_map<std::uint64_t, CoachPtr>;
using ScoutMap   = std::unordered_map<std::uint64_t, ScoutPtr>;
using AnalystMap = std::unordered_map<std::uint64_t, AnalystPtr>;

// Write a shared_ptr container as a count + id list (0 = null slot).
template <typename Vec>
void write_ids(BinW& w, const Vec& v) {
    w.u32(static_cast<std::uint32_t>(v.size()));
    for (const auto& e : v) w.u64(e ? e->id : 0);
}

std::vector<std::uint64_t> read_ids(BinR& r) {
    std::uint32_t n = r.count();
    std::vector<std::uint64_t> ids;
    ids.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) ids.push_back(r.u64());
    return ids;
}

template <typename T>
std::shared_ptr<T> resolve_id(
        const std::unordered_map<std::uint64_t, std::shared_ptr<T>>& m,
        std::uint64_t id) {
    if (id == 0) return nullptr;
    auto it = m.find(id);
    return it == m.end() ? nullptr : it->second;
}

// Resolve an id list, silently dropping unresolvable ids (defensive: with a
// well-formed save every id resolves because save() serializes every entity
// it references).
template <typename T>
std::vector<std::shared_ptr<T>> resolve_ids(
        const std::unordered_map<std::uint64_t, std::shared_ptr<T>>& m,
        const std::vector<std::uint64_t>& ids) {
    std::vector<std::shared_ptr<T>> out;
    out.reserve(ids.size());
    for (auto id : ids) {
        if (auto p = resolve_id(m, id)) out.push_back(p);
    }
    return out;
}

std::string now_stamp() {
    std::time_t t = std::time(nullptr);
    std::tm* tmv = std::localtime(&t);
    if (!tmv) return std::string();
    char buf[24];
    if (std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", tmv) == 0) return std::string();
    return std::string(buf);
}

// === Player ===============================================================
// Full state, fixed field order (matches read_player EXACTLY). Skipped on
// purpose: agent_pool (re-derived via update_agent_pool after restore),
// awards_seen_ (rebuild_awards_seen), pro/solo_match_history (replays, v1).
void write_player(BinW& w, const Player& p) {
    w.u64(p.id);
    w.str(p.name);
    w.str(p.first);
    w.str(p.last);
    w.str(p.country);
    w.str(p.country_iso);
    w.str(p.region);
    w.i32(p.age);
    w.i32(p.potential);
    w.i32(p.work_ethic);
    w.i32(p.consistency);
    w.i32(p.birth_year);
    w.str(p.birth_city);
    w.i32(static_cast<std::int32_t>(p.growth_archetype));
    w.i32(p.peak_age);
    w.i32(static_cast<std::int32_t>(p.rookie_archetype));
    w.i32(p.ambition);
    w.i32(p.loyalty);
    w.i32(p.greed);
    w.i32(p.ego);
    w.i32(p.reputation);
    w.i32(static_cast<std::int32_t>(p.archetype));
    w.i32(static_cast<std::int32_t>(p.desire));
    w.i32(p.agent_pool_size);
    w.i32(p.cross_role_min);
    w.b(p.is_igl);
    w.b(p.is_flex);
    w.i32(p.tend_play_aggressive);
    w.i32(p.tend_lurk_vs_execute);
    w.i32(p.tend_vocal);
    w.i32(p.tend_adaptive);

    // Attribute block — count-prefixed so a build with a different Attr enum
    // refuses the file instead of misreading every later byte.
    w.u32(static_cast<std::uint32_t>(kAttrCount));
    for (std::size_t i = 0; i < kAttrCount; ++i) w.i32(p.attributes[i]);
    for (std::size_t i = 0; i < kAttrCount; ++i) w.f64(p.attr_growth_carry[i]);

    w.str(p.team_name);
    w.i32(p.contract_years);
    w.i32(static_cast<std::int32_t>(p.contract_role));
    w.i32(p.years_unsigned);
    w.b(p.is_retired);

    // Contract (promised_role is a BOOL flag in Player.h, not a Role enum).
    w.i32(p.contract.amount_k);
    w.i32(p.contract.exp_year);
    w.i32(p.contract.signing_bonus_k);
    w.b(p.contract.promised_starter);
    w.b(p.contract.promised_role);
    w.b(p.contract.promise_active);

    w.map_str_f64(p.mood.per_team);
    w.f64(p.discontent);
    w.b(p.transfer_requested);
    w.i32(p.transfer_request_year);
    w.i32(p.joined_year);
    w.i32(p.tenure_years_at_org);
    w.i32(p.titles_with_org);
    w.i32(p.last_org_title_count);
    w.f64(p.restlessness);

    w.u32(static_cast<std::uint32_t>(p.salary_log.size()));
    for (const auto& e : p.salary_log) { w.i32(e.first); w.i32(e.second); }

    // ProgressionSnapshot (declared order).
    const ProgressionSnapshot& ps = p.last_snapshot;
    w.i32(ps.career_kills);   w.i32(ps.career_deaths);   w.i32(ps.career_assists);
    w.i32(ps.career_fb);      w.i32(ps.career_fd);
    w.i32(ps.career_survivals); w.i32(ps.career_trades);
    w.i32(ps.career_rounds);  w.i32(ps.career_matches);
    w.f64(ps.career_rating_total);
    w.i32(ps.solo_mmr);
    w.i32(ps.solo_wins);      w.i32(ps.solo_losses);
    w.i32(ps.solo_kills);     w.i32(ps.solo_deaths);

    w.b(p.potential_scouted);

    w.i32(p.career_kills);    w.i32(p.career_deaths);    w.i32(p.career_assists);
    w.i32(p.career_fb);       w.i32(p.career_fd);
    w.i32(p.career_survivals); w.i32(p.career_trades);   w.i32(p.career_rounds);
    w.i32(p.career_matches);
    w.f64(p.career_rating_total);
    w.i32(p.career_damage);
    w.i32(p.career_hs_hits);
    w.i32(p.career_rounds_with_kast);
    w.i32(p.career_mvps);
    w.i32(p.career_max_match_kills);
    w.i32(p.career_max_match_kd_x100);
    w.i32(p.career_grand_final_clutches);
    w.i32(p.career_seasons_played);
    w.b(p.ever_top20_solo);
    w.i32(p.career_max_ovr);

    w.i32(static_cast<std::int32_t>(p.trajectory_class));
    w.i32(p.ovr_at_last_tick);
    w.f64(p.last_form_rating);

    w.i32(p.season_kills);    w.i32(p.season_deaths);    w.i32(p.season_assists);
    w.i32(p.season_fb);       w.i32(p.season_fd);
    w.i32(p.season_survivals); w.i32(p.season_trades);   w.i32(p.season_rounds);
    w.i32(p.season_rounds_with_kast);
    w.i32(p.season_matches);
    w.f64(p.season_rating_total);
    w.i32(p.season_damage);
    w.i32(p.season_hs_hits);

    w.u32(static_cast<std::uint32_t>(p.tourn_stats.size()));
    for (const auto& kv : p.tourn_stats) {
        w.str(kv.first);
        const auto& t = kv.second;
        w.i32(t.maps); w.i32(t.matches); w.i32(t.rounds);
        w.i32(t.kills); w.i32(t.deaths); w.i32(t.assists);
        w.i32(t.fb); w.i32(t.fd); w.i32(t.survivals); w.i32(t.trades);
        w.i32(t.rounds_with_kast);
        w.i32(t.damage); w.i32(t.hs_hits);
        w.f64(t.rating_total);
    }

    w.f64(p.igl_impact_total);
    w.f64(p.igl_impact_season);
    w.i32(p.igl_match_count);
    w.f64(p.igl_impact_season_peak);
    w.i32(p.season_pressure_matches);
    w.i32(p.season_intl_matches);
    w.i32(p.season_offrole_matches);
    w.f64(p.season_offrole_rating_total);

    w.u32(static_cast<std::uint32_t>(p.season_role_maps.size()));   // 4
    for (int v : p.season_role_maps) w.i32(v);
    w.i32(p.season_clutch_pts);

    w.i32(p.solo_mmr);
    w.i32(p.peak_mmr);
    w.i32(p.solo_kills); w.i32(p.solo_deaths);
    w.i32(p.solo_wins);  w.i32(p.solo_losses);

    w.i32(static_cast<std::int32_t>(p.primary_role));

    w.u32(static_cast<std::uint32_t>(p.agent_mastery.size()));
    for (const auto& kv : p.agent_mastery) {
        w.str(kv.first);
        w.i32(kv.second.matches);
        w.f64(kv.second.avg_rating);
        w.f64(kv.second.peak_rating);
        w.i32(kv.second.last_played_year);
    }

    w.i32(p.adaptation_months_remaining);
    w.str(p.last_seen_team_region);

    w.u32(static_cast<std::uint32_t>(p.map_mastery.size()));
    for (const auto& kv : p.map_mastery) {
        w.str(kv.first);
        w.i32(kv.second.matches);
        w.f64(kv.second.avg_rating);
        w.f64(kv.second.peak_rating);
        w.i32(kv.second.last_played_year);
    }

    w.vec_str(p.badges);
    w.vec_str(p.awards);

    w.u32(static_cast<std::uint32_t>(p.history.size()));
    for (const auto& h : p.history) {
        w.i32(h.year);
        w.i32(h.age);
        w.str(h.team);
        w.f64(h.rating);
        w.f64(h.kd);
        w.f64(h.kast);
        w.str(h.placement);
        w.vec_str(h.awards);
        w.i32(h.salary_k);
        w.i32(h.matches);
        w.i32(h.kills);
        w.i32(h.deaths);
        w.i32(h.assists);
        w.i32(h.ovr);
    }

    w.u32(static_cast<std::uint32_t>(p.map_stats.size()));
    for (const auto& kv : p.map_stats) {
        w.str(kv.first);
        w.i32(kv.second.count);
        w.f64(kv.second.rating_total);
    }
}

PlayerPtr read_player(BinR& r) {
    // Pre-construction fields buffered in locals: the Player ctor needs
    // (name, age, attrs, region, potential, work_ethic, consistency), and
    // those sit interleaved with other fields in the stream.
    std::uint64_t id = r.u64();
    std::string name        = r.str();
    std::string first       = r.str();
    std::string last        = r.str();
    std::string country     = r.str();
    std::string country_iso = r.str();
    std::string region      = r.str();
    int age         = r.i32();
    int potential   = r.i32();
    int work_ethic  = r.i32();
    int consistency = r.i32();
    int birth_year  = r.i32();
    std::string birth_city = r.str();
    auto growth = static_cast<GrowthArchetype>(r.i32());
    int  peak_age = r.i32();
    auto rookie = static_cast<RookieArchetype>(r.i32());
    int ambition = r.i32();
    int loyalty  = r.i32();
    int greed    = r.i32();
    int ego      = r.i32();
    int reputation = r.i32();
    auto archetype = static_cast<Archetype>(r.i32());
    auto desire    = static_cast<Desire>(r.i32());
    int agent_pool_size = r.i32();
    int cross_role_min  = r.i32();
    bool is_igl  = r.b();
    bool is_flex = r.b();
    int tend_aggr  = r.i32();
    int tend_lurk  = r.i32();
    int tend_vocal = r.i32();
    int tend_adapt = r.i32();

    std::uint32_t attr_n = r.count();
    if (attr_n != static_cast<std::uint32_t>(kAttrCount))
        throw std::runtime_error("attribute count mismatch (save from an incompatible build)");
    Attributes attrs{};
    for (std::size_t i = 0; i < kAttrCount; ++i) attrs[i] = r.i32();
    std::array<double, kAttrCount> carry{};
    for (std::size_t i = 0; i < kAttrCount; ++i) carry[i] = r.f64();

    // Construct via the real ctor (it consumes rng + stamps a fresh id —
    // both harmless here: every field it rolls is overwritten below, the id
    // is overwritten with the saved one, and the rng engine itself is
    // restored AFTER all entity construction inside SaveGame::load).
    auto p = std::make_shared<Player>(name, age, attrs, region,
                                      potential, work_ethic, consistency);
    p->id = id;
    p->first = std::move(first);
    p->last = std::move(last);
    p->country = std::move(country);
    p->country_iso = std::move(country_iso);
    p->birth_year = birth_year;
    p->birth_city = std::move(birth_city);
    p->growth_archetype = growth;
    p->peak_age = peak_age;
    p->rookie_archetype = rookie;
    p->ambition = ambition;
    p->loyalty = loyalty;
    p->greed = greed;
    p->ego = ego;
    p->reputation = reputation;
    p->archetype = archetype;
    p->desire = desire;
    p->agent_pool_size = agent_pool_size;
    p->cross_role_min = cross_role_min;
    p->is_igl = is_igl;
    p->is_flex = is_flex;
    p->tend_play_aggressive = tend_aggr;
    p->tend_lurk_vs_execute = tend_lurk;
    p->tend_vocal = tend_vocal;
    p->tend_adaptive = tend_adapt;
    // Re-assert the saved attributes: the ctor's generate_badges() may have
    // applied badge mods on top of the array we passed in. The saved values
    // already INCLUDE the player's badge effects, so they are authoritative.
    p->attributes = attrs;
    p->attr_growth_carry = carry;

    p->team_name = r.str();
    p->contract_years = r.i32();
    p->contract_role = static_cast<Role>(r.i32());
    p->years_unsigned = r.i32();
    p->is_retired = r.b();

    p->contract.amount_k = r.i32();
    p->contract.exp_year = r.i32();
    p->contract.signing_bonus_k = r.i32();
    p->contract.promised_starter = r.b();
    p->contract.promised_role = r.b();
    p->contract.promise_active = r.b();

    p->mood.per_team = r.map_str_f64();
    p->discontent = r.f64();
    p->transfer_requested = r.b();
    p->transfer_request_year = r.i32();
    p->joined_year = r.i32();
    p->tenure_years_at_org = r.i32();
    p->titles_with_org = r.i32();
    p->last_org_title_count = r.i32();
    p->restlessness = r.f64();

    {
        std::uint32_t n = r.count();
        p->salary_log.clear();
        p->salary_log.reserve(n);
        for (std::uint32_t i = 0; i < n; ++i) {
            int y = r.i32();
            int s = r.i32();
            p->salary_log.emplace_back(y, s);
        }
    }

    ProgressionSnapshot& ps = p->last_snapshot;
    ps.career_kills = r.i32();   ps.career_deaths = r.i32();   ps.career_assists = r.i32();
    ps.career_fb = r.i32();      ps.career_fd = r.i32();
    ps.career_survivals = r.i32(); ps.career_trades = r.i32();
    ps.career_rounds = r.i32();  ps.career_matches = r.i32();
    ps.career_rating_total = r.f64();
    ps.solo_mmr = r.i32();
    ps.solo_wins = r.i32();      ps.solo_losses = r.i32();
    ps.solo_kills = r.i32();     ps.solo_deaths = r.i32();

    p->potential_scouted = r.b();

    p->career_kills = r.i32();   p->career_deaths = r.i32();   p->career_assists = r.i32();
    p->career_fb = r.i32();      p->career_fd = r.i32();
    p->career_survivals = r.i32(); p->career_trades = r.i32(); p->career_rounds = r.i32();
    p->career_matches = r.i32();
    p->career_rating_total = r.f64();
    p->career_damage = r.i32();
    p->career_hs_hits = r.i32();
    p->career_rounds_with_kast = r.i32();
    p->career_mvps = r.i32();
    p->career_max_match_kills = r.i32();
    p->career_max_match_kd_x100 = r.i32();
    p->career_grand_final_clutches = r.i32();
    p->career_seasons_played = r.i32();
    p->ever_top20_solo = r.b();
    p->career_max_ovr = r.i32();

    p->trajectory_class = static_cast<Player::TrajClass>(r.i32());
    p->ovr_at_last_tick = r.i32();
    p->last_form_rating = r.f64();

    p->season_kills = r.i32();   p->season_deaths = r.i32();   p->season_assists = r.i32();
    p->season_fb = r.i32();      p->season_fd = r.i32();
    p->season_survivals = r.i32(); p->season_trades = r.i32(); p->season_rounds = r.i32();
    p->season_rounds_with_kast = r.i32();
    p->season_matches = r.i32();
    p->season_rating_total = r.f64();
    p->season_damage = r.i32();
    p->season_hs_hits = r.i32();

    {
        std::uint32_t n = r.count();
        p->tourn_stats.clear();
        p->tourn_stats.reserve(n);
        for (std::uint32_t i = 0; i < n; ++i) {
            std::string key = r.str();
            Player::TournStatLine t;
            t.maps = r.i32(); t.matches = r.i32(); t.rounds = r.i32();
            t.kills = r.i32(); t.deaths = r.i32(); t.assists = r.i32();
            t.fb = r.i32(); t.fd = r.i32(); t.survivals = r.i32(); t.trades = r.i32();
            t.rounds_with_kast = r.i32();
            t.damage = r.i32(); t.hs_hits = r.i32();
            t.rating_total = r.f64();
            p->tourn_stats[key] = t;
        }
    }

    p->igl_impact_total = r.f64();
    p->igl_impact_season = r.f64();
    p->igl_match_count = r.i32();
    p->igl_impact_season_peak = r.f64();
    p->season_pressure_matches = r.i32();
    p->season_intl_matches = r.i32();
    p->season_offrole_matches = r.i32();
    p->season_offrole_rating_total = r.f64();

    {
        std::uint32_t n = r.count();
        if (n != static_cast<std::uint32_t>(p->season_role_maps.size()))
            throw std::runtime_error("season_role_maps size mismatch");
        for (auto& v : p->season_role_maps) v = r.i32();
    }
    p->season_clutch_pts = r.i32();

    p->solo_mmr = r.i32();
    p->peak_mmr = r.i32();
    p->solo_kills = r.i32(); p->solo_deaths = r.i32();
    p->solo_wins = r.i32();  p->solo_losses = r.i32();

    p->primary_role = static_cast<Role>(r.i32());

    {
        std::uint32_t n = r.count();
        p->agent_mastery.clear();
        p->agent_mastery.reserve(n);
        for (std::uint32_t i = 0; i < n; ++i) {
            std::string key = r.str();
            AgentMastery m;
            m.matches = r.i32();
            m.avg_rating = r.f64();
            m.peak_rating = r.f64();
            m.last_played_year = r.i32();
            p->agent_mastery[key] = m;
        }
    }

    p->adaptation_months_remaining = r.i32();
    p->last_seen_team_region = r.str();

    {
        std::uint32_t n = r.count();
        p->map_mastery.clear();
        p->map_mastery.reserve(n);
        for (std::uint32_t i = 0; i < n; ++i) {
            std::string key = r.str();
            MapMastery m;
            m.matches = r.i32();
            m.avg_rating = r.f64();
            m.peak_rating = r.f64();
            m.last_played_year = r.i32();
            p->map_mastery[key] = m;
        }
    }

    p->badges = r.vec_str();
    p->awards = r.vec_str();

    {
        std::uint32_t n = r.count();
        p->history.clear();
        p->history.reserve(n);
        for (std::uint32_t i = 0; i < n; ++i) {
            MatchHistoryEntry h;
            h.year = r.i32();
            h.age = r.i32();
            h.team = r.str();
            h.rating = r.f64();
            h.kd = r.f64();
            h.kast = r.f64();
            h.placement = r.str();
            h.awards = r.vec_str();
            h.salary_k = r.i32();
            h.matches = r.i32();
            h.kills = r.i32();
            h.deaths = r.i32();
            h.assists = r.i32();
            h.ovr = r.i32();
            p->history.push_back(std::move(h));
        }
    }

    {
        std::uint32_t n = r.count();
        p->map_stats.clear();
        p->map_stats.reserve(n);
        for (std::uint32_t i = 0; i < n; ++i) {
            std::string key = r.str();
            MapStat s;
            s.count = r.i32();
            s.rating_total = r.f64();
            p->map_stats[key] = s;
        }
        // Preserve the ctor invariant that every current map has an entry
        // (the save may predate a newly added map). emplace never clobbers.
        for (const auto& m : maps()) p->map_stats.emplace(m.name, MapStat{});
    }

    return p;
}

// === Staff (Coach / Scout / Analyst — all-public fields) ==================
void write_coach(BinW& w, const Coach& c) {
    w.u64(c.id);
    w.str(c.name);
    w.str(c.region);
    w.str(c.country);
    w.str(c.country_iso);
    w.str(c.team_name);
    w.i32(c.tactical);
    w.i32(c.development);
    w.i32(c.leadership);
    w.i32(c.experience);
    w.i32(c.salary_k);
    w.i32(c.contract_years);
    w.i32(c.contract_exp_year);
    w.i32(c.age);
    w.b(c.is_retired);
    w.i32(static_cast<std::int32_t>(c.personality));
    w.i32(c.reputation);
    w.i32(c.career_titles);
    w.i32(c.career_seasons);
    w.i32(c.career_coty);
}

CoachPtr read_coach(BinR& r) {
    std::uint64_t id = r.u64();
    std::string name = r.str();
    std::string region = r.str();
    auto c = std::make_shared<Coach>(name, region);
    c->id = id;
    c->country = r.str();
    c->country_iso = r.str();
    c->team_name = r.str();
    c->tactical = r.i32();
    c->development = r.i32();
    c->leadership = r.i32();
    c->experience = r.i32();
    c->salary_k = r.i32();
    c->contract_years = r.i32();
    c->contract_exp_year = r.i32();
    c->age = r.i32();
    c->is_retired = r.b();
    c->personality = static_cast<CoachPersonality>(r.i32());
    c->reputation = r.i32();
    c->career_titles = r.i32();
    c->career_seasons = r.i32();
    c->career_coty = r.i32();
    return c;
}

void write_scout(BinW& w, const Scout& s) {
    w.u64(s.id);
    w.str(s.name);
    w.str(s.region);
    w.str(s.country);
    w.str(s.country_iso);
    w.str(s.team_name);
    w.i32(s.judgement);
    w.i32(s.network);
    w.i32(s.projection);
    w.i32(s.experience);
    w.i32(s.salary_k);
    w.i32(s.contract_years);
    w.i32(s.contract_exp_year);
    w.i32(s.age);
    w.b(s.is_retired);
    w.i32(static_cast<std::int32_t>(s.personality));
    w.i32(s.reputation);
    w.i32(s.career_finds);
    w.i32(s.career_seasons);
}

ScoutPtr read_scout(BinR& r) {
    std::uint64_t id = r.u64();
    std::string name = r.str();
    std::string region = r.str();
    auto s = std::make_shared<Scout>(name, region);
    s->id = id;
    s->country = r.str();
    s->country_iso = r.str();
    s->team_name = r.str();
    s->judgement = r.i32();
    s->network = r.i32();
    s->projection = r.i32();
    s->experience = r.i32();
    s->salary_k = r.i32();
    s->contract_years = r.i32();
    s->contract_exp_year = r.i32();
    s->age = r.i32();
    s->is_retired = r.b();
    s->personality = static_cast<ScoutPersonality>(r.i32());
    s->reputation = r.i32();
    s->career_finds = r.i32();
    s->career_seasons = r.i32();
    return s;
}

void write_analyst(BinW& w, const Analyst& a) {
    w.u64(a.id);
    w.str(a.name);
    w.str(a.region);
    w.str(a.country);
    w.str(a.country_iso);
    w.str(a.team_name);
    w.i32(a.tactical_read);
    w.i32(a.opponent_insight);
    w.i32(a.prep);
    w.i32(a.experience);
    w.i32(a.salary_k);
    w.i32(a.contract_years);
    w.i32(a.contract_exp_year);
    w.i32(a.age);
    w.b(a.is_retired);
    w.i32(static_cast<std::int32_t>(a.personality));
    w.i32(a.reputation);
    w.i32(a.career_reports);
    w.i32(a.career_seasons);
}

AnalystPtr read_analyst(BinR& r) {
    std::uint64_t id = r.u64();
    std::string name = r.str();
    std::string region = r.str();
    auto a = std::make_shared<Analyst>(name, region);
    a->id = id;
    a->country = r.str();
    a->country_iso = r.str();
    a->team_name = r.str();
    a->tactical_read = r.i32();
    a->opponent_insight = r.i32();
    a->prep = r.i32();
    a->experience = r.i32();
    a->salary_k = r.i32();
    a->contract_years = r.i32();
    a->contract_exp_year = r.i32();
    a->age = r.i32();
    a->is_retired = r.b();
    a->personality = static_cast<AnalystPersonality>(r.i32());
    a->reputation = r.i32();
    a->career_reports = r.i32();
    a->career_seasons = r.i32();
    return a;
}

}  // namespace

// ==========================================================================
// === SaveGame::save =======================================================
// ==========================================================================
bool SaveGame::save(const GameManager& gm, const std::string& path,
                    std::string* err) {
    auto fail = [&](const std::string& m) {
        if (err) *err = m;
        return false;
    };

    // v1 gate: a live tournament holds mid-bracket Series state that v1 does
    // not serialize. Refuse BEFORE touching the disk so nothing is written.
    if (!gm.active_tournaments.empty())
        return fail("Cannot save while a tournament is in progress - finish the bracket first.");

    // ---- Collect entity tables (each entity exactly once, deduped by id) ----
    // Teams first (rosters are the primary player source), then players and
    // staff from every owning container. The never-free invariant means every
    // referenced entity is still alive; collecting from ALL reference sites
    // (leagues, ladders, HoF, favorites, watchlist, awards, FA/retired pools)
    // guarantees every id written later resolves on load.
    std::vector<TeamPtr> teams;
    std::unordered_set<std::uint64_t> team_seen;
    auto add_team = [&](const TeamPtr& t) {
        if (t && team_seen.insert(t->id).second) teams.push_back(t);
    };
    for (const auto& kv : gm.tier_leagues_)
        for (const auto& lg : kv.second) {
            if (!lg) continue;
            for (const auto& t : lg->teams()) add_team(t);
            for (const auto& t : lg->past_champions()) add_team(t);
        }
    for (const auto& kv : gm.intl_qualifiers_)
        for (const auto& t : kv.second) add_team(t);
    for (const auto& pr : gm.last_promo_rel_) {
        for (const auto& t : pr.promoted) add_team(t);
        for (const auto& t : pr.relegated) add_team(t);
        add_team(pr.ascension_champion);
    }
    add_team(gm.user_team);

    std::vector<PlayerPtr> players;
    std::unordered_set<std::uint64_t> player_seen;
    auto add_player = [&](const PlayerPtr& p) {
        if (p && player_seen.insert(p->id).second) players.push_back(p);
    };
    for (const auto& t : teams)
        for (const auto& p : t->roster) add_player(p);
    for (const auto& kv : gm.solo_qs)
        if (kv.second)
            for (const auto& p : kv.second->global_ladder()) add_player(p);
    for (const auto& p : gm.hall_of_fame) add_player(p);
    for (const auto& p : gm.favorite_players) add_player(p);
    for (const auto& e : gm.watchlist_) add_player(e.player);
    auto add_award_players = [&](const std::vector<GameManager::SeasonAward>& v) {
        for (const auto& a : v) {
            add_player(a.winner);
            for (const auto& f : a.finalists) add_player(f);
        }
    };
    add_award_players(gm.awards_history);
    add_award_players(gm.last_season_awards);

    std::vector<CoachPtr> coaches;
    std::unordered_set<std::uint64_t> coach_seen;
    auto add_coach = [&](const CoachPtr& c) {
        if (c && coach_seen.insert(c->id).second) coaches.push_back(c);
    };
    std::vector<ScoutPtr> scouts;
    std::unordered_set<std::uint64_t> scout_seen;
    auto add_scout = [&](const ScoutPtr& s) {
        if (s && scout_seen.insert(s->id).second) scouts.push_back(s);
    };
    std::vector<AnalystPtr> analysts;
    std::unordered_set<std::uint64_t> analyst_seen;
    auto add_analyst = [&](const AnalystPtr& a) {
        if (a && analyst_seen.insert(a->id).second) analysts.push_back(a);
    };
    for (const auto& t : teams) {
        add_coach(t->head_coach);
        add_scout(t->head_scout);
        add_analyst(t->head_analyst);
    }
    for (const auto& c : gm.free_coaches_) add_coach(c);
    for (const auto& c : gm.retired_coaches_) add_coach(c);
    for (const auto& s : gm.free_scouts_) add_scout(s);
    for (const auto& s : gm.retired_scouts_) add_scout(s);
    for (const auto& a : gm.free_analysts_) add_analyst(a);
    for (const auto& a : gm.retired_analysts_) add_analyst(a);
    auto add_award_coaches = [&](const std::vector<GameManager::SeasonAward>& v) {
        for (const auto& a : v) {
            add_coach(a.coach_winner);
            for (const auto& f : a.coach_finalists) add_coach(f);
        }
    };
    add_award_coaches(gm.awards_history);
    add_award_coaches(gm.last_season_awards);

    // ---- Write ----
    BinW w(path);
    if (!w.out.is_open())
        return fail("Cannot open save file for writing: " + path);

    // Header (exactly what peek() reads).
    w.raw(kMagic, sizeof kMagic);
    w.u32(kVersion);
    w.str(gm.user_team ? gm.user_team->name : std::string("No Club"));
    w.i32(gm.year);
    w.i32(gm.day_in_year + 1);   // 1-based display day
    w.str(now_stamp());

    // 1. RNG engine state. Written first in the body, but on load it is
    //    APPLIED LAST: Player/Team ctors consume rng during reconstruction,
    //    which would otherwise corrupt the restored stream.
    {
        std::ostringstream os;
        os << rng().engine();
        w.str(os.str());
    }

    // 2. Name-uniqueness registries (players, orgs, gamertags).
    w.vec_str(snapshot_used_names());
    w.vec_str(snapshot_taken_team_names());
    w.vec_str(snapshot_used_handles());

    // 3a. Players.
    w.u32(static_cast<std::uint32_t>(players.size()));
    for (const auto& p : players) write_player(w, *p);

    // 3b. Staff.
    w.u32(static_cast<std::uint32_t>(coaches.size()));
    for (const auto& c : coaches) write_coach(w, *c);
    w.u32(static_cast<std::uint32_t>(scouts.size()));
    for (const auto& s : scouts) write_scout(w, *s);
    w.u32(static_cast<std::uint32_t>(analysts.size()));
    for (const auto& a : analysts) write_analyst(w, *a);

    // 3c. Teams. Defined as a lambda inside save() so it inherits SaveGame's
    // friend access to Team::trophy_history_ / finals_history_.
    auto write_team = [&](const Team& t) {
        w.u64(t.id);
        w.str(t.name);
        w.str(t.tag);
        w.i64(static_cast<std::int64_t>(t.budget));
        w.str(t.region);
        w.str(t.home_country);
        w.str(t.home_country_iso);
        w.str(t.home_city);
        w.i32(t.color_primary_r); w.i32(t.color_primary_g); w.i32(t.color_primary_b);
        w.i32(t.color_accent_r);  w.i32(t.color_accent_g);  w.i32(t.color_accent_b);
        w.i32(static_cast<std::int32_t>(t.logo_shape));
        w.i32(static_cast<std::int32_t>(t.strategy));
        w.i32(static_cast<std::int32_t>(t.previous_strategy));
        w.i32(static_cast<std::int32_t>(t.window));
        w.i32(t.prestige);
        w.i32(t.sponsorship_k);
        w.i32(t.core_wage_pressure_pct);
        w.i32(t.titles_total_cached);
        w.str(t.sponsor_name);
        w.i32(static_cast<std::int32_t>(t.sponsor_req_type));
        w.i32(t.sponsor_req_value);
        w.i32(t.sponsor_reward_k);
        w.b(t.sponsor_active);
        w.b(t.sponsor_credited);
        w.i32(t.last_revenue_k);
        w.i32(t.projected_income_k);
        w.i32(t.committed_payroll_k);
        w.i32(t.wage_envelope_k);
        w.i32(t.net_transfer_k);
        w.i32(static_cast<std::int32_t>(t.wealth_tier));
        w.u32(static_cast<std::uint32_t>(t.transfer_log_.size()));
        for (const auto& tr : t.transfer_log_) {
            w.i32(tr.year);
            w.str(tr.player);
            w.str(tr.from_team);
            w.str(tr.to_team);
            w.i32(tr.fee_k);
            w.i32(tr.wage_k);
        }
        w.i32(t.memory.rookie_success);
        w.i32(t.memory.import_success);
        w.i32(t.memory.veteran_success);
        w.i32(t.memory.financial_discipline);
        w.i32(t.memory.stability_culture);
        w.i32(t.memory.star_dependency);
        w.i32(t.cuts_this_year_);
        w.i32(static_cast<std::int32_t>(t.personality));
        w.i32(static_cast<std::int32_t>(t.comp_identity));
        w.f64(t.identity.aggression);
        w.f64(t.identity.dev_youth);
        w.i32(static_cast<std::int32_t>(t.identity.comp_lean));
        w.i32(static_cast<std::int32_t>(t.identity.brand));
        w.b(t.identity.user_chosen);
        w.u32(static_cast<std::uint32_t>(t.target_comp.need.size()));   // 4
        for (int v : t.target_comp.need) w.i32(v);
        w.i32(t.scout_prefs.youth);
        w.i32(t.scout_prefs.mechanics);
        w.i32(t.scout_prefs.smarts);
        w.i32(t.scout_prefs.aggressiveness);
        w.i32(t.scout_prefs.flexibility);
        w.i32(t.scout_prefs.clutch);
        w.i32(t.scout_prefs.potential);
        w.i32(t.scout_prefs.experience);
        w.u32(static_cast<std::uint32_t>(t.comp_override.size()));
        for (const auto& kv : t.comp_override) {
            w.str(kv.first);
            w.i32(static_cast<std::int32_t>(kv.second));
        }
        w.u32(static_cast<std::uint32_t>(t.agent_override.size()));
        for (const auto& kv : t.agent_override) {
            w.str(kv.first);
            w.vec_str(kv.second);
        }
        w.u32(static_cast<std::uint32_t>(t.map_prep.size()));
        for (const auto& kv : t.map_prep) {
            w.str(kv.first);
            w.i32(kv.second.level);
        }
        write_ids(w, t.roster);   // ORDER PRESERVED — first 5 are the starters
        w.u64(t.head_coach ? t.head_coach->id : 0);
        w.u64(t.head_scout ? t.head_scout->id : 0);
        w.u64(t.head_analyst ? t.head_analyst->id : 0);
        w.i32(t.wins);
        w.i32(t.losses);
        w.i32(t.phase_wins);
        w.i32(t.phase_losses);
        w.u32(static_cast<std::uint32_t>(t.recent_results.size()));
        for (std::uint8_t v : t.recent_results) w.u8(v);
        w.u32(static_cast<std::uint32_t>(t.history.size()));
        for (const auto& h : t.history) {
            w.i32(h.year);
            w.i32(h.wins);
            w.i32(h.losses);
        }
        w.u32(static_cast<std::uint32_t>(t.trophy_history_.size()));
        for (const auto& tp : t.trophy_history_) { w.i32(tp.first); w.str(tp.second); }
        w.u32(static_cast<std::uint32_t>(t.finals_history_.size()));
        for (const auto& fp : t.finals_history_) { w.i32(fp.first); w.str(fp.second); }
        // Chemistry — ChemKey holds raw Player*; persist as stable id pairs.
        w.u32(static_cast<std::uint32_t>(t.chemistry.size()));
        for (const auto& kv : t.chemistry) {
            w.u64(kv.first.a ? kv.first.a->id : 0);
            w.u64(kv.first.b ? kv.first.b->id : 0);
            w.f64(kv.second);
        }
    };
    w.u32(static_cast<std::uint32_t>(teams.size()));
    for (const auto& t : teams) write_team(*t);

    // 4. Leagues — full pyramid, per region. Matchups are stored as INDICES
    //    into that league's teams vector (-1 = null slot) so the mid-season
    //    schedule survives the round-trip byte-exact.
    w.u32(static_cast<std::uint32_t>(gm.tier_leagues_.size()));
    for (const auto& kv : gm.tier_leagues_) {
        w.str(kv.first);
        w.u32(static_cast<std::uint32_t>(kv.second.size()));
        for (const auto& lg : kv.second) {
            w.str(lg->name());
            w.str(lg->region());
            w.i32(lg->tier());
            w.str(lg->division_name());
            write_ids(w, lg->teams());
            write_ids(w, lg->past_champions());
            std::unordered_map<const Team*, std::int32_t> idx;
            const auto& lteams = lg->teams();
            for (std::size_t i = 0; i < lteams.size(); ++i)
                if (lteams[i]) idx[lteams[i].get()] = static_cast<std::int32_t>(i);
            auto team_index = [&](const TeamPtr& t) -> std::int32_t {
                if (!t) return -1;
                auto it = idx.find(t.get());
                return it == idx.end() ? -1 : it->second;
            };
            const auto& weeks = lg->weekly_matchups();
            w.u32(static_cast<std::uint32_t>(weeks.size()));
            for (const auto& week : weeks) {
                w.u32(static_cast<std::uint32_t>(week.size()));
                for (const auto& m : week) {
                    w.i32(team_index(m.a));
                    w.i32(team_index(m.b));
                }
            }
        }
    }

    // 5. Solo-queue ladders — order preserved (it IS the leaderboard).
    w.u32(static_cast<std::uint32_t>(gm.solo_qs.size()));
    for (const auto& kv : gm.solo_qs) {
        w.str(kv.first);
        if (kv.second) write_ids(w, kv.second->global_ladder());
        else w.u32(0);
    }

    // 6. GameManager scalars + collections. Fixed order — the load block
    //    below mirrors this sequence EXACTLY.
    w.u64(gm.user_team ? gm.user_team->id : 0);
    w.b(gm.user_auto_manage);
    w.i32(static_cast<std::int32_t>(gm.user_philosophy));
    w.i32(gm.year);
    w.i32(gm.day_in_year);
    w.i32(gm.day_total);
    w.i32(gm.day_in_phase);
    w.i32(gm.days_since_progression);
    w.i32(gm.phase_idx);
    w.i32(gm.current_week);
    w.i32(gm.t2_week);
    w.f64(gm.world_difficulty_);
    w.b(gm.in_preseason_buffer_);
    w.i32(gm.preseason_days_left_);
    w.b(gm.sponsor_choice_pending_);
    w.u32(static_cast<std::uint32_t>(gm.pending_sponsor_offers_.size()));
    for (const auto& o : gm.pending_sponsor_offers_) {
        w.str(o.name);
        w.str(o.requirement_label);
        w.i32(static_cast<std::int32_t>(o.type));
        w.i32(o.requirement_value);
        w.i32(o.reward_k);
    }
    w.i32(gm.scout_credits_);
    w.i32(gm.scout_credits_year_);
    w.i32(static_cast<std::int32_t>(gm.scout_focus_));
    w.str(gm.scout_focus_region_);
    w.i32(static_cast<std::int32_t>(gm.scout_focus_role_));
    w.u32(static_cast<std::uint32_t>(gm.scout_assignments_.size()));
    for (const auto& a : gm.scout_assignments_) {
        w.u64(a.player_id);
        w.i32(a.days_remaining);
        w.i32(a.total_days);
        w.i32(a.money_k);
        w.i32(a.coverage);
        w.i32(a.depth);
        w.b(a.done);
    }
    w.u32(static_cast<std::uint32_t>(gm.scout_report_accuracy_.size()));
    for (const auto& kv : gm.scout_report_accuracy_) {
        w.u64(kv.first);
        w.i32(kv.second);
    }
    w.u32(static_cast<std::uint32_t>(gm.scout_briefs_.size()));
    for (const auto& b : gm.scout_briefs_) {
        w.str(b.name);
        w.i32(b.role_idx);
        w.i32(b.age_min);
        w.i32(b.age_max);
        w.i32(b.min_pot);
        w.str(b.region);
        w.i32(b.days_active);
        w.i32(b.revealed);
        w.i32(b.match_total);
    }
    write_ids(w, gm.favorite_players);
    w.u32(static_cast<std::uint32_t>(gm.watchlist_.size()));
    for (const auto& e : gm.watchlist_) {
        w.u64(e.player ? e.player->id : 0);
        w.str(e.note);
        w.i32(static_cast<std::int32_t>(e.status));
    }
    write_ids(w, gm.hall_of_fame);
    w.u32(static_cast<std::uint32_t>(gm.mailbox.size()));
    for (const auto& m : gm.mailbox) {
        w.i32(m.id);
        w.i32(m.year);
        w.i32(m.day);
        w.i32(static_cast<std::int32_t>(m.category));
        w.str(m.subject);
        w.str(m.body);
        w.b(m.read);
        w.b(m.important);
        w.str(m.player_name);
        w.u64(m.player_id);
        w.str(m.team_name);
        w.i32(static_cast<std::int32_t>(m.link));
        w.i32(m.amount_k);
    }
    w.i32(gm.next_mail_id_);
    w.set_str(gm.mail_emitted_keys_);
    w.u32(static_cast<std::uint32_t>(gm.news_feed.size()));
    for (const auto& n : gm.news_feed) {
        w.i32(n.year);
        w.i32(n.day_in_year);
        w.str(n.category);
        w.str(n.headline);
        w.str(n.body);
        w.str(n.team_name);
        w.str(n.player_name);
    }
    w.set_str(gm.news_pushed_events_);
    w.set_str(gm.news_emitted_keys_);
    w.set_str(gm.news_emitted_career_keys_);
    w.str(gm.last_mvp_leader_name_);
    w.str(gm.current_board_objective_);
    w.i32(gm.board_target_placement_);
    w.f64(gm.board_ambition_);
    w.u32(static_cast<std::uint32_t>(gm.board_objectives_.size()));
    for (const auto& o : gm.board_objectives_) {
        w.i32(static_cast<std::int32_t>(o.kind));
        w.i32(o.target);
        w.b(o.mandatory);
        w.str(o.text);
    }
    auto write_awards = [&](const std::vector<GameManager::SeasonAward>& v) {
        w.u32(static_cast<std::uint32_t>(v.size()));
        for (const auto& a : v) {
            w.i32(a.year);
            w.str(a.category);
            w.u64(a.winner ? a.winner->id : 0);
            write_ids(w, a.finalists);
            w.u32(static_cast<std::uint32_t>(a.scores.size()));
            for (double s : a.scores) w.f64(s);
            w.str(a.explanation);
            w.u64(a.coach_winner ? a.coach_winner->id : 0);
            write_ids(w, a.coach_finalists);
        }
    };
    write_awards(gm.awards_history);
    write_awards(gm.last_season_awards);
    w.u32(static_cast<std::uint32_t>(gm.intl_qualifiers_.size()));
    for (const auto& kv : gm.intl_qualifiers_) {
        w.str(kv.first);
        write_ids(w, kv.second);
    }
    w.u32(static_cast<std::uint32_t>(gm.last_promo_rel_.size()));
    for (const auto& pr : gm.last_promo_rel_) {
        w.str(pr.region);
        write_ids(w, pr.promoted);
        write_ids(w, pr.relegated);
        w.u64(pr.ascension_champion ? pr.ascension_champion->id : 0);
    }
    write_ids(w, gm.free_coaches_);
    write_ids(w, gm.free_scouts_);
    write_ids(w, gm.free_analysts_);
    write_ids(w, gm.retired_coaches_);
    write_ids(w, gm.retired_scouts_);
    write_ids(w, gm.retired_analysts_);
    w.b(gm.historic_snapshot_initialized_);
    w.i32(gm.last_power_tick_day_);
    w.i32(gm.last_community_tick_day_);

    // 7. Footer — truncation canary.
    w.u32(kFooterSentinel);

    w.out.flush();
    if (!w.out.good()) {
        // badbit is sticky, so this catches any failed write above. Remove
        // the partial file — a half-written save must never be loadable.
        w.out.close();
        std::remove(path.c_str());
        return fail("I/O failure while writing save file: " + path);
    }
    return true;
}

// ==========================================================================
// === SaveGame::load =======================================================
// ==========================================================================
bool SaveGame::load(GameManager& gm, const std::string& path,
                    std::string* err) {
    auto fail = [&](const std::string& m) {
        if (err) *err = m;
        return false;
    };

    BinR r(path);
    if (!r.in.is_open())
        return fail("Cannot open save file: " + path);

    try {
        // ---- Header — verified BEFORE reset_world so a wrong/corrupt file
        //      never destroys the live world.
        char magic[4];
        r.raw(magic, sizeof magic);
        if (std::memcmp(magic, kMagic, sizeof kMagic) != 0)
            return fail("Not a VLR Manager save file (bad magic).");
        std::uint32_t ver = r.u32();
        if (ver != kVersion)
            return fail("Save version mismatch (file v" + std::to_string(ver) +
                        ", expected v" + std::to_string(kVersion) + ").");
        (void)r.str();   // club       (header display only)
        (void)r.i32();   // year       (header display only)
        (void)r.i32();   // day        (header display only)
        (void)r.str();   // saved_at   (header display only)

        // ---- 1+2. RNG state + name registries: READ now, APPLIED at the end.
        // reset_world() + every Player/Team ctor below consume rng, and
        // reset_world() wipes the registries — so both must be re-applied
        // AFTER reconstruction, not here.
        std::string rng_state = r.str();
        std::vector<std::string> used_names   = r.vec_str();
        std::vector<std::string> taken_teams  = r.vec_str();
        std::vector<std::string> used_handles = r.vec_str();

        // ---- Wipe the current world. From here on a failure leaves gm empty
        // but consistent (caller can boot a fresh world or retry the load).
        gm.reset_world();

        // ---- 3a. Players.
        PlayerMap player_map;
        {
            std::uint32_t n = r.count();
            player_map.reserve(n);
            for (std::uint32_t i = 0; i < n; ++i) {
                PlayerPtr p = read_player(r);
                player_map[p->id] = p;
            }
        }

        // ---- 3b. Staff.
        CoachMap coach_map;
        {
            std::uint32_t n = r.count();
            coach_map.reserve(n);
            for (std::uint32_t i = 0; i < n; ++i) {
                CoachPtr c = read_coach(r);
                coach_map[c->id] = c;
            }
        }
        ScoutMap scout_map;
        {
            std::uint32_t n = r.count();
            scout_map.reserve(n);
            for (std::uint32_t i = 0; i < n; ++i) {
                ScoutPtr s = read_scout(r);
                scout_map[s->id] = s;
            }
        }
        AnalystMap analyst_map;
        {
            std::uint32_t n = r.count();
            analyst_map.reserve(n);
            for (std::uint32_t i = 0; i < n; ++i) {
                AnalystPtr a = read_analyst(r);
                analyst_map[a->id] = a;
            }
        }

        // ---- 3c. Teams. Lambda inside load() for friend access to the
        //      private trophy/finals history.
        TeamMap team_map;
        auto read_team = [&]() -> TeamPtr {
            std::uint64_t id = r.u64();
            std::string name = r.str();
            std::string tag = r.str();
            long long budget = static_cast<long long>(r.i64());
            std::string region = r.str();
            // Ctor consumes rng + stamps a fresh id — both fine, see the
            // rng-last note above; every rolled field is overwritten below.
            auto t = std::make_shared<Team>(name, budget, region);
            t->id = id;
            t->tag = std::move(tag);
            t->home_country = r.str();
            t->home_country_iso = r.str();
            t->home_city = r.str();
            t->color_primary_r = r.i32(); t->color_primary_g = r.i32(); t->color_primary_b = r.i32();
            t->color_accent_r = r.i32();  t->color_accent_g = r.i32();  t->color_accent_b = r.i32();
            t->logo_shape = static_cast<LogoShape>(r.i32());
            t->strategy = static_cast<Team::Strategy>(r.i32());
            t->previous_strategy = static_cast<Team::Strategy>(r.i32());
            t->window = static_cast<TeamWindow>(r.i32());
            t->prestige = r.i32();
            t->sponsorship_k = r.i32();
            t->core_wage_pressure_pct = r.i32();
            t->titles_total_cached = r.i32();
            t->sponsor_name = r.str();
            t->sponsor_req_type = static_cast<SponsorReqType>(r.i32());
            t->sponsor_req_value = r.i32();
            t->sponsor_reward_k = r.i32();
            t->sponsor_active = r.b();
            t->sponsor_credited = r.b();
            t->last_revenue_k = r.i32();
            t->projected_income_k = r.i32();
            t->committed_payroll_k = r.i32();
            t->wage_envelope_k = r.i32();
            t->net_transfer_k = r.i32();
            t->wealth_tier = static_cast<WealthTier>(r.i32());
            {
                std::uint32_t n = r.count();
                t->transfer_log_.clear();
                t->transfer_log_.reserve(n);
                for (std::uint32_t i = 0; i < n; ++i) {
                    TransferRecord tr;
                    tr.year = r.i32();
                    tr.player = r.str();
                    tr.from_team = r.str();
                    tr.to_team = r.str();
                    tr.fee_k = r.i32();
                    tr.wage_k = r.i32();
                    t->transfer_log_.push_back(std::move(tr));
                }
            }
            t->memory.rookie_success = r.i32();
            t->memory.import_success = r.i32();
            t->memory.veteran_success = r.i32();
            t->memory.financial_discipline = r.i32();
            t->memory.stability_culture = r.i32();
            t->memory.star_dependency = r.i32();
            t->cuts_this_year_ = r.i32();
            t->personality = static_cast<Personality>(r.i32());
            t->comp_identity = static_cast<CompIdentity>(r.i32());
            t->identity.aggression = r.f64();
            t->identity.dev_youth = r.f64();
            t->identity.comp_lean = static_cast<CompTag>(r.i32());
            t->identity.brand = static_cast<BrandTag>(r.i32());
            t->identity.user_chosen = r.b();
            {
                std::uint32_t n = r.count();
                if (n != static_cast<std::uint32_t>(t->target_comp.need.size()))
                    throw std::runtime_error("CompPlan role count mismatch");
                for (auto& v : t->target_comp.need) v = r.i32();
            }
            t->scout_prefs.youth = r.i32();
            t->scout_prefs.mechanics = r.i32();
            t->scout_prefs.smarts = r.i32();
            t->scout_prefs.aggressiveness = r.i32();
            t->scout_prefs.flexibility = r.i32();
            t->scout_prefs.clutch = r.i32();
            t->scout_prefs.potential = r.i32();
            t->scout_prefs.experience = r.i32();
            {
                std::uint32_t n = r.count();
                t->comp_override.clear();
                for (std::uint32_t i = 0; i < n; ++i) {
                    std::string key = r.str();
                    t->comp_override[key] = static_cast<CompTag>(r.i32());
                }
            }
            {
                std::uint32_t n = r.count();
                t->agent_override.clear();
                for (std::uint32_t i = 0; i < n; ++i) {
                    std::string key = r.str();
                    t->agent_override[key] = r.vec_str();
                }
            }
            {
                std::uint32_t n = r.count();
                t->map_prep.clear();
                for (std::uint32_t i = 0; i < n; ++i) {
                    std::string key = r.str();
                    Team::MapPrep mp;
                    mp.level = r.i32();
                    t->map_prep[key] = mp;
                }
            }
            // Roster — order preserved (first 5 = starters). Unresolvable ids
            // are dropped defensively; with a well-formed save none are.
            t->roster = resolve_ids(player_map, read_ids(r));
            t->head_coach = resolve_id(coach_map, r.u64());
            t->head_scout = resolve_id(scout_map, r.u64());
            t->head_analyst = resolve_id(analyst_map, r.u64());
            t->wins = r.i32();
            t->losses = r.i32();
            t->phase_wins = r.i32();
            t->phase_losses = r.i32();
            {
                std::uint32_t n = r.count();
                t->recent_results.clear();
                t->recent_results.reserve(n);
                for (std::uint32_t i = 0; i < n; ++i) t->recent_results.push_back(r.u8());
            }
            {
                std::uint32_t n = r.count();
                t->history.clear();
                t->history.reserve(n);
                for (std::uint32_t i = 0; i < n; ++i) {
                    TeamHistoryEntry h{};
                    h.year = r.i32();
                    h.wins = r.i32();
                    h.losses = r.i32();
                    t->history.push_back(h);
                }
            }
            {
                std::uint32_t n = r.count();
                t->trophy_history_.clear();
                t->trophy_history_.reserve(n);
                for (std::uint32_t i = 0; i < n; ++i) {
                    int y = r.i32();
                    std::string ev = r.str();
                    t->trophy_history_.emplace_back(y, std::move(ev));
                }
            }
            {
                std::uint32_t n = r.count();
                t->finals_history_.clear();
                t->finals_history_.reserve(n);
                for (std::uint32_t i = 0; i < n; ++i) {
                    int y = r.i32();
                    std::string ev = r.str();
                    t->finals_history_.emplace_back(y, std::move(ev));
                }
            }
            {
                // Chemistry edges rebuilt from restored PlayerPtrs' RAW
                // pointers, canonicalized smaller-pointer-first to match
                // record_chemistry's key invariant. Edges whose player no
                // longer resolves are dropped (they'd be unreachable anyway).
                std::uint32_t n = r.count();
                t->chemistry.clear();
                for (std::uint32_t i = 0; i < n; ++i) {
                    std::uint64_t ida = r.u64();
                    std::uint64_t idb = r.u64();
                    double val = r.f64();
                    PlayerPtr pa = resolve_id(player_map, ida);
                    PlayerPtr pb = resolve_id(player_map, idb);
                    if (!pa || !pb || pa == pb) continue;
                    Player* lo = pa.get() < pb.get() ? pa.get() : pb.get();
                    Player* hi = pa.get() < pb.get() ? pb.get() : pa.get();
                    t->chemistry[ChemKey{lo, hi}] = val;
                }
            }
            return t;
        };
        {
            std::uint32_t n = r.count();
            team_map.reserve(n);
            for (std::uint32_t i = 0; i < n; ++i) {
                TeamPtr t = read_team();
                team_map[t->id] = t;
            }
        }

        // ---- 4. Leagues (full pyramid).
        {
            std::uint32_t region_n = r.count();
            for (std::uint32_t ri = 0; ri < region_n; ++ri) {
                std::string region_key = r.str();
                std::uint32_t tier_n = r.count();
                std::vector<std::shared_ptr<League>> tiers;
                tiers.reserve(tier_n);
                for (std::uint32_t ti = 0; ti < tier_n; ++ti) {
                    std::string lname = r.str();
                    std::string lregion = r.str();
                    int ltier = r.i32();
                    std::string ldivision = r.str();
                    // League team list: positional — the matchup indices
                    // below point into THIS vector, so a missing team is a
                    // hard corruption (never a skip).
                    std::vector<std::uint64_t> team_ids = read_ids(r);
                    std::vector<TeamPtr> lteams;
                    lteams.reserve(team_ids.size());
                    for (auto tid : team_ids) {
                        TeamPtr t = resolve_id(team_map, tid);
                        if (!t) throw std::runtime_error("league references an unknown team id");
                        lteams.push_back(t);
                    }
                    std::vector<TeamPtr> champs =
                        resolve_ids(team_map, read_ids(r));
                    auto lg = std::make_shared<League>(lname, lteams, lregion,
                                                       ltier, ldivision);
                    lg->past_champions() = std::move(champs);
                    // Restore the saved schedule verbatim (friend access) —
                    // regenerating would reset a mid-season round-robin.
                    std::uint32_t week_n = r.count();
                    lg->weekly_matchups_.clear();
                    lg->weekly_matchups_.reserve(week_n);
                    for (std::uint32_t wi = 0; wi < week_n; ++wi) {
                        std::uint32_t match_n = r.count();
                        std::vector<LeagueMatchup> week;
                        week.reserve(match_n);
                        for (std::uint32_t mi = 0; mi < match_n; ++mi) {
                            std::int32_t ia = r.i32();
                            std::int32_t ib = r.i32();
                            LeagueMatchup m;
                            if (ia >= 0 && static_cast<std::size_t>(ia) < lteams.size())
                                m.a = lteams[static_cast<std::size_t>(ia)];
                            if (ib >= 0 && static_cast<std::size_t>(ib) < lteams.size())
                                m.b = lteams[static_cast<std::size_t>(ib)];
                            week.push_back(std::move(m));
                        }
                        lg->weekly_matchups_.push_back(std::move(week));
                    }
                    tiers.push_back(std::move(lg));
                }
                gm.tier_leagues_[region_key] = tiers;
                // CRITICAL ALIAS: leagues[region] must be the SAME shared_ptr
                // as tier_leagues_[region][0] — every legacy region-keyed call
                // site reads `leagues`, and pro/rel moves teams through the
                // tier vector. Two separate League objects would fork reality.
                if (!tiers.empty()) gm.leagues[region_key] = tiers[0];
            }
        }

        // ---- 5. Solo-queue ladders.
        {
            std::uint32_t n = r.count();
            for (std::uint32_t i = 0; i < n; ++i) {
                std::string region_key = r.str();
                auto eng = std::make_shared<SoloQEngine>(region_key);
                std::vector<std::uint64_t> ids = read_ids(r);
                auto& ladder = eng->global_ladder();
                ladder.reserve(ids.size());
                for (auto pid : ids) {
                    if (PlayerPtr p = resolve_id(player_map, pid))
                        ladder.push_back(p);   // order preserved = leaderboard
                }
                gm.solo_qs[region_key] = eng;
            }
        }

        // ---- 6. GameManager state (mirrors the save block exactly).
        std::uint64_t user_team_id = r.u64();
        gm.user_auto_manage = r.b();
        gm.user_philosophy = static_cast<GameManager::ClubPhilosophy>(r.i32());
        gm.year = r.i32();
        gm.day_in_year = r.i32();
        gm.day_total = r.i32();
        gm.day_in_phase = r.i32();
        gm.days_since_progression = r.i32();
        gm.phase_idx = r.i32();
        gm.current_week = r.i32();
        gm.t2_week = r.i32();
        gm.world_difficulty_ = r.f64();
        gm.in_preseason_buffer_ = r.b();
        gm.preseason_days_left_ = r.i32();
        gm.sponsor_choice_pending_ = r.b();
        {
            std::uint32_t n = r.count();
            gm.pending_sponsor_offers_.clear();
            gm.pending_sponsor_offers_.reserve(n);
            for (std::uint32_t i = 0; i < n; ++i) {
                SponsorOffer o;
                o.name = r.str();
                o.requirement_label = r.str();
                o.type = static_cast<SponsorReqType>(r.i32());
                o.requirement_value = r.i32();
                o.reward_k = r.i32();
                gm.pending_sponsor_offers_.push_back(std::move(o));
            }
        }
        gm.scout_credits_ = r.i32();
        gm.scout_credits_year_ = r.i32();
        gm.scout_focus_ = static_cast<GameManager::ScoutFocus>(r.i32());
        gm.scout_focus_region_ = r.str();
        gm.scout_focus_role_ = static_cast<Role>(r.i32());
        {
            std::uint32_t n = r.count();
            gm.scout_assignments_.clear();
            gm.scout_assignments_.reserve(n);
            for (std::uint32_t i = 0; i < n; ++i) {
                GameManager::ScoutAssignment a;
                a.player_id = r.u64();
                a.days_remaining = r.i32();
                a.total_days = r.i32();
                a.money_k = r.i32();
                a.coverage = r.i32();
                a.depth = r.i32();
                a.done = r.b();
                gm.scout_assignments_.push_back(a);
            }
        }
        {
            std::uint32_t n = r.count();
            gm.scout_report_accuracy_.clear();
            gm.scout_report_accuracy_.reserve(n);
            for (std::uint32_t i = 0; i < n; ++i) {
                std::uint64_t pid = r.u64();
                gm.scout_report_accuracy_[pid] = r.i32();
            }
        }
        {
            std::uint32_t n = r.count();
            gm.scout_briefs_.clear();
            gm.scout_briefs_.reserve(n);
            for (std::uint32_t i = 0; i < n; ++i) {
                GameManager::ScoutBrief b;
                b.name = r.str();
                b.role_idx = r.i32();
                b.age_min = r.i32();
                b.age_max = r.i32();
                b.min_pot = r.i32();
                b.region = r.str();
                b.days_active = r.i32();
                b.revealed = r.i32();
                b.match_total = r.i32();
                gm.scout_briefs_.push_back(std::move(b));
            }
        }
        gm.favorite_players = resolve_ids(player_map, read_ids(r));
        {
            std::uint32_t n = r.count();
            gm.watchlist_.clear();
            gm.watchlist_.reserve(n);
            for (std::uint32_t i = 0; i < n; ++i) {
                std::uint64_t pid = r.u64();
                std::string note = r.str();
                auto status = static_cast<GameManager::WatchStatus>(r.i32());
                if (PlayerPtr p = resolve_id(player_map, pid)) {
                    GameManager::WatchEntry e;
                    e.player = p;
                    e.note = std::move(note);
                    e.status = status;
                    gm.watchlist_.push_back(std::move(e));
                }
            }
        }
        gm.hall_of_fame = resolve_ids(player_map, read_ids(r));
        {
            std::uint32_t n = r.count();
            gm.mailbox.clear();
            gm.mailbox.reserve(n);
            for (std::uint32_t i = 0; i < n; ++i) {
                GameManager::MailItem m;
                m.id = r.i32();
                m.year = r.i32();
                m.day = r.i32();
                m.category = static_cast<GameManager::MailCategory>(r.i32());
                m.subject = r.str();
                m.body = r.str();
                m.read = r.b();
                m.important = r.b();
                m.player_name = r.str();
                m.player_id = r.u64();
                m.team_name = r.str();
                m.link = static_cast<GameManager::MailLink>(r.i32());
                m.amount_k = r.i32();
                gm.mailbox.push_back(std::move(m));
            }
        }
        gm.next_mail_id_ = r.i32();
        gm.mail_emitted_keys_ = r.set_str();
        {
            std::uint32_t n = r.count();
            gm.news_feed.clear();
            gm.news_feed.reserve(n);
            for (std::uint32_t i = 0; i < n; ++i) {
                GameManager::NewsItem item;
                item.year = r.i32();
                item.day_in_year = r.i32();
                item.category = r.str();
                item.headline = r.str();
                item.body = r.str();
                item.team_name = r.str();
                item.player_name = r.str();
                gm.news_feed.push_back(std::move(item));
            }
        }
        gm.news_pushed_events_ = r.set_str();
        gm.news_emitted_keys_ = r.set_str();
        gm.news_emitted_career_keys_ = r.set_str();
        gm.last_mvp_leader_name_ = r.str();
        gm.current_board_objective_ = r.str();
        gm.board_target_placement_ = r.i32();
        gm.board_ambition_ = r.f64();
        {
            std::uint32_t n = r.count();
            gm.board_objectives_.clear();
            gm.board_objectives_.reserve(n);
            for (std::uint32_t i = 0; i < n; ++i) {
                BoardObjective o;
                o.kind = static_cast<BoardObjective::Kind>(r.i32());
                o.target = r.i32();
                o.mandatory = r.b();
                o.text = r.str();
                gm.board_objectives_.push_back(std::move(o));
            }
        }
        auto read_awards = [&]() {
            std::uint32_t n = r.count();
            std::vector<GameManager::SeasonAward> v;
            v.reserve(n);
            for (std::uint32_t i = 0; i < n; ++i) {
                GameManager::SeasonAward a;
                a.year = r.i32();
                a.category = r.str();
                a.winner = resolve_id(player_map, r.u64());
                a.finalists = resolve_ids(player_map, read_ids(r));
                std::uint32_t sn = r.count();
                a.scores.reserve(sn);
                for (std::uint32_t si = 0; si < sn; ++si) a.scores.push_back(r.f64());
                a.explanation = r.str();
                a.coach_winner = resolve_id(coach_map, r.u64());
                a.coach_finalists = resolve_ids(coach_map, read_ids(r));
                v.push_back(std::move(a));
            }
            return v;
        };
        gm.awards_history = read_awards();
        gm.last_season_awards = read_awards();
        {
            std::uint32_t n = r.count();
            for (std::uint32_t i = 0; i < n; ++i) {
                std::string region_key = r.str();
                gm.intl_qualifiers_[region_key] =
                    resolve_ids(team_map, read_ids(r));
            }
        }
        {
            std::uint32_t n = r.count();
            gm.last_promo_rel_.clear();
            gm.last_promo_rel_.reserve(n);
            for (std::uint32_t i = 0; i < n; ++i) {
                GameManager::PromoRelResult pr;
                pr.region = r.str();
                pr.promoted = resolve_ids(team_map, read_ids(r));
                pr.relegated = resolve_ids(team_map, read_ids(r));
                pr.ascension_champion = resolve_id(team_map, r.u64());
                gm.last_promo_rel_.push_back(std::move(pr));
            }
        }
        gm.free_coaches_ = resolve_ids(coach_map, read_ids(r));
        gm.free_scouts_ = resolve_ids(scout_map, read_ids(r));
        gm.free_analysts_ = resolve_ids(analyst_map, read_ids(r));
        gm.retired_coaches_ = resolve_ids(coach_map, read_ids(r));
        gm.retired_scouts_ = resolve_ids(scout_map, read_ids(r));
        gm.retired_analysts_ = resolve_ids(analyst_map, read_ids(r));
        gm.historic_snapshot_initialized_ = r.b();
        // Power/community RANKINGS stay empty on purpose (transient, rebuilt
        // on their weekly tick) — but the tick CURSORS are restored so the
        // rebuild fires on the original cadence, not immediately.
        gm.last_power_tick_day_ = r.i32();
        gm.last_community_tick_day_ = r.i32();

        // ---- 7. Footer — refuse a truncated file even if everything above
        //      happened to parse.
        if (r.u32() != kFooterSentinel)
            throw std::runtime_error("save footer missing (file truncated)");

        // ---- Post-load fixups (ORDER MATTERS) ----

        // (a) Resolve the user's club by stable id.
        gm.user_team = resolve_id(team_map, user_team_id);

        // (b) Name registries — restored only NOW: reset_world() wiped them
        //     and nothing during reconstruction touches them (all names were
        //     passed into ctors, never drawn from the pools).
        restore_name_registries(std::move(used_names), std::move(taken_teams));
        restore_used_handles(std::move(used_handles));

        // (c) Entity-id counter: bump past the max restored id so every
        //     future rookie/team/staffer gets a fresh, non-colliding id.
        std::uint64_t max_id = 0;
        for (const auto& kv : player_map)  max_id = (std::max)(max_id, kv.first);
        for (const auto& kv : coach_map)   max_id = (std::max)(max_id, kv.first);
        for (const auto& kv : scout_map)   max_id = (std::max)(max_id, kv.first);
        for (const auto& kv : analyst_map) max_id = (std::max)(max_id, kv.first);
        for (const auto& kv : team_map)    max_id = (std::max)(max_id, kv.first);
        reset_entity_id_counter(max_id + 1);

        // (d) RNG engine — applied LAST. Every Player/Team ctor above (and
        //     reset_world's name-pool shuffle) consumed rng; overwriting the
        //     engine now puts the stream exactly where the saved world left
        //     it, so a save/load round-trip continues the same timeline.
        {
            std::istringstream is(rng_state);
            is >> rng().engine();
            if (is.fail())
                throw std::runtime_error("rng state corrupt in save file");
        }

        // (e) World-year mirror for Match.cpp's mastery stamping.
        set_current_world_year(gm.year);

        // (f) Per-player derived state: agent pools re-derive from the
        //     restored attributes/mastery/role; the awards hash mirror
        //     re-syncs with the restored awards vector.
        for (const auto& kv : player_map) {
            kv.second->update_agent_pool();
            kv.second->rebuild_awards_seen();
        }

        return true;
    } catch (const std::exception& e) {
        return fail(std::string("Load failed: ") + e.what());
    } catch (...) {
        return fail("Load failed: unknown error.");
    }
}

// ==========================================================================
// === SaveGame::peek =======================================================
// ==========================================================================
SaveGame::SlotInfo SaveGame::peek(const std::string& path) {
    SlotInfo info;
    try {
        BinR r(path);
        if (!r.in.is_open()) return info;
        char magic[4];
        r.raw(magic, sizeof magic);
        if (std::memcmp(magic, kMagic, sizeof kMagic) != 0) return info;
        if (r.u32() != kVersion) return info;
        info.club = r.str();
        info.year = r.i32();
        info.day = r.i32();
        info.saved_at = r.str();
        info.exists = true;
    } catch (...) {
        info = SlotInfo{};
    }
    return info;
}

}  // namespace vlr
