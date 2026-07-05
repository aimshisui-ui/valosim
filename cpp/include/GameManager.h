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

class SaveGame;   // binary save/load (SaveGame.cpp) — friended below for private world state

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

// One of the three season-start sponsor offers presented to the user at
// preseason (item 4). reward_k is a MODEST one-time lump (75-200) credited to
// the club budget at year-end if the requirement is met.
struct SponsorOffer {
    std::string    name;
    std::string    requirement_label;   // human-readable goal
    SponsorReqType type = SponsorReqType::Placement;
    int            requirement_value = 0;   // meaning depends on type
    int            reward_k = 0;
};

// === Dynamic board objectives (Group E) =================================
// The board sets several CONCURRENT goals at the start of each season — a
// mandatory league placement plus 2-3 "stretch" goals chosen by the club's
// wealth + strategy. Each is evaluated ON-DEMAND from live game state (no
// continuous tracking), surfaced on the dashboard, and tallied at year-end to
// colour the board's verdict + mail.
struct BoardObjective {
    enum class Kind : std::uint8_t {
        Placement,     // finish top-N in the league (the mandatory primary)
        SignStar,      // roster carries a player rated OVR >= target
        DevelopYouth,  // an U22 player reaches OVR >= target
        Finance,       // end the season with budget >= target ($K)
        WinTrophy      // win at least `target` trophies this season
    };
    Kind        kind     = Kind::Placement;
    int         target   = 0;
    bool        mandatory = false;
    std::string text;     // cached human description
};
struct ObjectiveStatus {
    bool        met = false;
    double      pct = 0.0;        // 0..1 progress for a bar
    std::string progress;         // e.g. "currently 3rd" / "best OVR 81 / 84"
};

class GameManager {
public:
    // Save/load needs to read+write the private world state (tick cursors,
    // news dedup sets, world_difficulty_, ...) without widening the public API.
    friend class SaveGame;

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

    // When true, the user has opted to let the AI GM run their team — the
    // year-end deep offseason pass (ai_full_offseason_pass) runs on the user
    // team exactly like every AI team, instead of only the below-5-starters
    // safety net. Lets a player "simulate" a career without hands-on roster
    // management. Cleared by reset_world.
    bool user_auto_manage = false;

    // Pre-match TEAM TALK (one-shot, user-only; dynasty path never sets it so the
    // sim stream is untouched). 0=none, 1=Calm (edge when favored), 2=Fire-up
    // (edge when underdog), 3=Focus (small edge always). Consumed + cleared by
    // the next user series in sim_series_returning_all_matches.
    int user_team_talk = 0;

    // WS-B: the user's CHOSEN club philosophy (B1). Emergent = behave like an AI
    // club (identity derived from state). The 5 others are user-selectable: they
    // set the user team's identity directly AND bias its AI auto-decisions. Reset
    // to Emergent in reset_world; seeded from NewGameConfig at world init.
    enum class ClubPhilosophy : std::uint8_t {
        Emergent, AggressiveAcademy, TacticalMethodical,
        WinNowSpender, DefensiveStructured, BalancedFlexible
    };
    ClubPhilosophy user_philosophy = ClubPhilosophy::Emergent;
    // Map a chosen philosophy to a fixed TeamIdentity (user_chosen=true); for
    // Emergent it falls back to compute_team_identity(base). Single mapping point.
    static TeamIdentity philosophy_to_identity(ClubPhilosophy p, const Team& base);

    // FA coach pool for the coach market (run_coach_market). Expired + poached
    // coaches land here (reputation intact) and get re-hired, so a decorated
    // name circulates through the league rather than vanishing. Kept stocked
    // with fresh up-and-comers each year.
    std::vector<CoachPtr> free_coaches_;
    std::vector<ScoutPtr> free_scouts_;   // WS-A: FA scout pool (mirrors free_coaches_)
    std::vector<AnalystPtr> free_analysts_; // Match-Prep: FA analyst pool (mirrors free_scouts_)

    // NEVER-FREE retention pools for RETIRED staff. A retiring coach/scout is
    // pulled off its team but must not have its last shared_ptr dropped mid-world
    // (the invariant + WS-B career_coty history depend on it surviving). Unlike
    // free_coaches_ these are append-only and uncapped — retired staff are never
    // re-hired (run_coach_market filters is_retired) so they only need to persist,
    // mirroring how retired PLAYERS persist in the solo_q ladders. Cleared only in
    // reset_world alongside free_coaches_/free_scouts_.
    std::vector<CoachPtr> retired_coaches_;
    std::vector<ScoutPtr> retired_scouts_;
    std::vector<AnalystPtr> retired_analysts_;   // never-free: retired analysts persist

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

