#pragma once

#include "Match.h"
#include "Team.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace vlr {

struct AggregatedStats {
    int k = 0, d = 0, a = 0, fb = 0, fd = 0;
    double rating = 0.0;
    int matches = 0;
};

class Series {
public:
    Series(TeamPtr t1, TeamPtr t2, int best_of = 3, std::string event_name = "Match");

    bool is_over() const;

    void add_match_data(const Match& m);
    void finalize_stats(bool is_league_play = false);

    int t1_wins() const noexcept { return t1_wins_; }
    int t2_wins() const noexcept { return t2_wins_; }
    int best_of() const noexcept { return best_of_; }
    const std::string& event_name() const noexcept { return event_name_; }
    const TeamPtr& team1() const noexcept { return team1_; }
    const TeamPtr& team2() const noexcept { return team2_; }
    const TeamPtr& winner() const noexcept { return winner_; }
    const TeamPtr& loser() const noexcept { return loser_; }
    const std::unordered_map<Player*, AggregatedStats>& aggregate_stats() const noexcept {
        return aggregate_stats_;
    }
    // Single shared RecordedMatchPtr from the most recent add_match_data call.
    // Callers that need their own handle should reuse this rather than calling
    // make_recorded_match() a second time on the same Match (which deep-copies
    // the entire round_history + match_stats — the dominant memory cost).
    const RecordedMatchPtr& last_recording() const noexcept { return last_recording_; }

    // Every map recording, in play order. Used by the live viewer + by
    // GameManager::play_tournament_round to recover the user's series
    // even when player_history's 10-entry cap would otherwise evict
    // earlier maps.
    const std::vector<RecordedMatchPtr>& all_recordings() const noexcept { return all_recordings_; }

    // Per-map history for in-series adaptation (Team::build_round_selection
    // reads this to bias comp choice on later maps). MapResultEntry lives
    // in Agent.h so it can be passed around without dragging Series.h.
    const std::vector<MapResultEntry>& map_history() const noexcept { return map_history_; }

private:
    TeamPtr team1_;
    TeamPtr team2_;
    int     best_of_;
    std::string event_name_;
    int     t1_wins_ = 0;
    int     t2_wins_ = 0;
    TeamPtr winner_;
    TeamPtr loser_;
    RecordedMatchPtr last_recording_;
    std::vector<RecordedMatchPtr> all_recordings_;
    std::vector<MapResultEntry>   map_history_;
    std::unordered_map<Player*, AggregatedStats> aggregate_stats_;
};

}  // namespace vlr
