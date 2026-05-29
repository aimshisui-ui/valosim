#pragma once

#include "League.h"
#include "SoloQ.h"
#include "Series.h"
#include "Team.h"
#include "Tournament.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace vlr {

// Power & Community ranking entry — one per team in a ranking list.
// Power Rankings are analyst-driven (form / prestige / map diff / tournament
// finishes / SoS). Community Rankings are reactionary (popularity + flashy
// moments + narrative buzz) so the two lists can disagree in interesting
// ways. Both are recomputed weekly inside advance_day.
struct PowerRanking {
    Team* team = nullptr;
    int rank = 0;
    int prev_rank = 0;            // -1 = unranked previously
    double score = 0.0;
    std::string tier;             // "S", "A", "B", "C", "Bubble"
    std::string analyst_note;     // 1-sentence flavor
    std::vector<int> rank_history; // last 8 weeks
};

// Community ranking shares PowerRanking's shape but adds popularity +
// narrative_boost so the UI can display a "fan favorite vs. analyst pick"
// gap. Kept as a separate struct (not an alias) so the UI/code can show
// both fields without touching the analyst version.
struct CommunityRanking : PowerRanking {
    double popularity_score = 0.0;
    double narrative_boost = 0.0;
};

// Result of a single advance_day() call. The UI consumes this to decide
// whether to pop the live-match modal, refresh KPIs, and show progression
// notifications.
struct DayResult {
    int  year = 0;
    int  day_in_year = 0;       // 0..199
    int  day_total = 0;
    std::string phase;
    std::string label;          // e.g. "Stage 1 Round 3"
    bool was_matchday = false;
    int  matches_simulated = 0; // AI matches that played out today
    // The user's own match for today, if any. user_match_recording is the
    // FIRST map (kept for backwards compat / quick access). user_match_series
    // contains ALL maps of the series so the Live Viewer can step through
    // each map of a BO3/BO5 with continuous score tracking.
    RecordedMatchPtr user_match_recording;
    std::vector<RecordedMatchPtr> user_match_series;
    std::string      user_match_opp_name;
    std::string      user_match_event;
    // Progression: populated when a 30-day tick fires this day.
    bool progression_ran = false;
    std::vector<ProgressionReport> progression_reports;
    // Year ticked over (offseason completed)?
    bool year_rolled = false;
};

// A scheduled fixture as the calendar sees it — used by the upcoming-fixtures
// UI panel. Only stage round-robin fixtures are pre-populated; tournament
// matchups are added dynamically as rounds resolve.
struct UpcomingFixture {
    int  day_in_year = 0;
    std::string region;
    TeamPtr a, b;
    std::string label;          // "Stage 1 Round 5"
};

// User-supplied "New Game" wizard inputs. Consumed by
// `GameManager::initialize_world_with_config` which generates the world per
// the existing seeded logic, THEN overlays these choices onto the user's
// team (rename / recolor / set city + ISO / apply budget tier + prestige).
//
// Cross-agent contract: the wizard UI (Agent C) populates this and calls
// `reset_world` + `initialize_world_with_config` to (re)boot the world.
// `difficulty` is stored verbatim into `world_difficulty_`; nothing reads
// it yet (future AI-strength gating work).
struct NewGameConfig {
    std::string user_team_name = "VLR Manager";
    std::string user_org_city  = "";          // empty -> pick from Americas pool
    std::string user_org_country_iso = "us";
    int starting_year = 2026;
    // Region the user's org plays in — must be one of "Americas"/"EMEA"/"Pacific".
    std::string user_region = "Americas";
    // Starting budget tier. Affects initial budget + prestige.
    enum class OrgTier : std::uint8_t { Rich, Contender, Mid, Budget, Expansion };
    OrgTier user_tier = OrgTier::Mid;
    // Team primary/accent colors — if both 0, derive from name hash.
    std::uint32_t user_color_primary = 0;
    std::uint32_t user_color_accent  = 0;
    // Difficulty multiplier 0.7..1.3 (1.0 = neutral; <1 = AI weaker; >1 = AI stronger).
    double difficulty = 1.0;
};