    // WS-C international qualification: each region's LATEST regional-playoff
    // finish, in PLACEMENT order [1st, 2nd, 3rd, 4th]. Captured when a Regionals
    // tournament finishes; consumed at Masters (top 3/region) / Champions (top
    // 4/region) generation so internationals are seeded by who actually PLACED
    // in the regional playoffs (not season win totals). Overwritten per region
    // each regional cycle; cleared in reset_world.
    std::unordered_map<std::string, std::vector<TeamPtr>> intl_qualifiers_;

    // === Multi-tier pyramid (Challengers / lower divisions) ==============
    // region -> [tier1, tier2, ...]. Index 0 ALIASES the same LeaguePtr as
    // leagues[region] (shared_ptr — teams owned once). The legacy `leagues`
    // map stays tier-1-only so every region-keyed call site is untouched;
    // the full pyramid lives here. Tiers are 1-based (index = tier - 1).
    std::unordered_map<std::string, std::vector<std::shared_ptr<League>>> tier_leagues_;
    int t2_week = 0;  // separate stage cursor for lower-tier round-robins

    // Last year-end promotion/relegation outcome (surfaced in UI + news).
    struct PromoRelResult {
        std::string region;
        std::vector<TeamPtr> promoted;       // moved UP into tier-1
        std::vector<TeamPtr> relegated;      // moved DOWN into tier-2
        TeamPtr ascension_champion;          // tier-2 team that won the Ascension bracket
    };
    std::vector<PromoRelResult> last_promo_rel_;

    // Tier lookups (1-based; default tier 1). These scan the small per-region
    // league vector (<=12 teams each) — no cached Team field needed.
    std::shared_ptr<League> league_at(const std::string& region, int tier) const;
    std::shared_ptr<League> league_for(const TeamPtr& t) const;  // the league a team sits in now
    int tier_count(const std::string& region) const;
    int tier_of(const TeamPtr& t) const;  // a team's current tier; 1 if not found

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

    // USER-driven staff hire/replace (web bridge action). Resolves a free-agent
    // staffer by stable id from the matching FA pool (free_coaches_/free_scouts_/
    // free_analysts_) and installs them on the user team, recycling any displaced
    // incumbent back into the pool exactly like run_coach_market's poach path — so
    // it is NEVER-FREE safe (no staff shared_ptr is dropped). role is one of
    // "coach", "scout", "analyst". No rng, no struct change, balance-neutral;
    // mutates only the user's session world. Returns true on a successful hire.
    bool user_hire_staff(const std::string& role, std::uint64_t staff_id);

    // USER-driven staff FIRE (web action). Recycles the incumbent coach/scout/
    // analyst back into its FA pool (set team_name="Free Agent", push_back) then
    // nulls the team slot — NEVER-FREE safe (mirrors run_coach_market's expiry; the
    // staffer's shared_ptr survives in the pool, re-hireable). role one of
    // "coach"/"scout"/"analyst". Returns false if no such incumbent. User session only.
    bool user_fire_staff(const std::string& role);

    // Resolve a player by stable id across the WHOLE world — every tier-1 league
    // roster + every region's SoloQ ladder (free agents + retired persist there)
    // + the hall of fame. Read-only; returns nullptr if not found. Powers the web
    // player-profile modal which can open ANY player (rival/FA/retired/HoF), unlike
    // the god-mode verbs which intentionally resolve only user_team->roster.
    PlayerPtr find_player_by_id(std::uint64_t id) const;

    // Walk all leagues and sync every rostered player to their team's
    // region. Called at world-init + year-end to keep ladders consistent
    // with roster moves.
    void sync_all_ranked_regions();

    // List of upcoming stage matchups by day (used by the Calendar UI).
    std::vector<UpcomingFixture> upcoming_fixtures(int max_days_ahead = 14) const;

