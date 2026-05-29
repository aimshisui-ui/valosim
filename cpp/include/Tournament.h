#pragma once

#include "Series.h"
#include "Team.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vlr {

class SoloQEngine;

enum class TournamentFormat : std::uint8_t {
    SingleElim,
    DoubleElim,
    GroupThenPlayoffs
};

struct GroupStanding {
    TeamPtr team;
    int     wins   = 0;
    int     losses = 0;
    int     map_diff = 0;
    int     round_diff = 0;
    bool    eliminated = false;
};

// Per-player aggregated stat line across an ENTIRE tournament. One entry per
// player who has played >=1 map so far. Safe to read mid-event. Built
// incrementally as each Series resolves (see aggregate_player_stats()).
// Counting fields are SUMS; rating/adr/hs_pct/kast are maps-weighted means.
struct TournamentPlayerStat {
    Player* player = nullptr;          // never null in the returned vector
    std::string team_name;             // team they played for in this event
    int    maps         = 0;
    int    rounds       = 0;
    int    kills        = 0;
    int    deaths       = 0;
    int    assists      = 0;
    int    first_kills  = 0;
    int    first_deaths = 0;
    int    clutches     = 0;
    double rating       = 0.0;   // aggregate VLR rating across maps
    double adr          = 0.0;   // avg damage / round
    double hs_pct       = 0.0;   // 0..100
    double kast         = 0.0;   // 0..100
    std::string top_agent;       // most-played agent name (may be "")
};

// One played matchup snapshot. The UI uses this to render the full bracket
// retrospectively (all rounds played + the next round scheduled).
struct BracketMatch {
    TeamPtr a, b;          // a is top seed/UB origin, b is bottom seed/LB origin
    TeamPtr winner;        // null if not yet played
    int     a_score = 0;   // map wins (BO3/BO5 series)
    int     b_score = 0;
    bool    played   = false;
    bool    is_lower = false;
    int     best_of  = 3;  // 3 for all bracket matches except LB Final + GF (5)
    std::string label;     // "Upper Bracket Semifinal" etc.
};

class Tournament {
public:
    // Default ctor: DoubleElim. SingleElim only as a fallback for <4 teams.
    Tournament(std::string name, std::vector<TeamPtr> teams,
               TournamentFormat format = TournamentFormat::DoubleElim);

    std::vector<Series> play_round(std::unordered_map<std::string, SoloQEngine*>& solo_qs,
                                   int current_year);

    bool finished() const noexcept { return current_phase_ == Phase::Done; }
    const TeamPtr& champion() const noexcept { return champion_; }
    const TeamPtr& runner_up() const noexcept { return runner_up_; }
    const std::vector<TeamPtr>& semifinalists() const noexcept { return semifinalists_; }
    int round_num() const noexcept { return round_num_; }
    const std::string& name() const noexcept { return name_; }
    TournamentFormat   format() const noexcept { return format_; }

    // Currently-scheduled matchups. For DoubleElim, `current_matchups_` are
    // upper-bracket games and `lower_matchups_` are lower-bracket games for
    // the same round. Both sets play during one play_round() call.
    const std::vector<std::pair<TeamPtr, TeamPtr>>& current_matchups() const noexcept {
        return current_matchups_;
    }
    const std::vector<std::pair<TeamPtr, TeamPtr>>& lower_matchups() const noexcept {
        return lower_matchups_;
    }

    bool user_team_in_round(const TeamPtr& user) const;

    const std::vector<std::vector<GroupStanding>>& groups() const noexcept { return groups_; }
    bool group_stage_complete() const noexcept { return group_stage_complete_; }

    const std::vector<TeamPtr>& eliminated() const noexcept { return eliminated_; }

    // Initial seeded teams (UB R1 entry order). Empty until start_bracket runs.
    const std::vector<TeamPtr>& initial_seeding() const noexcept { return initial_seeding_; }
    // Full history of UB/LB rounds played so far. Indexed by round number,
    // each entry is the list of matchups in that round (with results).
    const std::vector<std::vector<BracketMatch>>& ub_history() const noexcept { return ub_history_; }
    const std::vector<std::vector<BracketMatch>>& lb_history() const noexcept { return lb_history_; }
    // Played GF. Exactly 1 entry once the GF resolves. (User spec 2026-05-14:
    // VCT-style single GF — no bracket reset.)
    const std::vector<BracketMatch>& gf_history() const noexcept { return gf_history_; }

    // Double-elim queries — useful for the tournament UI to label rounds.
    bool in_grand_final() const noexcept { return phase_label_ == "Grand Final"; }
    const std::string& current_round_label() const noexcept { return phase_label_; }