class GameManager {
public:
    GameManager();

    void initialize_world();

    // Alternative entry that boots a fresh world AND applies the user's New
    // Game wizard selections on top. Returns false on a malformed cfg
    // (currently always true — kept boolean so a future validator can fail
    // out without overloading via exception). `log` collects any narration
    // emitted by the underlying seeded init.
    //
    // Contract:
    //  * Calls (effectively) the same seeding logic as `initialize_world`.
    //  * Overlays: renames user_team, recolors, applies budget tier mapping
    //    Rich=$3M Contender=$2M Mid=$1.5M Budget=$0.8M Expansion=$0.5M and
    //    prestige Rich=85 Contender=70 Mid=55 Budget=40 Expansion=25.
    //  * Picks user_team from the configured `user_region` (defaults to
    //    Americas if the region is unknown).
    //  * Stores `cfg.difficulty` in `world_difficulty_` for later gating.
    bool initialize_world_with_config(const NewGameConfig& cfg,
                                      std::vector<std::string>& log);

    // Clear every owned container so a fresh `initialize_world` /
    // `initialize_world_with_config` can boot a new world without
    // re-launching the exe. After reset, year resets to default; the
    // caller is expected to re-init immediately after.
    void reset_world();

    // Difficulty multiplier captured by the wizard. 1.0 = neutral.
    // Currently informational — gating callsites (AI strength curves /
    // duel power deltas) will be wired by Agent C / future work.
    double world_difficulty() const { return world_difficulty_; }

    TeamPtr user_team;

    int year = 2026;
    int day_in_year = 0;        // 0..199 (each season is a 200-day cycle)
    int day_total   = 0;        // strictly increasing across years
    int day_in_phase = 0;       // resets each phase
    int days_since_progression = 0;  // 30-day tick

    static constexpr int kDaysPerYear = 200;

    static const std::vector<std::string>& phases();
    int phase_idx = 0;
    int current_week = 0;       // index into League::weekly_matchups during STAGE

    const std::string& current_phase() const;
    // (days_per_round, total_matchdays) for the current phase.
    std::pair<int,int> current_phase_pacing() const;
    int next_user_match_day_in_year() const;  // -1 if none ahead

    std::unordered_map<std::string, std::shared_ptr<League>> leagues;
    std::unordered_map<std::string, std::shared_ptr<SoloQEngine>> solo_qs;

    std::vector<std::shared_ptr<Tournament>> active_tournaments;

    // Advance time by exactly one day; the FM-style core loop. Simulates
    // AI matches if it's a matchday, returns the user's match recording
    // for the UI to play back, runs monthly progression on 30-day ticks.
    DayResult advance_day(std::vector<std::string>& log);

    // Fast-forward helpers. They internally call advance_day in a loop and
    // bail early if the user has a match pending. The UI uses these as
    // "Continue", "Skip to next match", "Skip to year end" buttons.
    DayResult advance_to_next_user_match(std::vector<std::string>& log);
    DayResult advance_to_next_phase(std::vector<std::string>& log);

    // Advance the calendar until the next playoff phase (REGIONALS, MASTERS,
    // or CHAMPIONS) is about to start, then STOP without playing any
    // playoff matches. The user can then click Continue to actually start
    // the playoff matchday.
    DayResult advance_to_playoffs(std::vector<std::string>& log);

    void simulate_solo_q_day();
    void simulate_full_season(std::vector<std::string>& log);

    // Move a player between solo Q ladders so their ranked region matches
    // their current pro team. Idempotent — safe to call repeatedly.
    void sync_player_ranked_region(const PlayerPtr& p, const std::string& target_region);