    // === Opposition scouting report (Scouting&Match-Prep increment B) =========
    // A COMPUTED pre-match view of an opponent (MoodBreakdown/mvp_score_breakdown
    // pattern — no persisted state, safe to call per-frame). Section DEPTH and
    // figure ACCURACY both scale with the USER's head_analyst: a weak/absent
    // analyst gives a shallow, fuzzy report; a strong one reveals key players,
    // comp tendencies, role reads + a recommended counter. Suite-only (read-only
    // flavour; never touches the match/comp/duel).
    struct OppositionReport {
        bool        valid = false;          // false if opp has no usable data
        std::string opp_name, opp_region;
        int         detail_level = 1;       // 1..5 sections unlocked (analyst depth)
        double      accuracy = 0.0;         // 0..0.6 (analyst report_accuracy_mult)
        bool        has_analyst = false;
        int         opp_ovr_est = 0;        // estimated opponent roster OVR
        int         ovr_band = 0;           // +/- uncertainty (shrinks with accuracy)
        std::string form;                   // recent W-L (most recent first)
        std::vector<std::string> key_players;   // depth>=2: "Name (Role, OVR)"
        std::string comp_tendency;          // depth>=3: preferred comp tag
        std::string role_read;              // depth>=4: strongest / softest role
        std::string recommendation;         // depth>=5: suggested counter
    };
    OppositionReport scout_opposition(const Team& opp) const;

    void favorite_player(const PlayerPtr& p);
    void unfavorite_player(const PlayerPtr& p);
    bool is_favorited(const PlayerPtr& p) const;

    // === Watchlist (Scouting&Match-Prep increment C) ==========================
    // Transfer/scouting TARGET list, DISTINCT from favorite_players (the GOAT/HoF
    // bookmark). Holds a PlayerPtr (never-free, survives retirement) + a user
    // note + a manual status. Dedup is by player identity (pointer == the exact
    // player, so it is namesake-safe). Cleared at new world like favorites.
    enum class WatchStatus : std::uint8_t { Watching, Scouting, Bidding };
    struct WatchEntry {
        PlayerPtr   player;
        std::string note;
        WatchStatus status = WatchStatus::Watching;
    };
    void watch_player(const PlayerPtr& p);
    void unwatch_player(const PlayerPtr& p);
    bool is_watched(const PlayerPtr& p) const;
    WatchEntry* watch_entry(const PlayerPtr& p);   // editable entry; nullptr if not watched
    std::vector<WatchEntry>&       watchlist()       { return watchlist_; }
    const std::vector<WatchEntry>& watchlist() const { return watchlist_; }

    std::vector<PlayerPtr>      favorite_players;
    std::vector<WatchEntry>     watchlist_;     // transfer/scouting targets
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
        std::vector<double>    scores;     // parallel to finalists/coach_finalists
        std::string explanation;
        // WS-B: a SeasonAward represents EITHER a player award (winner set) or a
        // coach award (coach_winner set, e.g. "Coach of the Year"). The UI keys
        // off whichever pointer is non-null. CoachPtr keeps the coach alive in
        // the append-only history exactly like player awards (never-free safe).
        CoachPtr    coach_winner;
        std::vector<CoachPtr> coach_finalists;
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

    // === Mail / Inbox ===
    // A per-USER-club inbox. The engine pushes a MailItem at user-relevant
    // event sites (contract expiry, awards, transfers, board objectives,
    // results...) and the UI shows them with an unread badge + reading pane.
    //
    // CRITICAL never-free invariant: a MailItem stores ONLY strings + ids,
    // NEVER a PlayerPtr/Team* — the inbox can outlive the entities it
    // references (a player retires/leaves), so deep-links resolve by NAME at
    // click time and no-op if the target is gone. Items are user-club-relevant
    // BY CONSTRUCTION (every generation site guards on user_team), so the UI
    // needs no per-row ownership filter. In-memory only (no save/load exists).
    enum class MailCategory {
        Board,      // board objectives, expectations, end-of-season verdicts
        Transfer,   // poach/market moves, rival interest, scouting completed
        Contract,   // expiry warnings, re-sign success/fail, discontent
        Award,      // your player wins MVP/role/IGL/Rookie/HoF induction
        Squad,      // morale/transfer-request, breakout, retirement countdown
        Result,     // your tournament champion/runner-up, big upset
        Media,      // sponsor messages, milestone press, dynasty/streak press
        COUNT       // sentinel — number of categories (for UI iteration)
    };
    enum class MailLink {
        None,
        Player,      // OpenPlayerModal(player_name) -> profile modal
        Market,      // jump to the transfer Market screen
        Manager,     // jump to the Manager (board/objectives) home
        Finance,     // jump to the Finance dashboard
        Roster,      // jump to the Roster screen
        Negotiation  // open the re-sign modal for player_name (must be owned)
    };
    struct MailItem {
        int          id        = 0;     // monotonic, from next_mail_id_
        int          year      = 0;     // GameManager::year at push
        int          day       = 0;     // GameManager::day_in_year (0..199)
        MailCategory category  = MailCategory::Media;
        std::string  subject;           // list line + reading-pane title
        std::string  body;              // reading-pane prose (multi-line ok)
        bool         read      = false;
        bool         important = false; // protected from cap-drop; starred
        std::string  player_name;       // deep-link DISPLAY name (empty if N/A)
        std::uint64_t player_id = 0;    // STABLE deep-link key (0 = none/legacy).
                                        // Resolve by THIS first; player_name is a
                                        // non-unique gamertag (namesakes exist),
                                        // so name-only resolution opened the wrong
                                        // player when an active namesake shadowed
                                        // a retired subject (HoF / retirement mail).
        std::string  team_name;         // context (empty if N/A)
        MailLink     link      = MailLink::None;
        int          amount_k  = 0;     // optional money context (fee/bonus)
    };
    std::vector<MailItem> mailbox;          // most-recent first; capped 150
    int next_mail_id_ = 1;
    std::unordered_set<std::string> mail_emitted_keys_;  // per-season dedup

