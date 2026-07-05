#pragma once

#include "Team.h"

#include <string>
#include <vector>

namespace vlr {

class SaveGame;   // binary save/load (SaveGame.cpp) — friended for weekly_matchups_

struct LeagueMatchup {
    TeamPtr a;
    TeamPtr b;
};

class League {
public:
    // Save/load rebuilds a League via the ctor then overwrites the private
    // schedule (weekly_matchups_) in place — the saved matchup order is
    // authoritative mid-season, so it must NOT be regenerated on load.
    friend class SaveGame;

    // region/tier/division default so every existing 2-arg call site compiles
    // unchanged. tier 1 = top division (VCT); tier 2 = Challengers; etc.
    League(std::string name, std::vector<TeamPtr> teams,
           std::string region = "", int tier = 1, std::string division_name = "");

    void generate_schedule();

    const std::string& name() const noexcept { return name_; }
    std::vector<TeamPtr>& teams() noexcept { return teams_; }
    const std::vector<TeamPtr>& teams() const noexcept { return teams_; }
    const std::vector<std::vector<LeagueMatchup>>& weekly_matchups() const noexcept {
        return weekly_matchups_;
    }
    std::vector<TeamPtr>& past_champions() noexcept { return past_champions_; }

    int  tier() const noexcept { return tier_; }
    void set_tier(int t) noexcept { tier_ = t; }
    const std::string& region() const noexcept { return region_; }
    const std::string& division_name() const noexcept { return division_name_; }

private:
    std::string name_;
    std::vector<TeamPtr> teams_;
    std::string region_;
    int tier_ = 1;
    std::string division_name_;
    std::vector<std::vector<LeagueMatchup>> weekly_matchups_;
    std::vector<TeamPtr> past_champions_;
};

}  // namespace vlr