    // Walk all leagues and sync every rostered player to their team's
    // region. Called at world-init + year-end to keep ladders consistent
    // with roster moves.
    void sync_all_ranked_regions();

    // List of upcoming stage matchups by day (used by the Calendar UI).
    std::vector<UpcomingFixture> upcoming_fixtures(int max_days_ahead = 14) const;

    void favorite_player(const PlayerPtr& p);
    void unfavorite_player(const PlayerPtr& p);
    bool is_favorited(const PlayerPtr& p) const;

    std::vector<PlayerPtr>      favorite_players;
    std::vector<PlayerPtr>      hall_of_fame;
    std::vector<ProgressionReport> recent_progression_reports;  // last tick's

    // === End-of-season award presentation ===
    // Computed by compute_season_awards() at year-end. Each award lists the
    // winner + top-3 finalists + the score/blurb that decided it. The UI
    // shows them on the Awards Recap screen at year rollover.
    struct SeasonAward {
        int         year   = 0;
        std::string category;          // "MVP" / "Duelist of the Year" / ...
        PlayerPtr   winner;
        std::vector<PlayerPtr> finalists;  // top 3 incl winner, sorted desc
        std::vector<double>    scores;     // parallel to finalists
        std::string explanation;
    };
    std::vector<SeasonAward> awards_history;       // all-time
    std::vector<SeasonAward> last_season_awards;   // current year's recap

    // === Living-world news feed ===
    // Roster moves, coach decisions, awards, retirements, MVP-race shifts
    // are pushed here as they happen so the UI can show "Breaking News"
    // cards instead of hiding everything in the Event Log. Most-recent
    // first; capped at 200 entries to keep memory bounded.
    struct NewsItem {
        int  year         = 0;
        int  day_in_year  = 0;
        std::string category;     // "Roster Move", "Awards", "Retirement", "MVP Race", "Coaching"
        std::string headline;
        std::string body;
        std::string team_name;    // optional context link
        std::string player_name;  // optional context link
    };
    std::vector<NewsItem> news_feed;
    void push_news(const NewsItem& n);

    // Auto-push 1-3 news items announcing a tournament's champion + headlines
    // (Finals MVP / champion run summary). Called from play_tournament_round
    // and force_finish_stale_tournaments the moment a Tournament transitions
    // into Phase::Done, so LAN/international wins surface in the news feed.
    // `gf_series` may be null if the GF Series object isn't available
    // (e.g. force-finish path) — in that case the MVP item is skipped and
    // we fall back to a champion-run summary using bracket history.
    // Idempotent: dedups by (tournament name, year) via news_pushed_events_.
    void push_tournament_news(const Tournament& tour, int year,
                              const Series* gf_series);

    // Dedup key set for push_tournament_news. Key format: "<tour name>/<year>".
    // Cleared at year rollover (run_end_of_year) so the same event name in
    // a future season triggers fresh news.
    std::unordered_set<std::string> news_pushed_events_;

    // === Mid-season MVP race / power rankings ===
    // Updated every monthly progression pass. Top N candidates per
    // category with a current "race score" (incorporating stats, team
    // success, international weight). Surfaced in the Awards screen so
    // the season builds drama rather than computing silently at year end.
    struct RaceCandidate {
        PlayerPtr   player;
        double      score = 0.0;
        std::string blurb;   // "Up 2 spots", "Climbing fast", "Slipping"
        int         delta = 0;   // change vs. previous tick
    };
    struct RaceLeaderboard {
        std::string category;
        std::vector<RaceCandidate> candidates;
    };
    std::vector<RaceLeaderboard> mvp_race;       // current snapshot per award
    void update_mvp_race();