    // Per-season board objective state (issued at season start, judged at
    // year end). Strings/ints only; cleared in reset_world like the inbox.
    std::string current_board_objective_;   // human-readable target line
    int board_target_placement_ = 0;        // <=N final placement target (0=n/a)
    double board_ambition_ = 0.0;           // scaled expectation score (verdict tone)
    std::vector<BoardObjective> board_objectives_;   // dynamic goal set (Group E)

    // === Scouting fog-of-war (Group C) ===================================
    // The user org's scouting assignments left THIS SEASON. Each scout reveals
    // one unscouted player's exact potential (Player::potential_scouted) — a
    // permanent reveal. Refilled at season start by reset_scout_credits() to a
    // base + prestige bonus so better-run orgs see more of the market. The fog
    // is UI-only pressure on the USER: AI orgs reason on true values internally.
    int scout_credits_ = 0;
    int scout_credits_year_ = -1;   // last season the pool was refilled (dedup)

    // === Preseason buffer (item 2) — boot-only, NOT a recurring phase ===
    // After team creation the first few advance_day calls are no-match preseason
    // days hosting the board email + sponsor pick + roster setup, before Stage 1.
    // Kept off the 11-phase ring (a recurring phantom phase would break the
    // 200-day calendar) via these flags.
    bool in_preseason_buffer_ = false;
    int  preseason_days_left_ = 0;

    // === Season-start sponsor offer (item 4) — presented each season ===
    // Set at the start of every season (preseason day 1 + each OFFSEASON->STAGE1
    // wrap). The UI shows a 3-offer modal while pending; choose_sponsor clears it.
    bool sponsor_choice_pending_ = false;
    std::vector<SponsorOffer> pending_sponsor_offers_;

    // Fire the per-season setup: issue the board objective + generate the 3
    // sponsor offers and flag the choice as pending. Called from the preseason
    // buffer (season 1) and the OFFSEASON->STAGE1 wrap (seasons 2+).
    void start_of_season_setup(std::vector<std::string>& log);

    // Push a mail to the user inbox. Stamps id + (year/day if left 0), inserts
    // at the head, and enforces the bounded-storage drop policy: if over the
    // cap, drop the OLDEST read && !important item; only if none qualifies drop
    // the absolute oldest. Protects unread + important Board mail from loss.
    void push_mail(MailItem m);
    // Count of unread mail (drives the nav badge). Cheap (<=150 items).
    int unread_mail_count() const;
    // Per-season dedup guard: returns true the FIRST time a key is seen this
    // season (and records it); false on repeats. Keys are cleared at year
    // rollover alongside news_emitted_keys_ so the same event mails next
    // season. Use a key like "mail/<event>/<player_or_team>/<year>".
    bool mail_seen_this_season(const std::string& key);

    // Convenience chokepoint for the event sites: push a mail to the user
    // inbox IFF `team` names the user's club, deduped per-season by
    // `dedup_key`. No-op pre-game (user_team null) or when team != user club,
    // so every call site stays a guarded one-liner and the user-team-ONLY
    // invariant lives in exactly one place.
    // player_id (trailing, default 0): the STABLE deep-link key for the subject.
    // Pass p->id at every MailLink::Player / MailLink::Negotiation site so the
    // reading pane can open the EXACT player even across namesakes / retirement.
    // Left 0 for non-player mail (and legacy callers, which still compile).
    void mail_user(const std::string& team, const std::string& dedup_key,
                   MailCategory cat, const std::string& subject,
                   const std::string& body, MailLink link = MailLink::None,
                   const std::string& player_name = "",
                   int amount_k = 0, bool important = false,
                   std::uint64_t player_id = 0);

