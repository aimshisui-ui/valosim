#include "Goat.h"

#include <algorithm>
#include <initializer_list>
#include <string_view>

namespace vlr {

namespace {

// Count [A]-prefixed season awards whose body contains `needle` but NOT
// any string in `exclude`. Used for role / MVP / IGL breakdowns. Always
// gated on the "[A] " prefix so a malformed "[X] Duelist ..." entry can
// never inflate the count.
int count_season_awards_matching(
        const Player& p,
        std::string_view needle,
        std::initializer_list<std::string_view> exclude = {}) {
    int n = 0;
    for (auto& a : p.awards) {
        if (a.size() < 4 || a.compare(0, 4, "[A] ") != 0) continue;
        if (a.find(needle) == std::string::npos) continue;
        bool skip = false;
        for (auto& e : exclude) {
            if (!e.empty() && a.find(e) != std::string::npos) { skip = true; break; }
        }
        if (!skip) ++n;
    }
    return n;
}

}  // namespace

double goat_career_score(const Player& p, const GoatWeights& w) {
    // Safety belt: dedupe_awards is invoked at every year-end inside
    // Player::save_history_and_progress, so duplicates should not be
    // present here. We deliberately do NOT re-dedupe (this is a const
    // hot-path) — trust the year-end pass.
    double s = 0.0;

    // Championships — match strictly on the literal prefix the pin logic
    // emits. Anything with a malformed prefix (e.g. "[X] ..." from a
    // future bug) is silently ignored.
    s += w.champ_world    * p.award_count_by_prefix("[W] ");
    s += w.champ_masters  * p.award_count_by_prefix("[M] ");
    s += w.champ_regional * p.award_count_by_prefix("[T] ");

    // Individual season awards — all [A]-prefixed.
    s += w.mvp_award * count_season_awards_matching(p, "MVP");
    int role_titles = 0;
    role_titles += count_season_awards_matching(p, "Duelist of the Year");
    role_titles += count_season_awards_matching(p, "Initiator of the Year");
    role_titles += count_season_awards_matching(p, "Controller of the Year");
    role_titles += count_season_awards_matching(p, "Sentinel of the Year");
    s += w.role_award * role_titles;
    s += w.igl_award  * count_season_awards_matching(p, "IGL of the Year");

    // Sustained pro performance
    s += w.per_match * p.career_matches;
    double avg_rat = p.avg_match_rating();
    if (avg_rat > 1.0) s += w.avg_rating_bonus * (avg_rat - 1.0);

    double peak_rat = 0.0;
    for (auto& h : p.history) peak_rat = std::max(peak_rat, h.rating);
    s += w.peak_rating_bonus * std::max(0.0, peak_rat - 1.0);

    s += w.per_kill        * p.career_kills;
    s += w.per_assist      * p.career_assists;
    s += w.per_first_blood * p.career_fb;
    s += w.per_mvp_match   * p.career_mvps;

    // HoF milestones
    if (p.career_max_match_kills >= 30)    s += w.bonus_30_kill_match;
    if (p.career_max_match_kd_x100 >= 200) s += w.bonus_2kd_match;
    s += w.bonus_gf_clutch * p.career_grand_final_clutches;
    if (p.ever_top20_solo) s += w.bonus_top20_solo;

    // Ranked
    if (p.peak_mmr > 2000) s += w.peak_mmr_above_2k * (p.peak_mmr - 2000);
    s += w.per_solo_win * p.solo_wins;

    // IGL strategic impact — accumulated career total. Stat-decoupled lane
    // so an all-time IGL with mediocre frag stats can still hit the board.
    s += w.igl_impact_per_career * p.igl_impact_total;

    return s;
}

double goat_season_score(const Player& p, int season_idx, const GoatWeights& w) {
    if (season_idx < 0 || season_idx >= (int)p.history.size()) return 0.0;
    const auto& h = p.history[static_cast<std::size_t>(season_idx)];

    double s = 0.0;

    // Award counts for THIS season specifically. h.awards is a *cumulative
    // snapshot* of the player's awards as of year-end of season `h.year`,
    // so older awards are also in there — we MUST filter by year string
    // or every season after a championship would falsely re-credit it.
    // (PROJECT_GUIDE §4.13, §8.15 — DO NOT REMOVE THE YEAR FILTER.)
    std::string yr_str = std::to_string(h.year);
    int worlds = 0, masters = 0, regionals = 0;
    for (auto& a : h.awards) {
        // Year-substring gate first — preserves the cumulative-snapshot fix.
        if (a.find(yr_str) == std::string::npos) continue;
        // Then dispatch by literal prefix. Unrecognized prefixes are
        // silently skipped (defensive — never crash, never score garbage).
        if (a.size() < 4) continue;
        if      (a.compare(0, 4, "[W] ") == 0) ++worlds;
        else if (a.compare(0, 4, "[M] ") == 0) ++masters;
        else if (a.compare(0, 4, "[T] ") == 0) ++regionals;
        // [A] season awards + anything else: not a championship — skip.
    }
    s += w.champ_world    * worlds;
    s += w.champ_masters  * masters;
    s += w.champ_regional * regionals;

    // Season rating + KD weighting.
    if (h.rating > 1.0) s += w.avg_rating_bonus * 0.6 * (h.rating - 1.0);
    s += w.peak_rating_bonus * std::max(0.0, h.rating - 1.0) * 0.5;
    if (h.kd > 1.0) s += 8.0 * (h.kd - 1.0);

    // Per-season volume bonus, but smaller than the career version.
    s += w.per_match * 30.0;  // ~assume one season ~ 30 matches

    return s;
}

std::vector<GoatRow> compute_goat_career(
        const std::vector<PlayerPtr>& all_players,
        const GoatWeights& w,
        std::size_t max_rows) {
    std::vector<GoatRow> rows;
    rows.reserve(all_players.size());
    for (auto& p : all_players) {
        if (!p) continue;
        if (p->career_matches < 10) continue;  // min-games filter
        GoatRow r;
        r.player = p;
        r.score  = goat_career_score(*p, w);
        rows.push_back(std::move(r));
    }
    std::sort(rows.begin(), rows.end(),
              [](const GoatRow& a, const GoatRow& b) { return a.score > b.score; });
    if (rows.size() > max_rows) rows.resize(max_rows);
    return rows;
}

std::vector<GoatRow> compute_goat_season(
        const std::vector<PlayerPtr>& all_players,
        const GoatWeights& w,
        std::size_t max_rows) {
    std::vector<GoatRow> rows;
    for (auto& p : all_players) {
        if (!p) continue;
        for (int i = 0; i < (int)p->history.size(); ++i) {
            const auto& h = p->history[static_cast<std::size_t>(i)];
            if (h.rating <= 0.0) continue;
            GoatRow r;
            r.player = p;
            r.season = h.year;
            r.season_team = h.team;
            r.season_rating = h.rating;
            r.season_kd = h.kd;
            r.score = goat_season_score(*p, i, w);
            rows.push_back(std::move(r));
        }
    }
    std::sort(rows.begin(), rows.end(),
              [](const GoatRow& a, const GoatRow& b) { return a.score > b.score; });
    if (rows.size() > max_rows) rows.resize(max_rows);
    return rows;
}

}  // namespace vlr