    // === Power Rankings + Community Rankings ===
    // Weekly auto-update inside advance_day. Power Rankings are computed
    // from analytical signals (form, prestige, map diff, tournament
    // finishes, strength of schedule). Community Rankings layer popularity
    // + flashy moments + narrative buzz on top so the two lists diverge.
    // Both are keyed by region ("Americas" / "EMEA" / "Pacific") plus the
    // synthetic "International" bucket (top 4 per region merged).
    void compute_power_rankings();
    void compute_community_rankings();
    const std::vector<PowerRanking>& power_rankings_for(const std::string& region) const;
    const std::vector<CommunityRanking>& community_rankings_for(const std::string& region) const;
    int power_rank_of(Team* t) const;           // 0 if unranked
    int community_rank_of(Team* t) const;       // 0 if unranked
    int next_power_tick_in_days() const;        // for UI status

    // Public so smoke tests + future external tooling can drive a series
    // directly without going through advance_day. Returns ALL recordings
    // (one per map played) — empty if either team is null. Caller may
    // pass is_league_play=false for one-off / friendly fixtures.
    std::vector<RecordedMatchPtr> sim_series_returning_all_matches(
        TeamPtr a, TeamPtr b, int best_of,
        const std::string& event, bool is_league_play);

private:
    void generate_tournaments_if_needed();
    // If a phase ends with leftover unfinished tournaments, play their
    // remaining rounds in one shot then clear active_tournaments.
    // Prevents stat-duplication bugs from brackets leaking into the
    // next phase. Called from advance_day at phase boundaries.
    void force_finish_stale_tournaments(std::vector<std::string>& log);

    // Stage round: simulates *all* AI matchups of week `current_week` across
    // all 3 leagues. If the user's team is in this round, also simulates
    // their match privately and returns the recording in `out_user_rec`.
    // Advances `current_week`.
    void play_stage_round(std::vector<std::string>& log,
                          RecordedMatchPtr& out_user_rec,
                          std::vector<RecordedMatchPtr>& out_user_series,
                          std::string& out_user_event,
                          std::string& out_user_opp);

    // Tournament round: ditto for one round of the active tournaments.
    void play_tournament_round(std::vector<std::string>& log,
                               RecordedMatchPtr& out_user_rec,
                               std::vector<RecordedMatchPtr>& out_user_series,
                               std::string& out_user_event,
                               std::string& out_user_opp);

    void run_end_of_year(std::vector<std::string>& log);

    // Monthly tick: ages snapshots, applies attribute growth/decline.
    void run_monthly_progression(std::vector<std::string>& log,
                                 std::vector<ProgressionReport>& out_reports);

    // Strict-personality coaches may bench / release underperforming players
    // mid-season (rating < 0.95 over recent matches). Replacement chance is
    // gated by personality_replacement_aggressiveness(). Runs every monthly
    // tick after progression so cuts react to recent form.
    void run_mid_season_replacements(std::vector<std::string>& log);

    // Compute end-of-year awards: MVP, Duelist/Initiator/Controller/Sentinel
    // of the Year, plus IGL of the Year. Each award has qualifying gates
    // (min matches + role membership). Top 3 finalists per category. Award
    // text is appended to the winning player's `awards` vector and to
    // awards_history.
    void compute_season_awards(std::vector<std::string>& log);

    // Initial snake-draft roster fill. Used at world init (and after a
    // big league reset) to give every team a believable starting roster
    // built from the same shared FA pool. Each round each team picks
    // the best available player by their strategy/comp/needs. Respects
    // import cap, salary range, and the one-IGL-per-team rule.
    void run_initial_snake_draft(std::vector<std::string>& log);

    std::unordered_map<std::string, SoloQEngine*> raw_solo_q_map();

    // === Rankings storage ===
    // Keyed by region (kRegions entries) or "International".
    std::unordered_map<std::string, std::vector<PowerRanking>>     power_rankings_;
    std::unordered_map<std::string, std::vector<CommunityRanking>> community_rankings_;
    int last_power_tick_day_     = -10;  // -10 sentinel: never ticked this year
    int last_community_tick_day_ = -10;