    // Season-start: derive the board's placement objective from the user club's
    // wealth tier + league size, store it (current_board_objective_/
    // board_target_placement_), and mail it — plus contract-expiry warnings for
    // owned players entering their final year. No-op pre-game.
    void issue_board_objective(std::vector<std::string>& log);

    // === Dynamic board objectives (Group E) ==============================
    // Regenerate the season's objective set (mandatory placement + 2-3 stretch
    // goals by club profile). Called from issue_board_objective. No-op pre-game.
    void generate_board_objectives();
    const std::vector<BoardObjective>& board_objectives() const { return board_objectives_; }
    // Evaluate one objective against the CURRENT game state (standings, roster,
    // budget, this-season trophies). Pure read — safe to call every frame.
    ObjectiveStatus evaluate_objective(const BoardObjective& o) const;
    // How many objectives are currently met (year-end verdict input).
    int board_objectives_met() const;
    // Year-end (BEFORE the wins reset): compute the user's final placement,
    // mail a met/missed verdict against the stored objective, and a sponsor
    // outlook mail. Clears the objective so next season issues a fresh one.
    void judge_board_objective();

    // === Scouting fog-of-war (Group C) ===================================
    int  scout_credits() const noexcept { return scout_credits_; }
    // WS-A INC-4: the transfer-window gate. Returns true today (no behaviour
    // change). It currently wraps the USER's discretionary signings (Make-Offer
    // + the two GUI FA-sign paths) and the AI scouting-pass poach. NOTE: the AI
    // OFFSEASON market (B8 walk-poach, ai_full_offseason_pass, lower-tier) and
    // in-season run_mid_season_replacements are NOT yet wrapped — the future
    // "transfer windows" workstream must add those guards (each BEFORE its
    // release_player, to stay never-free) when it makes the AI respect windows.
    bool transfers_open() const noexcept { return true; }

    // Refill scouting assignments for a new season (driven by the head scout).
    void reset_scout_credits();
    // The deterministic id-seeded potential band for a player, shrunk toward the
    // true value by the USER's head-scout quality (band_tighten_mult). The ENGINE
    // owns the hash so Market + Profile + every frame agree (WS-A INC-3). The
    // true potential always stays inside [lo,hi]; never pinned exactly.
    void scouted_band(const Player& p, int& lo, int& hi) const;
    // Spend one credit to permanently reveal a player's exact potential.
    // Returns false (no-op) if out of credits, the player is null/already
    // scouted, or there's no user org.
    bool scout_player(const PlayerPtr& p);

    // === Scout assignment / focus (Scouting&Match-Prep increment D) ==========
    // Direct the head scout to a FOCUS so the season's scout_credits_ auto-reveal
    // matching prospects (highest-ceiling first) instead of the user clicking
    // each player. Persists until changed; cleared at new world. Suite-only (it
    // only flips the UI-fog flag potential_scouted + spends credits — AI reasons
    // on true values regardless, so no on-field / AI-decision change).
    enum class ScoutFocus : std::uint8_t { None, Region, Role, Watchlist };
    ScoutFocus  scout_focus_ = ScoutFocus::None;
    std::string scout_focus_region_;              // for ScoutFocus::Region
    Role        scout_focus_role_ = Role::Count;  // for ScoutFocus::Role
    // Season-start: spend remaining scout_credits_ on the assigned focus. Called
    // from start_of_season_setup right after reset_scout_credits().
    void run_scout_assignment(std::vector<std::string>& log);

    // === Time-based scouting assignments (web "Living Scouting Network") =======
    // A commissioned assessment of one player over 1-3 weeks. Duration + accuracy
    // are driven by the head scout's COVERAGE of the target's country (Scout.h's
    // scout_country_coverage) + the money invested. USER-ONLY: the queue is empty
    // on the dynasty/test path, so tick_scout_assignments is a no-op there and the
    // --dynasty stream stays byte-identical.
    struct ScoutAssignment {
        std::uint64_t player_id = 0;
        int  days_remaining = 0;
        int  total_days     = 0;
        int  money_k        = 0;
        int  coverage       = 0;   // scout coverage of the target's country (0-100)
        int  depth          = 0;   // scout read depth (0-100)
        bool done           = false;
    };
    std::vector<ScoutAssignment>           scout_assignments_;      // cleared in reset_world
    std::unordered_map<std::uint64_t, int> scout_report_accuracy_;  // player_id -> final accuracy 0-100

