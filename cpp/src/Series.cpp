#include "Series.h"

namespace vlr {

Series::Series(TeamPtr t1, TeamPtr t2, int bo, std::string ev)
    : team1_(std::move(t1)), team2_(std::move(t2)),
      best_of_(bo), event_name_(std::move(ev)) {
    auto seed = [&](const TeamPtr& t) {
        std::size_t n = std::min<std::size_t>(5, t->roster.size());
        for (std::size_t i = 0; i < n; ++i)
            aggregate_stats_.emplace(t->roster[i].get(), AggregatedStats{});
    };
    seed(team1_);
    seed(team2_);
}

bool Series::is_over() const {
    if (best_of_ == 2) {
        // BO2 (group stage) — always exactly 2 maps, 1-1 ties allowed.
        // Without this special case best_of_/2+1=2 and a 1-1 series would
        // play a third map, which inflates history + group standings.
        return (t1_wins_ + t2_wins_) >= 2;
    }
    int needed = best_of_ / 2 + 1;
    return t1_wins_ >= needed || t2_wins_ >= needed;
}

void Series::add_match_data(const Match& m) {
    // A real map can never end 0-0 (the round loop always pushes a winner to
    // >=13, and Match::play() forfeits a short roster 13-0). A 0-0 here would
    // mean no contest at all: award neither side rather than silently
    // crediting team2 via a bare else. Genuine non-zero ties are unreachable
    // in legal play (win-by-2) but resolved deterministically for safety.
    if (m.team1_score() == 0 && m.team2_score() == 0) {
        // no-contest: no map win credited
    } else if (m.team1_score() > m.team2_score()) {
        t1_wins_ += 1;
    } else if (m.team2_score() > m.team1_score()) {
        t2_wins_ += 1;
    } else {
        t1_wins_ += 1;  // deterministic tiebreak (no phantom team2 bias)
    }

    // Per-map adaptation history (Pillar 4). Derive each team's comp tag
    // from the chosen agents — count roles, snap to the closest CompTag.
    {
        auto comp_tag_for = [&](const TeamPtr& team) -> CompTag {
            std::array<int, static_cast<std::size_t>(Role::Count)> have{};
            for (auto& kv : m.chosen_agents()) {
                if (!kv.second) continue;
                bool on_team = false;
                for (auto& rp : team->roster) {
                    if (rp.get() == kv.first) { on_team = true; break; }
                }
                if (!on_team) continue;
                have[static_cast<std::size_t>(kv.second->role)]++;
            }
            // Pick whichever role has 2+; matches the comp_tag_of layout.
            if (have[1] >= 2) return CompTag::DoubleInitiator;
            if (have[0] >= 2) return CompTag::DoubleDuelist;
            if (have[2] >= 2) return CompTag::DoubleController;
            if (have[3] >= 2) return CompTag::DoubleSentinel;
            return CompTag::DoubleInitiator;
        };

        MapResultEntry entry;
        entry.map_name  = m.map().name;
        entry.t1_comp   = comp_tag_for(team1_);
        entry.t2_comp   = comp_tag_for(team2_);
        entry.t1_score  = m.team1_score();
        entry.t2_score  = m.team2_score();
        entry.t1_won    = m.team1_score() > m.team2_score();
        map_history_.push_back(std::move(entry));
    }

    for (auto& kv : m.match_stats()) {
        // Lazy-insert for players not seeded by the constructor. The ctor
        // pre-seeds roster[0..min(5,size)] which covers the common case, but
        // a future mid-series substitution / trade could surface a new
        // starter in match_stats. Without this insert, that player's stats
        // would be silently dropped.
        auto it = aggregate_stats_.find(kv.first);
        if (it == aggregate_stats_.end()) {
            it = aggregate_stats_.emplace(kv.first, AggregatedStats{}).first;
        }
        it->second.k += kv.second.k;
        it->second.d += kv.second.d;
        it->second.a += kv.second.a;
        it->second.fb += kv.second.fb;
        it->second.fd += kv.second.fd;
        it->second.rating += kv.second.rating;
        it->second.matches += 1;
    }

    // Record the match once (shared by all 10 participants) and pin it to
    // each starter's history. This is what powers in-game match replays.
    // The caller can also retrieve this RecordedMatchPtr via last_recording()
    // — important so we don't deep-copy the same Match twice (large match
    // round_history and match_stats are the dominant memory cost).
    RecordedMatchPtr rec = make_recorded_match(m);
    last_recording_ = rec;
    all_recordings_.push_back(rec);
    // Friendly matches (e.g. tournament-bracket "Watch" previews) must not
    // pollute pro_match_history. Match::play already gates career counters
    // on friendly_, but the pin step below is the other half of the
    // isolation contract — without this guard, every Watch click would
    // inject 3-5 demo recordings into the players' profile match history.
    if (!m.is_friendly()) {
        auto pin = [&](const TeamPtr& t) {
            std::size_t n = std::min<std::size_t>(5, t->roster.size());
            for (std::size_t i = 0; i < n; ++i) {
                auto& v = t->roster[i]->pro_match_history;
                v.insert(v.begin(), rec);
                if (v.size() > 10) v.resize(10);
            }
        };
        pin(team1_);
        pin(team2_);
    }
}

void Series::finalize_stats(bool is_league_play) {
    for (auto& kv : aggregate_stats_) {
        if (kv.second.matches > 0)
            kv.second.rating = std::round((kv.second.rating / kv.second.matches) * 100.0) / 100.0;
    }
    if (t1_wins_ > t2_wins_)      { winner_ = team1_; loser_ = team2_; }
    else if (t2_wins_ > t1_wins_) { winner_ = team2_; loser_ = team1_; }
    // BO2 ties: leave winner_/loser_ null so neither side gets a W/L.
    if (is_league_play && winner_ && loser_) {
        winner_->wins += 1; winner_->phase_wins += 1;
        loser_->losses += 1; loser_->phase_losses += 1;
        // Rolling last-10 form for the Standings sparkline (Group F).
        auto push_form = [](const TeamPtr& t, std::uint8_t r) {
            t->recent_results.push_back(r);
            if (t->recent_results.size() > 10)
                t->recent_results.erase(t->recent_results.begin());
        };
        push_form(winner_, 1);
        push_form(loser_, 0);
    }
}

}  // namespace vlr