    // === News emitter state ===
    // Generic dedup set for the year-scoped emitters (breakout, slump, hot
    // streak, transfer rumor, rivalry-night, upset, retirement countdown,
    // historic performance, dynasty watch). Keys are emitter-specific
    // strings ("breakout/<player_id>/<year>", etc.). Cleared in
    // run_end_of_year AFTER the year-end emitter pass so each new season
    // gets a fresh slate.
    std::unordered_set<std::string> news_emitted_keys_;

    // Career-scoped dedup set for once-in-a-lifetime achievements
    // (milestone-career/kills/5000/<player>, milestone-career/matches/...,
    // milestone-career/trophies/...). Never cleared — the whole point is
    // that crossing 5000 career kills should fire ONCE in a player's life,
    // not every subsequent year. Persists across year rollovers and
    // survives reset_world() until explicitly cleared by reset_world().
    std::unordered_set<std::string> news_emitted_career_keys_;

    // === MVP race state ===
    // Last MVP-leader name pushed to the news feed via update_mvp_race.
    // Promoted from a function-static so reset_world() can clear it; a
    // function-local static would outlive reset_world() and leak the
    // previous world's leader name into the new world, suppressing the
    // first MVP-takes-lead news item.
    std::string last_mvp_leader_name_;

    // emit_hot_streak_news: tracks consecutive series wins per team across
    // multiple advance_day calls. Resets to 0 on a loss. Emits at 5 / 8 /
    // 12 streak thresholds (tier 1/2/3 headlines).
    std::unordered_map<Team*, int> consecutive_series_wins_;

    // emit_rivalry_match_night: pairwise head-to-head counts maintained
    // across the season. The PairHash below normalises (a,b) vs (b,a) so
    // we get a symmetric counter without two map lookups.
    struct PairHash {
        std::size_t operator()(const std::pair<Team*, Team*>& p) const noexcept {
            // Symmetric: hash the ordered pair (lower-ptr, higher-ptr) so
            // (a,b) and (b,a) collide intentionally.
            auto a = reinterpret_cast<std::uintptr_t>(p.first);
            auto b = reinterpret_cast<std::uintptr_t>(p.second);
            if (a > b) std::swap(a, b);
            return std::hash<std::uintptr_t>()(a) ^ (std::hash<std::uintptr_t>()(b) << 1);
        }
    };
    struct PairEq {
        bool operator()(const std::pair<Team*, Team*>& x,
                        const std::pair<Team*, Team*>& y) const noexcept {
            return (x.first == y.first && x.second == y.second)
                || (x.first == y.second && x.second == y.first);
        }
    };
    // Legacy total counter. Kept as an alias for any external reader that
    // still asks for "total meetings"; new code prefers h2h_total() which
    // sums the two split maps below. Bumped by both the regular- and
    // playoff-tagged increment paths so its semantics are unchanged.
    std::unordered_map<std::pair<Team*, Team*>, int, PairHash, PairEq> h2h_counts_;

    // Split head-to-head counts: regular-season (league stage matchups) vs.
    // playoff (tournament/bracket matchups). Both are season-scoped — cleared
    // at year rollover alongside h2h_counts_.
    std::unordered_map<std::pair<Team*, Team*>, int, PairHash, PairEq> h2h_counts_regular_;
    std::unordered_map<std::pair<Team*, Team*>, int, PairHash, PairEq> h2h_counts_playoff_;