    // Commission the user's head scout to assess `target` over 1-3 weeks; deducts
    // money_k from the club budget. Faster/deeper with more coverage + money; an
    // out-of-knowledge target is slow, costly and vague. Returns the planned
    // duration in days (0 = failed). Replaces any active assignment for the player.
    int  commission_scout(const PlayerPtr& target, int money_k);
    // Tick every active assignment one day; complete + reveal any that finish.
    // Called once per advance_day. No-op when the queue is empty (dynasty-safe).
    void tick_scout_assignments();
    // Final report accuracy for a completed scout (coverage x depth), 0 if none.
    int  scout_report_accuracy(std::uint64_t player_id) const;
    // The active (unfinished) assignment for a player, or nullptr.
    const ScoutAssignment* active_scout_assignment(std::uint64_t player_id) const;

    // === FM-style scouting BRIEFS — a filter-based assignment the head scout
    // works over game-time, revealing matching players (potential_scouted +
    // scout_report_accuracy) within the scout's region COVERAGE (Scout.h). USER-
    // ONLY: the vector is empty on the dynasty/test path so tick_scout_briefs is a
    // no-op there and the --dynasty stream stays byte-identical.
    struct ScoutBrief {
        std::string name;
        int  role_idx = -1;       // -1 = any role
        int  age_min  = 15;
        int  age_max  = 40;
        int  min_pot  = 0;        // minimum ACTUAL potential to surface
        std::string region;       // "" = any region (scout coverage still gates)
        int  days_active = 0;     // ticks since created
        int  revealed    = 0;     // matching players scouted so far
        int  match_total = 0;     // matching players in the world (recomputed each tick)
    };
    std::vector<ScoutBrief> scout_briefs_;   // cleared in reset_world
    bool brief_matches(const ScoutBrief& b, const Player& p) const;
    void add_scout_brief(const ScoutBrief& b);
    void clear_scout_brief(int idx);
    // Tick every brief one day: the head scout reveals a few matching, in-coverage,
    // unscouted players. Called once per advance_day. No-op when empty (dynasty-safe).
    void tick_scout_briefs();

    // The user's club name is EXCLUSIVE: any generated club (any region/tier) that
    // carries the same name gets a fresh procedural identity, so brackets/standings
    // never show two clubs with one name. Called after the wizard rename and after a
    // god-mode rename. User-path only — never on the dynasty sim.
    void resolve_user_name_collision();

    // Collision-proof "is this player on MY roster?" — matches by player id against
    // user_team->roster, NOT by team-name string. By-name resolution breaks when the
    // user's club name coincides with a rival's (own players then read as scoutable/
    // biddable rivals); id membership is unambiguous. Used by every export that gates
    // scout/bid/fog on ownership.
    bool player_on_user_team(std::uint64_t player_id) const;

    // === User Make-Offer for contracted players (Group D) ================
    struct TransferBidOutcome {
        enum Code { Accepted, Rejected, CantAfford, Invalid } code = Invalid;
        int fee_k    = 0;   // the fee that was offered
        int asking_k = 0;   // the seller's asking threshold (counter hint)
        std::string message;
    };
    // Submit a bid of `fee_offer_k` for `target` (owned by `seller`). Validates,
    // checks the user club's affordability (fee + wage), then the seller's
    // will_sell; on acceptance executes the move (fee paid seller<-buyer, player
    // released + signed to the user club — never-free safe). years 0 = engine
    // picks the contract length.
    TransferBidOutcome user_transfer_bid(const TeamPtr& seller, const PlayerPtr& target,
                                         int fee_offer_k, int years);

    // DIAGNOSTIC (read-only): measure the live tier-1 season-KD distribution +
    // the international-exposed subset (mean over deaths>0, p50, p90, max) and
    // enumerate intl outliers (intl maps>20 && kd>1.5). Appends summary lines to
    // `out`. Used to calibrate KD-compression targets BEFORE any tuning.
    void diagnose_season_kd(std::vector<std::string>& out) const;

