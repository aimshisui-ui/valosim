#pragma once

#include "Team.h"

#include <string>
#include <vector>

namespace vlr {

struct LeagueMatchup {
    TeamPtr a;
    TeamPtr b;
};

class League {
public:
    League(std::string name, std::vector<TeamPtr> teams);

    void generate_schedule();

    const std::string& name() const noexcept { return name_; }
    std::vector<TeamPtr>& teams() noexcept { return teams_; }
    const std::vector<TeamPtr>& teams() const noexcept { return teams_; }
    const std::vector<std::vector<LeagueMatchup>>& weekly_matchups() const noexcept {
        return weekly_matchups_;
    }
    std::vector<TeamPtr>& past_champions() noexcept { return past_champions_; }

private:
    std::string name_;
    std::vector<TeamPtr> teams_;
    std::vector<std::vector<LeagueMatchup>> weekly_matchups_;
    std::vector<TeamPtr> past_champions_;
};

}  // namespace vlr
