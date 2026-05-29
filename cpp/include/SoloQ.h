#pragma once

#include "Player.h"
#include "Team.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace vlr {

class SoloQEngine {
public:
    explicit SoloQEngine(std::string region) : region_(std::move(region)) {}

    void populate_initial_ladder(int count = 500);
    PlayerPtr generate_rookie();

    std::vector<PlayerPtr> get_leaderboard() const;
    void simulate_ranked_day(int loops = 1);

    // Drop the host into a fresh ranked lobby of 9 nearest-MMR players,
    // balance the teams, simulate the match, and return the replayable
    // recording. Used by the "Queue Ranked Match" button on player
    // profiles so the user can spectate any player on demand.
    RecordedMatchPtr force_solo_match(PlayerPtr host);

    const std::string& region() const noexcept { return region_; }
    std::vector<PlayerPtr>& global_ladder() noexcept { return ladder_; }
    const std::vector<PlayerPtr>& global_ladder() const noexcept { return ladder_; }

private:
    std::string region_;
    std::vector<PlayerPtr> ladder_;
    PlayerPtr generate_internal(int min_age, int max_age);
};

}  // namespace vlr