    // DIAGNOSTICS (read-only) for the anti-dynasty + intl-KD tuning. Run after
    // an N-season sim; both read accumulated persistent state.
    //  - dynasty: scans tier-1 trophy_cases for back-to-back world champions +
    //    3-titles-in-5y windows + champion parity (how wide-open the league is).
    //  - intl KD: scans per-tournament buckets (tourn_stats) for Masters/
    //    Champions, computing the TRUE intl-map K/D each season (not diluted by
    //    domestic maps) + how often a 1.5+ intl season occurs.
    void diagnose_dynasty_rate(std::vector<std::string>& out) const;
    void diagnose_intl_kd(std::vector<std::string>& out) const;

    // === Season-start sponsor (item 4) ===
    // Build 3 tier-1 sponsor offers for the user club to choose from at
    // preseason (Prestige placement / Performance win-count / Growth milestone).
    std::vector<SponsorOffer> generate_sponsor_offers() const;
    // Stamp the user's chosen offer onto user_team (resets the credited flag).
    // Call with sponsor_active left false (a declined/empty offer) to clear.
    void choose_sponsor(const SponsorOffer& offer);
    // Year-end: if the active sponsor's requirement was met, credit the modest
    // lump to the user club budget + mail confirmation. Idempotent (credited
    // flag). Reads final standings, so it runs BEFORE the year-end wins reset.
    void settle_sponsor();
    // User's final placement in their tier-1 league: {rank, league_size} by
    // (wins desc, losses asc). {0,0} if the user club isn't in a tier-1 league.
    std::pair<int, int> user_league_placement() const;

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
    // Transparent per-factor breakdown of the MVP composite — lets the UI show
    // the user EXACTLY why a player tops the chart (rating x clutch x IGL x intl
    // x playoff x team = total). `factors` carries only the NON-neutral
    // multipliers (label, value) for compact display; `total` is their product
    // x rating. qualified=false (total 0) below the 0.95 rating floor.
    struct MvpFactorBreakdown {
        double rating = 0.0, clutch = 1.0, igl = 1.0, sos = 1.0,
               playoff = 1.0, team = 1.0, total = 0.0;
        bool   qualified = false;
        bool   is_igl = false, attended_intl = false, won_intl = false;
        int    clutch_pts = 0, pressure_matches = 0, intl_matches = 0, team_wins = 0;
        std::vector<std::pair<std::string, double>> factors;  // (label, multiplier)
    };
    struct RaceCandidate {
        PlayerPtr   player;
        double      score = 0.0;
        std::string blurb;   // "Up 2 spots", "Climbing fast", "Slipping"
        int         delta = 0;   // change vs. previous tick
        MvpFactorBreakdown breakdown;   // populated for the MVP Race category
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
    // WS-C: when a REGIONALS tournament finishes, record its playoff PLACEMENT
    // [1st,2nd,3rd,4th] into intl_qualifiers_[region] so the next international
    // is seeded by who actually placed (Masters top-3, Champions top-4). No-op
    // for non-Regionals tournaments.
    void capture_regional_qualifiers(const Tournament& tour);
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

    // Shared season MVP / Player-of-the-Year composite score. Used by BOTH
    // compute_season_awards (the actual award) and update_mvp_race (the
    // season-long race the player watches) so the race predicts the award
    // rather than ranking by a divergent formula. Returns 0 below the rating
    // floor. team_wins_for_player() is the supporting team-wins lookup.
    double mvp_composite_score(const Player& p) const;
    int    team_wins_for_player(const Player& p) const;
    // (MvpFactorBreakdown is declared above, before RaceCandidate, which holds one.)
    MvpFactorBreakdown mvp_score_breakdown(const Player& p) const;

    // Count how many of the Hall-of-Fame accomplishment criteria a player
    // meets (0..10). Shared by the HoF induction gate AND the retirement
    // send-off (which fires for HoF-bound / near-miss careers) so the two
    // never drift apart. Pure read; does NOT apply the career-seasons floor
    // or the induction threshold — callers decide those.
    int hof_criteria_count(const Player& p) const;

    // Initial snake-draft roster fill. Used at world init (and after a
    // big league reset) to give every team a believable starting roster
    // built from the same shared FA pool. Each round each team picks
    // the best available player by their strategy/comp/needs. Respects
    // import cap, salary range, and the one-IGL-per-team rule.
    void run_initial_snake_draft(std::vector<std::string>& log);

