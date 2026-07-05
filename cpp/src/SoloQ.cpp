#include "SoloQ.h"

#include "Match.h"

#include <algorithm>
#include <numeric>

namespace vlr {

namespace {
bool has_badge(const Player& p, std::string_view n) {
    for (auto& b : p.badges) if (b == n) return true;
    return false;
}

// =========================================================================
// Ranked matchmaking — balanced lobby formation.
//
// Goals (in priority order):
//   1. Match expected outcome to ~50/50.
//   2. Account for hidden skill: a player whose attributes outpace their
//      MMR (climbing smurf) gets weighted up; a player who's tilted /
//      decaying gets weighted down.
//   3. Role diversity: each team should ideally have 1-2 duelists. A
//      triple-controller "no entry" team feels broken.
//   4. Spread aggressiveness: too much agg on one team turns into solo
//      pushes and blow-out losses.
//
// Algorithm:
//   * Compute per-player skill = MMR(50%) + OVR(40%) + consistency(10%).
//   * Snake-draft top->bottom into A,B,B,A,A,B,B,A,A,B (variance-minimising).
//   * If the resulting team-skill gap is > threshold, try one swap that
//     reduces the gap.
//   * Apply role-diversity correction: if one team has 0 duelists, swap
//     in a duelist from the other team for a non-duelist.
// =========================================================================

double player_skill_score(const Player& p) {
    double mmr_norm = clamp_v((p.solo_mmr - 1100.0) / 3000.0, 0.0, 1.0);   // 1100..4100
    double ovr_norm = clamp_v((p.ovr() - 30.0) / 70.0, 0.0, 1.0);
    double cons_norm = clamp_v((p.consistency - 30.0) / 70.0, 0.0, 1.0);
    // Aggressiveness gives a slight bump (carry potential), but anchored.
    double agg_bonus = clamp_v((at(p.attributes, Attr::Aggressiveness) - 50.0) / 200.0, -0.05, 0.05);
    return mmr_norm * 0.50 + ovr_norm * 0.40 + cons_norm * 0.10 + agg_bonus;
}

double team_skill_total(const std::vector<PlayerPtr>& v) {
    double s = 0.0;
    for (auto& p : v) s += player_skill_score(*p);
    return s;
}

int count_role(const std::vector<PlayerPtr>& v, Role r) {
    int n = 0;
    for (auto& p : v) if (p->primary_role == r) ++n;
    return n;
}

// Snake-draft a 10-man lobby into two 5-man teams to minimise expected
// outcome gap. Modifies the vectors in place.
void balance_lobby(std::vector<PlayerPtr>& lobby,
                   std::vector<PlayerPtr>& t1,
                   std::vector<PlayerPtr>& t2) {
    t1.clear();
    t2.clear();

    // Sort lobby by skill descending.
    std::sort(lobby.begin(), lobby.end(),
              [](const PlayerPtr& a, const PlayerPtr& b) {
                  return player_skill_score(*a) > player_skill_score(*b);
              });

    // Snake order: A B B A A B B A A B  (variance-minimising for 10).
    static const int kSnake[10] = {0, 1, 1, 0, 0, 1, 1, 0, 0, 1};
    for (int i = 0; i < 10 && i < static_cast<int>(lobby.size()); ++i) {
        (kSnake[i] == 0 ? t1 : t2).push_back(lobby[i]);
    }

    // Local-search: try swapping one pair if gap is wide. The "skill gap"
    // we tolerate is ~0.05 normalised units (~5%); larger -> obvious stomp.
    auto gap = [&]() { return team_skill_total(t1) - team_skill_total(t2); };
    int safety = 6;
    while (std::abs(gap()) > 0.05 && safety-- > 0) {
        double best_improve = 0.0;
        int best_i = -1, best_j = -1;
        double cur = gap();
        for (std::size_t i = 0; i < t1.size(); ++i) {
            for (std::size_t j = 0; j < t2.size(); ++j) {
                double diff_after = cur - 2.0 * (player_skill_score(*t1[i]) -
                                                  player_skill_score(*t2[j]));
                double improve = std::abs(cur) - std::abs(diff_after);
                if (improve > best_improve) {
                    best_improve = improve;
                    best_i = static_cast<int>(i);
                    best_j = static_cast<int>(j);
                }
            }
        }
        if (best_i < 0) break;
        std::swap(t1[best_i], t2[best_j]);
    }

    // Role diversity: each team wants at least one duelist if any exist
    // in the lobby.
    auto fix_role = [&](Role r, int min_per_team) {
        int t1n = count_role(t1, r);
        int t2n = count_role(t2, r);
        // If one team has 0 and the other has 2+, swap a non-r from the
        // deprived team with a r from the rich team — but only if the
        // skill gap stays small.
        auto try_balance = [&](std::vector<PlayerPtr>& deprived,
                               std::vector<PlayerPtr>& rich,
                               Role need) {
            for (std::size_t i = 0; i < deprived.size(); ++i) {
                if (deprived[i]->primary_role == need) continue;
                for (std::size_t j = 0; j < rich.size(); ++j) {
                    if (rich[j]->primary_role != need) continue;
                    // Tentative swap; accept if gap doesn't widen badly.
                    double cur = std::abs(team_skill_total(t1) - team_skill_total(t2));
                    std::swap(deprived[i], rich[j]);
                    double after = std::abs(team_skill_total(t1) - team_skill_total(t2));
                    if (after < cur + 0.06) return;  // accepted
                    std::swap(deprived[i], rich[j]);  // revert
                }
            }
        };
        if (t1n < min_per_team && t2n > min_per_team) try_balance(t1, t2, r);
        else if (t2n < min_per_team && t1n > min_per_team) try_balance(t2, t1, r);
    };
    fix_role(Role::Duelist, 1);
    // Soft preference: each team should have at most 2 controllers. If one
    // team is triple-controller, swap a controller for a flex.
    if (count_role(t1, Role::Controller) >= 3 && count_role(t2, Role::Controller) <= 1) {
        fix_role(Role::Controller, 2);  // try to redistribute
    }

    // Every team must have EXACTLY one IGL. We handle two violations:
    //   (a) one team has 0 IGLs — swap an IGL in from the rich side
    //   (b) one team has 2+ IGLs — swap an excess IGL out for a non-IGL
    // Both cases prefer swaps that don't widen the skill gap by >0.08.
    auto count_igl = [](const std::vector<PlayerPtr>& v) {
        int n = 0; for (auto& p : v) if (p->is_igl) ++n; return n;
    };
    auto try_balance_igl = [&](std::vector<PlayerPtr>& deprived,
                               std::vector<PlayerPtr>& rich) {
        for (std::size_t i = 0; i < deprived.size(); ++i) {
            if (deprived[i]->is_igl) continue;
            for (std::size_t j = 0; j < rich.size(); ++j) {
                if (!rich[j]->is_igl) continue;
                double cur = std::abs(team_skill_total(t1) - team_skill_total(t2));
                std::swap(deprived[i], rich[j]);
                double after = std::abs(team_skill_total(t1) - team_skill_total(t2));
                if (after < cur + 0.08) return;  // accepted within tolerance
                std::swap(deprived[i], rich[j]);  // revert
            }
        }
    };
    int t1_igl = count_igl(t1), t2_igl = count_igl(t2);
    // Case (a): one team missing entirely
    if (t1_igl == 0 && t2_igl >= 2)      try_balance_igl(t1, t2);
    else if (t2_igl == 0 && t1_igl >= 2) try_balance_igl(t2, t1);

    // Case (b): one team has too many (2+) and the other has at least 1.
    // Swap one of the excess IGLs out for a non-IGL on the other side.
    t1_igl = count_igl(t1); t2_igl = count_igl(t2);
    auto try_drop_excess = [&](std::vector<PlayerPtr>& over,
                               std::vector<PlayerPtr>& under) {
        for (std::size_t i = 0; i < over.size(); ++i) {
            if (!over[i]->is_igl) continue;
            for (std::size_t j = 0; j < under.size(); ++j) {
                if (under[j]->is_igl) continue;
                double cur = std::abs(team_skill_total(t1) - team_skill_total(t2));
                std::swap(over[i], under[j]);
                double after = std::abs(team_skill_total(t1) - team_skill_total(t2));
                if (after < cur + 0.10) return;  // accept
                std::swap(over[i], under[j]);  // revert
            }
        }
    };
    if      (t1_igl >= 2 && t2_igl >= 1) try_drop_excess(t1, t2);
    else if (t2_igl >= 2 && t1_igl >= 1) try_drop_excess(t2, t1);
}

}  // namespace

RecordedMatchPtr SoloQEngine::force_solo_match(PlayerPtr host) {
    // Retired players have left competition entirely — they no longer queue
    // ranked (simulate_ranked_day skips them) and cannot be force-queued either.
    if (!host || host->is_retired) return nullptr;
    // Take the 9 nearest-MMR players regardless of region so the user can
    // spectate any specific person without long matchmaking waits.
    std::vector<PlayerPtr> pool;
    pool.reserve(ladder_.size());
    for (auto& p : ladder_) {
        if (p && p != host && !p->is_retired) pool.push_back(p);
    }
    std::sort(pool.begin(), pool.end(),
              [&](const PlayerPtr& a, const PlayerPtr& b) {
                  return std::abs(a->solo_mmr - host->solo_mmr) <
                         std::abs(b->solo_mmr - host->solo_mmr);
              });
    if (pool.size() < 9) return nullptr;
    pool.resize(9);
    pool.insert(pool.begin(), host);

    std::vector<PlayerPtr> t1, t2;
    balance_lobby(pool, t1, t2);

    const auto& M = maps()[static_cast<std::size_t>(
        rng().irange(0, static_cast<int>(maps().size()) - 1))];
    auto blue = std::make_shared<Team>("Blue", 0LL, region_);
    auto red  = std::make_shared<Team>("Red",  0LL, region_);
    blue->roster = t1; red->roster = t2;
    blue->target_comp = random_comp();
    red->target_comp  = random_comp();

    Match match(blue, red, M, /*is_solo_q=*/true, "Ranked Match");
    match.play();

    // Apply MMR + W/L exactly like a normal solo Q match would.
    auto& stats = match.match_stats();
    auto apply = [&](const std::vector<PlayerPtr>& tm, bool win) {
        for (auto& p : tm) {
            auto sit = stats.find(p.get());
            double rating = (sit != stats.end()) ? sit->second.rating : 1.0;
            if (win) {
                p->solo_mmr += clamp_v(20 + static_cast<int>((rating - 1.0) * 35.0), 15, 50);
                p->solo_wins += 1;
            } else {
                p->solo_mmr -= clamp_v(12 - static_cast<int>((rating - 1.0) * 15.0), 5, 25);
                p->solo_losses += 1;
            }
            p->solo_mmr = std::max(1100, p->solo_mmr);
            p->peak_mmr = std::max(p->peak_mmr, p->solo_mmr);
        }
    };
    apply(t1, match.team1_score() > match.team2_score());
    apply(t2, match.team2_score() > match.team1_score());

    RecordedMatchPtr rec = make_recorded_match(match);
    for (auto& p : pool) {
        auto& v = p->solo_match_history;
        v.insert(v.begin(), rec);
        if (v.size() > 10) v.resize(10);
    }
    return rec;
}

PlayerPtr SoloQEngine::generate_internal(int min_age, int max_age) {
    // Solo Q ladder fill is by far the biggest single perf cost at world
    // boot (900 players × 3 regions × 20-sim MC = ~6.5M progression ticks).
    // These players are background filler — they rarely get scouted; their
    // `potential` only matters if they get promoted to a team. Skip MC and
    // let Player ctor's cheap `mean + irange(5,20)` baseline stand. If a
    // team later signs them, the existing match-driven progression takes
    // over and they accrue real career stats either way.
    PlayerPtr p = vlr::generate_player(min_age, max_age, region_, /*deep_potential=*/false);
    p->solo_mmr = 1100;
    ladder_.push_back(p);
    return p;
}

void SoloQEngine::populate_initial_ladder(int count) {
    ladder_.reserve(ladder_.size() + count);
    for (int i = 0; i < count; ++i) generate_internal(17, 25);
}

PlayerPtr SoloQEngine::generate_rookie() {
    return generate_internal(16, 18);
}

std::vector<PlayerPtr> SoloQEngine::get_leaderboard() const {
    // The ranked leaderboard reflects the LIVE ranked scene. Retired players are
    // never erased from ladder_ (never-free invariant) and — since they no longer
    // queue — their solo_mmr is frozen at its pre-retirement peak, while active
    // players reset to 1100 each new season. Including them would let retired
    // "ghosts" permanently occupy the top of the board and displace live climbers
    // from the HoF top-20 criterion (the other caller). Exclude them here so both
    // the GUI board and the top-20 tracking see only active players.
    std::vector<PlayerPtr> v;
    v.reserve(ladder_.size());
    for (auto& p : ladder_) if (p && !p->is_retired) v.push_back(p);
    std::sort(v.begin(), v.end(),
              [](const PlayerPtr& a, const PlayerPtr& b) { return a->solo_mmr > b->solo_mmr; });
    return v;
}

void SoloQEngine::simulate_ranked_day(int loops) {
    // Defensive per-player per-day cap. Even with reduced loop counts and
    // activity probabilities, a "Ranked Junkie" who lands every roll can
    // still inflate their season totals well past the 250-400 target.
    // Track how many times each player has been scheduled into a lobby
    // across all loops in this single simulate_ranked_day call and cap
    // them at kPerPlayerDailyCap. This is a soft ceiling, not a per-season
    // limit (which would feel arbitrary).
    std::unordered_map<Player*, int> matches_this_day;
    constexpr int kPerPlayerDailyCap = 4;

    for (int it = 0; it < loops; ++it) {
        std::vector<PlayerPtr> active;
        active.reserve(ladder_.size());
        for (auto& p : ladder_) {
            // Retired players are never erased from the ladder (the never-free
            // invariant keeps their shared_ptr alive for match-history refs),
            // but they must NOT keep queueing ranked — skip them. This also
            // keeps per-day scheduling cost O(active), not O(total ladder).
            if (!p || p->is_retired) continue;
            // Skip players who've already hit their daily cap.
            auto& count = matches_this_day[p.get()];
            if (count >= kPerPlayerDailyCap) continue;

            double chance = (p->team_name == "Free Agent") ? 0.32 : 0.30;
            if (has_badge(*p, "Scrimmer"))      chance *= 0.5;
            if (has_badge(*p, "Ranked Junkie")) chance *= 1.5;
            if (rng().chance(chance)) {
                active.push_back(p);
                count += 1;  // count when scheduled, not when match resolves
            }
        }
        std::sort(active.begin(), active.end(),
                  [](const PlayerPtr& a, const PlayerPtr& b) { return a->solo_mmr > b->solo_mmr; });

        std::vector<PlayerPtr> unmatched;
        while (active.size() >= 10) {
            PlayerPtr host = active.front();
            active.erase(active.begin());
            int host_mmr = host->solo_mmr;
            std::vector<std::size_t> valid;
            valid.reserve(9);

            if (host_mmr >= 4000) {
                std::vector<std::size_t> rad, imm;
                for (std::size_t i = 0; i < active.size(); ++i) {
                    if (active[i]->solo_mmr >= 4000) rad.push_back(i);
                    else if (active[i]->solo_mmr >= 3700) imm.push_back(i);
                }
                if (rad.size() + imm.size() >= 9) {
                    int rad_pick = std::min<int>(static_cast<int>(rad.size()), rng().irange(2, 6));
                    int imm_pick = 9 - rad_pick;
                    if (static_cast<int>(imm.size()) < imm_pick) {
                        rad_pick += (imm_pick - static_cast<int>(imm.size()));
                        imm_pick = static_cast<int>(imm.size());
                    }
                    for (int i = 0; i < rad_pick; ++i) valid.push_back(rad[i]);
                    for (int i = 0; i < imm_pick; ++i) valid.push_back(imm[i]);
                }
            } else if (host_mmr >= 3700) {
                for (std::size_t i = 0; i < active.size() && valid.size() < 9; ++i) {
                    if (active[i]->solo_mmr >= 2800) valid.push_back(i);
                }
            } else {
                for (std::size_t i = 0; i < active.size() && valid.size() < 9; ++i) {
                    if (active[i]->solo_mmr >= host_mmr - 800) valid.push_back(i);
                }
            }

            if (valid.size() != 9) { unmatched.push_back(host); continue; }

            std::vector<PlayerPtr> lobby;
            lobby.reserve(10);
            lobby.push_back(host);
            std::sort(valid.begin(), valid.end(), std::greater<std::size_t>());
            for (auto idx : valid) {
                lobby.push_back(active[idx]);
                active.erase(active.begin() + static_cast<std::ptrdiff_t>(idx));
            }

            std::vector<PlayerPtr> t1, t2;
            balance_lobby(lobby, t1, t2);

            const auto& M = maps()[static_cast<std::size_t>(rng().irange(0, static_cast<int>(maps().size()) - 1))];
            auto blue = std::make_shared<Team>("Blue", 0LL, region_);
            auto red  = std::make_shared<Team>("Red",  0LL, region_);
            blue->roster = t1; red->roster  = t2;
            blue->target_comp = random_comp();
            red->target_comp  = random_comp();

            Match match(blue, red, M, true, "Ranked Match");
            match.play();

            auto& stats = match.match_stats();
            auto apply = [&](const std::vector<PlayerPtr>& tm, bool win) {
                for (auto& p : tm) {
                    auto sit = stats.find(p.get());
                    double rating = (sit != stats.end()) ? sit->second.rating : 1.0;
                    if (win) {
                        p->solo_mmr += clamp_v(20 + static_cast<int>((rating - 1.0) * 35.0), 15, 50);
                        p->solo_wins += 1;
                    } else {
                        p->solo_mmr -= clamp_v(12 - static_cast<int>((rating - 1.0) * 15.0), 5, 25);
                        p->solo_losses += 1;
                    }
                    p->solo_mmr = std::max(1100, p->solo_mmr);
                    p->peak_mmr = std::max(p->peak_mmr, p->solo_mmr);
                }
            };
            apply(t1, match.team1_score() > match.team2_score());
            apply(t2, match.team2_score() > match.team1_score());

            // Persist a full replay of this ranked match so the player can
            // re-watch from their profile. Shared across all 10 participants.
            RecordedMatchPtr rec = make_recorded_match(match);
            for (auto& p : lobby) {
                auto& v = p->solo_match_history;
                v.insert(v.begin(), rec);
                if (v.size() > 10) v.resize(10);
            }
        }
        for (auto& p : unmatched) active.insert(active.begin(), p);
    }
}

}  // namespace vlr
