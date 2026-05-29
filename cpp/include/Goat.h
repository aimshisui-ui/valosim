#pragma once

#include "Player.h"

#include <memory>
#include <string>
#include <vector>

namespace vlr {

// Coefficients for the all-time / season GOAT score. Exposed in the GOAT
// Lab UI so the user can tune what counts as "greatest of all time" — some
// people care most about championships, others about peak seasons, others
// about longevity.
struct GoatWeights {
    // === Championships (string-match player.awards[]) ===
    double champ_world      = 35.0;   // World Champions title
    double champ_masters    = 18.0;   // Masters title
    double champ_regional   = 8.0;    // Regional title

    // === Individual season awards (added round 12) ===
    double mvp_award        = 25.0;   // [A] MVP
    double role_award       = 12.0;   // [A] Duelist/Initiator/Controller/Sentinel of the Year
    double igl_award        = 14.0;   // [A] IGL of the Year

    // === Sustained pro performance ===
    double per_match        = 0.05;
    double avg_rating_bonus = 80.0;
    double peak_rating_bonus= 40.0;
    double per_kill         = 0.005;
    double per_assist       = 0.003;
    double per_first_blood  = 0.025;
    double per_mvp_match    = 0.30;   // career_mvps (in-match MVPs)

    // === Hall-of-Fame milestones ===
    double bonus_30_kill_match    = 8.0;   // had a 30+ kill pro match
    double bonus_2kd_match        = 8.0;   // had a 2.0 KD pro match
    double bonus_gf_clutch        = 18.0;  // per Grand Final clutch
    double bonus_top20_solo       = 6.0;   // ever hit top 20 solo

    // === Solo Q dominance ===
    double peak_mmr_above_2k = 0.005;
    double per_solo_win      = 0.01;

    // === IGL strategic impact (Pillar 1) ===
    // Bonus per igl_impact point accumulated over the player's career. Lets
    // pure tactical IGLs climb the GOAT board without elite frag stats.
    double igl_impact_per_career = 0.25;
};

// Score one player on the all-time formula.
double goat_career_score(const Player& p, const GoatWeights& w);

// Score a single season's history entry. season_idx is into p.history.
double goat_season_score(const Player& p, int season_idx, const GoatWeights& w);

// One row of the Goat table for the UI.
struct GoatRow {
    PlayerPtr   player;
    double      score = 0.0;
    int         season = -1;       // -1 for career rows
    std::string season_team;       // team during that season (season rows)
    double      season_rating = 0; // for season rows
    double      season_kd = 0;
};

// Compute career-mode rankings across all players. Sorted descending.
std::vector<GoatRow> compute_goat_career(
        const std::vector<PlayerPtr>& all_players,
        const GoatWeights& w,
        std::size_t max_rows = 100);

// Compute season-mode rankings. Each player can contribute multiple rows
// (one per season); same player can show up many times if they had several
// great years.
std::vector<GoatRow> compute_goat_season(
        const std::vector<PlayerPtr>& all_players,
        const GoatWeights& w,
        std::size_t max_rows = 100);

}  // namespace vlr