    // One entry per Grand Final (and any LB Final / playoff finishing series)
    // completed this season. Lets the UI render an "all-time finals between
    // these two teams" mini-history. Cleared at year rollover.
    struct H2HSeriesFinal {
        Team*       winner = nullptr;
        Team*       loser  = nullptr;
        std::string event_name;
        int         year         = 0;
        int         day_in_year  = 0;
    };
    std::vector<H2HSeriesFinal> h2h_finals_log_;

public:
    // Head-to-head queries. Symmetric in (a, b) — order doesn't matter.
    // h2h_total returns the sum of regular + playoff meetings this season.
    int h2h_total(Team* a, Team* b) const;
    int h2h_regular(Team* a, Team* b) const;
    int h2h_playoff(Team* a, Team* b) const;
    // Returns every captured Grand Final / LB Final between the two teams
    // (cleared at year rollover — for current-season UI only).
    std::vector<H2HSeriesFinal> h2h_finals_between(Team* a, Team* b) const;

    // Free-agent decay state for UI display. Returns the asking price (in
    // thousand-dollar units) the player is currently demanding, factoring
    // in OFFSEASON daily decay. Equivalent to reading p->contract.amount_k
    // directly — exposed as a method so future implementations can layer
    // additional decay state without breaking UI consumers.
    int fa_current_demand_k(const Player& p) const;

private:

    // OFFSEASON FA price decay (B8). When an offseason day ticks we walk
    // every unsigned Free Agent and shave ~3% off their asking price +
    // mood demand, flooring at 70% of their entered-offseason value. The
    // first decay tick of any given offseason stamps each FA's baseline
    // demand into fa_demand_baseline_k_; reset_world + year rollover wipe
    // the map so the next offseason starts fresh with the (possibly
    // new-rookie) FA cohort.
    std::unordered_map<Player*, int> fa_demand_baseline_k_;

    // Decay step the offseason engine applies — exposed as a member so
    // tests / future tuning can override without recompiling Match.cpp.
    // 0.03 = 3% per day (B8 spec). Min floor 0.70 = 70% of baseline.
    static constexpr double kFADecayRate          = 0.03;
    static constexpr double kFADecayFloorFraction = 0.70;

    // Walk every Free Agent and apply one day of offseason demand decay.
    // Idempotent w.r.t. baseline stamping (only stamps on first sight).
    void run_offseason_fa_decay();

    // emit_historic_performance year-start snapshots. Empty in year 1.
    std::unordered_map<Player*, int> max_kills_snapshot_at_year_start_;
    std::unordered_map<Player*, int> max_kd_snapshot_at_year_start_;
    std::unordered_map<Player*, int> gf_clutches_snapshot_at_year_start_;
    // Last-year season rating per player, used by retirement-countdown to
    // confirm "two consecutive sub-par seasons". Snapshotted at year-end.
    std::unordered_map<Player*, double> last_year_rating_snapshot_;
    // 2-years-ago season rating for the retirement-countdown 2-year window.
    std::unordered_map<Player*, double> two_years_ago_rating_snapshot_;
    bool historic_snapshot_initialized_ = false;

    // === World-level scalars set by the New Game wizard ===
    // 1.0 = neutral. Stored verbatim from NewGameConfig.difficulty so
    // future AI-strength callsites can read it without reaching back to
    // the wizard. Not consumed by the engine yet.
    double world_difficulty_ = 1.0;

    // === News emitters (Deliverable 3) ===
    void emit_breakout_news();          // (1) STAGE 1, past day 50
    void emit_slump_news();              // (2) weekly tick
    void emit_hot_streak_news(Team* winner, Team* loser);  // (3) per series
    void emit_milestone_news();          // (4) year-end
    void emit_retirement_countdown();    // (5) year-end
    void emit_transfer_rumor();          // (6) advance_day stage 1/3
    void emit_rivalry_match_night();     // (7) advance_day matchday
    void emit_massive_upset(const Tournament& tour, Team* winner, Team* loser);  // (8)
    void emit_historic_performance();    // (9) year-end
    void emit_dynasty_watch();           // (10) year-end
    // Snapshot helper for emit_historic_performance — called at the END of
    // run_end_of_year (after emit_historic_performance has fired for this
    // year) so next year's comparison has a baseline.
    void snapshot_year_end_state();
};

}  // namespace vlr