    // Bracket round counts (size of the per-side history vectors). Useful for
    // smoke tests that assert exactly 3 UB rounds + 4 LB rounds in an 8-team
    // double-elim, etc.
    int upper_bracket_round_count() const noexcept {
        return static_cast<int>(ub_history_.size());
    }
    int lower_bracket_round_count() const noexcept {
        return static_cast<int>(lb_history_.size());
    }
    // Convenience: best-of count for the next match in a particular side
    // (used by UI to render "BO3" / "BO5" badges on upcoming matchups).
    // Returns 0 if no LB/UB match is currently scheduled on that side.
    int current_upper_best_of() const noexcept { return current_ub_bo_; }
    int current_lower_best_of() const noexcept { return current_lb_bo_; }

    // Players who appeared as starters in at least one map of this tournament.
    // Built incrementally as Series finish. award_event_titles filters the
    // champion's roster against this set so signed-but-never-played roster
    // members don't receive a phantom trophy.
    const std::unordered_set<Player*>& participated_players() const noexcept {
        return participated_players_;
    }

    // Snapshot of the actual title-receiving roster: players who BOTH sat on
    // the champion's roster at award time AND played at least one map.
    // Empty until award_event_titles fires. Read-only for UI / audit tooling.
    const std::vector<Player*>& winning_roster_snapshot() const noexcept {
        return winning_roster_snapshot_;
    }

    // Per-player stats aggregated across every map played in this tournament
    // so far (groups + playoffs). One entry per player with >=1 map. Empty
    // before the first map. Safe to call mid-event. Returned by value;
    // entries never contain a null player. Counting stats are sums;
    // rating/adr/hs_pct/kast are maps-weighted means.
    std::vector<TournamentPlayerStat> aggregate_player_stats() const;

    // One-shot guard: true once award_event_titles has run for this tournament.
    // External callers (force_finish_stale_tournaments etc.) can check this
    // before re-invoking award logic, though the internal guard inside
    // award_event_titles is the authoritative one.
    bool titles_already_pinned() const noexcept { return pinned_titles_already_; }

    // Debug-only bracket sanity walker. Returns true if the bracket state
    // looks internally consistent (alive pools + history + GF state agree),
    // false otherwise — in which case out_err is filled with a 1-line
    // explanation of the first anomaly found. Intended for the UI to wire
    // an assertion / debug-overlay; never affects play_round behavior.
    // Read-only; safe to call mid-event.
    bool validate_bracket_state(std::string& out_err) const;

private:
    enum class Phase : std::uint8_t {
        Groups,
        Bracket,        // active double/single-elim rounds
        GrandFinal,
        Done
    };

    std::string                              name_;
    std::vector<TeamPtr>                     teams_;
    TournamentFormat                         format_;
    Phase                                    current_phase_ = Phase::Bracket;

    // Currently-scheduled matchups for the next play_round call.
    std::vector<std::pair<TeamPtr, TeamPtr>> current_matchups_;     // UB matches
    std::vector<std::shared_ptr<Series>>     active_series_;
    std::vector<std::pair<TeamPtr, TeamPtr>> lower_matchups_;       // LB matches
    std::vector<std::shared_ptr<Series>>     lower_series_;

    // Double-elim alive pools. Updated after each play_round call.
    std::vector<TeamPtr>                     ub_alive_;             // winners still in UB
    std::vector<TeamPtr>                     lb_alive_;             // winners still in LB
    std::vector<TeamPtr>                     ub_just_dropped_;      // freshly dropped from UB this round
    int  ub_round_ = 0;
    int  lb_round_ = 0;

    // Group stage state.
    std::vector<std::vector<GroupStanding>>  groups_;
    int  group_match_idx_ = 0;
    bool group_stage_complete_ = false;

    int                                      round_num_ = 1;
    std::string                              phase_label_ = "Round 1";
    // Per-side label + BO for the round currently scheduled. Captured at
    // schedule_next_bracket_round time so play_round can stamp BracketMatch
    // history rows with the exact (label, best_of) the round was scheduled
    // with — instead of recomputing from teams_remaining heuristics that
    // can get confused when LB Semi vs LB Final both have 2 teams alive.
    std::string                              current_ub_label_;
    std::string                              current_lb_label_;
    int                                      current_ub_bo_ = 0;
    int                                      current_lb_bo_ = 0;
    TeamPtr                                  champion_;
    TeamPtr                                  runner_up_;             // GF loser
    std::vector<TeamPtr>                     semifinalists_;         // top-4 finishers
    std::vector<TeamPtr>                     eliminated_;
    // Bracket viz support: full history for the UI to render an NBA-style
    // bracket. Populated at the end of each play_round call.
    std::vector<TeamPtr>                          initial_seeding_;
    std::vector<std::vector<BracketMatch>>        ub_history_;
    std::vector<std::vector<BracketMatch>>        lb_history_;
    std::vector<BracketMatch>                     gf_history_;

