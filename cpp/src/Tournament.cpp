#include "Tournament.h"

#include "Match.h"
#include "SoloQ.h"

#include <algorithm>

namespace vlr {

Tournament::Tournament(std::string n, std::vector<TeamPtr> tms, TournamentFormat fmt)
    : name_(std::move(n)), teams_(std::move(tms)), format_(fmt) {
    // Per-tournament player-stat accumulation starts empty. The ctor is the
    // sole true (re)initialization point — start_bracket() is ALSO called
    // mid-event by resolve_groups_to_playoffs(), where the group-stage
    // totals must survive into playoffs, so the reset must NOT live there.
    player_stat_accum_.clear();
    player_agent_maps_.clear();
    stat_absorbed_recs_.clear();
    if (format_ == TournamentFormat::GroupThenPlayoffs) {
        current_phase_ = Phase::Groups;
        setup_groups();
    } else {
        current_phase_ = Phase::Bracket;
        start_bracket(teams_);
    }
}

bool Tournament::user_team_in_round(const TeamPtr& user) const {
    for (auto& mu : current_matchups_) if (mu.first == user || mu.second == user) return true;
    for (auto& mu : lower_matchups_)   if (mu.first == user || mu.second == user) return true;
    if (current_phase_ == Phase::Groups) {
        for (auto& g : groups_) for (auto& gs : g) if (gs.team == user) return true;
    }
    return false;
}

// Standard bracket-seed expansion: 1v8/4v5/2v7/3v6 etc. for any power of 2.
std::vector<TeamPtr> Tournament::seed_bracket_pairs(const std::vector<TeamPtr>& seeded) const {
    int n = static_cast<int>(seeded.size());
    if (n <= 1) return seeded;
    if ((n & (n - 1)) != 0) return seeded;  // non-power-of-two: pass through
    std::vector<int> order = {1};
    while ((int)order.size() < n) {
        int total = (int)order.size() * 2;
        std::vector<int> next;
        next.reserve(static_cast<std::size_t>(total));
        for (int s : order) {
            next.push_back(s);
            next.push_back(total + 1 - s);
        }
        order = std::move(next);
    }
    std::vector<TeamPtr> out;
    out.reserve(static_cast<std::size_t>(n));
    for (int s : order) {
        int idx = s - 1;
        if (idx >= 0 && idx < n) out.push_back(seeded[static_cast<std::size_t>(idx)]);
    }
    return out;
}

int Tournament::bracket_bo_for(bool is_lower, int teams_in_round, bool is_lb_final) const {
    // Per user spec (2026-05): every bracket round is BO3 EXCEPT the LB
    // Final and the Grand Final, which are BO5. Even the UB Final stays
    // BO3 — the user explicitly wants BO5 reserved for the two highest-
    // stakes matches only. (User spec 2026-05-14: VCT-style single GF —
    // no bracket reset.)
    //
    // GF is scheduled directly in schedule_next_bracket_round with a
    // hardcoded `5` (not via this helper); it doesn't flow through here.
    // So this helper only ever needs to decide UB-rounds (always 3) and
    // LB-rounds (3 except when is_lb_final).
    (void)teams_in_round;  // currently unused; retained for future flexibility
    if (is_lower && is_lb_final) return 5;
    return 3;
}

std::string Tournament::bracket_label_for(bool is_lower, int teams_in_round,
                                          bool is_lb_final, int round_in_side) const {
    if (is_lower) {
        // LB Final = the merge of the lone LB survivor + UB Final loser.
        // Detect via is_lb_final flag (NOT teams_in_round) because LB Semi
        // and LB R1 in a 4-team bracket can both have teams_in_round==2.
        // For a "Semifinal" label we look at the round POSITION: in 8+-team
        // brackets, the round immediately preceding LB Final is the Semi;
        // in a 4-team bracket the first LB round is just "LB R1" (there is
        // no LB Semi distinct from LB R1).
        if (is_lb_final) return "Lower Bracket Final";
        // For 8-team DE: LB rounds in order are R1, R2, Semi, Final.
        // For 16-team DE: R1, R2, R3, R4, Semi, Final.
        // Heuristic: a pure-LB 2-team round that comes AFTER at least one
        // merge (i.e. lb_round_ >= 3) is the Semi. round_in_side here is
        // the about-to-be-incremented value, so >=3 means "third or later
        // LB round" — at which point a 2-team field is the Semifinal.
        if (teams_in_round == 2 && round_in_side >= 3) return "Lower Bracket Semifinal";
        return "Lower Bracket R" + std::to_string(round_in_side);
    }
    if (teams_in_round == 2)  return "Upper Bracket Final";
    if (teams_in_round == 4)  return "Upper Bracket Semifinal";
    if (teams_in_round == 8)  return "Upper Bracket Quarterfinal";
    if (teams_in_round == 16) return "Upper Bracket Round of 16";
    return "Upper Bracket R" + std::to_string(round_in_side);
}

void Tournament::pair_matchups(const std::vector<TeamPtr>& teams,
                                std::vector<std::pair<TeamPtr, TeamPtr>>& out_pairs,
                                std::vector<std::shared_ptr<Series>>& out_series,
                                int best_of, const std::string& label) {
    out_pairs.clear();
    out_series.clear();
    for (std::size_t i = 0; i + 1 < teams.size(); i += 2) {
        out_pairs.emplace_back(teams[i], teams[i + 1]);
        out_series.push_back(std::make_shared<Series>(
            teams[i], teams[i + 1], best_of, name_ + " - " + label));
    }
    if (teams.size() % 2 == 1) {
        out_pairs.emplace_back(teams.back(), TeamPtr{});
        out_series.push_back(nullptr);
    }
}

void Tournament::start_bracket(const std::vector<TeamPtr>& seeded) {
    ub_alive_.clear();
    lb_alive_.clear();
    ub_just_dropped_.clear();
    ub_round_ = 0;
    lb_round_ = 0;
    current_phase_ = Phase::Bracket;

    // Initial UB round 1: pair seeded teams. ub_alive_ stays empty here;
    // play_round() populates it with the winners of UB R1.
    auto paired = seed_bracket_pairs(seeded);
    initial_seeding_ = paired;
    ub_history_.clear();
    lb_history_.clear();
    gf_history_.clear();
    ub_round_ = 1;
    int teams_remaining = static_cast<int>(paired.size());
    int  bo  = bracket_bo_for(/*is_lower=*/false, teams_remaining, /*is_lb_final=*/false);
    std::string lbl = bracket_label_for(/*is_lower=*/false, teams_remaining,
                                         /*is_lb_final=*/false, ub_round_);
    pair_matchups(paired, current_matchups_, active_series_, bo, lbl);
    lower_matchups_.clear();
    lower_series_.clear();
    current_ub_label_ = lbl;
    current_ub_bo_    = bo;
    current_lb_label_.clear();
    current_lb_bo_    = 0;
    phase_label_      = lbl;
}

// Capture every starter (Player*) from each map of a finished Series into
// participated_players_. Called from play_round after each Series finalizes.
// Uses history_record->blue_team / red_team (vector<Player*>) — these list
// only the 5 actual starters per side, NOT bench/sub roster members who
// never appeared in chosen_agents_ for that map.
void Tournament::absorb_series_participants(const Series& s) {
    for (const auto& rec : s.all_recordings()) {
        if (!rec || !rec->history_record) continue;
        for (Player* p : rec->history_record->blue_team) {
            if (p) participated_players_.insert(p);
        }
        for (Player* p : rec->history_record->red_team) {
            if (p) participated_players_.insert(p);
        }

        // --- Per-tournament player stat accumulation -------------------
        // Dedup at the RecordedMatch* level: this exact map recording must
        // only fold into the running totals once, even if the owning Series
        // is absorbed twice (natural Done transition + the defensive
        // force_finish_stale_tournaments path). Recordings are unique per
        // map and shared, so the raw pointer is a stable identity.
        if (!stat_absorbed_recs_.insert(rec.get()).second) continue;

        const HistoryRecord& hr = *rec->history_record;
        // Total rounds in this map (e.g. 13-9 -> 22). blue/red_score are
        // the per-map round scores stamped by make_recorded_match.
        const int map_rounds = rec->blue_score + rec->red_score;

        // match_stats holds the rich per-player numbers (damage, hs_hits,
        // rounds_with_kast, clutch_pts, per-map rating). May be null on
        // legacy recordings — fall back to the lighter HistoryRecord stats.
        const std::unordered_map<Player*, PlayerMatchStats>* ms =
            rec->match_stats ? rec->match_stats.get() : nullptr;

        auto fold_side = [&](const std::vector<Player*>& side,
                             bool is_blue) {
            const std::string& side_team =
                is_blue ? rec->blue_name : rec->red_name;
            for (Player* p : side) {
                if (!p) continue;
                TournamentPlayerStat& acc = player_stat_accum_[p];
                acc.player    = p;
                // Latest team the player suited up for in this event.
                if (!side_team.empty()) acc.team_name = side_team;
                acc.maps   += 1;
                acc.rounds += map_rounds;

                std::string agent_name;
                if (ms) {
                    auto it = ms->find(p);
                    if (it != ms->end()) {
                        const PlayerMatchStats& st = it->second;
                        acc.kills        += st.k;
                        acc.deaths       += st.d;
                        acc.assists      += st.a;
                        acc.first_kills  += st.fb;
                        acc.first_deaths += st.fd;
                        acc.clutches     += st.clutch_pts;
                        // rating/adr/hs/kast accumulate as per-map SUMS;
                        // divided by maps in aggregate_player_stats().
                        const double rd = static_cast<double>(
                            std::max(1, map_rounds));
                        acc.rating += st.rating;
                        acc.adr    += static_cast<double>(st.damage) / rd;
                        acc.hs_pct += (st.hs_hits /
                            static_cast<double>(std::max(1, st.k))) * 100.0;
                        acc.kast   += (st.rounds_with_kast / rd) * 100.0;
                    }
                }
                // Agent name: prefer HistoryRecord::stats (PlayerLine.agent),
                // which Match.cpp populates from chosen_agents_.
                auto lit = hr.stats.find(p);
                if (lit != hr.stats.end() && !lit->second.agent.empty()
                    && lit->second.agent != "?") {
                    agent_name = lit->second.agent;
                }
                if (!agent_name.empty())
                    player_agent_maps_[p][agent_name] += 1;
            }
        };
        fold_side(hr.blue_team, /*is_blue=*/true);
        fold_side(hr.red_team,  /*is_blue=*/false);
    }
}

std::vector<TournamentPlayerStat> Tournament::aggregate_player_stats() const {
    std::vector<TournamentPlayerStat> out;
    out.reserve(player_stat_accum_.size());
    for (const auto& kv : player_stat_accum_) {
        if (!kv.first || kv.second.maps <= 0) continue;
        TournamentPlayerStat ps = kv.second;  // copy running totals
        const double m = static_cast<double>(ps.maps);
        // Convert the per-map SUMS into maps-weighted means.
        ps.rating /= m;
        ps.adr    /= m;
        ps.hs_pct /= m;
        ps.kast   /= m;
        // Most-played agent in this tournament (ties -> first by name for
        // determinism). Leaves "" if no agent was ever recorded.
        auto ait = player_agent_maps_.find(kv.first);
        if (ait != player_agent_maps_.end()) {
            int best = 0;
            for (const auto& ag : ait->second) {
                if (ag.second > best ||
                    (ag.second == best && ag.first < ps.top_agent)) {
                    best = ag.second;
                    ps.top_agent = ag.first;
                }
            }
        }
        out.push_back(std::move(ps));
    }
    return out;
}

void Tournament::award_event_titles(int current_year) {
    // Belt-and-suspenders: refuse to run twice for the same Tournament.
    // Prevents double-pinning if both the natural play_round Done transition
    // AND force_finish_stale_tournaments race the same tournament. Combined
    // with the per-player "already-have-this-award" dedup below, double
    // crediting is impossible.
    if (pinned_titles_already_) return;
    if (!champion_) return;

    // Resolve runner-up + semifinalists from the elimination order.
    // eliminated_ is appended-to as teams drop, so the last team to
    // be added is the GF loser (runner-up). The two added immediately
    // before are the SF losers (top-4 finishers).
    runner_up_.reset();
    semifinalists_.clear();
    if (!eliminated_.empty()) {
        runner_up_ = eliminated_.back();
    }
    int n_elim = static_cast<int>(eliminated_.size());
    for (int i = std::max(0, n_elim - 3); i < n_elim - 1; ++i) {
        if (eliminated_[i]) semifinalists_.push_back(eliminated_[i]);
    }

    auto kind = [&]() -> std::string {
        if (name_.find("Regionals") != std::string::npos) return "Regional";
        if (name_.find("MASTERS")   != std::string::npos) return "Masters";
        if (name_.find("CHAMPIONS") != std::string::npos) return "World";
        return "Event";
    }();
    // No current code path produces a tournament name that doesn't match one
    // of the three known kinds. If we ever hit "Event" here it means a new
    // event family was added without updating this switch — skip the award
    // rather than minting a meaningless "[*] Event Champ YYYY" badge.
    if (kind == "Event") return;
    auto prefix = [&]() -> std::string {
        if (kind == "Regional") return "[T] ";
        if (kind == "Masters")  return "[M] ";
        if (kind == "World")    return "[W] ";
        return "[*] ";
    }();

    // === Build the eligible recipient list =================================
    // Filter: (champion's current roster) ∩ (players who started a map this
    // tournament). This handles the two failure modes of the naive "credit
    // whole roster" approach:
    //   (a) Phantom roster member — signed mid-tournament (e.g. via
    //       run_mid_season_replacements between matchdays) but never appeared
    //       in chosen_agents_ for any map. Filtered OUT here.
    //   (b) Departed contributor — won the GF then immediately got traded
    //       before award_event_titles fires. Their existing player.awards
    //       isn't stripped (the trophy already lives on the Player object),
    //       and they're filtered out of the NEW pin — which is correct,
    //       because in practice award_event_titles runs in the same call
    //       frame as the GF Series finalizing so this case is moot. Snapshot
    //       captured below for UI/audit consumption either way.
    // The bench/sub case is naturally handled: a player on the roster who
    // never appeared as a starter in any map (chosen_agents_ keys) isn't in
    // participated_players_ and so gets no trophy.
    winning_roster_snapshot_.clear();
    for (auto& p : champion_->roster) {
        if (!p) continue;
        if (participated_players_.count(p.get())) {
            winning_roster_snapshot_.push_back(p.get());
        }
    }

    // === Pin to player.awards ==============================================
    // Award string format MUST match the historical pattern — GOAT scoring
    // and HoF criteria substring-match on these exact tokens:
    //   "[T] Regional Champ YYYY"
    //   "[M] Masters Champ YYYY"
    //   "[W] World Champion YYYY"
    // Do NOT alter the format. See §8 pitfall #15 (cumulative awards
    // snapshot + year-substring filter in goat_season_score).
    std::string champ_word = (kind == "World") ? "Champion" : "Champ";
    std::string award = prefix + kind + " " + champ_word + " "
                      + std::to_string(current_year);
    for (Player* p : winning_roster_snapshot_) {
        if (!p) continue;
        bool exists = false;
        for (auto& a : p->awards) if (a == award) { exists = true; break; }
        if (!exists) p->awards.push_back(award);
    }

    // Only the CHAMPION earns an award tag. Finalists / semifinalists do
    // NOT get participation badges — major achievements are reserved for
    // actually winning the event. (UI still shows their placement on the
    // bracket via runner_up_ / semifinalists_ accessors.) See §5.17.

    // Flip the fuse last so a partial-failure path doesn't leave the
    // tournament marked done-without-pinning.
    pinned_titles_already_ = true;
}

void Tournament::setup_groups() {
    groups_.clear();
    int total = static_cast<int>(teams_.size());
    if (total < 4) {
        format_ = TournamentFormat::SingleElim;
        current_phase_ = Phase::Bracket;
        start_bracket(teams_);
        return;
    }
    int group_count = (total >= 16) ? 4 : 2;
    int per_group = total / group_count;
    groups_.resize(static_cast<std::size_t>(group_count));
    for (int i = 0; i < total; ++i) {
        int round_idx = i / group_count;
        int g = (round_idx % 2 == 0) ? (i % group_count)
                                     : (group_count - 1 - (i % group_count));
        if ((int)groups_[g].size() >= per_group) continue;
        GroupStanding gs; gs.team = teams_[i];
        groups_[g].push_back(gs);
    }
    current_matchups_.clear();
    active_series_.clear();
    for (auto& g : groups_) {
        for (std::size_t i = 0; i < g.size(); ++i) {
            for (std::size_t j = i + 1; j < g.size(); ++j) {
                current_matchups_.emplace_back(g[i].team, g[j].team);
                active_series_.push_back(std::make_shared<Series>(
                    g[i].team, g[j].team, 2, name_ + " - Group Stage"));
            }
        }
    }
    group_match_idx_ = 0;
    phase_label_ = "Group Stage";
}

void Tournament::resolve_groups_to_playoffs() {
    std::vector<TeamPtr> playoff;
    for (auto& g : groups_) {
        // stable_sort + name as final fallback gives a deterministic ordering
        // when (wins, map_diff, round_diff) ties — important because BO2 ties
        // are allowed in the group stage so 3-way ties happen in practice.
        std::stable_sort(g.begin(), g.end(), [](const GroupStanding& a, const GroupStanding& b) {
            if (a.wins != b.wins)             return a.wins > b.wins;
            if (a.map_diff != b.map_diff)     return a.map_diff > b.map_diff;
            if (a.round_diff != b.round_diff) return a.round_diff > b.round_diff;
            // Final tiebreaker: alphabetical by team name. Head-to-head would
            // be more intuitive but requires per-pair match results we don't
            // store on GroupStanding; name keeps it deterministic + cheap.
            static const std::string empty_name;
            const std::string& an = a.team ? a.team->name : empty_name;
            const std::string& bn = b.team ? b.team->name : empty_name;
            return an < bn;
        });
        for (std::size_t i = 0; i < g.size(); ++i) {
            if (i < 2) playoff.push_back(g[i].team);
            else { g[i].eliminated = true; eliminated_.push_back(g[i].team); }
        }
    }
    std::vector<TeamPtr> seeded;
    if (groups_.size() == 2 && playoff.size() == 4) {
        // Cross-group: A1 vs B2, B1 vs A2 (avoid same-group rematch).
        // seed_bracket_pairs for n=4 yields order [0,3,1,2] => matches
        // (input[0]-input[3]) and (input[1]-input[2]). So feed it
        // [A1, B1, A2, B2] to get A1-B2 and B1-A2.
        seeded = {playoff[0], playoff[2], playoff[1], playoff[3]};
    } else if (groups_.size() == 4 && playoff.size() == 8) {
        // Strict cross-bracket seeding: every group's #1 and #2 sit in
        // OPPOSITE halves of the bracket so a same-group rematch can only
        // happen in the Final, never in the QF or SF.
        //
        // playoff layout (set by the loop above): [A1, A2, B1, B2, C1, C2, D1, D2].
        // seed_bracket_pairs(8) reorders any 8-team input to bracket order
        // [s0, s7, s3, s4, s1, s6, s2, s5], producing QF pairs
        // (s0-s7), (s3-s4), (s1-s6), (s2-s5). Top half = {s0,s7,s3,s4};
        // bottom half = {s1,s6,s2,s5}.
        //
        // Target QFs: A1-B2, C1-D2 (top half) | B1-A2, D1-C2 (bottom half).
        // Solving for s[i] gives:
        //   s0=A1=p[0], s7=B2=p[3], s3=C1=p[4], s4=D2=p[7],
        //   s1=B1=p[2], s6=A2=p[1], s2=D1=p[6], s5=C2=p[5].
        // Verified: every group has exactly one team in each half, so
        // no same-group meeting before the Final.
        seeded = {playoff[0], playoff[2], playoff[6], playoff[4],
                  playoff[7], playoff[5], playoff[1], playoff[3]};
    } else {
        seeded = playoff;
    }
    group_stage_complete_ = true;
    format_ = TournamentFormat::DoubleElim;
    current_phase_ = Phase::Bracket;
    start_bracket(seeded);
}

// Run a full series between a and b. Both teams refresh roster from FA pool
// before play.
Series Tournament::play_series(const TeamPtr& a, const TeamPtr& b,
                                std::shared_ptr<Series>& series,
                                std::unordered_map<std::string, SoloQEngine*>& solo_qs) {
    auto fa1 = solo_qs.count(a->region) ? solo_qs[a->region]->global_ladder() : std::vector<PlayerPtr>{};
    auto fa2 = solo_qs.count(b->region) ? solo_qs[b->region]->global_ladder() : std::vector<PlayerPtr>{};
    a->auto_fill_roster(fa1);
    b->auto_fill_roster(fa2);
    while (!series->is_over()) {
        const auto& M = maps()[static_cast<std::size_t>(rng().irange(0, static_cast<int>(maps().size()) - 1))];
        Match m(a, b, M, false, series->event_name(), /*friendly=*/false,
                &series->map_history());
        m.play();
        series->add_match_data(m);
    }
    series->finalize_stats(false);
    return *series;
}

// === Build the next round's matchups based on current bracket state ======
//
// === Phantom-round audit (2026-05) ============================================
// The previous merge-vs-pure decision tree mis-ordered the late LB rounds. For
// 8 teams, after UB Final + LB R2 we had:
//     ub_alive_=[UBF winner]   ub_just_dropped_=[UBF loser]   lb_alive_=[A,B]
// The old code took the `have_drops && have_lb` branch with mismatched sizes,
// scheduled ONE match (A vs drop), and left B as a leftover in lb_alive_. The
// "LB Final" therefore played BEFORE the LB Semifinal — and the leftover B
// then faced the merge-winner in a round the UI labeled "Lower Final" a
// SECOND time. Result: two consecutive rounds both labeled "Lower Final",
// producing the phantom extra round the user noticed.
//
// Fix: when drops exist but lb_alive_ has more survivors than drops, run a
// PURE LB round first (the Semifinal) and HOLD the drops in ub_just_dropped_
// for the following round. Drops merge only once lb_alive_ has narrowed to
// match the drop count — at that moment the merge IS the LB Final.
// =============================================================================
void Tournament::schedule_next_bracket_round() {
    current_matchups_.clear();
    active_series_.clear();
    lower_matchups_.clear();
    lower_series_.clear();
    current_ub_label_.clear();
    current_lb_label_.clear();
    current_ub_bo_ = 0;
    current_lb_bo_ = 0;

    // === Grand Final precondition ===
    // Both brackets have a single survivor (no fresh drops pending). Single
    // BO5 decides the champion — no bracket reset, regardless of who wins.
    // (User spec 2026-05-14: VCT-style single GF, not FGC-style reset.)
    bool gf_ready = (ub_alive_.size() == 1 && lb_alive_.size() == 1 &&
                     ub_just_dropped_.empty());
    if (gf_ready) {
        current_phase_ = Phase::GrandFinal;
        current_matchups_.emplace_back(ub_alive_.front(), lb_alive_.front());
        active_series_.push_back(std::make_shared<Series>(
            ub_alive_.front(), lb_alive_.front(), 5, name_ + " - Grand Final"));
        phase_label_      = "Grand Final";
        current_ub_label_ = "Grand Final";
        current_ub_bo_    = 5;
        return;
    }

    // === Schedule UB round if 2+ alive in UB ===
    if (ub_alive_.size() >= 2) {
        ++ub_round_;
        int ub_remaining = static_cast<int>(ub_alive_.size());
        int  bo  = bracket_bo_for(/*is_lower=*/false, ub_remaining, /*is_lb_final=*/false);
        std::string lbl = bracket_label_for(/*is_lower=*/false, ub_remaining,
                                             /*is_lb_final=*/false, ub_round_);
        // For UB matches we DON'T re-seed each round; the bracket already
        // determined who plays whom by virtue of who advanced from where.
        // CRITICAL: snapshot ub_alive_ into a local before pair_matchups,
        // then CLEAR ub_alive_. play_round will re-populate ub_alive_ with
        // the winners of the round we just scheduled. Without the clear,
        // ub_alive_ accumulates teams across rounds and the bracket size
        // doubles every round (the historical "tournament leaks +
        // thousands of matches" career-stat bug — §8 pitfall #8).
        std::vector<TeamPtr> ub_round_teams = ub_alive_;
        ub_alive_.clear();
        pair_matchups(ub_round_teams, current_matchups_, active_series_, bo, lbl);
        current_ub_label_ = lbl;
        current_ub_bo_    = bo;
        phase_label_      = lbl;
    }

    // === Schedule LB round ===
    // Decide how to mix lb_alive_ + ub_just_dropped_ into the next LB round.
    //
    // Canonical double-elim cadence (UB has K rounds = log2(N) total):
    //   LB R1: pair UB R1 losers among themselves         (pure of fresh drops)
    //   LB R2: LB R1 winners vs UB R2 losers              (merge 1:1)
    //   LB R3: LB R2 winners play each other              (pure)
    //   LB R4: LB R3 winners vs UB R3 losers              (merge 1:1)
    //   ... alternating until 2 teams left in LB ...
    //   LB Semifinal: pure pairing of last 2 lb survivors
    //   LB Final: that LB survivor vs UB Final loser      (merge of 1:1, BO5)
    //
    // The merge / pure choice depends on whether the freshly-dropped UB teams
    // are READY to enter (sizes match the LB veteran pool) or have to wait
    // one round for the veterans to narrow down. Previously the code merged
    // greedily on any size match >= 1 and held leftovers — that's the
    // phantom-round bug. New rule: merge ONLY when lb_alive_.size() ==
    // ub_just_dropped_.size(). If lb_alive_ is bigger, run a pure round
    // first; drops keep waiting in ub_just_dropped_.
    bool have_drops = !ub_just_dropped_.empty();
    bool have_lb    = !lb_alive_.empty();

    std::vector<TeamPtr> lb_round_teams;
    bool schedule_lb = false;
    bool is_lb_final = false;

    if (have_drops && !have_lb) {
        // LB R1: pair the drops among themselves
        lb_round_teams = ub_just_dropped_;
        ub_just_dropped_.clear();
        schedule_lb = true;
    } else if (have_drops && have_lb &&
               lb_alive_.size() == ub_just_dropped_.size()) {
        // Merge round - sizes match, pair LB veterans against new drops 1:1.
        // If this is the FINAL merge (one of each, AND UB has a single
        // survivor waiting for GF), this IS the Lower Bracket Final.
        is_lb_final = (lb_alive_.size() == 1 && ub_just_dropped_.size() == 1 &&
                       ub_alive_.size() == 1);
        for (std::size_t i = 0; i < lb_alive_.size(); ++i) {
            lb_round_teams.push_back(lb_alive_[i]);
            lb_round_teams.push_back(ub_just_dropped_[i]);
        }
        lb_alive_.clear();
        ub_just_dropped_.clear();
        schedule_lb = true;
    } else if (have_lb && lb_alive_.size() >= 2) {
        // Pure LB round - LB veterans play each other. Drops (if any) wait
        // in ub_just_dropped_ for the next call when sizes will match.
        // This is the LB Semifinal (or earlier pure LB round).
        lb_round_teams = lb_alive_;
        lb_alive_.clear();
        schedule_lb = true;
    }
    // (If have_lb but lb_alive_.size() == 1 with no drops, we wait for the
    // next UB round to drop someone, or for GF to be ready. Shouldn't
    // normally hit this with the new logic since a lone lb survivor with no
    // drops means UB still has rounds to play.)

    if (schedule_lb) {
        ++lb_round_;
        int lb_remaining = static_cast<int>(lb_round_teams.size());
        int  bo  = bracket_bo_for(/*is_lower=*/true, lb_remaining, is_lb_final);
        std::string lbl = bracket_label_for(/*is_lower=*/true, lb_remaining,
                                             is_lb_final, lb_round_);
        pair_matchups(lb_round_teams, lower_matchups_, lower_series_, bo, lbl);
        current_lb_label_ = lbl;
        current_lb_bo_    = bo;
        // If we scheduled LB, prefer its label (more dramatic) over UB's
        // unless UB is final.
        if (current_matchups_.empty() || ub_alive_.size() < 2) {
            phase_label_ = lbl;
        }
    }

    // === Edge case: no scheduled matches but we expected some ===
    // If nothing got scheduled and we're not done, the bracket is in a
    // weird state. Force a champion declaration to avoid an infinite loop.
    if (current_matchups_.empty() && lower_matchups_.empty() &&
        current_phase_ != Phase::GrandFinal) {
        if (!ub_alive_.empty()) {
            champion_ = ub_alive_.front();
            current_phase_ = Phase::Done;
        } else if (!lb_alive_.empty()) {
            champion_ = lb_alive_.front();
            current_phase_ = Phase::Done;
        }
    }
}

std::vector<Series> Tournament::play_round(
    std::unordered_map<std::string, SoloQEngine*>& solo_qs, int current_year) {
    std::vector<Series> logs;

    // ===== GROUP STAGE =====
    if (current_phase_ == Phase::Groups) {
        std::size_t per_round = std::max<std::size_t>(1, groups_.size());
        std::size_t end_idx = std::min(current_matchups_.size(),
                                       static_cast<std::size_t>(group_match_idx_) + per_round);
        for (std::size_t i = static_cast<std::size_t>(group_match_idx_); i < end_idx; ++i) {
            auto& mu = current_matchups_[i];
            if (!mu.first || !mu.second) continue;
            auto& series = active_series_[i];
            auto fa1 = solo_qs.count(mu.first->region)  ? solo_qs[mu.first->region]->global_ladder()  : std::vector<PlayerPtr>{};
            auto fa2 = solo_qs.count(mu.second->region) ? solo_qs[mu.second->region]->global_ladder() : std::vector<PlayerPtr>{};
            mu.first->auto_fill_roster(fa1);
            mu.second->auto_fill_roster(fa2);
            int t1_maps = 0, t2_maps = 0, t1_rounds = 0, t2_rounds = 0;
            while (!series->is_over()) {
                const auto& M = maps()[static_cast<std::size_t>(rng().irange(0, static_cast<int>(maps().size()) - 1))];
                Match m(mu.first, mu.second, M, false, series->event_name(),
                        /*friendly=*/false, &series->map_history());
                m.play();
                series->add_match_data(m);
                if (m.team1_score() > m.team2_score()) ++t1_maps; else ++t2_maps;
                t1_rounds += m.team1_score();
                t2_rounds += m.team2_score();
            }
            series->finalize_stats(false);
            // Track every starter who appeared in this Series so
            // award_event_titles can filter the champion roster to actual
            // participants. Group-stage maps still count — a player who only
            // played group stage and was then traded off the champ before
            // playoffs would not be on the champ's roster at award time
            // anyway, so the ∩ filter naturally excludes them.
            absorb_series_participants(*series);
            logs.push_back(*series);
            for (auto& g : groups_) {
                for (auto& gs : g) {
                    if (gs.team == mu.first) {
                        // BO2 tie: winner is null — credit neither side
                        // with a W or an L. Map diff still applies.
                        if (series->winner() == mu.first)       ++gs.wins;
                        else if (series->winner() == mu.second) ++gs.losses;
                        gs.map_diff   += t1_maps - t2_maps;
                        gs.round_diff += t1_rounds - t2_rounds;
                    } else if (gs.team == mu.second) {
                        if (series->winner() == mu.second)      ++gs.wins;
                        else if (series->winner() == mu.first)  ++gs.losses;
                        gs.map_diff   += t2_maps - t1_maps;
                        gs.round_diff += t2_rounds - t1_rounds;
                    }
                }
            }
        }
        group_match_idx_ = static_cast<int>(end_idx);
        ++round_num_;
        if (static_cast<std::size_t>(group_match_idx_) >= current_matchups_.size()) {
            resolve_groups_to_playoffs();
        }
        return logs;
    }

    // ===== BRACKET ROUND (UB + LB in parallel) =====
    if (current_phase_ == Phase::Bracket) {
        // Snapshot per-side labels + BO captured at schedule time. We stamp
        // each BracketMatch history row with these so the UI/tests see the
        // exact (label, best_of) the round was created with — even after
        // schedule_next_bracket_round overwrites these members for the
        // *next* round at the end of this call.
        std::string ub_label = current_ub_label_.empty() ? phase_label_ : current_ub_label_;
        int         ub_bo    = current_ub_bo_ > 0 ? current_ub_bo_ : 3;
        std::string lb_label = current_lb_label_;
        int         lb_bo    = current_lb_bo_ > 0 ? current_lb_bo_ : 3;
        // Play UB matches, advance winners to ub_alive_, drop losers to ub_just_dropped_
        std::vector<BracketMatch> ub_round_snap;
        ub_round_snap.reserve(current_matchups_.size());
        for (std::size_t i = 0; i < current_matchups_.size(); ++i) {
            auto& mu = current_matchups_[i];
            if (!mu.second) {
                ub_alive_.push_back(mu.first);
                BracketMatch bm; bm.a = mu.first; bm.b.reset();
                bm.winner = mu.first; bm.played = true; bm.is_lower = false;
                bm.label = ub_label;
                bm.best_of = ub_bo;
                ub_round_snap.push_back(bm);
                continue;
            }
            auto& series = active_series_[i];
            Series s = play_series(mu.first, mu.second, series, solo_qs);
            absorb_series_participants(s);
            logs.push_back(s);
            ub_alive_.push_back(s.winner());
            TeamPtr loser = (s.winner() == mu.first) ? mu.second : mu.first;
            BracketMatch bm; bm.a = mu.first; bm.b = mu.second;
            bm.winner = s.winner(); bm.played = true; bm.is_lower = false;
            bm.a_score = s.t1_wins(); bm.b_score = s.t2_wins();
            bm.label = ub_label;
            bm.best_of = series ? series->best_of() : ub_bo;
            ub_round_snap.push_back(bm);
            // SingleElim: losers eliminated immediately
            if (format_ == TournamentFormat::SingleElim) {
                eliminated_.push_back(loser);
            } else {
                ub_just_dropped_.push_back(loser);
            }
        }
        if (!ub_round_snap.empty()) ub_history_.push_back(std::move(ub_round_snap));

        // Play LB matches, advance winners to lb_alive_, eliminate losers
        std::vector<BracketMatch> lb_round_snap;
        lb_round_snap.reserve(lower_matchups_.size());
        for (std::size_t i = 0; i < lower_matchups_.size(); ++i) {
            auto& mu = lower_matchups_[i];
            if (!mu.second) {
                lb_alive_.push_back(mu.first);
                BracketMatch bm; bm.a = mu.first; bm.b.reset();
                bm.winner = mu.first; bm.played = true; bm.is_lower = true;
                bm.label = lb_label;
                bm.best_of = lb_bo;
                lb_round_snap.push_back(bm);
                continue;
            }
            auto& series = lower_series_[i];
            Series s = play_series(mu.first, mu.second, series, solo_qs);
            absorb_series_participants(s);
            logs.push_back(s);
            lb_alive_.push_back(s.winner());
            TeamPtr loser = (s.winner() == mu.first) ? mu.second : mu.first;
            BracketMatch bm; bm.a = mu.first; bm.b = mu.second;
            bm.winner = s.winner(); bm.played = true; bm.is_lower = true;
            bm.a_score = s.t1_wins(); bm.b_score = s.t2_wins();
            bm.label = lb_label;
            bm.best_of = series ? series->best_of() : lb_bo;
            lb_round_snap.push_back(bm);
            eliminated_.push_back(loser);
        }
        if (!lb_round_snap.empty()) lb_history_.push_back(std::move(lb_round_snap));

        // SingleElim shortcut: if 1 alive in UB, that's the champ.
        if (format_ == TournamentFormat::SingleElim) {
            if (ub_alive_.size() == 1) {
                champion_ = ub_alive_.front();
                current_phase_ = Phase::Done;
                current_matchups_.clear();
                active_series_.clear();
                award_event_titles(current_year);
                return logs;
            }
            current_matchups_.clear();
            active_series_.clear();
            ++round_num_;
            ++ub_round_;
            int rem = static_cast<int>(ub_alive_.size());
            // SingleElim uses UB-only rounds; no LB Final concept. All BO3
            // under new spec (BO5 reserved for LB Final + GF only, and
            // SingleElim has neither — losers eliminated direct).
            int  se_bo  = bracket_bo_for(/*is_lower=*/false, rem, /*is_lb_final=*/false);
            std::string se_lbl = bracket_label_for(/*is_lower=*/false, rem,
                                                    /*is_lb_final=*/false, ub_round_);
            pair_matchups(ub_alive_, current_matchups_, active_series_, se_bo, se_lbl);
            current_ub_label_ = se_lbl;
            current_ub_bo_    = se_bo;
            phase_label_      = se_lbl;
            // Clear ub_alive_ now; play_round's UB loop appends winners next
            // call, so this pool must start empty each round.
            ub_alive_.clear();
            return logs;
        }

        // DoubleElim: schedule the next round
        ++round_num_;
        schedule_next_bracket_round();
        return logs;
    }

    // ===== GRAND FINAL =====
    // Single BO5 decides the champion. No reset, regardless of which side
    // wins. UB-seed advantage is implicit in the seeding (UB winner skips
    // LB; LB winner had to climb out).
    // Tournament ends after GF. No reset. (User spec 2026-05-14: VCT-style single GF.)
    if (current_phase_ == Phase::GrandFinal) {
        if (current_matchups_.empty()) return logs;
        auto& mu = current_matchups_.front();
        auto& series = active_series_.front();
        Series s = play_series(mu.first, mu.second, series, solo_qs);
        absorb_series_participants(s);
        logs.push_back(s);
        BracketMatch bm; bm.a = mu.first; bm.b = mu.second;
        bm.winner = s.winner(); bm.played = true; bm.is_lower = false;
        bm.a_score = s.t1_wins(); bm.b_score = s.t2_wins();
        bm.label = "Grand Final";
        bm.best_of = series ? series->best_of() : 5;
        gf_history_.push_back(bm);
        // Winner of the GF is the champion, no exceptions.
        champion_ = s.winner();
        TeamPtr runner_up = (champion_ == mu.first) ? mu.second : mu.first;
        eliminated_.push_back(runner_up);
        // Tournament ends after GF. No reset. (User spec 2026-05-14: VCT-style single GF.)
        current_phase_ = Phase::Done;
        current_matchups_.clear();
        active_series_.clear();
        award_event_titles(current_year);
        return logs;
    }

    return logs;
}

bool Tournament::validate_bracket_state(std::string& out_err) const {
    out_err.clear();
    // Pure brackets (no group stage) only — Groups phase has a different
    // invariant set we don't bother to check here.
    if (current_phase_ == Phase::Groups) return true;
    if (current_phase_ == Phase::Done) {
        if (!champion_) {
            out_err = "Phase::Done but champion_ is null";
            return false;
        }
        return true;
    }

    // Total teams alive at any moment must be <= initial seeding size.
    const int seed_n = static_cast<int>(initial_seeding_.size());
    const int alive_n = static_cast<int>(ub_alive_.size() + lb_alive_.size()
                                          + ub_just_dropped_.size());
    if (seed_n > 0 && alive_n > seed_n) {
        out_err = "alive pools (UB=" + std::to_string(ub_alive_.size())
                + " LB=" + std::to_string(lb_alive_.size())
                + " drops=" + std::to_string(ub_just_dropped_.size())
                + ") exceed initial seeding (" + std::to_string(seed_n) + ")";
        return false;
    }

    // GF preconditions check.
    if (current_phase_ == Phase::GrandFinal) {
        if (ub_alive_.size() != 1 || lb_alive_.size() != 1
            || !ub_just_dropped_.empty()) {
            out_err = "GrandFinal phase requires exactly 1 UB + 1 LB alive "
                      "and 0 fresh drops";
            return false;
        }
        if (current_matchups_.size() != 1) {
            out_err = "GrandFinal phase requires exactly 1 scheduled match";
            return false;
        }
    }

    // History rows should be monotonically non-empty (we never push an
    // empty round, by construction in play_round). Sanity-check label /
    // best_of fields are stamped.
    for (std::size_t i = 0; i < ub_history_.size(); ++i) {
        const auto& round = ub_history_[i];
        if (round.empty()) {
            out_err = "ub_history_[" + std::to_string(i) + "] is empty";
            return false;
        }
        for (const auto& bm : round) {
            if (bm.played && !bm.winner) {
                out_err = "ub_history_ row has played=true but null winner";
                return false;
            }
        }
    }
    for (std::size_t i = 0; i < lb_history_.size(); ++i) {
        const auto& round = lb_history_[i];
        if (round.empty()) {
            out_err = "lb_history_[" + std::to_string(i) + "] is empty";
            return false;
        }
        for (const auto& bm : round) {
            if (bm.played && !bm.winner) {
                out_err = "lb_history_ row has played=true but null winner";
                return false;
            }
        }
    }

    // GF history must hold exactly one entry once the GF resolves (per
    // user spec 2026-05-14: single GF, no bracket reset).
    if (current_phase_ == Phase::Done && gf_history_.size() != 1) {
        out_err = "Done phase but gf_history_.size() != 1 ("
                + std::to_string(gf_history_.size()) + ")";
        return false;
    }

    // BO sanity: every UB round + non-LB-Final LB round should be BO3;
    // LB Final + GF should be BO5. (Group stage is BO1 inline and not
    // in either *_history_ vector.)
    for (const auto& round : ub_history_) {
        for (const auto& bm : round) {
            if (bm.b && bm.best_of != 3) {
                out_err = "UB bracket match expected BO3 (got "
                        + std::to_string(bm.best_of) + ")";
                return false;
            }
        }
    }
    for (std::size_t i = 0; i < lb_history_.size(); ++i) {
        const auto& round = lb_history_[i];
        bool is_lb_final_row = false;
        for (const auto& bm : round) {
            if (bm.label.find("Final") != std::string::npos) {
                is_lb_final_row = true; break;
            }
        }
        for (const auto& bm : round) {
            if (!bm.b) continue;
            int want = is_lb_final_row ? 5 : 3;
            if (bm.best_of != want) {
                out_err = "LB bracket match expected BO" + std::to_string(want)
                        + " (got " + std::to_string(bm.best_of) + ")";
                return false;
            }
        }
    }
    for (const auto& bm : gf_history_) {
        if (bm.b && bm.best_of != 5) {
            out_err = "GF expected BO5 (got " + std::to_string(bm.best_of) + ")";
            return false;
        }
    }
    return true;
}

}  // namespace vlr
