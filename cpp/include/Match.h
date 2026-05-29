#pragma once

#include "Agent.h"
#include "Common.h"
#include "Player.h"
#include "Team.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace vlr {

// Per-kill context, used to weight the kill contribution component of the
// VLR-style rating (see Match::compute_rating). Mirrors the situational
// modifiers VLR.gg applies: clutch bonus, advantage diminishing returns,
// trade timing, eco mismatch, and post-plant / late-round dampening.
struct KillEvent {
    int  round_num   = 0;
    int  alive_diff  = 0;   // killer_team_alive - opp_team_alive BEFORE the kill
    int  killer_tier = 3;   // 1=pistol .. 5=op
    int  victim_tier = 3;
    bool traded      = false;   // killer swung in <3s after a teammate died
    bool post_plant  = false;
    bool post_round  = false;   // round outcome was already decided
};

struct DeathEvent {
    int  round_num   = 0;
    int  alive_diff  = 0;   // your team alive - opp alive BEFORE death
    int  killer_tier = 3;
    int  victim_tier = 3;
    bool was_traded  = false;   // a teammate killed your killer in <3s
    bool post_plant  = false;
    bool post_round  = false;
};

struct PlayerMatchStats {
    int    k = 0, d = 0, a = 0;
    int    fb = 0, fd = 0;
    int    damage = 0;
    int    survivals = 0, trades = 0;
    double rating = 1.0;
    int    hs_hits = 0;
    int    mk2 = 0, mk3 = 0, mk4 = 0, mk5 = 0;
    int    max_dmg = 0;
    int    clutch_pts = 0;

    // VLR rating support: per-round binary flags for KAST.
    // (Each round can only contribute 1 to "with_kast" regardless of stat count.)
    int    rounds_with_k = 0;
    int    rounds_with_a = 0;
    int    rounds_with_s = 0;
    int    rounds_with_t = 0;
    int    rounds_with_kast = 0;

    // Situational kill/death contribution (already weighted, see compute_rating).
    double weighted_kill_contrib  = 0.0;
    double weighted_death_contrib = 0.0;

    std::vector<KillEvent>  kill_events;
    std::vector<DeathEvent> death_events;
};

struct RoundEvent {
    Player* killer;
    Player* victim;
    int     team_won;
    int     t1_alive;
    int     t2_alive;
};

struct RoundLog {
    int  round = 0;
    std::string winner_name;
    int  t1_score = 0;
    int  t2_score = 0;
    int  t1_invest = 0;
    int  t2_invest = 0;
    std::vector<RoundEvent> events;
};

struct PlayerLine {
    std::string agent;
    int k = 0, d = 0, a = 0, fb = 0, fd = 0;
    double rating = 0.0;
    int hs = 0, fb_pct = 0;
    std::string mk;
    int max_dmg = 0;
    int clutch = 0;
};

struct HistoryRecord {
    std::string event;
    std::string map_name;
    std::string blue_name;
    std::string red_name;
    std::string score;
    std::vector<Player*> blue_team;
    std::vector<Player*> red_team;
    Player* blue_mvp = nullptr;
    Player* red_mvp = nullptr;
    Player* mvp = nullptr;
    std::unordered_map<Player*, PlayerLine> stats;
};

class Match {
public:
    // friendly = true means stats are computed locally (so the live viewer
    // shows real numbers) but NEVER transferred to player career counters.
    // Used for the "Watch Match" demo screen + replays so demo viewing
    // doesn't pollute career totals.
    Match(TeamPtr t1, TeamPtr t2, GameMap map, bool is_solo_q = false,
          std::string event_name = "Ranked", bool friendly = false,
          // Optional series adaptation history. When non-null the per-map
          // agent picker (Team::build_round_selection) consumes this to
          // bias toward / away from previously-used comps.
          const std::vector<MapResultEntry>* prior_results = nullptr);

    void play();

    int  team1_score() const noexcept { return team1_score_; }
    int  team2_score() const noexcept { return team2_score_; }
    Player* match_mvp() const noexcept { return match_mvp_; }
    Player* t1_mvp() const noexcept { return t1_mvp_; }
    Player* t2_mvp() const noexcept { return t2_mvp_; }
    const HistoryRecord& history_record() const noexcept { return history_record_; }
    const std::string& storyline() const noexcept { return storyline_; }
    const std::vector<RoundLog>& round_history() const noexcept { return round_history_; }
    const std::unordered_map<Player*, PlayerMatchStats>& match_stats() const noexcept {
        return match_stats_;
    }
    // Read-only access to the per-player agent assignments produced by
    // build_round_selection. Used by Series::add_match_data to derive each
    // team's comp tag for the per-map adaptation history.
    const std::unordered_map<Player*, const Agent*>& chosen_agents() const noexcept {
        return chosen_agents_;
    }
    const TeamPtr& team1() const noexcept { return team1_; }
    const TeamPtr& team2() const noexcept { return team2_; }
    const GameMap& map() const noexcept { return map_; }
    const std::string& event_name() const noexcept { return event_name_; }
    bool is_solo_q() const noexcept { return is_solo_q_; }
    bool is_friendly() const noexcept { return friendly_; }

private:
    TeamPtr team1_;
    TeamPtr team2_;
    GameMap map_;
    bool    is_solo_q_;
    bool    friendly_ = false;
    std::string event_name_;

    int team1_score_ = 0;
    int team2_score_ = 0;
    int t1_bank_ = 4000;
    int t2_bank_ = 4000;
    int t1_loss_streak_ = 0;
    int t2_loss_streak_ = 0;
    double t1_ovr_ = 0.0;
    double t2_ovr_ = 0.0;

    std::unordered_map<Player*, const Agent*> chosen_agents_;
    std::unordered_map<Player*, PlayerMatchStats> match_stats_;

    Player* t1_mvp_ = nullptr;
    Player* t2_mvp_ = nullptr;
    Player* match_mvp_ = nullptr;

    std::vector<RoundLog> round_history_;
    HistoryRecord         history_record_;
    std::string           storyline_;
};

// Capture an immutable replay of a played Match into a heap-shared
// RecordedMatch. Cheap, called once per match when persisting history.
RecordedMatchPtr make_recorded_match(const Match& m);

}  // namespace vlr