    // === Multi-tier helpers (Challengers + promotion/relegation) =========
    // Build `count` teams for a region at a given tier, using kTiers ranges
    // for budget/prestige so lower tiers are generated weaker than tier-1.
    std::vector<TeamPtr> build_tier_teams(const std::string& region, int tier);
    // After the tier-1 snake draft, generate every lower-tier league + roster
    // (cheap auto_fill from the leftover regional FA pool). Populates
    // tier_leagues_[region][1..]. tier_leagues_[region][0] is set by the
    // caller to alias leagues[region].
    void generate_lower_tiers(std::vector<std::string>& log);
    // Simulate one lower-tier stage round (all regions, all tiers>=2) using
    // the t2_week cursor. Mirrors play_stage_round but records into the
    // lower-tier leagues' team W/L.
    void play_tier_stage_round();
    // Year-end Ascension bracket + relegation gauntlet per region. Inserted
    // in run_end_of_year AFTER roster cleanup, BEFORE the per-league rebuild
    // loop regenerates schedules. Moves TeamPtrs between tiers (no destroy).
    void resolve_promotion_relegation(std::vector<std::string>& log);
    // Run a single-elim bracket over `field` to a champion, synchronously,
    // via the existing series engine (is_league_play=false → no phantom W/L).
    TeamPtr run_bracket_to_completion(std::vector<TeamPtr> field,
                                      const std::string& event_name);
    // Move a team between adjacent tiers (pointer move; region unchanged).
    // apply_promotion moves t UP from (to_tier+1) into to_tier; apply_relegation
    // moves t DOWN from from_tier into (from_tier+1). Budget/prestige adjust.
    void apply_promotion(const TeamPtr& t, const std::string& region, int to_tier);
    void apply_relegation(const TeamPtr& t, const std::string& region, int from_tier);
    // Run the FULL offseason maintenance (cleanup of retired/expired,
    // ai_full_offseason_pass, W/L reset, coach handling, schedule regen) for
    // every lower-tier league — mirrors what the tier-1 loop does. Fixes the
    // "tier-2 never refreshes / wins never reset" defects (feasibility B1/B2).
    void run_lower_tier_offseason(std::vector<std::string>& log);

    // FM-depth SCOUTING PIPELINE. After the tier-1 offseason settles rosters,
    // each tier-1 AI team scouts TWO talent sources its offseason FA pool never
    // sees: (1) tier-2 RISERS (high-ceiling / hot-form players rostered on
    // Challengers teams) and (2) top SOLO-Q free agents. It signs one ONLY when
    // Team::score_scout_target says the player fills a genuine role hole/need
    // AND is a real upgrade over the incumbent. Poaches route through
    // release_player + sign_player (never-free safe); the poached tier-2 hole is
    // refilled by run_lower_tier_offseason, which runs right after. User team
    // skipped unless user_auto_manage.
    void run_scouting_pass(int sign_year, std::vector<std::string>& log);

    // FM-depth finance: at year-end, project each club's next-season committed
    // payroll, income, wage envelope, and wealth tier (READ-ONLY signals the AI
    // spend paths + the finance dashboard consume). No gating here.
    void project_team_finances(Team& t, int tier, int year);

    // Dynamic sponsorship (FM-depth divergence): the market target an org would
    // command from prestige + recent results + star power, and an EMA step toward
    // it so income DRIFTS (compounds) rather than snapping — rich orgs pull away,
    // a bad run erodes income, all clamped to [kSponsorFloorK, kSponsorCeilK] so
    // there's no runaway or death spiral.
    int  predict_sponsorship_k(const Team& t) const;
    void apply_dynamic_sponsorship(Team& t, int tier, int year);

    // FM-depth COACH MARKET: expire AI coaches whose contracts ended into the FA
    // pool, then hire/POACH — a rich org lands the best available (highest-rep
    // affordable) coach and poaches a much better one over its current; a poor
    // org settles for a cheaper up-and-comer. Skips the user team (user-managed).
    void run_coach_market(std::vector<std::string>& log);
    void run_scout_market(std::vector<std::string>& log);   // WS-A: scout staff market
    void run_analyst_market(std::vector<std::string>& log); // Match-Prep: analyst staff market
    // WS-A INC-6: scan every tier at year-end for YOUNG players who just signed
    // their FIRST pro deal (a soloq grinder breaking into the pyramid) and turn
    // it into a visible "Prospect" news beat + a user mail (own academy signings
    // + league-wide generational talents); credits the signing club's scout.
    void announce_prospect_discovery(std::vector<std::string>& log);

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
