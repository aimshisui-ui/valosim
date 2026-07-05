#include "League.h"

namespace vlr {

League::League(std::string n, std::vector<TeamPtr> ts,
               std::string region, int tier, std::string division_name)
    : name_(std::move(n)), teams_(std::move(ts)),
      region_(std::move(region)), tier_(tier),
      division_name_(std::move(division_name)) {}

void League::generate_schedule() {
    weekly_matchups_.clear();
    for (auto& t : teams_) if (t) { t->phase_wins = 0; t->phase_losses = 0; }

    std::vector<TeamPtr> roster = teams_;
    if (roster.size() % 2 != 0) roster.push_back(TeamPtr{});

    std::size_t n = roster.size();
    for (std::size_t fixture = 1; fixture < n; ++fixture) {
        std::vector<LeagueMatchup> week;
        week.reserve(n / 2);
        for (std::size_t i = 0; i < n / 2; ++i) {
            const TeamPtr& a = roster[i];
            const TeamPtr& b = roster[n - 1 - i];
            if (a && b) week.push_back({a, b});
        }
        TeamPtr last = roster.back();
        roster.pop_back();
        roster.insert(roster.begin() + 1, last);
        weekly_matchups_.push_back(std::move(week));
    }
}

}  // namespace vlr