    // === Award integrity tracking (see §4.9 / §5.17 / §8 pitfall #21) =====
    // Set of players who appeared as starters in at least one map of this
    // tournament. Populated by absorb_series_participants after every Series
    // resolves. award_event_titles uses (champion_roster ∩ participated_players_)
    // to compute the actual title recipients — keeps phantom roster members
    // (signed mid-tournament, never played a map) from being credited.
    std::unordered_set<Player*>                   participated_players_;

    // One-shot fuse. award_event_titles checks this and short-circuits if true.
    // Belt-and-suspenders against double-firing from any code path
    // (force_finish_stale_tournaments + natural Done transition).
    bool                                          pinned_titles_already_ = false;

    // Snapshotted recipient list at pin time — exposed via
    // winning_roster_snapshot() for audit / UI consumers.
    std::vector<Player*>                          winning_roster_snapshot_;

    // === Per-tournament player stat accumulation ========================
    // Running totals keyed by Player*. Counting fields hold sums; the
    // rating/adr/hs_pct/kast fields hold per-map SUMS while accumulating
    // and are divided by `maps` only at snapshot time in
    // aggregate_player_stats() (maps-weighted mean). team_name is the team
    // the player was rostered on for the maps they played here.
    std::unordered_map<Player*, TournamentPlayerStat> player_stat_accum_;
    // Per-player agent -> maps-played tally, used to derive top_agent.
    std::unordered_map<Player*, std::unordered_map<std::string, int>>
                                                  player_agent_maps_;
    // Dedup fuse: every RecordedMatch already folded into the accumulator.
    // Keyed by the raw RecordedMatch pointer (recordings are unique per map
    // and shared, so this is stable). Guards against the same Series being
    // absorbed twice (natural Done transition + force_finish_stale path).
    std::unordered_set<const RecordedMatch*>      stat_absorbed_recs_;

    // Helpers.
    void  setup_groups();
    void  resolve_groups_to_playoffs();
    void  start_bracket(const std::vector<TeamPtr>& seeded);
    void  schedule_next_bracket_round();
    void  pair_matchups(const std::vector<TeamPtr>& teams,
                        std::vector<std::pair<TeamPtr, TeamPtr>>& out_pairs,
                        std::vector<std::shared_ptr<Series>>& out_series,
                        int best_of, const std::string& label);
    // Context-aware BO decision. Per the current user spec:
    //   - Group stage: BO1 (handled inline in setup_groups; not via this helper)
    //   - All UB bracket rounds (R1 ... UB Final): BO3
    //   - All LB bracket rounds EXCEPT LB Final: BO3
    //   - LB Final, Grand Final: BO5
    // Parameters:
    //   is_lower       — true if this is an LB round, false for UB
    //   teams_in_round — count of teams that will play this single round
    //                    (so a 2-team UB round = UB Final, a 2-team LB round
    //                    may be LB Semi OR LB Final depending on context)
    //   is_lb_final    — only meaningful when is_lower==true: true iff this
    //                    LB round is the merge of the lone LB survivor with
    //                    the UB Final loser (i.e. the GF-challenger decider)
    int   bracket_bo_for(bool is_lower, int teams_in_round, bool is_lb_final) const;
    // Round-label generator. Returns user-spec labels:
    //   UB: "Upper Bracket R1" / "...Quarterfinal" / "...Semifinal" / "...Final"
    //   LB: "Lower Bracket R1" / "...R2" / "...Semifinal" / "...Final"
    // ub_round / lb_round are 1-indexed round numbers within that side, used
    // only for the early "R<N>" labels where the team-count alone is ambiguous.
    std::string bracket_label_for(bool is_lower, int teams_in_round,
                                   bool is_lb_final, int round_in_side) const;
    std::vector<TeamPtr> seed_bracket_pairs(const std::vector<TeamPtr>& seeded) const;
    void  award_event_titles(int current_year);
    // Walk a finished Series's recordings and add every starter (Player*)
    // to participated_players_. Called from play_round after each Series
    // wraps, so the set is fully populated by the time award_event_titles
    // fires.
    void  absorb_series_participants(const Series& s);
    Series play_series(const TeamPtr& a, const TeamPtr& b,
                        std::shared_ptr<Series>& series,
                        std::unordered_map<std::string, SoloQEngine*>& solo_qs);
};

}  // namespace vlr
