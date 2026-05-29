#include "GameManager.h"

#include "Match.h"
#include "Names.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <unordered_set>

namespace vlr {

namespace {
const std::vector<std::string> kPhases = {
    "STAGE 1", "REGIONALS 1", "MASTERS 1",
    "STAGE 2", "REGIONALS 2", "MASTERS 2",
    "STAGE 3", "REGIONALS 3", "CHAMPIONS",
    "AWARDS",
    "OFFSEASON"   // user re-signs / signs FAs / trades; AI doesn't touch user team
};
}  // namespace

const std::vector<std::string>& GameManager::phases() { return kPhases; }
GameManager::GameManager() = default;

const std::string& GameManager::current_phase() const {
    return kPhases[static_cast<std::size_t>(phase_idx) % kPhases.size()];
}

std::pair<int,int> GameManager::current_phase_pacing() const {
    const std::string& p = current_phase();
    if (p.find("STAGE")     != std::string::npos) return {3, 11};   // 11 rounds, every 3rd day
    // Tournament pacing: short calendar windows. The actual bracket round
    // counts (~7-16 rounds for various event sizes) almost never fit in
    // these windows — that's intentional. force_finish_stale_tournaments
    // runs at phase end and plays whatever rounds are left in one shot,
    // closing the bracket cleanly. This keeps the calendar quick to skim
    // while still guaranteeing the bracket finishes (the historical
    // "tournament leaks into next phase" bug that produced thousands of
    // career matches).
    // Tournament pacing — generous enough that the entire bracket plays
    // out matchday-by-matchday so the user actually sees rounds advance.
    // Previous tight pacing forced force_finish_stale_tournaments to
    // silently complete the bracket at phase end, which made it look
    // like "playoffs never happened". Force_finish is still wired in as
    // a safety net for any leftover.
    //   8-team double-elim regionals ≈ 7-9 rounds
    //   8-team groups+playoffs masters ≈ 6 group rounds + 7 bracket rounds
    //   16-team groups+playoffs champs ≈ 6 group rounds + 7 bracket rounds
    if (p.find("REGIONALS") != std::string::npos) return {1, 9};
    if (p.find("MASTERS")   != std::string::npos) return {1, 14};
    if (p.find("CHAMPIONS") != std::string::npos) return {1, 16};
    if (p == "AWARDS")                            return {1, 1};
    if (p == "OFFSEASON")                         return {1, 14};   // 14 days of free agency
    return {1, 1};
}

std::unordered_map<std::string, SoloQEngine*> GameManager::raw_solo_q_map() {
    std::unordered_map<std::string, SoloQEngine*> out;
    for (auto& kv : solo_qs) out[kv.first] = kv.second.get();
    return out;
}

void GameManager::initialize_world() {
    leagues.clear();
    solo_qs.clear();

    // Main countries per region — these are the ones a team is most likely
    // to be headquartered in. Mirrors the designer-provided spec.
    auto main_countries_for = [](Region r) -> std::vector<const Country*> {
        std::vector<const Country*> out;
        std::vector<std::string> wanted;
        switch (r) {
            case Region::Americas:
                wanted = {"Brazil","USA","Canada","Mexico","Chile","Argentina"};
                break;
            case Region::EMEA:
                wanted = {"Germany","Sweden","UK","Ukraine","France","Turkey",
                          "Finland","Scotland","Ireland"};
                break;
            case Region::Pacific:
                wanted = {"Australia","South Korea","Japan","Singapore",
                          "Thailand","Vietnam","Philippines"};
                break;
            default: break;
        }
        for (auto& n : wanted) {
            const Country* c = find_country(n);
            if (c) out.push_back(c);
        }
        return out;
    };

    auto stamp_dob = [&](const PlayerPtr& p) {
        if (!p) return;
        if (p->birth_year == 0) p->birth_year = year - p->age;
        if (p->birth_city.empty()) p->birth_city = pick_city(p->country_iso);
    };

    for (auto* region_str : kRegions) {
        std::string r(region_str);
        Region reg = region_from_str(r);
        auto sq = std::make_shared<SoloQEngine>(r);
        // Larger initial ladder so the regional ranked scene has enough
        // depth to organically push ~15 players into Radiant range each
        // season (top 1.5-2% of a ~900-player ladder = ~15 Radiants).
        sq->populate_initial_ladder(900);
        for (auto& p : sq->global_ladder()) stamp_dob(p);
        solo_qs[r] = sq;

        auto mains = main_countries_for(reg);

        std::vector<TeamPtr> region_teams;
        region_teams.reserve(12);
        for (int i = 0; i < 12; ++i) {
            std::string tn = take_team_name();
            // 2026-05 budget rebalance: salaries cap at $180K so an AI team's
            // realistic payroll lands $150K-$450K. Old range $1.2M-$1.8M was
            // ~5x too generous for the new salary scale. Scaled by ~0.40.
            long long budget = rng().irange(480000, 720000);
            auto team = std::make_shared<Team>(std::move(tn), budget, r);

            // Home country: weighted pick from this region's main countries.
            if (!mains.empty()) {
                std::vector<int> w;
                for (auto* c : mains) w.push_back(c->weight);
                int idx = rng().weighted_index(w);
                if (idx < 0) idx = 0;
                const Country* home = mains[idx];
                team->home_country     = home->name;
                team->home_country_iso = home->iso;
                team->home_city        = pick_city(home->iso);
            }

            team->head_coach = generate_coach(r);
            team->head_coach->team_name = team->name;
            team->head_coach->contract_exp_year = year + team->head_coach->contract_years - 1;

            // Initial prestige + sponsorship from random spread; will get
            // re-classified after the snake draft. Sponsorship scales with
            // prestige so contender-tier orgs have more revenue.
            team->prestige = rng().irange(35, 80);
            // 2026-05 rebalance: old formula (600 + prestige*20) produced
            // up to ~$2.2M annual sponsorship — wildly too much for the new
            // budget scale. New formula yields $200K-$840K and keeps mid
            // tiers solvent without making them rich.
            team->sponsorship_k = 200 + team->prestige * 8;

            // Pre-classify with empty roster so the draft can use a
            // strategy hint right away. Roster classification runs again
            // after the draft.
            team->strategy = classify_team_strategy(*team);
            region_teams.push_back(team);
        }

        auto league = std::make_shared<League>("VCT " + r, region_teams);
        league->generate_schedule();
        leagues[r] = league;
    }

    // === Snake draft fills every team's roster from a shared FA pool ===
    // Replaces the old per-team auto_fill_roster sequential signing.
    std::vector<std::string> draft_log;
    run_initial_snake_draft(draft_log);
    for (auto& kv : solo_qs) {
        for (auto& p : kv.second->global_ladder()) stamp_dob(p);
    }
    // After the draft, re-classify strategies based on actual roster
    // composition (potential, age, OVR mix). Then sync every signed
    // player's solo Q region to their pro team's region.
    for (auto& kv : leagues) {
        for (auto& t : kv.second->teams()) {
            if (!t) continue;
            t->strategy = classify_team_strategy(*t);
            // Seed `previous_strategy` to the initial classification so the
            // FIRST year-end inertia roll sees `suggested == previous`
            // (fast-path) and year-1 behaviour exactly matches the
            // pre-inertia world. Subsequent years will see real
            // transitions filtered through commit_strategy_with_inertia.
            t->previous_strategy = t->strategy;
            // Canonical roster invariants: each starting 5 ends up with
            // (a) the positional shape 1D + 1C + 1S + 1I + 1Flex driven
            // by target_comp.need (Option A: Flex extends one role-bucket
            // for the given map), and (b) exactly one is_igl flag set
            // — IGL is a leadership designation that overlays whichever
            // gameplay position the player is on (Controller IGL,
            // Initiator IGL, etc.). is_igl and is_flex are independent:
            // a player can be both ("Flex IGL"). sign_player already
            // calls these on every add, but a defensive pass here
            // guarantees consistency on fresh saves + protects against
            // any future code path that mutates the roster outside
            // sign_player().
            t->enforce_one_igl();
            t->enforce_one_flex();
        }
    }
    sync_all_ranked_regions();

    auto& america = leagues["Americas"]->teams();
    if (!america.empty()) {
        user_team = america.front();
        // Keep the team's pre-generated name (no Team_NNNN ever produced
        // by take_team_name now). Bump budget so the user starts with
        // some flexibility.
        // 2026-05 budget rebalance: $1.5M was a "premium starter" amount
        // on the old scale; $800K plays the same role on the new $250K-$2M
        // scale (well-off mid-tier with room to chase upgrades).
        user_team->budget = 800000;
        user_team->prestige = 65;
        // Sponsorship matches the new formula (200 + prestige*8 = 720)
        // but is bumped slightly so the user feels "premium" out of the gate.
        user_team->sponsorship_k = 800;
        for (auto& p : user_team->roster) p->team_name = user_team->name;
        if (user_team->head_coach) user_team->head_coach->team_name = user_team->name;
    }
}

void GameManager::reset_world() {
    // Wipe every container the game owns so a fresh initialize_world (or
    // initialize_world_with_config) can boot cleanly without restarting
    // the exe. Order doesn't matter — these are all owned containers with
    // no cross-references.
    leagues.clear();
    solo_qs.clear();
    active_tournaments.clear();

    user_team.reset();

    // Time + phase counters back to defaults.
    year                     = 2026;
    day_in_year              = 0;
    day_total                = 0;
    day_in_phase             = 0;
    days_since_progression   = 0;
    phase_idx                = 0;
    current_week             = 0;

    // User-facing lists.
    favorite_players.clear();
    hall_of_fame.clear();
    recent_progression_reports.clear();

    // Awards / news / race state.
    awards_history.clear();
    last_season_awards.clear();
    news_feed.clear();
    news_pushed_events_.clear();
    mvp_race.clear();

    // Power / community rankings.
    power_rankings_.clear();
    community_rankings_.clear();
    last_power_tick_day_     = -10;
    last_community_tick_day_ = -10;

    // News emitter state.
    news_emitted_keys_.clear();
    // Career-keyed milestones are normally never cleared (they track
    // once-in-a-lifetime achievements that must not re-fire). reset_world
    // is the one exception — a brand-new world starts with no career
    // history at all, so we wipe the set to match.
    news_emitted_career_keys_.clear();
    consecutive_series_wins_.clear();
    h2h_counts_.clear();
    h2h_counts_regular_.clear();
    h2h_counts_playoff_.clear();
    h2h_finals_log_.clear();
    last_mvp_leader_name_.clear();
    fa_demand_baseline_k_.clear();

    // Year-start snapshots — the historic-performance emitter consumes
    // these. Cleared + initialized=false so the first new year skips its
    // diff pass per existing behavior.
    max_kills_snapshot_at_year_start_.clear();
    max_kd_snapshot_at_year_start_.clear();
    gf_clutches_snapshot_at_year_start_.clear();
    last_year_rating_snapshot_.clear();
    two_years_ago_rating_snapshot_.clear();
    historic_snapshot_initialized_ = false;

    // Reset difficulty to neutral. Wizard re-applies it via
    // initialize_world_with_config.
    world_difficulty_ = 1.0;

    // Wipe Common.cpp's team-name / player-name "handed out" bookkeeping
    // so the new world doesn't immediately fall through to numeric
    // fallbacks.
    reset_name_caches();
}

bool GameManager::initialize_world_with_config(const NewGameConfig& cfg,
                                               std::vector<std::string>& log) {
    // 1. Stamp the wizard year BEFORE the seeded init so any roster /
    //    contract math that reads `year` starts from the user's selection
    //    instead of the default 2026.
    year = cfg.starting_year;

    // 2. Generate the world via the existing seeded path. This populates
    //    leagues, solo_qs, runs the snake draft, and picks user_team from
    //    Americas' first slot — we re-assign it below if the wizard chose
    //    a different region.
    initialize_world();

    // 3. Region override. If the wizard region doesn't match the default
    //    Americas pick, re-elect user_team from the configured region's
    //    league. Unknown regions silently fall back to the existing
    //    Americas pick.
    std::string region = cfg.user_region;
    if (region != "Americas" && region != "EMEA" && region != "Pacific") {
        log.emplace_back("[NewGame] Unknown region '" + region
                         + "' — defaulting to Americas.");
        region = "Americas";
    }
    if (region != "Americas") {
        auto it = leagues.find(region);
        if (it != leagues.end() && !it->second->teams().empty()) {
            auto new_user = it->second->teams().front();
            user_team = new_user;
        }
    }

    if (!user_team) {
        log.emplace_back("[NewGame] No user_team after init — aborting overlay.");
        return false;
    }

    // 4. Overlay the user-supplied identity onto user_team. We're
    //    replacing the procedurally-generated values — the team keeps
    //    its rostered players, schedule slot, coach, etc.
    if (!cfg.user_team_name.empty()) {
        // Free the procedurally-allocated name back to the pool? The
        // taken-set semantics make that hard — easier to just record
        // the new name. The displaced name simply leaks (one entry per
        // wizard run; not a problem).
        user_team->name = cfg.user_team_name;
        user_team->tag  = make_team_tag(cfg.user_team_name);
        for (auto& p : user_team->roster) p->team_name = user_team->name;
        if (user_team->head_coach) user_team->head_coach->team_name = user_team->name;
        // Reseed colors + logo from the new name so visual identity tracks
        // the rename. seed_team_colors uses an internal hash, and the Team
        // ctor's hash-derived logo would now be stale — recompute it the
        // same way the ctor does.
        seed_team_colors(*user_team);
        constexpr int kLogoCount = static_cast<int>(LogoShape::Count);
        std::size_t h = std::hash<std::string>{}(user_team->name);
        user_team->logo_shape = static_cast<LogoShape>(
            h % static_cast<std::size_t>(kLogoCount));
    }

    // 5. City + country ISO override. Empty city -> keep the seeded pick.
    if (!cfg.user_org_country_iso.empty()) {
        user_team->home_country_iso = cfg.user_org_country_iso;
        const Country* c = find_country_iso(cfg.user_org_country_iso);
        if (c) user_team->home_country = c->name;
    }
    if (!cfg.user_org_city.empty()) {
        user_team->home_city = cfg.user_org_city;
    } else if (user_team->home_city.empty() && !cfg.user_org_country_iso.empty()) {
        user_team->home_city = pick_city(cfg.user_org_country_iso);
    }

    // 6. Color overlay. Both 0 -> keep hash-derived colors from seed.
    if (cfg.user_color_primary != 0 || cfg.user_color_accent != 0) {
        auto unpack = [](std::uint32_t rgb, int& R, int& G, int& B) {
            R = static_cast<int>((rgb >> 16) & 0xFFu);
            G = static_cast<int>((rgb >>  8) & 0xFFu);
            B = static_cast<int>((rgb      ) & 0xFFu);
        };
        if (cfg.user_color_primary != 0) {
            unpack(cfg.user_color_primary,
                   user_team->color_primary_r,
                   user_team->color_primary_g,
                   user_team->color_primary_b);
        }
        if (cfg.user_color_accent != 0) {
            unpack(cfg.user_color_accent,
                   user_team->color_accent_r,
                   user_team->color_accent_g,
                   user_team->color_accent_b);
        }
    }

    // 7. Budget tier mapping. 2026-05 rebalance — salaries cap at $180K, so
    //    org-tier budgets rescaled to the new $250K-$2M scale:
    //      Rich      = $2.0M (max — title contenders can stack)
    //      Contender = $1.2M
    //      Mid       = $0.8M
    //      Budget    = $0.4M
    //      Expansion = $0.25M
    //    Prestige unchanged. Sponsorship matches the new formula
    //    200 + prestige*8 (see initialize_world).
    long long budget   = 800000;
    int       prestige = 55;
    switch (cfg.user_tier) {
        case NewGameConfig::OrgTier::Rich:       budget = 2000000; prestige = 85; break;
        case NewGameConfig::OrgTier::Contender:  budget = 1200000; prestige = 70; break;
        case NewGameConfig::OrgTier::Mid:        budget =  800000; prestige = 55; break;
        case NewGameConfig::OrgTier::Budget:     budget =  400000; prestige = 40; break;
        case NewGameConfig::OrgTier::Expansion:  budget =  250000; prestige = 25; break;
    }
    user_team->budget        = budget;
    user_team->prestige      = prestige;
    user_team->sponsorship_k = 200 + prestige * 8;

    // 8. Difficulty stash. Clamp to the spec §2A range (0.7..1.3) so a
    //    runaway wizard value can't break later math. The wizard slider
    //    publishes inside this range; the clamp is defense-in-depth.
    world_difficulty_ = clamp_v(cfg.difficulty, 0.7, 1.3);

    log.emplace_back("[NewGame] World seeded for year " + std::to_string(year)
                     + " — user_team '" + user_team->name + "' in " + region
                     + ", budget $" + std::to_string(budget / 1000) + "K"
                     + ", prestige " + std::to_string(prestige)
                     + ", difficulty " + std::to_string(world_difficulty_));
    return true;
}

void GameManager::force_finish_stale_tournaments(std::vector<std::string>& log) {
    // Defensive: if a phase ends with leftover unfinished tournaments,
    // play their remaining rounds in one go and then clear. Prevents the
    // tournament from "leaking" into the next phase's matchdays, which
    // would silently re-credit stats to the affected teams (the source
    // of the historical "thousands of matches" career-stat bug).
    if (active_tournaments.empty()) return;
    auto raw = raw_solo_q_map();
    int safety = 64;
    bool any_played = false;
    while (safety-- > 0) {
        bool any_unfinished = false;
        for (auto& tour : active_tournaments) {
            if (!tour->finished()) { any_unfinished = true; break; }
        }
        if (!any_unfinished) break;
        for (auto& tour : active_tournaments) {
            if (tour->finished()) continue;
            tour->play_round(raw, year);
            any_played = true;
        }
    }
    if (any_played) {
        log.emplace_back("[Tournament] Force-finished stale brackets at phase end.");
    }
    // Auto-news for any bracket that finished here (or that finished earlier
    // in play_tournament_round but missed news because the dedup key was
    // already set — this is a no-op in that case). We don't have the GF
    // Series object on this path, so MVP item is skipped — champion +
    // run-summary still publish.
    for (auto& tour : active_tournaments) {
        if (!tour) continue;
        if (!tour->finished()) continue;
        push_tournament_news(*tour, year, /*gf_series=*/nullptr);
        // Record trophy on the champion's team. Defensive against double-
        // recording: if the same tournament was already finished and news-
        // pushed in play_tournament_round, push_tournament_news's dedup
        // short-circuits, but record_trophy has no built-in dedup —
        // Agent B's storage layer is expected to dedup on (year, name).
        if (tour->champion()) {
            tour->champion()->record_trophy(year, tour->name());
        }
        // Best-effort GF capture for the H2H finals log. We don't have the
        // GF Series object on the force-finish path, so day_in_year is
        // stamped as "today" (the phase boundary). Duplicate-guarded by
        // refusing to insert if the same (year, event_name) already lives
        // in the log — the natural play_tournament_round path adds the
        // entry FIRST when both paths fire on the same bracket.
        if (tour->champion() && tour->runner_up()) {
            bool already = false;
            for (auto& f : h2h_finals_log_) {
                if (f.year == year && f.event_name == tour->name()) {
                    already = true; break;
                }
            }
            if (!already) {
                H2HSeriesFinal f;
                f.winner = tour->champion().get();
                f.loser  = tour->runner_up().get();
                f.event_name = tour->name();
                f.year = year;
                f.day_in_year = day_in_year;
                h2h_finals_log_.push_back(f);
            }
        }
    }
    active_tournaments.clear();
}

void GameManager::generate_tournaments_if_needed() {
    const std::string& phase = current_phase();
    // active_tournaments is cleared exclusively by force_finish_stale_tournaments
    // at phase boundaries. Do NOT auto-clear finished tournaments mid-phase —
    // doing so causes a brand-new bracket to spawn on the next matchday because
    // the empty-active check below will fire again, doubling [T]/[M]/[W] awards
    // and stat lines for every playoff cycle.
    if (phase.find("REGIONALS") != std::string::npos && active_tournaments.empty()) {
        // Regional playoffs: 8-team double-elim. Top 8 from stage W/L
        // standings, seeded high-vs-low. Single-elim if a region has fewer
        // than 8 teams (shouldn't happen with the default world).
        for (auto& kv : leagues) {
            auto& lg = kv.second;
            std::vector<TeamPtr> sorted_teams;
            for (auto& t : lg->teams()) if (t) sorted_teams.push_back(t);
            std::sort(sorted_teams.begin(), sorted_teams.end(),
                      [](const TeamPtr& a, const TeamPtr& b) { return a->phase_wins > b->phase_wins; });
            std::vector<TeamPtr> top;
            for (std::size_t i = 0; i < std::min<std::size_t>(8, sorted_teams.size()); ++i)
                top.push_back(sorted_teams[i]);
            TournamentFormat fmt = (top.size() >= 8)
                ? TournamentFormat::DoubleElim : TournamentFormat::SingleElim;
            active_tournaments.push_back(std::make_shared<Tournament>(
                kv.first + " Regionals", top, fmt));
        }
    } else if ((phase.find("MASTERS") != std::string::npos || phase.find("CHAMPIONS") != std::string::npos)
               && active_tournaments.empty()) {
        // International events: group stage -> single-elim playoff. Masters
        // pulls 8 teams (2 per region + best-of-rest), Champions pulls more.
        std::vector<TeamPtr> globals;
        for (auto& kv : leagues) {
            auto& lg = kv.second;
            std::vector<TeamPtr> sorted_teams;
            for (auto& t : lg->teams()) if (t) sorted_teams.push_back(t);
            std::sort(sorted_teams.begin(), sorted_teams.end(),
                      [](const TeamPtr& a, const TeamPtr& b) { return a->wins > b->wins; });
            for (std::size_t i = 0; i < std::min<std::size_t>(2, sorted_teams.size()); ++i)
                globals.push_back(sorted_teams[i]);
        }
        std::vector<TeamPtr> rem;
        for (auto& kv : leagues) {
            for (auto& t : kv.second->teams()) {
                if (!t) continue;
                bool already = std::find(globals.begin(), globals.end(), t) != globals.end();
                if (!already) rem.push_back(t);
            }
        }
        std::sort(rem.begin(), rem.end(),
                  [](const TeamPtr& a, const TeamPtr& b) { return a->ovr() > b->ovr(); });
        // Champions takes a wider field (16 teams w/ 4 groups). Masters is
        // tighter (8 teams w/ 2 groups).
        std::size_t target = (phase.find("CHAMPIONS") != std::string::npos) ? 16 : 8;
        for (std::size_t i = 0; globals.size() < target && i < rem.size(); ++i) globals.push_back(rem[i]);
        active_tournaments.push_back(std::make_shared<Tournament>(
            phase, globals, TournamentFormat::GroupThenPlayoffs));
    }
}

std::vector<RecordedMatchPtr> GameManager::sim_series_returning_all_matches(
        TeamPtr a, TeamPtr b, int best_of,
        const std::string& event, bool is_league_play) {
    if (!a || !b) return {};
    // Defensive lookup: solo_qs[region] would default-construct a null
    // shared_ptr if a team's region is missing from the map (shouldn't
    // happen with a well-formed world but a malformed save or partially-
    // initialized state could trigger it). Skip the series cleanly.
    auto sit1 = solo_qs.find(a->region);
    auto sit2 = solo_qs.find(b->region);
    if (sit1 == solo_qs.end() || !sit1->second
        || sit2 == solo_qs.end() || !sit2->second) {
        return {};
    }
    auto fa1 = sit1->second->global_ladder();
    auto fa2 = sit2->second->global_ladder();
    a->auto_fill_roster(fa1, year);
    b->auto_fill_roster(fa2, year);

    auto series = std::make_shared<Series>(a, b, best_of, event);
    std::vector<RecordedMatchPtr> recordings;
    while (!series->is_over()) {
        const auto& M = maps()[static_cast<std::size_t>(
            rng().irange(0, static_cast<int>(maps().size()) - 1))];
        // Pass the (possibly empty) per-map history so the team picker
        // can bias agent selection away from comps that lost earlier
        // maps. This is the wiring point that makes map 2 / map 3 of a
        // BO3/BO5 actually look different from map 1.
        Match m(a, b, M, false, event, /*friendly=*/false, &series->map_history());
        m.play();
        // Series::add_match_data creates the canonical RecordedMatchPtr
        // and pins it to player histories. Reuse that handle here instead
        // of make_recorded_match() ing a second time — deep-copying the
        // round_history is the dominant memory cost (multiple MB per match
        // at scale) and we'd otherwise allocate two distinct copies.
        series->add_match_data(m);
        if (auto rec = series->last_recording()) {
            recordings.push_back(rec);
        }
    }
    series->finalize_stats(is_league_play);
    return recordings;
}

void GameManager::play_stage_round(std::vector<std::string>& log,
                                   RecordedMatchPtr& out_user_rec,
                                   std::vector<RecordedMatchPtr>& out_user_series,
                                   std::string& out_user_event,
                                   std::string& out_user_opp) {
    out_user_rec.reset();
    out_user_series.clear();

    char buf[256];
    std::snprintf(buf, sizeof(buf), "[Day %d] Stage round %d simulating across all regions",
                  day_in_year, current_week + 1);
    log.emplace_back(buf);

    for (auto& kv : leagues) {
        auto& lg = kv.second;
        if (current_week >= static_cast<int>(lg->weekly_matchups().size())) continue;
        std::string event_label = "VCT " + kv.first + " Stage " + std::to_string(phase_idx + 1);
        for (auto& mu : lg->weekly_matchups()[current_week]) {
            if (!mu.a || !mu.b) continue;
            bool user_in = (mu.a == user_team || mu.b == user_team);
            auto recs = sim_series_returning_all_matches(mu.a, mu.b, /*bo=*/3,
                                                          event_label,
                                                          /*league_play=*/true);
            if (user_in && !recs.empty()) {
                out_user_rec = recs.front();
                out_user_series = recs;
                out_user_event = event_label;
                out_user_opp = (mu.a == user_team) ? mu.b->name : mu.a->name;
                std::snprintf(buf, sizeof(buf),
                              "  >> Your match (deferred to live viewer): vs %s  (%zu maps)",
                              out_user_opp.c_str(), recs.size());
                log.emplace_back(buf);
            }

            // === Per-series narrative tracking ===
            // Bump head-to-head + update consecutive-series-wins for the
            // (7) rivalry and (3) hot-streak emitters. We don't have a Series
            // object exposed here (sim_series_returning_all_matches returns
            // recordings only), so we derive the winner from the recordings'
            // aggregate score across maps. Best-effort — if recs is empty or
            // ambiguous, skip the bump.
            if (!recs.empty()) {
                int a_maps_won = 0;
                int b_maps_won = 0;
                for (auto& rec : recs) {
                    if (!rec) continue;
                    auto t1 = rec->team1.lock();
                    auto t2 = rec->team2.lock();
                    if (!t1 || !t2) continue;
                    // blue/red maps to team1/team2 in this engine; align via the
                    // matchup's a/b by pointer identity.
                    if (rec->blue_score > rec->red_score) {
                        if (t1 == mu.a) ++a_maps_won; else if (t1 == mu.b) ++b_maps_won;
                    } else if (rec->red_score > rec->blue_score) {
                        if (t2 == mu.a) ++a_maps_won; else if (t2 == mu.b) ++b_maps_won;
                    }
                }
                Team* winner = nullptr;
                Team* loser  = nullptr;
                if (a_maps_won > b_maps_won) { winner = mu.a.get(); loser = mu.b.get(); }
                else if (b_maps_won > a_maps_won) { winner = mu.b.get(); loser = mu.a.get(); }
                if (winner && loser) {
                    // Regular-season (league stage) meeting: bump the
                    // regular split AND the legacy total so any external
                    // reader stuck on h2h_counts_ keeps observing the
                    // same totals.
                    auto k = std::make_pair(winner, loser);
                    h2h_counts_[k] += 1;
                    h2h_counts_regular_[k] += 1;
                    consecutive_series_wins_[winner] += 1;
                    consecutive_series_wins_[loser]   = 0;
                    emit_hot_streak_news(winner, loser);
                }
            }
        }
        for (auto& t : lg->teams()) if (t) t->budget += 10000;
    }
    current_week += 1;
}

void GameManager::play_tournament_round(std::vector<std::string>& log,
                                         RecordedMatchPtr& out_user_rec,
                                         std::vector<RecordedMatchPtr>& out_user_series,
                                         std::string& out_user_event,
                                         std::string& out_user_opp) {
    out_user_rec.reset();
    out_user_series.clear();
    auto raw = raw_solo_q_map();

    for (auto& tour : active_tournaments) {
        if (tour->finished()) continue;
        log.emplace_back("[" + tour->name() + "] Round " + std::to_string(tour->round_num()));

        auto series_logs = tour->play_round(raw, year);
        for (auto& s : series_logs) {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "  > %s %d-%d %s [%s]",
                          s.team1()->name.c_str(), s.t1_wins(), s.t2_wins(),
                          s.team2()->name.c_str(), s.winner() ? s.winner()->name.c_str() : "?");
            log.emplace_back(buf);

            // === Per-series narrative tracking (tournament path) ===
            // Mirrors the same bookkeeping as play_stage_round. We have a
            // real Series here so winner derivation is exact.
            Team* winner = s.winner() ? s.winner().get() : nullptr;
            Team* loser  = nullptr;
            if (winner) {
                loser = (winner == s.team1().get()) ? s.team2().get() : s.team1().get();
            }
            if (winner && loser) {
                // Tournament/playoff meeting: bump the playoff split AND
                // the legacy total. Series in tournament brackets always
                // count as playoffs even at the group stage — groups are
                // a tournament construct, distinct from regular season.
                auto k = std::make_pair(winner, loser);
                h2h_counts_[k] += 1;
                h2h_counts_playoff_[k] += 1;
                consecutive_series_wins_[winner] += 1;
                consecutive_series_wins_[loser]   = 0;
                emit_hot_streak_news(winner, loser);
                emit_massive_upset(*tour, winner, loser);
            }

            if (user_team && (s.team1() == user_team || s.team2() == user_team)) {
                // Pull the maps from the Series itself (Series::all_recordings)
                // — the previous approach of diffing player.pro_match_history
                // before/after silently lost maps once the user already had
                // 10+ entries (cap evicts as fast as you insert). Map 1
                // would survive but Map 2+ would vanish from the live viewer.
                out_user_series.assign(s.all_recordings().begin(),
                                       s.all_recordings().end());
                // Belt-and-suspenders: if all_recordings somehow came back
                // empty (shouldn't happen), fall back to history scan so
                // the viewer at least shows Map 1.
                if (out_user_series.empty() && !user_team->roster.empty()) {
                    auto& hist = user_team->roster.front()->pro_match_history;
                    if (!hist.empty() && hist.front()) {
                        out_user_series.push_back(hist.front());
                    }
                }
                if (!out_user_series.empty()) {
                    out_user_rec = out_user_series.front();
                }
                out_user_event = tour->name();
                out_user_opp = (s.team1() == user_team) ? s.team2()->name : s.team1()->name;
                char dbg[160];
                std::snprintf(dbg, sizeof(dbg),
                    "  >> Your match: vs %s -> %d-%d (%zu maps captured)",
                    out_user_opp.c_str(), s.t1_wins(), s.t2_wins(),
                    out_user_series.size());
                log.emplace_back(dbg);
            }
        }
        if (tour->finished()) {
            log.emplace_back("[!] Champion: " + tour->champion()->name);
            // Auto-news: identify the GF Series within this round's series_logs
            // (last series whose teams are champion/runner_up) so we can
            // derive the Finals MVP from its aggregate_stats. If for any
            // reason it isn't present we still push the champion + run-
            // summary news items via a null gf_series.
            const Series* gf_series = nullptr;
            auto champ = tour->champion();
            auto ru    = tour->runner_up();
            for (auto it = series_logs.rbegin(); it != series_logs.rend(); ++it) {
                const auto& s = *it;
                bool match = (s.team1() == champ && s.team2() == ru) ||
                             (s.team1() == ru    && s.team2() == champ);
                if (match) { gf_series = &s; break; }
            }
            push_tournament_news(*tour, year, gf_series);
            // Record the trophy on the champion's team trophy_case so the
            // dynasty / community-rankings emitters can read a coherent
            // history. Agent B is implementing Team::record_trophy + the
            // backing storage; we just have to call it once per finish.
            if (champ) {
                champ->record_trophy(year, tour->name());
            }
            // Record the GF outcome for the H2H finals log so the UI can
            // render an "all-time finals between these two teams" mini-
            // history. Cleared at year rollover.
            if (champ && ru) {
                H2HSeriesFinal f;
                f.winner = champ.get();
                f.loser  = ru.get();
                f.event_name = tour->name();
                f.year = year;
                f.day_in_year = day_in_year;
                h2h_finals_log_.push_back(f);
            }
        }
    }
    // NOTE: do NOT clear active_tournaments here even when every bracket is
    // Done. force_finish_stale_tournaments at phase end is the sole owner of
    // that clear. Auto-clearing here used to cause duplicate brackets to
    // spawn on the next matchday (REGIONALS pacing is 9 days but a bracket
    // typically finishes in ~6 play_round calls), which silently doubled
    // awards and stats per playoff cycle.
}

void GameManager::run_monthly_progression(std::vector<std::string>& log,
                                           std::vector<ProgressionReport>& out_reports) {
    out_reports.clear();
    int reports = 0;

    // Map each player to their team's coach (if any).
    std::unordered_map<Player*, const Coach*> coach_for;
    for (auto& kv : leagues) {
        for (auto& t : kv.second->teams()) {
            if (!t) continue;
            const Coach* c = t->head_coach.get();
            for (auto& p : t->roster) coach_for[p.get()] = c;
        }
    }

    for (auto& kv : solo_qs) {
        for (auto& p : kv.second->global_ladder()) {
            if (p->is_retired) continue;
            const Coach* c = nullptr;
            auto it = coach_for.find(p.get());
            if (it != coach_for.end()) c = it->second;
            ProgressionReport r = p->apply_monthly_progression(c, year, day_in_year);
            // Refresh agent pool occasionally — attribute growth can shift role fit.
            if (!r.changes.empty()) p->update_agent_pool();
            if (!r.changes.empty()) ++reports;
            // Only store reports for "interesting" players: user roster + a
            // sample. Avoids the report list ballooning to 1500+ entries.
            bool is_user = false;
            if (user_team) {
                for (auto& up : user_team->roster) if (up.get() == p.get()) { is_user = true; break; }
            }
            if (is_user || (!r.changes.empty() && rng().chance(0.02))) {
                out_reports.push_back(r);
            }
        }
    }
    char buf[128];
    std::snprintf(buf, sizeof(buf), "[Day %d] Monthly progression: %d players grew/declined.",
                  day_in_year, reports);
    log.emplace_back(buf);
}

DayResult GameManager::advance_day(std::vector<std::string>& log) {
    DayResult r;
    r.year = year;
    r.day_in_year = day_in_year;
    r.day_total = day_total;
    r.phase = current_phase();

    // Agent mastery: Match.cpp's record_agent_performance reads the world
    // year via current_world_year(). Stamp it once per day so every pro
    // match played today is timestamped correctly for decay logic.
    set_current_world_year(year);

    if (current_phase() == "AWARDS") {
        // Year-end: awards calculated, retirements processed, FA pool
        // refreshed, year++. After this, phase advances to OFFSEASON
        // (NOT directly to STAGE 1) so the user has time to manage their
        // roster, sign FAs, re-sign players, etc. before the new season.
        run_end_of_year(log);
        // run_end_of_year now leaves phase_idx alone — we manually advance
        // to OFFSEASON here.
        phase_idx += 1;
        day_in_year = 0;
        day_in_phase = 0;
        days_since_progression = 0;
        r.year_rolled = true;
        r.was_matchday = false;
        r.label = "Offseason begins";
        ++day_total;
        return r;
    }
    if (current_phase() == "OFFSEASON") {
        // No matches play. The user is free to interact with FA market,
        // re-sign their own players, manage trades. After total_rounds
        // (14) days, phase wraps to STAGE 1 of the new year.
        auto [dpr, total_rounds] = current_phase_pacing();
        day_in_phase += 1;
        day_in_year += 1;
        day_total += 1;
        days_since_progression += 1;
        r.was_matchday = false;
        r.label = "Offseason — manage your roster";
        // FA price decay (B8): each offseason day, unsigned FAs trim
        // their ask by ~kFADecayRate, flooring at kFADecayFloorFraction
        // of the value they entered offseason at. Runs BEFORE solo Q tick
        // so any solo Q event that signs a player doesn't get a stale
        // pre-decay price.
        run_offseason_fa_decay();
        // Solo Q ticks during offseason too (lighter)
        for (auto& kv : solo_qs) kv.second->simulate_ranked_day(1);
        if (day_in_phase >= total_rounds) {
            // === User-team AI fallback (Bug 3) ===
            // If the user reached the end of OFFSEASON without filling
            // their starting 5, run the SAME ai_manage_roster pipeline
            // every AI team gets at year-end. No special-case simplified
            // version — reuse the existing AI GM so the user team enters
            // STAGE 1 at full strength even if the player ignored the
            // offseason. Runs BEFORE phase_idx wraps so the user can
            // still see the resulting roster in their OFFSEASON screen
            // tick (the next tick fires STAGE 1).
            if (user_team && user_team->roster.size() < 5) {
                auto sit = solo_qs.find(user_team->region);
                if (sit != solo_qs.end() && sit->second) {
                    auto fa_pool = sit->second->global_ladder();
                    // Deep multi-stage GM pass: per-starter assessment,
                    // targeted upgrades, cascade-fill, bench depth, full
                    // per-move logging. Same depth all AI teams now use.
                    user_team->ai_full_offseason_pass(fa_pool, year, log);
                    log.emplace_back(
                        "[OFFSEASON] User team auto-filled by AI fallback "
                        "(roster was below 5 starters at offseason end).");
                    NewsItem n;
                    n.year = year;
                    n.day_in_year = day_in_year;
                    n.category = "Roster Move";
                    n.headline = user_team->name +
                        " front office completes offseason — roster auto-filled";
                    n.body = "With the user inactive during offseason, the AI GM "
                             "filled remaining slots from the free agent pool to "
                             "enter the season at full strength.";
                    n.team_name = user_team->name;
                    push_news(n);
                }
            }
            // OFFSEASON complete — wrap to STAGE 1 of the new year.
            phase_idx = 0;
            day_in_phase = 0;
            current_week = 0;
            day_in_year = 0;
            r.label = "New season starting!";
        }
        return r;
    }

    auto [days_per_round, total_rounds] = current_phase_pacing();
    bool is_matchday = (day_in_phase % days_per_round == 0);
    // Cap total matchdays so the phase ends cleanly.
    int matchdays_done = day_in_phase / days_per_round;
    if (matchdays_done >= total_rounds) is_matchday = false;

    if (is_matchday) {
        const std::string& phase = current_phase();
        char lbuf[96];
        if (phase.find("STAGE") != std::string::npos) {
            std::snprintf(lbuf, sizeof(lbuf), "%s Round %d/%d",
                          phase.c_str(), current_week + 1, total_rounds);
            r.label = lbuf;
            std::string event, opp;
            RecordedMatchPtr user_rec;
            std::vector<RecordedMatchPtr> user_series;
            play_stage_round(log, user_rec, user_series, event, opp);
            r.user_match_recording = user_rec;
            r.user_match_series = user_series;
            r.user_match_event = event;
            r.user_match_opp_name = opp;
            r.was_matchday = true;
            r.matches_simulated = 18;
        } else {
            std::snprintf(lbuf, sizeof(lbuf), "%s — Round %d", phase.c_str(),
                          (matchdays_done + 1));
            r.label = lbuf;
            generate_tournaments_if_needed();
            std::string event, opp;
            RecordedMatchPtr user_rec;
            std::vector<RecordedMatchPtr> user_series;
            play_tournament_round(log, user_rec, user_series, event, opp);
            r.user_match_recording = user_rec;
            r.user_match_series = user_series;
            r.user_match_event = event;
            r.user_match_opp_name = opp;
            r.was_matchday = true;
            r.matches_simulated = 4;
        }
    } else {
        r.label = current_phase() + " — off-day";
    }

    // Solo Q ticks every day (more on off-days, when pros have downtime too).
    // Ramped up so each region's ladder sees enough match volume that a
    // realistic ~15 players climb into Radiant range over a season.
    int ranked_loops = is_matchday ? 3 : 6;
    for (auto& kv : solo_qs) kv.second->simulate_ranked_day(ranked_loops);

    day_in_phase += 1;
    matchdays_done = day_in_phase / days_per_round;
    if (matchdays_done >= total_rounds && day_in_phase % days_per_round == 0) {
        // Phase done. CRITICAL: force-finish any leftover bracket so the
        // next phase doesn't accidentally keep playing it (which would
        // silently double-count stats — root cause of the "thousands of
        // matches" bug).
        std::string ending_phase = current_phase();
        if (ending_phase.find("REGIONALS") != std::string::npos
            || ending_phase.find("MASTERS")   != std::string::npos
            || ending_phase.find("CHAMPIONS") != std::string::npos) {
            force_finish_stale_tournaments(log);
        }
        phase_idx += 1;
        day_in_phase = 0;
        if (current_phase().find("STAGE") != std::string::npos) {
            current_week = 0;
            // Reset per-stage W/L so REGIONALS qualification at the end
            // of THIS new stage reflects only this stage's matches (and
            // not the cumulative season — the season-wide tally lives
            // in t->wins / t->losses).
            for (auto& kv : leagues) {
                for (auto& t : kv.second->teams()) {
                    if (!t) continue;
                    t->phase_wins = 0;
                    t->phase_losses = 0;
                }
            }
        }
        if (current_phase().find("REGIONALS") != std::string::npos
            || current_phase().find("MASTERS") != std::string::npos
            || current_phase().find("CHAMPIONS") != std::string::npos) {
            generate_tournaments_if_needed();
        }
    }

    day_in_year += 1;
    day_total += 1;
    days_since_progression += 1;

    if (days_since_progression >= 30) {
        run_monthly_progression(log, r.progression_reports);
        run_mid_season_replacements(log);
        update_mvp_race();
        recent_progression_reports = r.progression_reports;
        days_since_progression = 0;
        r.progression_ran = true;
    }

    // === Weekly power/community ranking tick ===
    // Re-run rankings every 7 in-year days. Skipped during AWARDS/OFFSEASON
    // because those paths return early above; this only fires on real
    // playing days. The -10 sentinel guarantees the first tick of a new
    // year fires immediately (resets are wired in run_end_of_year).
    if (day_in_year - last_power_tick_day_ >= 7 && day_in_year != last_power_tick_day_) {
        compute_power_rankings();
        last_power_tick_day_ = day_in_year;
    }
    if (day_in_year - last_community_tick_day_ >= 7 && day_in_year != last_community_tick_day_) {
        compute_community_rankings();
        last_community_tick_day_ = day_in_year;
    }

    // === Per-day news emitters (1, 2, 6, 7) ===
    // (1) breakout — STAGE 1, past day 50
    if (current_phase() == "STAGE 1" && day_in_year > 50) {
        emit_breakout_news();
    }
    // (2) slump — cheap, weekly cadence piggy-backed off the power tick
    if (day_in_year - last_power_tick_day_ == 0
        && day_in_year != 0) {
        // we just ticked rankings this iteration; run weekly slump check too
        emit_slump_news();
    }
    // (6) transfer rumor — STAGE 1 (early) + STAGE 3 (late)
    if (current_phase() == "STAGE 1" || current_phase() == "STAGE 3") {
        emit_transfer_rumor();
    }
    // (7) rivalry match night — only on matchdays
    if (r.was_matchday) {
        emit_rivalry_match_night();
    }

    return r;
}

void GameManager::run_mid_season_replacements(std::vector<std::string>& log) {
    // Mid-season benching is now a HIGH bar — most underperformers ride it
    // out to offseason rather than getting cut. New rules:
    //   * 0.90 is the rating ceiling for "considered for cut" (was 0.95)
    //   * 0.89-0.90 players need REPEAT poor form, weak replacement, AND
    //     a strict coach to actually get cut
    //   * Below 0.85 = clear bench candidate
    //   * Considers: replacement quality, role scarcity, contract status,
    //     team success, recent vs long-term form
    int total_replacements = 0;
    for (auto& kv : leagues) {
        for (auto& t : kv.second->teams()) {
            if (!t || !t->head_coach) continue;
            const Coach& coach = *t->head_coach;
            double aggression = personality_replacement_aggressiveness(coach.personality);
            aggression *= clamp_v(1.20 - (coach.leadership / 200.0), 0.5, 1.2);

            // Find worst-performing starter under the 0.90 ceiling.
            PlayerPtr worst;
            double worst_rating = 0.90;
            for (std::size_t i = 0; i < std::min<std::size_t>(5, t->roster.size()); ++i) {
                auto& p = t->roster[i];
                if (!p) continue;
                if (p->season_matches < 6) continue;          // need a real sample
                double r = p->avg_match_rating();
                if (r >= 0.90) continue;
                // Young prospects (<=21 + high potential) almost always get
                // kept — they need playing time to develop.
                bool is_prospect = (p->age <= 21 && p->potential >= 75);
                if (is_prospect && rng().chance(0.85)) continue;
                // Marginal underperformers (0.89-0.90) only considered if
                // the coach is genuinely strict AND form is recent-bad.
                if (r >= 0.89 && aggression < 0.55) continue;
                if (r < worst_rating) { worst_rating = r; worst = p; }
            }
            if (!worst) continue;

            // Team success modifier — winning teams are MUCH more patient
            // even with a sub-0.90 player. Losing teams cut faster.
            double net_w = static_cast<double>(t->wins - t->losses);
            double team_success_mult = clamp_v(1.0 - net_w * 0.04, 0.55, 1.40);
            double cut_chance = aggression * team_success_mult;

            // Players in the 0.89-0.90 band only get cut on a much harsher
            // roll — most of them survive to offseason / contract end.
            if (worst_rating >= 0.89) cut_chance *= 0.30;
            else if (worst_rating >= 0.85) cut_chance *= 0.65;

            // Contract status — players with 1 year left don't trigger
            // mid-season cuts as often (let the contract simply expire).
            int yrs_left = worst->years_left(year);
            if (yrs_left <= 1) cut_chance *= 0.55;

            // Organizational memory — `stability_culture` shapes cut
            // willingness. Patient orgs rarely cut mid-season; impatient
            // ones swing the axe early.
            if      (t->memory.stability_culture >=  40) cut_chance *= 0.70;
            else if (t->memory.stability_culture <= -40) cut_chance *= 1.30;

            // Championship window — Closing teams are desperate to fix
            // their roster before the window slams shut; Closed teams
            // churn through rebuild candidates; Opening teams stay patient
            // and let young talent develop; Open contenders sit neutral.
            switch (t->window) {
                case TeamWindow::Closing: cut_chance *= 1.40; break;
                case TeamWindow::Closed:  cut_chance *= 1.20; break;
                case TeamWindow::Opening: cut_chance *= 0.70; break;
                case TeamWindow::Open:    /* no modifier */   break;
            }

            if (!rng().chance(clamp_v(cut_chance, 0.02, 0.85))) continue;

            // Find a replacement from regional FA pool. We're now stricter:
            // the replacement must be a clear upgrade (not just "good
            // enough"), or the cut doesn't happen. Role scarcity also
            // matters — if the FA pool has zero same-role candidates
            // within OVR range, we'd rather keep our underperformer.
            PlayerPtr best_replacement;
            double best_score = -1.0;
            int role_pool_count = 0;
            auto sit = solo_qs.find(t->region);
            if (sit == solo_qs.end()) continue;
            // Appetite-gate the foreign-FA portion of the replacement
            // scan. If the team has low import appetite (<0.22), only
            // domestic candidates are considered. Wealthy/contender
            // teams keep the global option open. STRICT v2: floor
            // 0.15 -> 0.22 to match the tighter appetite curve.
            const auto& pool_snapshot = sit->second->global_ladder();
            double mid_appetite = team_import_appetite(*t,
                                                       worst->primary_role,
                                                       &pool_snapshot);
            bool domestic_only = (mid_appetite < 0.22);
            for (const auto& fa : pool_snapshot) {
                if (!fa || fa->is_retired) continue;
                if (fa->team_name != "Free Agent") continue;
                if (fa->primary_role != worst->primary_role) continue;
                if (domestic_only && fa->region != t->region) continue;
                // Hard cap: per-team import limit (2). Don't even consider
                // a foreign FA if the roster is already maxed.
                if (fa->region != t->region) {
                    int imp = 0;
                    for (auto& x : t->roster) {
                        if (x && x->region != t->region) ++imp;
                    }
                    if (imp >= config().max_imports) continue;
                    if (fa->ovr() < 60.0) continue;  // import OVR floor
                }
                ++role_pool_count;
                // Replacement must MEET OR BEAT the cut player on OVR (was
                // "within 6 below"). Half-measure swaps tend to make team
                // worse and frustrate the user.
                if (fa->ovr() < worst->ovr()) continue;
                int demand = fa->amount_with_mood(fa->contract.amount_k, t->name);
                if (static_cast<long long>(demand) * 1000LL > t->budget / 4) continue;
                double score = fa->ovr() - (demand / 50.0);
                if (score > best_score) { best_score = score; best_replacement = fa; }
            }
            if (!best_replacement) continue;
            // Role scarcity gate — if the FA pool is thin on this role,
            // wait until offseason to make a move.
            if (role_pool_count < 4) continue;

            // Execute the swap. Released player goes back to FA pool.
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                "[%s] %s released %s (rating %.2f) and signed %s.",
                t->name.c_str(), coach_personality_name(coach.personality),
                worst->name.c_str(), worst_rating, best_replacement->name.c_str());
            log.emplace_back(buf);

            // Push a roster-move news item for the UI feed.
            NewsItem news;
            news.year = year; news.day_in_year = day_in_year;
            news.category = "Roster Move";
            char hbuf[200];
            std::snprintf(hbuf, sizeof(hbuf),
                "%s benches %s after %.2f rating, signs %s",
                t->name.c_str(), worst->name.c_str(), worst_rating,
                best_replacement->name.c_str());
            news.headline = hbuf;
            char bbuf[300];
            std::snprintf(bbuf, sizeof(bbuf),
                "%s head coach %s (%s archetype) made the call after a poor %s window. "
                "%s steps in from the free agent pool.",
                t->name.c_str(), coach.name.c_str(),
                coach_personality_name(coach.personality),
                worst->primary_role == Role::Duelist    ? "duelist"
              : worst->primary_role == Role::Initiator  ? "initiator"
              : worst->primary_role == Role::Controller ? "controller"
                                                         : "sentinel",
                best_replacement->name.c_str());
            news.body = bbuf;
            news.team_name = t->name;
            news.player_name = worst->name;
            push_news(news);

            t->release_player(worst);
            int demand = best_replacement->amount_with_mood(best_replacement->contract.amount_k, t->name);
            t->budget -= static_cast<long long>(demand) * 1000LL;
            best_replacement->contract.amount_k = demand;
            t->sign_player(best_replacement,
                           t->decide_contract_years(*best_replacement, year),
                           year);
            // Move new signing into team's regional solo Q ladder.
            sync_player_ranked_region(best_replacement, t->region);
            ++total_replacements;
        }
    }
    if (total_replacements > 0) {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
            "[Day %d] Mid-season: %d roster replacement(s) by strict coaches.",
            day_in_year, total_replacements);
        log.emplace_back(buf);
    }
}

DayResult GameManager::advance_to_next_user_match(std::vector<std::string>& log) {
    DayResult last;
    int safety = kDaysPerYear * 2;
    while (safety-- > 0) {
        DayResult r = advance_day(log);
        last = r;
        if (r.user_match_recording) break;
        if (r.year_rolled) break;
    }
    return last;
}

DayResult GameManager::advance_to_next_phase(std::vector<std::string>& log) {
    // Skip buttons should NOT pop the live viewer for user matches that
    // happen mid-skip — the user explicitly asked to fast-forward. We
    // discard intermediate recordings (they're still pinned to player
    // history for later review) and clear the final result's recording
    // so handle_day_result() doesn't open the modal on arrival.
    DayResult last;
    int starting_phase = phase_idx;
    int safety = kDaysPerYear * 2;
    while (safety-- > 0) {
        DayResult r = advance_day(log);
        last = r;
        if (phase_idx != starting_phase) break;
        if (r.year_rolled) break;
    }
    last.user_match_recording.reset();
    last.user_match_series.clear();
    return last;
}

void GameManager::simulate_solo_q_day() {
    for (auto& kv : solo_qs) kv.second->simulate_ranked_day(1);
}

void GameManager::simulate_full_season(std::vector<std::string>& log) {
    // Sim until CHAMPIONS finishes — i.e. the calendar advances to the
    // AWARDS phase. STOP there without consuming the AWARDS day, leaving
    // the user in the offseason. They click Continue once to fire the
    // year-end (HoF inductions + awards recap + new season setup).
    int safety = kDaysPerYear * 2;
    int starting_year = year;
    while (year == starting_year && safety-- > 0) {
        const std::string& ph = current_phase();
        if (ph == "AWARDS") {
            log.emplace_back("[Season] Champions concluded. Click Continue to "
                             "view awards recap and enter offseason.");
            return;
        }
        if (ph == "OFFSEASON") {
            // Offseason is a manual phase — don't blast through it. The user
            // has roster moves to make.
            log.emplace_back("[Offseason] Manage your roster manually. "
                             "Click Continue to advance day-by-day through offseason.");
            return;
        }
        DayResult r = advance_day(log);
        if (r.year_rolled) break;
    }
}

DayResult GameManager::advance_to_playoffs(std::vector<std::string>& log) {
    // Skip-to-playoffs semantics:
    //   1. If already in a playoff phase (REGIONALS / MASTERS / CHAMPIONS)
    //      this is a no-op — we don't advance through the current playoff
    //      to find the NEXT one. User wanted to be at playoffs; they're
    //      already there.
    //   2. If we're in a STAGE / OFFSEASON / AWARDS phase, advance day-
    //      by-day until the calendar enters a playoff phase, then STOP
    //      *before* any matchday of that phase fires.
    //   3. Any user matches that fire during the skip are auto-played
    //      silently — we explicitly clear user_match_recording before
    //      returning so handle_day_result doesn't pop the live viewer.
    auto is_playoffs_phase = [](const std::string& p) {
        return p.find("REGIONALS") != std::string::npos
            || p.find("MASTERS")   != std::string::npos
            || p.find("CHAMPIONS") != std::string::npos;
    };

    DayResult last;
    last.year = year; last.day_in_year = day_in_year;
    last.phase = current_phase();

    // Early no-op: already in playoffs.
    if (is_playoffs_phase(current_phase())) {
        last.label = "Already at playoffs (" + current_phase() + ")";
        log.emplace_back("[Skip] " + last.label);
        // Defensive: ensure no leftover viewer-trigger recording.
        last.user_match_recording.reset();
        last.user_match_series.clear();
        return last;
    }

    int safety = kDaysPerYear * 2;
    while (safety-- > 0) {
        // Re-check at each loop iteration BEFORE advancing — we want to
        // stop the moment the calendar transitions into a playoff phase,
        // never advance one matchday into it.
        if (is_playoffs_phase(current_phase()) && day_in_phase == 0) {
            last.label = "Playoffs ready: " + current_phase();
            last.phase = current_phase();
            log.emplace_back("[Skip] Stopped at start of " + current_phase() +
                             ". Click Continue to begin matchday.");
            // CRITICAL: clear any user_match recording from intermediate
            // skipped days so the live viewer doesn't auto-pop after skip.
            last.user_match_recording.reset();
            last.user_match_series.clear();
            return last;
        }
        if (current_phase() == "AWARDS") {
            last.label = "Offseason reached.";
            last.phase = current_phase();
            log.emplace_back("[Skip] No playoffs ahead — reached offseason.");
            last.user_match_recording.reset();
            last.user_match_series.clear();
            return last;
        }
        DayResult r = advance_day(log);
        last = r;
        if (r.year_rolled) break;
    }
    // Final safety strip in case the safety counter expired or year rolled.
    last.user_match_recording.reset();
    last.user_match_series.clear();
    return last;
}

int GameManager::next_user_match_day_in_year() const {
    if (!user_team) return -1;
    const std::string& phase = current_phase();
    auto [dpr, total] = current_phase_pacing();

    if (phase.find("STAGE") != std::string::npos) {
        // Find the next round that contains the user team.
        auto it = leagues.find(user_team->region);
        if (it == leagues.end()) return -1;
        auto& lg = it->second;
        for (int wk = current_week; wk < static_cast<int>(lg->weekly_matchups().size()); ++wk) {
            for (auto& mu : lg->weekly_matchups()[wk]) {
                if (mu.a == user_team || mu.b == user_team) {
                    int wk_offset = wk - current_week;
                    int day_offset = wk_offset * dpr - (day_in_phase % dpr);
                    return day_in_year + day_offset;
                }
            }
        }
        return -1;
    }
    // For tournaments, predicting future days requires the bracket to be
    // resolved first; just say "today or soon".
    if (!active_tournaments.empty()) {
        for (auto& tour : active_tournaments) {
            if (tour->user_team_in_round(user_team)) {
                return day_in_year;
            }
        }
    }
    return -1;
}

std::vector<UpcomingFixture> GameManager::upcoming_fixtures(int max_days_ahead) const {
    std::vector<UpcomingFixture> out;
    auto [dpr, total] = current_phase_pacing();
    if (current_phase().find("STAGE") == std::string::npos) return out;
    int day_into_round = day_in_phase % dpr;
    int days_until_next_round = day_into_round == 0 ? 0 : (dpr - day_into_round);

    for (auto& kv : leagues) {
        auto& lg = kv.second;
        for (int wk = current_week; wk < static_cast<int>(lg->weekly_matchups().size()); ++wk) {
            int wk_offset = wk - current_week;
            int day_off = days_until_next_round + wk_offset * dpr;
            if (day_off > max_days_ahead) break;
            for (auto& mu : lg->weekly_matchups()[wk]) {
                if (!mu.a || !mu.b) continue;
                UpcomingFixture f;
                f.day_in_year = day_in_year + day_off;
                f.region = kv.first;
                f.a = mu.a;
                f.b = mu.b;
                char b[64];
                std::snprintf(b, sizeof(b), "VCT %s Stage %d Rd %d",
                              kv.first.c_str(), phase_idx + 1, wk + 1);
                f.label = b;
                out.push_back(f);
            }
        }
    }
    std::sort(out.begin(), out.end(),
              [](const UpcomingFixture& a, const UpcomingFixture& b) {
                  return a.day_in_year < b.day_in_year;
              });
    return out;
}

void GameManager::run_end_of_year(std::vector<std::string>& log) {
    log.emplace_back("== END OF YEAR " + std::to_string(year) + " ==");

    // === Year-end news emitters (BEFORE counter reset) ===
    // The save_history_and_progress() pass below resets season_* counters,
    // so anything emitter (4)/(5)/(9)/(10) needs from this season must run
    // FIRST. (1)/(2)/(3)/(6)/(7)/(8) fire inline during the season and are
    // intentionally skipped at year-end.
    //
    // CRITICAL: dedup clears (news_pushed_events_, news_emitted_keys_) MUST
    // run AFTER these emitters — per spec §4.11 step 15. Reordered from the
    // previous "clear-at-top" structure (which let milestone-news re-fire
    // every subsequent year because the year-end emitters ran with a freshly
    // cleared dedup set). Milestone keys now live in news_emitted_career_keys_
    // (never cleared except by reset_world), so this reorder only affects
    // retirement-countdown / historic / dynasty — all of which embed the
    // year in their dedup keys anyway and thus self-dedup across years
    // even without the year-end clear.
    emit_milestone_news();
    emit_retirement_countdown();
    emit_historic_performance();
    emit_dynasty_watch();

    // === Dedup clears — spec §4.11 step 15: LAST, after the year-end
    // emitter pass has run. ===
    news_pushed_events_.clear();
    news_emitted_keys_.clear();

    // Compute awards FIRST, before save_history_and_progress resets the
    // season counters. Awards are computed against season_rating_total etc.
    compute_season_awards(log);

    for (auto& kv : leagues) {
        for (auto& t : kv.second->teams()) {
            if (!t) continue;
            // Sponsorship + base revenue paid at year-start. Sponsorship
            // scales with prestige; high-prestige orgs net more income.
            long long revenue = static_cast<long long>(t->sponsorship_k) * 1000LL;
            t->budget += revenue;
            t->pay_payroll(year);
            if (t->head_coach) {
                double mult = t->head_coach->dev_chance_mult();
                for (auto& p : t->roster) {
                    if (rng().chance((mult - 1.0) * 0.8)) {
                        Attr a = static_cast<Attr>(rng().irange(0, static_cast<int>(kAttrCount) - 1));
                        p->apply_attribute_delta(a, 1);
                    }
                }
                t->head_coach->experience = std::min(99, t->head_coach->experience + 1);
                t->head_coach->age += 1;
                if (t->head_coach->age >= 60 && rng().chance(0.25)) {
                    t->head_coach->is_retired = true;
                    t->head_coach.reset();
                }
            }
        }
    }

    // Stat sanity sweep: log + clamp any pathological totals so the
    // leaderboards stay believable. Anything beyond these thresholds is
    // almost certainly from a duplicate-bump bug.
    int sane_max_matches = 5000;
    int sane_max_kills   = 250000;
    for (auto& kv : solo_qs) {
        for (auto& p : kv.second->global_ladder()) {
            if (p->career_matches > sane_max_matches) {
                char buf[160];
                std::snprintf(buf, sizeof(buf),
                    "[STAT] CAPPED %s career_matches %d -> %d (overflow guard)",
                    p->name.c_str(), p->career_matches, sane_max_matches);
                log.emplace_back(buf);
                p->career_matches = sane_max_matches;
            }
            if (p->career_kills > sane_max_kills) {
                p->career_kills = sane_max_kills;
            }
        }
    }

    int retired_this_year = 0;
    int contracts_expired = 0;
    for (auto& kv : solo_qs) {
        for (auto& p : kv.second->global_ladder()) {
            p->save_history_and_progress(year);
            // NB: don't set team_name here — handled by the explicit roster
            // sweep below so retired/expired players actually leave the
            // team's roster vector (not just have their team_name flipped).
            if (p->team_name == "Free Agent") {
                p->decay_mood(0.05);
                p->decay_demands();
                Contract c = p->gen_contract(year, false, false);
                if (c.amount_k > p->contract.amount_k) p->contract.amount_k = c.amount_k;
            }

            // Probability-scaled retirement. Age is the dominant factor but
            // we also consider career success (no pro matches = bigger drop-out
            // chance), inactivity (years_unsigned), and motivation (work_ethic).
            // The hard 4-year unsigned rule already fires inside Player::
            // save_history_and_progress; this is the softer earlier exit.
            if (!p->is_retired) {
                double age_p = 0.0;
                if      (p->age >= 34) age_p = 0.55;
                else if (p->age >= 32) age_p = 0.30;
                else if (p->age >= 30) age_p = 0.12;
                else if (p->age >= 28) age_p = 0.04;

                // Long-prime veterans get a discount on their age curve —
                // career_matches >= 60 with avg rating >= 1.10 means the
                // player is still producing and likely to stick around.
                bool successful = p->career_matches >= 60 && p->avg_match_rating() >= 1.10;
                if (successful) age_p *= 0.55;

                // Inactivity bumps it up — sitting unsigned is a sign the
                // scene is moving on.
                if (p->years_unsigned >= 2) age_p += 0.15;
                if (p->years_unsigned >= 3) age_p += 0.20;

                // Low motivation / work ethic drops out faster.
                if (p->work_ethic < 35) age_p += 0.08;

                if (age_p > 0.0 && rng().chance(age_p)) {
                    p->is_retired = true;
                    p->team_name = "Retired";
                }
            }

            if (p->is_retired) ++retired_this_year;
        }
    }

    // === Top-20 solo Q rank tracking (HoF criterion) ===
    // After all season progression, mark anyone currently in their region's
    // top-20 ladder as having achieved the milestone permanently.
    for (auto& kv : solo_qs) {
        auto leaderboard = kv.second->get_leaderboard();
        for (std::size_t i = 0; i < std::min<std::size_t>(20, leaderboard.size()); ++i) {
            if (leaderboard[i]) leaderboard[i]->ever_top20_solo = true;
        }
    }

    // === Hall of Fame qualification — 4-of-9 strict criteria ===
    // Player must have played 3+ seasons AND satisfy at least 4 of the
    // following 9. Inducted only after retirement so legacies are
    // assessed against final career numbers.
    for (auto& kv : solo_qs) {
        for (auto& p : kv.second->global_ladder()) {
            if (!p || !p->is_retired) continue;
            if (p->career_seasons_played < 3) continue;
            if (std::find(hall_of_fame.begin(), hall_of_fame.end(), p) != hall_of_fame.end()) continue;

            int criteria = 0;
            // 1. Won an international event (Masters or Champions)
            for (auto& a : p->awards) {
                if (a.find("[M]") != std::string::npos ||
                    a.find("[W]") != std::string::npos) { ++criteria; break; }
            }
            // 2. Won a role award (Duelist/Initiator/Controller/Sentinel of the Year)
            for (auto& a : p->awards) {
                if (a.find("of the Year") != std::string::npos &&
                    a.find("MVP") == std::string::npos &&
                    a.find("IGL") == std::string::npos) { ++criteria; break; }
            }
            // 3. Won an MVP
            for (auto& a : p->awards) {
                if (a.find("MVP") != std::string::npos) { ++criteria; break; }
            }
            // 4. Maintained 1.2+ rating across an entire season
            for (auto& h : p->history) {
                if (h.rating >= 1.20 && p->career_seasons_played >= 3) { ++criteria; break; }
            }
            // 5. Reached top 20 in solo queue rankings
            if (p->ever_top20_solo) ++criteria;
            // 6. Earned over $1,000,000 career
            int total_earnings = 0;
            for (auto& s : p->salary_log) total_earnings += s.second;
            if (total_earnings >= 1000) ++criteria;  // 1000 K = $1M
            // 7. Dropped 30 kills in a pro match
            if (p->career_max_match_kills >= 30) ++criteria;
            // 8. Achieved 2.0 KD in a pro match
            if (p->career_max_match_kd_x100 >= 200) ++criteria;
            // 9. Clutched the final round of a Grand Final
            if (p->career_grand_final_clutches >= 1) ++criteria;
            // 10. (NEW — IGLs only) Career IGL strategic impact > 50.
            //     A HoF lane specifically for IGLs who never frag-carry but
            //     win matches by calling. We add this as an *additional*
            //     criterion (not a replacement) so the 4-of-N gate still
            //     fires for everyone else exactly as before.
            if (p->is_igl && p->igl_impact_total > 50.0) ++criteria;

            if (criteria >= 4) {
                hall_of_fame.push_back(p);
                NewsItem n;
                n.year = year; n.day_in_year = day_in_year;
                n.category = "Awards";
                n.headline = p->name + " inducted into the Hall of Fame";
                char body[200];
                std::snprintf(body, sizeof(body),
                    "After %d seasons, %s meets %d of the 9 HoF criteria.",
                    p->career_seasons_played, p->name.c_str(), criteria);
                n.body = body;
                n.player_name = p->name;
                push_news(n);
            }
        }
    }

    // === Organizational memory year-end update ===
    // Update each team's hidden rolling metrics (rookie_success,
    // import_success, veteran_success, financial_discipline,
    // stability_culture, star_dependency). MUST run AFTER awards have
    // been computed (so [T]/[M]/[W] pin-strings are visible on the
    // roster) but BEFORE the roster cleanup pass below (so players who
    // were on the roster THIS season are still attached and inspectable).
    // Fresh worlds start with all metrics at 0 so year-1 simulation
    // matches pre-memory behaviour exactly.
    for (auto& kv : leagues) {
        for (auto& t : kv.second->teams()) {
            if (!t) continue;
            t->update_org_memory(year);
        }
    }

    // === Championship window classification ===
    // Recompute each team's window AFTER update_org_memory (which decays
    // chemistry + prunes off-roster edges) and BEFORE
    // commit_strategy_with_inertia (so strategy reclassification can see
    // the freshly-classified window via the strategy logic if it ever
    // wants to). Pure read-only on the team's starting 5 — no side
    // effects beyond writing `t->window`.
    for (auto& kv : leagues) {
        for (auto& t : kv.second->teams()) {
            if (!t) continue;
            t->window = compute_team_window(*t, year);
        }
    }

    // === Deep re-sign + market-competition pass (B7 / B8) ====================
    // Replaces the old shallow "every expiring contract auto-extends" loop.
    //
    // For each AI team's expiring player (years_left <= 1 — last season just
    // played OR will expire next):
    //   1. Player computes their ask via Player::propose_resign_offer.
    //   2. Team scores team_eval_score (age / role / chemistry / memory /
    //      window / financials / form / strategy). team_eval < 0 -> walk.
    //   3. Team's counter-offer: stars (ovr >= 80) pay full; mid (65-79)
    //      counter at 90%; bench/decline floor at min_acceptable_k.
    //   4. Player::accepts_resign_offer gates the deal.
    //   5. Accept -> Team::resign_player + news. Walk -> release + open
    //      bidding round to other same-region teams (only for stars >= 75).
    //
    // User team is excluded — user decides their own re-signs in OFFSEASON.

    // Build a flat list of (team, player) pairs whose contracts are
    // expiring this year. Done up-front so we can iterate without
    // worrying about roster mutations mid-loop.
    struct ExpiringSlot {
        TeamPtr   team;
        PlayerPtr player;
    };
    std::vector<ExpiringSlot> expiring;
    for (auto& kv : leagues) {
        for (auto& t : kv.second->teams()) {
            if (!t) continue;
            // User team is INCLUDED. The AI re-sign loop runs as a fallback
            // for the user's expiring players — if the user already re-signed
            // them via the UI during the season, years_left > 1 and they're
            // filtered out here. If they didn't act, the AI logic gets one
            // last shot to extend before the player hits FA. Same decision
            // pipeline as AI teams (team_eval_score + counter-offer +
            // accepts_resign_offer); only the news headline differs.
            for (auto& p : t->roster) {
                if (!p) continue;
                if (p->is_retired) continue;
                if (p->team_name != t->name) continue;  // ghost guard
                if (p->years_left(year) <= 1) {
                    expiring.push_back({t, p});
                }
            }
        }
    }

    // team_eval_score: multi-factor signal answering "do we want to keep
    // this player?". Negative -> walk. Positive -> attempt re-sign.
    auto team_eval_score = [&](const Team& t, const Player& p,
                               const Player::ResignOffer& offer) -> double {
        double score = 0.0;

        // Age curve.
        if      (p.age <= 24) score += 30.0;
        else if (p.age <= 27) score += 20.0;
        else if (p.age <= 29) score +=  5.0;
        else                  score -= 15.0;

        // Positional scarcity — losing this player would leave the team
        // without their primary_role in the starting 5.
        {
            int same_role_count = 0;
            const std::size_t starters = std::min<std::size_t>(5, t.roster.size());
            for (std::size_t i = 0; i < starters; ++i) {
                if (!t.roster[i]) continue;
                if (t.roster[i].get() == &p) continue;
                if (t.roster[i]->primary_role == p.primary_role) ++same_role_count;
            }
            if (same_role_count == 0) score += 25.0;
        }

        // Chemistry total — sum of edges to every other roster member.
        {
            double chem_total = 0.0;
            for (const auto& mate : t.roster) {
                if (!mate || mate.get() == &p) continue;
                chem_total += t.chemistry_between(*mate, p);
            }
            score += chem_total * 5.0;
        }

        // Org memory bias.
        if (t.memory.veteran_success >= 30 && p.age >= 28) score += 10.0;
        if (t.memory.rookie_success  >= 30 && p.age <= 22) score += 15.0;

        // Championship window fit.
        const double p_ovr = p.ovr();
        if ((t.window == TeamWindow::Closing || t.window == TeamWindow::Open)
            && p_ovr >= 75.0) {
            score += 15.0;
        }
        if (t.window == TeamWindow::Closed && p.age >= 28) {
            score -= 25.0;
        }

        // Financial pressure — does this deal blow our per-slot budget?
        double budget_per_slot_avg = static_cast<double>(t.budget)
                                   / (1000.0 * 8.0);  // ~$K per slot heuristic
        if (offer.amount_k > budget_per_slot_avg) score -= 15.0;

        // Form bias from season average.
        double r = p.avg_match_rating();
        if      (r >= 1.10) score += 15.0;
        else if (r >= 1.00) score +=  5.0;
        else if (r >  0.0 && r <= 0.85) score -= 10.0;

        // Strategy fit overlay.
        if ((t.strategy == Team::Strategy::Contender ||
             t.strategy == Team::Strategy::WinNow) && p_ovr >= 80.0) {
            score += 20.0;
        }
        if ((t.strategy == Team::Strategy::Rebuilding ||
             t.strategy == Team::Strategy::TalentFarm) && p.age >= 28) {
            score -= 25.0;
        }

        return score;
    };

    // Counter-offer policy.
    auto counter_offer_k = [](const Player& p, const Player::ResignOffer& off) {
        const double ovr = p.ovr();
        int counter;
        if (ovr >= 80.0) {
            counter = off.amount_k;            // Stars: pay full ask.
        } else if (ovr >= 65.0) {
            counter = static_cast<int>(off.amount_k * 0.90);
        } else {
            counter = off.min_acceptable_k;    // Bench / decline: lowball.
        }
        if (counter < off.min_acceptable_k) counter = off.min_acceptable_k;
        return counter;
    };

    // News helpers — formatted headlines for the two outcomes.
    auto push_resign_news = [&](const TeamPtr& t, const PlayerPtr& p,
                                int years, int amount_k) {
        NewsItem n;
        n.year = year; n.day_in_year = day_in_year;
        n.category = "Roster Move";
        char buf[200];
        std::snprintf(buf, sizeof(buf),
            "%s re-signs with %s — %dy / $%dK/yr",
            p->name.c_str(), t->name.c_str(), years, amount_k);
        n.headline = buf;
        n.body = "Contract extension finalized at the offseason close.";
        n.team_name   = t->name;
        n.player_name = p->name;
        push_news(n);
    };
    auto push_walk_news = [&](const TeamPtr& t, const PlayerPtr& p) {
        NewsItem n;
        n.year = year; n.day_in_year = day_in_year;
        n.category = "Roster Move";
        n.headline = t->name + " lets " + p->name + " walk into free agency";
        n.body = "Talks broke down — the front office moves on.";
        n.team_name   = t->name;
        n.player_name = p->name;
        push_news(n);
    };
    auto push_poach_news = [&](const TeamPtr& winner, const PlayerPtr& p,
                               const std::string& prev_team_name) {
        NewsItem n;
        n.year = year; n.day_in_year = day_in_year;
        n.category = "Roster Move";
        n.headline = winner->name + " poaches " + p->name
                   + " from " + prev_team_name;
        n.body = "Open-market signing closes — competitor lands a free-agent star.";
        n.team_name   = winner->name;
        n.player_name = p->name;
        push_news(n);
    };

    // Players who walked end up in this list, paired with their old team —
    // the market-competition pass below tries to place stars elsewhere.
    struct WalkedSlot {
        TeamPtr   prev_team;   // who they just walked from
        PlayerPtr player;
    };
    std::vector<WalkedSlot> walked;

    for (auto& slot : expiring) {
        TeamPtr   t = slot.team;
        PlayerPtr p = slot.player;
        if (!t || !p) continue;

        Player::ResignOffer offer = p->propose_resign_offer(*t, year);
        int counter = counter_offer_k(*p, offer);

        double eval = team_eval_score(*t, *p, offer);
        long long deal_cost = static_cast<long long>(counter) * 1000LL;
        bool team_wants = (eval >= 0.0) && (t->budget >= deal_cost);

        // Team's counter-offer length runs through the same multi-factor
        // decision the rest of the AI uses — so a Closing-window team
        // pushes a SHORT extension while an Opening team commits LONG to
        // a young core. Capped at the player's max_acceptable_years so
        // we don't propose something they'd reject outright.
        int team_years = t->decide_contract_years(*p, year);
        if (team_years > offer.max_acceptable_years) team_years = offer.max_acceptable_years;
        if (team_years < 1) team_years = 1;

        bool deal_done = false;
        if (team_wants) {
            if (p->accepts_resign_offer(counter, team_years, *t)) {
                // CONTRACT YEAR FIX (2026-05-29): the year-end re-sign loop
                // runs INSIDE run_end_of_year, BEFORE the `year += 1` step
                // at the bottom of the function. New contracts logically
                // start in the upcoming season, NOT the just-finished one,
                // so we pass `year + 1` as the effective signing year.
                // Without this, a "3y deal" signed at year-end would set
                // exp_year = year + 2 → years_left at the start of the new
                // season is 2, one short of what the user/AI agreed to.
                if (t->resign_player(p, team_years, counter, year + 1)) {
                    push_resign_news(t, p, team_years, counter);
                    log.emplace_back(std::string("[RE-SIGN] ")
                                     + t->name + " extends "
                                     + p->name + " ("
                                     + std::to_string(team_years) + "y / $"
                                     + std::to_string(counter) + "K)");
                    deal_done = true;
                }
            }
        }
        if (!deal_done) {
            // Player walks. Cleanup pass below will release them via the
            // expired-contract gate — record the previous team so market
            // competition can route them to a competitor.
            walked.push_back({t, p});
            push_walk_news(t, p);
        }
    }

    // === B8. Market competition for expiring stars ============================
    // For each walked player with ovr >= 75, find up to 3 same-region rival
    // teams who could realistically out-bid the original — budget headroom,
    // positional need, sorted by composite appetite. Each makes ONE offer
    // (gen_contract with team context); player picks the highest
    // willingness × amount_k that clears their min_acceptable_k. If accepted
    // we sign them via the standard Team::sign_player path so all roster
    // invariants run.
    for (auto& w : walked) {
        if (!w.player || !w.prev_team) continue;
        if (w.player->is_retired) continue;
        const double pl_ovr = w.player->ovr();
        if (pl_ovr < 75.0) continue;  // Only stars get the bidding round.

        // Estimated market value as a budget floor for interested teams.
        int est_value_k = w.prev_team->market_value_estimate(*w.player);
        long long floor_budget = static_cast<long long>(est_value_k * 1.1) * 1000LL;

        // Collect candidate teams: same region, not the previous team, not
        // the user team, has budget headroom AND positional need for the
        // player's primary_role.
        struct Candidate {
            TeamPtr team;
            double  score;
        };
        std::vector<Candidate> candidates;
        auto lit = leagues.find(w.prev_team->region);
        if (lit == leagues.end()) continue;
        for (auto& other : lit->second->teams()) {
            if (!other) continue;
            if (other == w.prev_team) continue;
            if (other == user_team) continue;
            if (other->budget < floor_budget) continue;
            // Positional need: target_comp wants more of this role than we
            // currently have on the roster.
            int role_idx = static_cast<int>(w.player->primary_role);
            if (role_idx < 0 || role_idx >= static_cast<int>(Role::Count)) continue;
            int have = 0;
            for (const auto& m : other->roster) {
                if (m && m->primary_role == w.player->primary_role) ++have;
            }
            if (other->target_comp.need[role_idx] <= have) continue;

            // Composite appetite: import-appetite + budget + prestige normalised.
            double appetite = team_import_appetite(*other, w.player->primary_role);
            double composite = appetite
                             + static_cast<double>(other->budget) / 4'000'000.0
                             + other->prestige / 200.0;
            candidates.push_back({other, composite});
        }
        if (candidates.empty()) continue;
        std::sort(candidates.begin(), candidates.end(),
                  [](const Candidate& a, const Candidate& b) {
                      return a.score > b.score;
                  });
        if (candidates.size() > 3) candidates.resize(3);

        // Each candidate makes ONE offer. Pick the offer that maximises
        // willingness × amount_k AND clears the player's hard floor.
        struct Bid {
            TeamPtr   team;
            int       amount_k;
            int       years;
            double    composite;  // willingness * amount_k
        };
        std::vector<Bid> bids;
        for (auto& c : candidates) {
            Contract bc = w.player->gen_contract(year,
                                                 /*randomize_amount=*/true,
                                                 /*randomize_exp=*/false,
                                                 c.team.get());
            // Pull the willingness score by simulating the player's
            // proposed offer against this would-be team (it's the same
            // composite the UI surfaces and gives us a usable rank metric).
            Player::ResignOffer fresh = w.player->propose_resign_offer(*c.team, year);
            int amount = bc.amount_k;
            // Years from the BIDDING TEAM's multi-factor decision, not
            // gen_contract's random output. Different orgs will offer
            // different lengths for the same player based on their window/
            // strategy/budget — meaningful negotiation depth.
            int yrs = c.team->decide_contract_years(*w.player, year);
            if (amount < fresh.min_acceptable_k) continue;  // below floor
            // Team must actually be able to afford the deal.
            if (c.team->budget < static_cast<long long>(amount) * 1000LL) continue;
            double comp = fresh.willingness * static_cast<double>(amount);
            bids.push_back({c.team, amount, yrs, comp});
        }
        if (bids.empty()) continue;  // Nobody made an acceptable offer.
        std::sort(bids.begin(), bids.end(),
                  [](const Bid& a, const Bid& b) {
                      return a.composite > b.composite;
                  });
        Bid& win = bids.front();

        // Final acceptance check — same hard gate the re-sign path uses,
        // routed via the new team.
        if (!w.player->accepts_resign_offer(win.amount_k, win.years, *win.team)) continue;

        // Sign via the standard pipeline so IGL/Flex invariants run.
        // Player is still on the previous team's roster until the cleanup
        // pass; pre-emptively release them here so sign_player sees a
        // clean Free-Agent state. (release_player flips team_name and
        // bumps cuts_this_year_ — that's fine; the cleanup pass below
        // skips already-released players.)
        std::string prev_name = w.prev_team->name;
        w.prev_team->release_player(w.player);
        // Debit budget + sign. sign_player doesn't debit budget on its
        // own (only scout_top_fas / auto_fill_roster do), so do it here
        // to mirror what resign_player would have done.
        win.team->budget -= static_cast<long long>(win.amount_k) * 1000LL;
        // CONTRACT YEAR FIX (2026-05-29): year-end poach signing — pass
        // year + 1 because the new contract starts in the upcoming season,
        // not the season just ended. exp_year override matches.
        win.team->sign_player(w.player, win.years, year + 1);
        // Bake in the agreed amount + exp_year so it matches the bid.
        w.player->contract.amount_k = win.amount_k;
        w.player->contract.exp_year = year + win.years;   // (year+1) + win.years - 1
        w.player->contract_years    = win.years;

        push_poach_news(win.team, w.player, prev_name);
        log.emplace_back(std::string("[FA POACH] ")
                         + win.team->name + " signs "
                         + w.player->name + " away from "
                         + prev_name + " ("
                         + std::to_string(win.years) + "y / $"
                         + std::to_string(win.amount_k) + "K)");
    }

    // === Roster cleanup pass ===
    // BEFORE running ai_manage_roster, walk every team and explicitly
    // release players whose contracts have expired or who retired this
    // year. Without this, "ghost players" stay on rosters with the wrong
    // team_name flag, and the resigning loop below would re-sign retired
    // or moved-on players.
    for (auto& kv : leagues) {
        for (auto& t : kv.second->teams()) {
            if (!t) continue;
            std::vector<PlayerPtr> drops;
            for (auto& p : t->roster) {
                if (!p) continue;
                // Player's `exp_year` is INCLUSIVE — the last season they
                // play. Releasing at `exp_year <= year` (the year we just
                // finished) means a 3-year contract signed in 2026
                // (exp_year=2028) releases at end-of-2028, matching the
                // user-facing "year offseason -> FA" semantics. AI teams
                // already had a chance to re-extend such players in the
                // re-sign attempt pass directly above; anyone still
                // expiring here either failed their Desire check or
                // belongs to the user team.
                bool expired = (p->contract.exp_year <= year);
                bool retired = p->is_retired;
                if (expired || retired) drops.push_back(p);
            }
            for (auto& p : drops) {
                t->release_player(p);
                if (p->is_retired) p->team_name = "Retired";
                else { p->team_name = "Free Agent"; ++contracts_expired; }
            }
            // Also retire the head coach if they hit the age bar (already
            // handled above), but if they were left with team_name still
            // pointing at the team, clear the link too.
            if (t->head_coach && t->head_coach->is_retired) {
                t->head_coach.reset();
            }
        }
    }
    if (contracts_expired > 0) {
        char buf[96];
        std::snprintf(buf, sizeof(buf),
            "[CONTRACTS] %d expired contracts processed at year-end.",
            contracts_expired);
        log.emplace_back(buf);
    }

    for (auto& kv : leagues) {
        for (auto& t : kv.second->teams()) {
            if (!t) continue;
            t->save_history(year);
            t->wins = 0; t->losses = 0;
            t->phase_wins = 0; t->phase_losses = 0;

            if (!t->head_coach) {
                t->head_coach = generate_coach(t->region);
                t->head_coach->team_name = t->name;
                t->head_coach->contract_exp_year =
                    year + t->head_coach->contract_years - 1;
            }
            // Decrement and check existing coach contract — release if expired
            // (god-mode editor can extend before this fires).
            if (t->head_coach && t->head_coach->contract_exp_year > 0
                && t->head_coach->contract_exp_year < year) {
                t->head_coach.reset();
            }

            auto fa_pool = solo_qs[kv.first]->global_ladder();
            // Deep AI GM pass (Team::ai_full_offseason_pass): per-starter
            // value assessment + targeted upgrade scans + cascade-fill +
            // bench depth + full per-move logging. Same depth applied to
            // every team in the league — orgs actually act like front
            // offices, not auto_fill scripts.
            //
            // USER TEAM is still excluded here — the user is entering
            // OFFSEASON next and wants their own control. The OFFSEASON-end
            // fallback in advance_day() runs the same deep pass on the user
            // team only as a safety net if they ended OFFSEASON < 5 starters.
            if (t != user_team) {
                // CONTRACT YEAR FIX (2026-05-29): runs inside run_end_of_year
                // before `year += 1`. Signings here are for the UPCOMING
                // season, so pass year + 1 so internal sign_player calls
                // set exp_year correctly. The OFFSEASON-end user fallback
                // path (advance_day) already passes the post-incremented
                // year so this is the only callsite that needs the bump.
                t->ai_full_offseason_pass(fa_pool, year + 1, log);
            }
            t->refresh_target_comp();
            // Re-classify strategy after roster shuffle. Bumps prestige if
            // the team had a winning season.
            int net_w = t->history.empty() ? 0 : t->history.back().wins - t->history.back().losses;
            t->prestige = clamp_v(t->prestige + net_w / 3, 10, 99);
            // 2026-05 rebalance: was 600 + prestige*22 (up to ~$2.8M);
            // new formula yields $200K-$1K range, matching the new budget
            // scale ($250K-$2M). See B3 / classify_team_strategy thresholds.
            t->sponsorship_k = 200 + t->prestige * 8;
            // Strategy Inertia — orgs RESIST snapping to a new philosophy
            // on a single year's evidence. classify_team_strategy still
            // computes the data-driven suggestion; commit_strategy_with_inertia
            // probabilistically decides whether to actually adopt it or
            // stick with last year's strategy. Year-1 fast-path is
            // guaranteed by the initialize_world seed (previous_strategy
            // == strategy at world init).
            Team::Strategy suggested = classify_team_strategy(*t);
            t->strategy = commit_strategy_with_inertia(*t, suggested);

            // Re-sign loop hoisted ABOVE the cleanup pass — see the
            // "Re-sign attempt pass" earlier in run_end_of_year. Players
            // whose Desire rejects the auto-extend offer were released
            // by the cleanup pass; remaining roster members had their
            // exp_year bumped and are good to go.
        }
        kv.second->generate_schedule();
    }

    // After roster shuffle, re-sync every player's ranked region to their
    // current pro team's region. Players signed across regions (imports)
    // will be moved into the new region's ladder.
    sync_all_ranked_regions();

    // === Ranked MMR + season W/L reset ===
    // Every Valorant act/episode wipes ranked stats back to a baseline so
    // climbers restart fresh. solo_mmr resets to 1100, and the seasonal
    // ranked W/L counters reset alongside (career history is already
    // archived in p->history via save_history_and_progress above; that
    // entry has the season's rating + KD frozen for the leaderboards).
    // peak_mmr is preserved for HoF / GOAT career history.
    int mmr_resets = 0;
    for (auto& kv : solo_qs) {
        if (!kv.second) continue;
        for (auto& p : kv.second->global_ladder()) {
            if (!p) continue;
            if (p->is_retired) continue;
            // Sanity: keep peak_mmr in sync with the player's all-time best.
            if (p->solo_mmr > p->peak_mmr) p->peak_mmr = p->solo_mmr;
            p->solo_mmr = 1100;
            p->solo_wins = 0;
            p->solo_losses = 0;
            // Snapshot reset so monthly progression deltas don't see a
            // phantom MMR/W-L "drop" on the next tick.
            p->last_snapshot.solo_mmr = 1100;
            p->last_snapshot.solo_wins = 0;
            p->last_snapshot.solo_losses = 0;
            ++mmr_resets;
        }
    }
    {
        char buf[96];
        std::snprintf(buf, sizeof(buf),
            "[RANKED] New season: MMR + W/L reset for %d players.",
            mmr_resets);
        log.emplace_back(buf);
    }

    // Rookie generation: keep each region's ladder near the 900 target so
    // the ranked scene stays competitive and produces 15+ Radiants/year.
    int per_region_baseline = 55;
    int per_region_retired = std::max(0, retired_this_year / 3);
    int per_region_total = per_region_baseline + per_region_retired;
    for (auto& kv : solo_qs) {
        for (int i = 0; i < per_region_total; ++i) kv.second->generate_rookie();
    }

    // Snapshot year-end state for emitter (5) + (9) next-year comparisons.
    // Done AFTER emit_historic_performance/emit_retirement_countdown have
    // fired so the snapshot reflects the CURRENT year's final values.
    snapshot_year_end_state();

    year += 1;
    // NOTE: phase_idx is no longer reset here. advance_day's AWARDS handler
    // bumps it to OFFSEASON (idx 10), and the OFFSEASON handler wraps it
    // back to STAGE 1 (idx 0) after 14 days. This gives the user a window
    // to manage their roster manually before the new season starts.
    current_week = 0;
    // Defensive belt-and-braces clear: force_finish_stale_tournaments at
    // phase boundaries is the SOLE mid-phase clear path; in practice every
    // tournament should already be drained + active_tournaments empty by
    // the time we reach year-end. This clear is here purely to guard
    // against a malformed save / partial init state where a tournament
    // leaked through phase end. DO NOT remove without auditing the
    // active_tournaments lifecycle invariants documented in §4.11.
    active_tournaments.clear();

    // Reset weekly ranking ticks so the first day of the new year refreshes
    // immediately. Also clear h2h_counts_ + the regular/playoff splits +
    // finals log + consecutive_series_wins_ — these are season-scoped
    // narrative trackers, not career stats. The career-keyed milestone
    // set (news_emitted_career_keys_) is intentionally NOT cleared.
    last_power_tick_day_ = -10;
    last_community_tick_day_ = -10;
    h2h_counts_.clear();
    h2h_counts_regular_.clear();
    h2h_counts_playoff_.clear();
    h2h_finals_log_.clear();
    consecutive_series_wins_.clear();
    // Wipe FA decay baselines — rookies generated below + this year's
    // newly-released players form a fresh FA cohort with reset asking
    // prices. The next offseason re-stamps baselines on day 1.
    fa_demand_baseline_k_.clear();
}

void GameManager::sync_player_ranked_region(const PlayerPtr& p, const std::string& target_region) {
    if (!p) return;
    if (p->region == target_region) return;
    // Find the player in their current ladder and remove
    for (auto& kv : solo_qs) {
        if (!kv.second) continue;
        auto& ladder = kv.second->global_ladder();
        auto it = std::find(ladder.begin(), ladder.end(), p);
        if (it != ladder.end()) {
            ladder.erase(it);
            break;
        }
    }
    // Add to target region's ladder
    auto target_it = solo_qs.find(target_region);
    if (target_it != solo_qs.end() && target_it->second) {
        target_it->second->global_ladder().push_back(p);
    }
    p->region = target_region;
}

void GameManager::sync_all_ranked_regions() {
    for (auto& kv : leagues) {
        const std::string& team_region = kv.first;
        for (auto& t : kv.second->teams()) {
            if (!t) continue;
            for (auto& p : t->roster) {
                if (p && p->region != team_region) {
                    sync_player_ranked_region(p, team_region);
                }
            }
        }
    }
}

void GameManager::push_news(const NewsItem& n) {
    news_feed.insert(news_feed.begin(), n);
    if (news_feed.size() > 200) news_feed.resize(200);
}

// === H2H query helpers (B7) ==========================================
int GameManager::h2h_total(Team* a, Team* b) const {
    return h2h_regular(a, b) + h2h_playoff(a, b);
}
int GameManager::h2h_regular(Team* a, Team* b) const {
    if (!a || !b) return 0;
    auto it = h2h_counts_regular_.find(std::make_pair(a, b));
    if (it != h2h_counts_regular_.end()) return it->second;
    return 0;
}
int GameManager::h2h_playoff(Team* a, Team* b) const {
    if (!a || !b) return 0;
    auto it = h2h_counts_playoff_.find(std::make_pair(a, b));
    if (it != h2h_counts_playoff_.end()) return it->second;
    return 0;
}
std::vector<GameManager::H2HSeriesFinal>
GameManager::h2h_finals_between(Team* a, Team* b) const {
    std::vector<H2HSeriesFinal> out;
    if (!a || !b) return out;
    for (const auto& f : h2h_finals_log_) {
        bool match = (f.winner == a && f.loser == b)
                  || (f.winner == b && f.loser == a);
        if (match) out.push_back(f);
    }
    return out;
}

// === Offseason FA decay (B8) =========================================
void GameManager::run_offseason_fa_decay() {
    // Walk every region's ladder for unsigned FAs and shave their demand
    // each offseason day. Stamps a baseline on first sight so the floor is
    // a stable fraction of their entered-offseason ask, not a runaway
    // exponential. Skips signed players, retirees, and anyone whose
    // baseline is already at the floor.
    //
    // Note: the spec mentions skipping "players signed in last 24h" — but
    // by construction we only iterate Free Agents here. A player signed
    // in the last 24h has team_name != "Free Agent" and is filtered out
    // by the loop guard. (If we ever want a 24h-grace for newly-released
    // players who land back in the FA pool, we'd add a per-player
    // last_released_day timestamp; out of scope for this pack.)
    for (auto& kv : solo_qs) {
        if (!kv.second) continue;
        for (auto& p : kv.second->global_ladder()) {
            if (!p) continue;
            if (p->is_retired) continue;
            if (p->team_name != "Free Agent") continue;
            // Stamp baseline on first sight this offseason.
            auto bit = fa_demand_baseline_k_.find(p.get());
            int baseline = 0;
            if (bit == fa_demand_baseline_k_.end()) {
                baseline = std::max(15, p->contract.amount_k);
                fa_demand_baseline_k_[p.get()] = baseline;
            } else {
                baseline = bit->second;
            }
            int floor_k = std::max(15, static_cast<int>(
                std::floor(baseline * kFADecayFloorFraction)));
            if (p->contract.amount_k <= floor_k) {
                // Already at/under the floor — clamp up to the floor and
                // skip further decay so we don't grind below it.
                p->contract.amount_k = floor_k;
                continue;
            }
            int next_k = static_cast<int>(std::round(
                p->contract.amount_k * (1.0 - kFADecayRate)));
            if (next_k < floor_k) next_k = floor_k;
            if (next_k < 15) next_k = 15;
            p->contract.amount_k = next_k;
            // Mood demand decay — bring per-team mood scores toward
            // neutral so the player gets less picky as their stay in FA
            // lengthens. Player::decay_mood already exists for this
            // exact purpose. Same ~3%/day cadence (delta ~ 0.03).
            p->decay_mood(kFADecayRate);
        }
    }
}

// === FA demand helper (B8) ===========================================
int GameManager::fa_current_demand_k(const Player& p) const {
    // Current asking price in K. UI consumers should prefer this over
    // reading p.contract.amount_k directly so future implementations can
    // layer additional decay/state without breaking the UI.
    return p.contract.amount_k;
}

void GameManager::push_tournament_news(const Tournament& tour, int yr,
                                       const Series* gf_series) {
    // Idempotent: skip if we've already published news for this exact
    // (tournament name, year) pair this season.
    std::string dedup_key = tour.name() + "/" + std::to_string(yr);
    if (news_pushed_events_.count(dedup_key)) return;

    auto champion = tour.champion();
    if (!champion) return;  // not actually finished — refuse to push
    auto runner_up = tour.runner_up();

    // Map name fragment -> tier label. Mirrors Tournament::award_event_titles
    // — keep the two in sync if a new event family is added.
    std::string tier_label = "Event Champion";
    std::string tier_short = "title";
    if (tour.name().find("Regionals") != std::string::npos) {
        tier_label = "Regional Champion";
        tier_short = "regional";
    } else if (tour.name().find("MASTERS") != std::string::npos) {
        tier_label = "Masters Champion";
        tier_short = "Masters";
    } else if (tour.name().find("CHAMPIONS") != std::string::npos) {
        tier_label = "World Champion";
        tier_short = "Champions";
    }

    // ===== News item 1: Champion (mandatory) =====
    {
        NewsItem n;
        n.year = yr;
        n.day_in_year = day_in_year;
        n.category = "Tournament";
        n.headline = champion->name + " wins " + tour.name();
        char body[320];
        if (runner_up) {
            std::snprintf(body, sizeof(body),
                "Defeating %s in the grand final, %s takes home the %s title and is crowned %s %d.",
                runner_up->name.c_str(), champion->name.c_str(),
                tier_short.c_str(), tier_label.c_str(), yr);
        } else {
            std::snprintf(body, sizeof(body),
                "%s takes home the %s title and is crowned %s %d.",
                champion->name.c_str(), tier_short.c_str(),
                tier_label.c_str(), yr);
        }
        n.body = body;
        n.team_name = champion->name;
        push_news(n);
    }

    // ===== News item 2: Finals MVP / Top performer (best-effort) =====
    // Algorithm: scan the GF Series's aggregate_stats table for players
    // whose .team_name matches the champion (i.e. winning roster) and
    // pick the one with the highest aggregated rating across the GF maps.
    // If `gf_series` is null OR aggregates are empty (force-finish path),
    // skip this item rather than fabricate. We label it "Finals MVP" only
    // when sourced from the actual GF series; otherwise the field is
    // labeled honestly as "Top performer" — but right now we only push
    // this item when we have a real GF series to source from.
    if (gf_series) {
        Player*       best       = nullptr;
        double        best_rat   = -1.0;
        AggregatedStats best_stats{};
        for (auto& kv : gf_series->aggregate_stats()) {
            Player* p = kv.first;
            if (!p) continue;
            // Restrict to winning roster.
            bool on_winner = false;
            for (auto& rp : champion->roster) {
                if (rp.get() == p) { on_winner = true; break; }
            }
            if (!on_winner) continue;
            const AggregatedStats& s = kv.second;
            // Use the aggregated rating directly (already averaged across maps
            // by Series::finalize_stats per the existing pattern).
            if (s.rating > best_rat) {
                best_rat = s.rating;
                best = p;
                best_stats = s;
            }
        }
        if (best && best_stats.matches > 0) {
            NewsItem n;
            n.year = yr;
            n.day_in_year = day_in_year;
            n.category = "Tournament";
            n.headline = std::string(best->name) + " named Finals MVP at " + tour.name();
            char body[320];
            std::snprintf(body, sizeof(body),
                "%s closed out the grand final with %d kills, %d deaths, %d assists across %d map(s) — averaging a %.2f rating to lift %s.",
                best->name.c_str(), best_stats.k, best_stats.d, best_stats.a,
                best_stats.matches, best_stats.rating, champion->name.c_str());
            n.body = body;
            n.team_name = champion->name;
            n.player_name = best->name;
            push_news(n);
        }
    }

    // ===== News item 3: Champion run summary (best-effort) =====
    // Modern VCT format: single BO5 Grand Final, no bracket reset. Summarize
    // the run by which side the champion came from (UB undefeated vs LB grinder).
    {
        int ub_rounds = static_cast<int>(tour.ub_history().size());
        int lb_rounds = static_cast<int>(tour.lb_history().size());
        // Determine which side the champion came from. The naive
        // `(gf0.a == champion)` check breaks if the GF was scheduled with
        // the LB winner in slot A (we observed this in practice via the
        // bracket scheduler — slot order isn't guaranteed). New logic:
        //   1. If gf0.a == champion: came from upper.
        //   2. If gf0.b == champion: came from lower.
        //   3. Defensive fallback: scan ub_history for the champion's
        //      presence in any winner slot. Champion wins every UB round
        //      they appear in (UB winners always advance to UB-alive), so
        //      seeing them as a winner in any UB row means UB-undefeated.
        bool came_from_upper = false;
        if (!tour.gf_history().empty()) {
            const auto& gf0 = tour.gf_history().front();
            if (gf0.a == champion) {
                came_from_upper = true;
            } else if (gf0.b == champion) {
                came_from_upper = false;
            } else {
                // Fallback scan — should never hit in practice.
                for (const auto& round : tour.ub_history()) {
                    for (const auto& bm : round) {
                        if (bm.winner == champion) {
                            came_from_upper = true; break;
                        }
                    }
                    if (came_from_upper) break;
                }
            }
        }
        NewsItem n;
        n.year = yr;
        n.day_in_year = day_in_year;
        n.category = "Tournament";
        n.headline = champion->name + "'s road to the " + tier_short + " title";
        char body[360];
        if (came_from_upper) {
            std::snprintf(body, sizeof(body),
                "%s ran the upper bracket undefeated through %d round(s) before closing out %s in the grand final.",
                champion->name.c_str(), ub_rounds, tour.name().c_str());
        } else {
            std::snprintf(body, sizeof(body),
                "%s clawed through %d lower-bracket round(s) to take %s in the grand final.",
                champion->name.c_str(), lb_rounds, tour.name().c_str());
        }
        n.body = body;
        n.team_name = champion->name;
        push_news(n);
    }

    news_pushed_events_.insert(dedup_key);
}

void GameManager::update_mvp_race() {
    // Snapshot previous race scores to compute deltas
    std::unordered_map<Player*, double> prev_score;
    std::unordered_map<Player*, std::string> prev_category;
    for (auto& lb : mvp_race) {
        for (auto& c : lb.candidates) {
            if (c.player) {
                prev_score[c.player.get()] = c.score;
                prev_category[c.player.get()] = lb.category;
            }
        }
    }

    mvp_race.clear();

    auto season_rating = [](const Player& p) {
        return p.season_matches > 0 ? p.season_rating_total / p.season_matches : 0.0;
    };

    // Gather qualifying players
    std::vector<PlayerPtr> qualified;
    for (auto& kv : solo_qs) {
        for (auto& p : kv.second->global_ladder()) {
            if (!p) continue;
            if (p->season_matches < 4) continue;
            qualified.push_back(p);
        }
    }
    if (qualified.empty()) return;

    // Helper: compute hybrid score with stat + team success + international weight
    auto hybrid_score = [&](const PlayerPtr& p, double stat_weight) {
        double rat = season_rating(*p);
        double s = stat_weight * rat;
        // Team-success weighting via team wins
        for (auto& kv : leagues) {
            for (auto& t : kv.second->teams()) {
                if (!t) continue;
                for (auto& tp : t->roster) {
                    if (tp == p) {
                        s += t->wins * 0.015;
                        break;
                    }
                }
            }
        }
        // International weight: any award with current year + Masters/Champions tag
        std::string yr_str = std::to_string(year);
        for (auto& a : p->awards) {
            if (a.find(yr_str) == std::string::npos) continue;
            if (a.find("[M]") != std::string::npos) s += 0.30;
            if (a.find("[W]") != std::string::npos) s += 0.50;
        }
        return s;
    };

    auto build_lb = [&](const std::string& cat,
                       std::function<bool(const PlayerPtr&)> gate,
                       double stat_weight) {
        RaceLeaderboard lb;
        lb.category = cat;
        std::vector<std::pair<PlayerPtr, double>> ranked;
        for (auto& p : qualified) {
            if (!gate(p)) continue;
            ranked.emplace_back(p, hybrid_score(p, stat_weight));
        }
        std::sort(ranked.begin(), ranked.end(),
                  [](const std::pair<PlayerPtr, double>& a,
                     const std::pair<PlayerPtr, double>& b) {
                      return a.second > b.second;
                  });
        for (std::size_t i = 0; i < std::min<std::size_t>(8, ranked.size()); ++i) {
            RaceCandidate c;
            c.player = ranked[i].first;
            c.score  = ranked[i].second;
            // Compare against previous snapshot
            auto pit = prev_score.find(c.player.get());
            auto cit = prev_category.find(c.player.get());
            if (pit != prev_score.end() && cit != prev_category.end() && cit->second == cat) {
                double diff = c.score - pit->second;
                c.delta = (int)std::round(diff * 100.0);
                if (diff > 0.05)      c.blurb = "Climbing";
                else if (diff < -0.05) c.blurb = "Slipping";
                else                   c.blurb = "Steady";
            } else {
                c.blurb = "New entry";
                c.delta = 0;
            }
            lb.candidates.push_back(c);
        }
        if (!lb.candidates.empty()) mvp_race.push_back(lb);
    };

    auto attended_intl = [yr = year](const PlayerPtr& p) {
        std::string yr_str = std::to_string(yr);
        for (auto& a : p->awards) {
            if (a.find(yr_str) == std::string::npos) continue;
            if (a.find("[M]") != std::string::npos) return true;
            if (a.find("[W]") != std::string::npos) return true;
        }
        return false;
    };

    build_lb("MVP Race",
        [&](const PlayerPtr& p) { return attended_intl(p) || season_rating(*p) >= 1.10; },
        1.0);
    auto role_gate = [](Role r) {
        return [r](const PlayerPtr& p) { return p->primary_role == r; };
    };
    build_lb("Duelist Race",   role_gate(Role::Duelist),    0.9);
    build_lb("Initiator Race", role_gate(Role::Initiator),  0.9);
    build_lb("Controller Race",role_gate(Role::Controller), 0.9);
    build_lb("Sentinel Race",  role_gate(Role::Sentinel),   0.9);
    build_lb("IGL Race",
        [](const PlayerPtr& p) {
            if (!p->is_igl) return false;
            if (p->is_retired) return false;
            if (p->team_name == "Free Agent" || p->team_name == "Retired") return false;
            return true;
        },
        0.7);

    // Push a news item if the MVP leader changed. The leader-name tracker
    // is a private member (cleared in reset_world) instead of a function
    // static so reset_world() can wipe it cleanly — a function-static would
    // outlive the wipe and leak the previous world's leader into the new
    // world (the first MVP-takes-lead news item would be suppressed).
    if (!mvp_race.empty() && !mvp_race.front().candidates.empty()) {
        auto& mvp_leader = mvp_race.front().candidates.front().player;
        if (mvp_leader && mvp_leader->name != last_mvp_leader_name_) {
            NewsItem n;
            n.year = year; n.day_in_year = day_in_year;
            n.category = "MVP Race";
            n.headline = mvp_leader->name + " takes the top spot in the MVP race";
            char body[200];
            std::snprintf(body, sizeof(body),
                "%s currently leads the MVP race at %.2f rating, with team success boosting their case.",
                mvp_leader->name.c_str(),
                mvp_leader->season_matches > 0
                    ? mvp_leader->season_rating_total / mvp_leader->season_matches : 0.0);
            n.body = body;
            n.player_name = mvp_leader->name;
            n.team_name = mvp_leader->team_name;
            push_news(n);
            last_mvp_leader_name_ = mvp_leader->name;
        }
    }
}

void GameManager::run_initial_snake_draft(std::vector<std::string>& log) {
    // Build a shared FA pool from all regions (a player can sign abroad
    // subject to the per-team import cap of 2).
    std::vector<PlayerPtr> pool;
    for (auto& kv : solo_qs) {
        for (auto& p : kv.second->global_ladder()) {
            if (p && p->team_name == "Free Agent" && !p->is_retired) pool.push_back(p);
        }
    }

    // Build draft order: all teams, snake by region. We randomize the
    // order once so the league isn't always Americas-first, then snake.
    std::vector<TeamPtr> draft_order;
    for (auto& kv : leagues) {
        for (auto& t : kv.second->teams()) {
            if (t) draft_order.push_back(t);
        }
    }
    rng().shuffle(draft_order);
    if (draft_order.empty()) return;

    auto current_imports = [](const TeamPtr& t) {
        int n = 0; for (auto& x : t->roster) if (x->region != t->region) ++n;
        return n;
    };

    auto count_role = [](const TeamPtr& t, Role r) {
        int n = 0; for (auto& x : t->roster) if (x->primary_role == r) ++n;
        return n;
    };

    auto has_igl = [](const TeamPtr& t) {
        for (auto& x : t->roster) if (x->is_igl) return true;
        return false;
    };

    // Target role for the current team-pick: first unmet slot from
    // target_comp's need vector. Used by team_import_appetite for the
    // role-scarcity factor. Returns Role::Count if everything is filled.
    auto target_role_for = [&](const TeamPtr& t) {
        std::array<int, static_cast<std::size_t>(Role::Count)> have{};
        for (auto& x : t->roster) {
            if (!x) continue;
            have[static_cast<std::size_t>(x->primary_role)]++;
        }
        for (std::size_t i = 0; i < static_cast<std::size_t>(Role::Count); ++i) {
            if (t->target_comp.need[i] - have[i] > 0) {
                return static_cast<Role>(i);
            }
        }
        return Role::Count;
    };

    // Per-pick import-attempt gate. Reuses team_import_appetite
    // (Team.cpp) so wealthy/contender/role-starved teams roll for
    // imports while budget rebuilders almost never do. STRICT v2:
    // scaling 0.7 -> 0.55, floor 0.05 -> 0.03 so even strong import
    // candidates need the team's appetite to clear a real bar.
    auto roll_import_attempt = [&](const TeamPtr& t) {
        Role tr = target_role_for(t);
        double appetite = team_import_appetite(*t, tr, &pool);
        double p = appetite * 0.55;
        if (p < 0.03) p = 0.03;
        return rng().chance(p);
    };

    // Score a candidate FA for `t` taking strategy + role need + IGL +
    // import cap into account.
    auto score_candidate = [&](const PlayerPtr& fa, const TeamPtr& t,
                                int round_idx, int total_rounds,
                                bool allow_import) {
        // Hard cap: import limit
        if (fa->region != t->region && current_imports(t) >= config().max_imports) {
            return -1e9;
        }
        // Per-pick appetite gate: if this team didn't roll for an import
        // this pick, drop foreign candidates out of consideration.
        if (fa->region != t->region && !allow_import) {
            return -1e9;
        }
        // Hard cap: budget (rough — refined later by gen_contract)
        long long est_cost = (long long)fa->contract.amount_k * 1000LL;
        if (t->budget - est_cost < 100'000LL) return -1e9;

        // Strategy-driven base score
        double s = 0.0;
        switch (t->strategy) {
            case Team::Strategy::Contender:
            case Team::Strategy::WinNow:
                s = fa->ovr() * 1.4 + fa->career_matches * 0.10;
                break;
            case Team::Strategy::Rebuilding:
            case Team::Strategy::DevelopmentFocus:
                s = fa->potential * 1.2 + std::max(0, 25 - fa->age) * 6.0;
                break;
            case Team::Strategy::Bridge:
                s = fa->ovr() * 0.9 + fa->potential * 0.5;
                break;
            case Team::Strategy::BudgetRoster:
                s = fa->ovr() * 0.7 - fa->contract.amount_k * 0.3;
                break;
            case Team::Strategy::TalentFarm:
                s = (fa->potential - fa->ovr()) * 5.0 + std::max(0, 24 - fa->age) * 5.0;
                break;
        }

        // Role-need bonus from target_comp
        int need_idx = static_cast<int>(fa->primary_role);
        int have = count_role(t, fa->primary_role);
        int need = t->target_comp.need[need_idx] - have;
        if (need > 0) s += 60.0;
        else          s -= 30.0;

        // Late-round IGL nudge: if no IGL and we're in the last round,
        // strongly prefer is_igl candidates.
        if (round_idx >= total_rounds - 1 && !has_igl(t) && fa->is_igl) {
            s += 200.0;
        }

        // Avoid imports unless really good
        if (fa->region != t->region && fa->ovr() < 60.0) s -= 50.0;

        // === Organizational memory nudges ===================================
        // Past outcomes bias future signings — orgs LEARN.
        //  * Strong rookie pipeline -> bonus on young candidates
        //  * Burned-on-rookies      -> penalty on young candidates
        //  * Burned-on-imports      -> penalty on cross-region candidates
        //  * Strong veteran results -> bonus on older candidates
        //  * Tight financial discipline -> extra penalty on expensive deals
        const auto& mem = t->memory;
        if (mem.rookie_success >=  30 && fa->age <= 22) s += 25.0;
        if (mem.rookie_success <= -30 && fa->age <= 22) s -= 15.0;
        if (mem.import_success <= -30 && fa->region != t->region) s -= 30.0;
        if (mem.veteran_success >= 30 && fa->age >= 28) s += 20.0;
        if (mem.financial_discipline >= 30) {
            s -= 0.20 * static_cast<double>(fa->contract.amount_k);
        }

        // === Timeline fit (multi-year coherence) ===========================
        // Even at world boot, teams draft with future-coherence in mind:
        // an Opening rebuild favors young high-potential picks; a Closing
        // contender favors prime/old veterans. Weighted modestly (0.6) so
        // it nudges rather than dominates strategy-driven base.
        s += 0.60 * timeline_fit_score(*t, *fa);

        // === Risk tolerance via archetype consistency_mod ==================
        // Contenders pay for consistency (volatile pros get penalized).
        // Rebuilders gamble on volatile high-ceiling talent (volatile pros
        // with high potential get bonus). Mirrors the auto_fill::fill_one
        // logic so snake-draft and FA-fill produce coherent rosters.
        double cm = archetype_profile(fa->archetype).consistency_mod;
        double risk_bias = 0.0;
        if (t->strategy == Team::Strategy::Contender ||
            t->strategy == Team::Strategy::WinNow) {
            risk_bias = cm * 120.0;
        } else if (t->strategy == Team::Strategy::Rebuilding ||
                   t->strategy == Team::Strategy::DevelopmentFocus ||
                   t->strategy == Team::Strategy::TalentFarm) {
            if (fa->potential >= 80) risk_bias = -cm * 80.0;
        }
        s += risk_bias;

        return s;
    };

    int rounds = 5;
    int picks = 0;
    for (int r = 0; r < rounds; ++r) {
        bool reverse = (r % 2 == 1);
        for (int i = 0; i < (int)draft_order.size(); ++i) {
            int idx = reverse ? ((int)draft_order.size() - 1 - i) : i;
            auto& t = draft_order[idx];
            if (t->roster.size() >= 5) continue;

            // Per-pick: decide once whether this team will entertain
            // foreign FAs for THIS pick. Then score every available FA.
            bool allow_import = roll_import_attempt(t);
            PlayerPtr best;
            double best_score = -1e9;
            for (auto& fa : pool) {
                if (!fa || fa->team_name != "Free Agent") continue;
                double s = score_candidate(fa, t, r, rounds, allow_import);
                if (s > best_score) { best_score = s; best = fa; }
            }
            if (!best) continue;

            // Sign the player. Amount comes from gen_contract; years comes
            // from the team's multi-factor decide_contract_years (window,
            // strategy, age, potential, financial discipline, budget).
            Contract c = best->gen_contract(year, true, false, t.get());
            best->contract.amount_k = c.amount_k;
            int yrs = t->decide_contract_years(*best, year);
            best->contract.exp_year = year + yrs - 1;
            t->budget -= (long long)c.amount_k * 1000LL;
            t->sign_player(best, yrs, year);
            best->team_name = t->name;
            ++picks;
        }
    }

    // After 5 rounds, ensure every team has an IGL via Team::auto_fill_roster
    // promotion logic (it only promotes if missing; doesn't add players).
    for (auto& t : draft_order) {
        if (t->roster.size() < 5) {
            // Fall back to auto_fill if draft couldn't fully populate
            t->auto_fill_roster(pool, year);
        } else {
            // Just refresh comp + check IGL via a pass that won't add
            std::vector<PlayerPtr> empty;
            t->auto_fill_roster(empty, year);
        }
    }

    char buf[128];
    std::snprintf(buf, sizeof(buf),
        "[DRAFT] Initial snake draft: %d picks across %zu teams.",
        picks, draft_order.size());
    log.emplace_back(buf);

    // Per-region import distribution sanity log. STRICT v2 ambition-
    // gating targets ~2-3 teams-with-imports per region (Pacific often
    // 0-1). Anything beyond ~4 means the appetite curve drifted up.
    log.emplace_back("[Imports] season-start summary:");
    for (const char* region : kRegions) {
        int teams_with_imports = 0;
        int total_teams = 0;
        auto lit = leagues.find(region);
        if (lit != leagues.end()) {
            for (auto& t : lit->second->teams()) {
                if (!t) continue;
                ++total_teams;
                int imp = 0;
                for (auto& x : t->roster) {
                    if (x && x->region != t->region) ++imp;
                }
                if (imp > 0) ++teams_with_imports;
            }
        }
        char rbuf[160];
        std::snprintf(rbuf, sizeof(rbuf),
            "  %s: %d of %d teams have imports",
            region, teams_with_imports, total_teams);
        log.emplace_back(rbuf);
    }
}

void GameManager::compute_season_awards(std::vector<std::string>& log) {
    last_season_awards.clear();

    // Gather all players with enough season activity to qualify.
    // Hard gate: 8+ season matches AND not retired before season end.
    std::vector<PlayerPtr> qualified;
    for (auto& kv : solo_qs) {
        for (auto& p : kv.second->global_ladder()) {
            if (!p) continue;
            if (p->season_matches < 8) continue;
            qualified.push_back(p);
        }
    }
    if (qualified.empty()) return;

    auto season_rating = [](const Player& p) {
        return p.season_matches > 0
            ? p.season_rating_total / p.season_matches : 0.0;
    };

    // Did this player attend at least one international event this year?
    // We approximate via awards earned this year (Masters / World tag).
    auto attended_intl = [yr = year](const PlayerPtr& p) {
        std::string yr_str = std::to_string(yr);
        for (auto& a : p->awards) {
            if (a.find(yr_str) == std::string::npos) continue;
            if (a.find("[M]") != std::string::npos) return true;  // Masters
            if (a.find("[W]") != std::string::npos) return true;  // World/Champions
        }
        return false;
    };

    auto add_award = [&](const std::string& cat,
                          std::function<double(const Player&)> score,
                          std::function<bool(const PlayerPtr&)> gate,
                          const std::string& blurb) {
        std::vector<std::pair<PlayerPtr, double>> ranked;
        for (auto& p : qualified) {
            if (!gate(p)) continue;
            ranked.emplace_back(p, score(*p));
        }
        if (ranked.empty()) return;
        std::sort(ranked.begin(), ranked.end(),
                  [](const std::pair<PlayerPtr, double>& a,
                     const std::pair<PlayerPtr, double>& b) {
                      return a.second > b.second;
                  });

        SeasonAward aw;
        aw.year = year;
        aw.category = cat;
        aw.winner = ranked.front().first;
        aw.explanation = blurb;
        for (std::size_t i = 0; i < std::min<std::size_t>(3, ranked.size()); ++i) {
            aw.finalists.push_back(ranked[i].first);
            aw.scores.push_back(ranked[i].second);
        }
        last_season_awards.push_back(aw);
        awards_history.push_back(aw);

        // Pin the award to the winner's career awards.
        std::string award_str = "[A] " + cat + " " + std::to_string(year);
        bool exists = false;
        for (auto& a : aw.winner->awards) if (a == award_str) { exists = true; break; }
        if (!exists) aw.winner->awards.push_back(award_str);

        char buf[200];
        std::snprintf(buf, sizeof(buf), "[AWARD] %s %d: %s (%.2f)",
                      cat.c_str(), year, aw.winner->name.c_str(), ranked.front().second);
        log.emplace_back(buf);

        // Push to news feed
        NewsItem n;
        n.year = year; n.day_in_year = day_in_year;
        n.category = "Awards";
        n.headline = aw.winner->name + " wins " + cat + " " + std::to_string(year);
        char nbuf[260];
        std::snprintf(nbuf, sizeof(nbuf),
            "Winning over %zu finalists with a score of %.2f. %s",
            aw.finalists.size(), ranked.front().second, blurb.c_str());
        n.body = nbuf;
        n.player_name = aw.winner->name;
        n.team_name = aw.winner->team_name;
        push_news(n);
    };

    // === MVP — multi-factor scoring (Pillar 2) ===
    //
    // Replaces the old "highest season rating wins" with a composite score
    // so pure fraggers no longer monopolise MVP. Inputs:
    //   * season_rating     — the headline number, with a 0.95 floor.
    //   * Strength of schedule proxy via attended-international + team wins
    //     (more wins / Masters / Champions => tougher competition implied).
    //   * Clutch impact     — season_clutch_pts (1vN closures, capped boost).
    //   * IGL impact        — only for IGLs, capped at +35% so a calling-only
    //                         star can win without elite frag numbers.
    //   * Playoff weight    — season_pressure_matches (rewards showing up).
    //   * Team dependency   — small +10% for carrying a relatively weak roster
    //                         (career OVR vs MVP-race team wins as a proxy).
    //
    // Final formula:
    //   base   = season_rating  (must be >= 0.95 to qualify)
    //   clutch = 1 + min(0.30, season_clutch_pts / 30)
    //   igl    = is_igl ? 1 + min(0.35, igl_impact_season / 40) : 1
    //   sos    = (1.15 if attended_intl else 1.00)
    //          * (1.10 if team made finals-tier event this year else 1.00)
    //   playoff= 1 + min(0.20, season_pressure_matches / 10)
    //   dep    = 1.10 if player's team total wins < 18 AND season_rating>=1.05 else 1
    //   score  = base * clutch * igl * sos * playoff * dep
    //
    // The 0.95 floor + attended-intl-friendly gate keep results from being
    // diluted by stage-only role players.
    auto team_wins_for = [&](const Player& p) -> int {
        for (auto& kv : leagues) {
            for (auto& t : kv.second->teams()) {
                if (!t) continue;
                for (auto& tp : t->roster) {
                    if (tp.get() == &p) return t->wins;
                }
            }
        }
        return 0;
    };
    auto mvp_score = [&](const Player& p) -> double {
        double rating = season_rating(p);
        if (rating < 0.95) return 0.0;

        // Clutch factor (capped at +30%).
        double clutch = 1.0 + std::min(0.30, p.season_clutch_pts / 30.0);

        // IGL factor (capped at +35%). Reads igl_impact_season directly so
        // an IGL who anchors a contender all year racks up the bonus.
        double igl_fac = 1.0;
        if (p.is_igl) {
            igl_fac = 1.0 + std::min(0.35, p.igl_impact_season / 40.0);
        }

        // Strength-of-schedule proxy.
        double sos = 1.0;
        std::string yr_str = std::to_string(year);
        bool intl_this_year = false;
        bool finals_this_year = false;
        for (auto& a : p.awards) {
            if (a.find(yr_str) == std::string::npos) continue;
            if (a.find("[W]") != std::string::npos || a.find("[M]") != std::string::npos)
                intl_this_year = true;
            if (a.find("[W]") != std::string::npos || a.find("[M]") != std::string::npos ||
                a.find("[T]") != std::string::npos)
                finals_this_year = true;  // championship-tier finals appearance
        }
        if (intl_this_year)   sos *= 1.15;
        if (finals_this_year) sos *= 1.10;

        // Playoff / LAN weight (capped at +20%).
        double playoff = 1.0 + std::min(0.20, p.season_pressure_matches / 10.0);

        // Team success factor — REPLACES the old "dep" (which paradoxically
        // rewarded low-win teams). 2026-05-28 user feedback: "players on
        // mediocre or bad teams can win MVP too easily purely from stats".
        // New mapping rewards winning more aggressively:
        //   team_wins >= 28 -> ×1.25   (deep playoff run)
        //   team_wins >= 22 -> ×1.15   (solid contender)
        //   team_wins >= 16 -> ×1.05   (average)
        //   team_wins <  10 -> ×0.85   (losing team — meaningful penalty)
        // Historic statistical seasons can still overcome a weak record (a
        // 1.30 rating still beats most things even at ×0.85) but team
        // success now has real teeth.
        double team_factor = 1.0;
        int tw = team_wins_for(p);
        if      (tw >= 28) team_factor = 1.25;
        else if (tw >= 22) team_factor = 1.15;
        else if (tw >= 16) team_factor = 1.05;
        else if (tw <  10) team_factor = 0.85;

        return rating * clutch * igl_fac * sos * playoff * team_factor;
    };
    add_award("MVP",
        mvp_score,
        [&](const PlayerPtr& p) {
            // Soft attendance gate: prefer international qualifiers, but
            // allow domestic monsters with rating >= 1.20 to slip through
            // so a complete carry season on a weak team can still win.
            if (attended_intl(p)) return true;
            return season_rating(*p) >= 1.20;
        },
        "Multi-factor score: rating x clutch x IGL x SOS x playoff weight x team dependency.");

    // === Role of the Year awards ===
    // 2026-05-28: team success now factors in. Pure season_rating let role
    // awards go to stat-padders on losing teams while a comparable player
    // on a contender lost the trophy. New score:
    //   role_score = season_rating × team_role_factor × intl_factor
    //   team_role_factor: 1.20 at 28+ team wins, 1.10 at 22+, 1.03 at 16+,
    //                     0.90 at <10 (mirrors MVP rebalance, slightly less
    //                     aggressive so elite stats can still carry mid teams)
    //   intl_factor:      1.10 if attended any [M]/[W] this year, else 1.00
    auto role_team_factor = [&](const Player& p) -> double {
        int tw = team_wins_for(p);
        if (tw >= 28) return 1.20;
        if (tw >= 22) return 1.10;
        if (tw >= 16) return 1.03;
        if (tw <  10) return 0.90;
        return 1.00;
    };
    auto role_intl_factor = [&](const Player& p) -> double {
        std::string yr_str = std::to_string(year);
        for (const auto& a : p.awards) {
            if (a.find(yr_str) == std::string::npos) continue;
            if (a.find("[M]") != std::string::npos) return 1.10;
            if (a.find("[W]") != std::string::npos) return 1.10;
        }
        return 1.00;
    };
    auto role_score = [&](const Player& p) {
        return season_rating(p) * role_team_factor(p) * role_intl_factor(p);
    };
    auto role_gate = [&](Role r) {
        return [r](const PlayerPtr& p) { return p->primary_role == r; };
    };
    add_award("Duelist of the Year", role_score, role_gate(Role::Duelist),
        "Top Duelist score: rating x team success x international attendance.");
    add_award("Initiator of the Year", role_score, role_gate(Role::Initiator),
        "Top Initiator score: rating x team success x international attendance.");
    add_award("Controller of the Year", role_score, role_gate(Role::Controller),
        "Top Controller score: rating x team success x international attendance.");
    add_award("Sentinel of the Year", role_score, role_gate(Role::Sentinel),
        "Top Sentinel score: rating x team success x international attendance.");

    // === IGL of the Year — IGL-impact weighted (Pillar 1) ===
    // Replaces the old 0.6×rating + 0.4×wins composite. New mix is built so
    // a pure tactical IGL can win even on mediocre personal stats:
    //
    //   0.35 × season_rating
    // + 0.25 × per-match IGL impact (igl_impact_season / igl_match_count)
    // + 0.20 × team finals factor (1.0 if attended [M]/[W] this year, 0.5 if
    //          only [T], 0.0 otherwise — proxies "team success at scale")
    // + 0.20 × (team_wins / 40.0)
    //
    // Same strict gating as before: is_igl AND on a real pro roster.
    add_award("IGL of the Year",
        [&](const Player& p) {
            double rat = season_rating(p);
            // Per-match IGL impact — only credited if the player actually
            // logged IGL matches. Falls back to 0 cleanly otherwise.
            double per_match_impact = (p.igl_match_count > 0)
                ? (p.igl_impact_season / std::max(1, p.igl_match_count))
                : 0.0;
            // Cap at 2.0 so a single huge match can't dominate.
            if (per_match_impact > 2.0) per_match_impact = 2.0;

            // Team finals factor (cumulative-snapshot awards keyed by year).
            double team_finals = 0.0;
            std::string yr_str = std::to_string(year);
            for (auto& a : p.awards) {
                if (a.find(yr_str) == std::string::npos) continue;
                if (a.find("[W]") != std::string::npos) { team_finals = 1.00; break; }
                if (a.find("[M]") != std::string::npos) { team_finals = std::max(team_finals, 0.85); }
                else if (a.find("[T]") != std::string::npos) { team_finals = std::max(team_finals, 0.50); }
            }

            double team_w = 0.0;
            for (auto& kv : leagues) {
                for (auto& t : kv.second->teams()) {
                    if (!t) continue;
                    for (auto& tp : t->roster) {
                        if (tp.get() == &p) { team_w = t->wins; break; }
                    }
                }
            }
            return 0.35 * rat
                 + 0.25 * per_match_impact
                 + 0.20 * team_finals
                 + 0.20 * (team_w / 40.0);
        },
        [&](const PlayerPtr& p) {
            if (!p->is_igl) return false;
            if (p->is_retired) return false;
            if (p->team_name == "Free Agent" || p->team_name == "Retired") return false;
            // Must have actually played IGL matches this season. Stops a
            // late-season is_igl promotion from winning a player who never
            // actually called a round (also catches: anyone whose is_igl
            // flag got flipped on transiently by enforce_one_igl without
            // accruing any igl_impact). If no qualifier has IGL matches,
            // no IGL OTY is awarded this year — correct behavior.
            if (p->igl_match_count <= 0) return false;
            return true;
        },
        "Composite: rating, per-match IGL impact, team finals reach, and season wins.");
}

void GameManager::favorite_player(const PlayerPtr& p) {
    if (!p) return;
    if (std::find(favorite_players.begin(), favorite_players.end(), p) == favorite_players.end())
        favorite_players.push_back(p);
}
void GameManager::unfavorite_player(const PlayerPtr& p) {
    favorite_players.erase(
        std::remove(favorite_players.begin(), favorite_players.end(), p),
        favorite_players.end());
}
bool GameManager::is_favorited(const PlayerPtr& p) const {
    return std::find(favorite_players.begin(), favorite_players.end(), p) != favorite_players.end();
}

// =====================================================================
// === Power Rankings + Community Rankings =============================
// =====================================================================
namespace {

// Map score band + delta to an analyst flavor note. Deterministic (no rng).
// `delta = prev_rank - rank` (positive = climbing in ordinal).
std::string power_note_for(const std::string& tier, int delta, bool unranked_prev) {
    bool climbing = delta > 1 || unranked_prev;
    bool falling  = delta < -1;
    if (tier == "S") {
        if (climbing) return "Rolling through the bracket — title contender.";
        if (falling)  return "Title window cracking open for the chasers.";
        return "Top of the pyramid. Everyone's chasing them.";
    }
    if (tier == "A") {
        if (climbing) return "Quietly stacking wins — playoff lock.";
        if (falling)  return "Falling out of the top tier after a soft week.";
        return "Stable contender — playoff bracket bound.";
    }
    if (tier == "B") {
        if (climbing) return "Finding form at the right time.";
        if (falling)  return "Cooling off after rough split.";
        return "Middle of the pack — needs a signature win.";
    }
    if (tier == "C") {
        if (climbing) return "Sneaking up the ladder.";
        if (falling)  return "Slipping further from playoff math.";
        return "Streaky form. One good run from breaking through.";
    }
    // Bubble
    if (climbing) return "Bubble watch — close to breaking through.";
    if (falling)  return "Slipping. Roster questions emerging.";
    return "Bubble life. Wins on the schedule or roster moves incoming.";
}

std::string community_note_for(const std::string& tier, int delta,
                               bool unranked_prev, bool popular) {
    bool climbing = delta > 1 || unranked_prev;
    bool falling  = delta < -1;
    if (tier == "S") {
        if (popular)  return "Fan-favorite squad with the loudest crowd.";
        return "Stat sheet says S-tier — fans aren't sold yet.";
    }
    if (tier == "A") {
        if (climbing) return "Riding the rookie hype train.";
        return "Quiet professionals — analysts love them.";
    }
    if (tier == "B") {
        if (popular)  return "Loud fanbase keeping them in the conversation.";
        return "Decent results, decent buzz. Nothing electric.";
    }
    if (tier == "C") {
        if (falling)  return "Story fading from the timeline.";
        return "Niche following hanging on for the storylines.";
    }
    if (popular)  return "Loud fanbase, quiet results.";
    return "Forgotten team — no one's watching.";
}

// Convert a rank index (0-based) inside an N-team list to tier label.
// Region (12 teams) bands more steeply than the doc's 1-2=S example so a
// region list lines up with the doc spec: 1-2=S, 3-4=A, 5-6=B, 7-8=C, rest=Bubble.
// International is 12-team but doc asks 1-3=S, 4-6=A, 7-9=B, 10-12=C.
std::string tier_for_rank(int rank_1based, bool international) {
    if (international) {
        if (rank_1based <= 3)  return "S";
        if (rank_1based <= 6)  return "A";
        if (rank_1based <= 9)  return "B";
        return "C";
    }
    if (rank_1based <= 2)  return "S";
    if (rank_1based <= 4)  return "A";
    if (rank_1based <= 6)  return "B";
    if (rank_1based <= 8)  return "C";
    return "Bubble";
}

}  // namespace

void GameManager::compute_power_rankings() {
    // === Build per-region team lists ===
    std::unordered_map<std::string, std::vector<Team*>> teams_by_region;
    for (auto* region_str : kRegions) {
        std::string r(region_str);
        auto it = leagues.find(r);
        if (it == leagues.end() || !it->second) continue;
        for (auto& t : it->second->teams()) {
            if (t) teams_by_region[r].push_back(t.get());
        }
    }

    // Save previous ranks per team so we can compute delta + retain rank_history.
    std::unordered_map<Team*, int> prev_rank;
    std::unordered_map<Team*, std::vector<int>> prev_history;
    for (auto& kv : power_rankings_) {
        for (auto& pr : kv.second) {
            if (pr.team) {
                prev_rank[pr.team] = pr.rank;
                prev_history[pr.team] = pr.rank_history;
            }
        }
    }

    // Per-team scoring closure.
    auto score_team = [&](Team* t, const std::vector<Team*>& peer_list) -> double {
        if (!t) return 0.0;
        // win_rate over season wins/losses (proxy for "last 5 series" — the
        // engine doesn't expose a sliding window so we use the season tally).
        double w = static_cast<double>(t->wins);
        double l = static_cast<double>(t->losses);
        double win_rate = (w + l > 0.0) ? (w / (w + l)) : 0.5;

        // map_diff_season — derive from team->history if it has entries this
        // year, else default to (wins - losses) clamped 0..1 proxy.
        double map_diff = static_cast<double>(t->wins - t->losses);
        double map_diff_factor = std::min(1.0, std::max(0.0, (map_diff + 30.0) / 60.0));

        // tournament_finish_factor: scan trophy_case().ordered for this year.
        // Agent B contract: vector<pair<int,string>> ordered.
        double tour_factor = 0.0;
        const auto& tc = t->trophy_case();
        for (auto& entry : tc.ordered) {
            if (entry.first != year) continue;
            const std::string& ev = entry.second;
            if (ev.find("CHAMPIONS") != std::string::npos) tour_factor = std::max(tour_factor, 1.0);
            else if (ev.find("MASTERS") != std::string::npos) tour_factor = std::max(tour_factor, 1.0);
            else if (ev.find("Regionals") != std::string::npos) tour_factor = std::max(tour_factor, 1.0);
        }
        // No accessor for "finalist/SF this year" — we only credit champions.
        // Future enhancement once tournament-history accessors exist.

        // Strength of schedule: avg opponent prestige normalized 0..1.
        double opp_prestige_sum = 0.0; int opp_count = 0;
        for (Team* peer : peer_list) {
            if (!peer || peer == t) continue;
            opp_prestige_sum += peer->prestige;
            ++opp_count;
        }
        double sos = (opp_count > 0) ? (opp_prestige_sum / opp_count) / 99.0 : 0.5;

        double prestige_factor = std::min(1.0, t->prestige / 99.0);

        double score = 0.40 * win_rate
                     + 0.20 * prestige_factor
                     + 0.15 * map_diff_factor
                     + 0.15 * tour_factor
                     + 0.10 * sos;
        return score;
    };

    auto build_ranking_list = [&](const std::vector<Team*>& teams,
                                   bool international,
                                   std::vector<PowerRanking>& out) {
        out.clear();
        out.reserve(teams.size());
        struct Entry { Team* t; double s; };
        std::vector<Entry> entries;
        entries.reserve(teams.size());
        for (Team* t : teams) entries.push_back({t, score_team(t, teams)});
        std::stable_sort(entries.begin(), entries.end(),
            [](const Entry& a, const Entry& b) {
                if (a.s != b.s) return a.s > b.s;
                if (!a.t || !b.t) return a.t != nullptr;
                return a.t->name < b.t->name;
            });
        int rank = 1;
        for (auto& e : entries) {
            PowerRanking pr;
            pr.team = e.t;
            pr.rank = rank;
            pr.score = e.s;
            auto pit = prev_rank.find(e.t);
            pr.prev_rank = (pit != prev_rank.end()) ? pit->second : -1;
            pr.tier = tier_for_rank(rank, international);
            int delta = (pr.prev_rank > 0) ? (pr.prev_rank - pr.rank) : 0;
            pr.analyst_note = power_note_for(pr.tier, delta, pr.prev_rank < 0);
            // Carry history forward (cap 8).
            auto hit = prev_history.find(e.t);
            if (hit != prev_history.end()) pr.rank_history = hit->second;
            pr.rank_history.push_back(rank);
            if (pr.rank_history.size() > 8) {
                pr.rank_history.erase(pr.rank_history.begin(),
                    pr.rank_history.begin() + (pr.rank_history.size() - 8));
            }
            out.push_back(std::move(pr));
            ++rank;
        }
    };

    // === Per-region lists ===
    for (auto* region_str : kRegions) {
        std::string r(region_str);
        auto it = teams_by_region.find(r);
        if (it == teams_by_region.end()) continue;
        build_ranking_list(it->second, /*international=*/false, power_rankings_[r]);
    }

    // === International (top 4 per region by current score) ===
    std::vector<Team*> intl_pool;
    for (auto* region_str : kRegions) {
        std::string r(region_str);
        const auto& pr_list = power_rankings_[r];
        int taken = 0;
        for (auto& pr : pr_list) {
            if (!pr.team) continue;
            intl_pool.push_back(pr.team);
            if (++taken >= 4) break;
        }
    }
    build_ranking_list(intl_pool, /*international=*/true, power_rankings_["International"]);
}

void GameManager::compute_community_rankings() {
    // Snapshot previous community rank + score so the "lag" + delta tracking
    // works across ticks.
    std::unordered_map<Team*, int> prev_rank;
    std::unordered_map<Team*, double> prev_score;
    std::unordered_map<Team*, std::vector<int>> prev_history;
    for (auto& kv : community_rankings_) {
        for (auto& cr : kv.second) {
            if (cr.team) {
                prev_rank[cr.team] = cr.rank;
                prev_score[cr.team] = cr.score;
                prev_history[cr.team] = cr.rank_history;
            }
        }
    }

    // Build region team lists (mirror power_rankings).
    std::unordered_map<std::string, std::vector<Team*>> teams_by_region;
    for (auto* region_str : kRegions) {
        std::string r(region_str);
        auto it = leagues.find(r);
        if (it == leagues.end() || !it->second) continue;
        for (auto& t : it->second->teams()) {
            if (t) teams_by_region[r].push_back(t.get());
        }
    }

    // Count narrative buzz: news_feed items in last ~30 days that name a
    // team. Pre-bucket by team_name for cheap O(news) per pass.
    std::unordered_map<std::string, int> buzz_by_team;
    int cutoff_day  = day_in_year - 30;
    int cutoff_year = year;
    for (auto& n : news_feed) {
        // "last 30 days" — same year and within 30 days, OR previous year
        // tail wrap.
        bool in_window = false;
        if (n.year == cutoff_year && n.day_in_year >= cutoff_day) in_window = true;
        else if (n.year == cutoff_year - 1 && cutoff_day < 0
                 && n.day_in_year >= (kDaysPerYear + cutoff_day)) in_window = true;
        if (!in_window) continue;
        if (!n.team_name.empty()) buzz_by_team[n.team_name] += 1;
    }

    auto score_team = [&](Team* t) -> std::pair<double, double> {
        if (!t) return {0.0, 0.0};
        // Star presence
        double star = 0.0;
        for (auto& p : t->roster) {
            if (!p) continue;
            if (p->ovr() >= 80.0) star += 1.0;
            if (!p->signature_agent().empty()) star += 0.3;
        }
        // Normalise: 5 stars + 5 signatures = 6.5 cap → divide by 6.5.
        star = std::min(1.0, star / 6.5);

        // Flashy moments: scan roster's pro_match_history (cap ~10 entries)
        // for matches with kills>=25 OR rating>=1.40. Acts as a proxy for
        // "highlight reel buzz" — old games get evicted by the cap so the
        // window is naturally bounded.
        double flashy = 0.0;
        for (auto& p : t->roster) {
            if (!p) continue;
            for (auto& rec : p->pro_match_history) {
                if (!rec || !rec->match_stats) continue;
                auto sit = rec->match_stats->find(p.get());
                if (sit == rec->match_stats->end()) continue;
                const PlayerMatchStats& ps = sit->second;
                if (ps.k >= 25 || ps.rating >= 1.40) {
                    flashy += 1.0;
                }
            }
        }
        // Normalise: 5 starters x 2 highlight games = 10 → cap at 1.0.
        flashy = std::min(1.0, flashy / 10.0);

        // Narrative buzz — normalised by 10 (a busy team gets 5-10 news items
        // a month, so 10 is the soft cap).
        double buzz_raw = static_cast<double>(buzz_by_team[t->name]);
        double buzz = std::min(1.0, buzz_raw / 10.0);

        double prev = prev_score.count(t) ? prev_score[t] : 0.5;
        // 50% lag + 25% stars + 15% flashy + 10% buzz.
        double pop_score = 0.50 * prev + 0.25 * star + 0.15 * flashy + 0.10 * buzz;
        double narrative_boost_val = 0.25 * star + 0.10 * buzz;
        return {pop_score, narrative_boost_val};
    };

    auto build_ranking_list = [&](const std::vector<Team*>& teams,
                                   bool international,
                                   std::vector<CommunityRanking>& out) {
        out.clear();
        out.reserve(teams.size());
        struct Entry { Team* t; double s; double pop; double narrative; };
        std::vector<Entry> entries;
        entries.reserve(teams.size());
        for (Team* t : teams) {
            auto sp = score_team(t);
            entries.push_back({t, sp.first, sp.first, sp.second});
        }
        std::stable_sort(entries.begin(), entries.end(),
            [](const Entry& a, const Entry& b) {
                if (a.s != b.s) return a.s > b.s;
                if (!a.t || !b.t) return a.t != nullptr;
                return a.t->name < b.t->name;
            });
        int rank = 1;
        for (auto& e : entries) {
            CommunityRanking cr;
            cr.team = e.t;
            cr.rank = rank;
            cr.score = e.s;
            cr.popularity_score = e.pop;
            cr.narrative_boost = e.narrative;
            auto pit = prev_rank.find(e.t);
            cr.prev_rank = (pit != prev_rank.end()) ? pit->second : -1;
            cr.tier = tier_for_rank(rank, international);
            int delta = (cr.prev_rank > 0) ? (cr.prev_rank - cr.rank) : 0;
            bool popular = cr.popularity_score >= 0.55;
            cr.analyst_note = community_note_for(cr.tier, delta,
                                                  cr.prev_rank < 0, popular);
            auto hit = prev_history.find(e.t);
            if (hit != prev_history.end()) cr.rank_history = hit->second;
            cr.rank_history.push_back(rank);
            if (cr.rank_history.size() > 8) {
                cr.rank_history.erase(cr.rank_history.begin(),
                    cr.rank_history.begin() + (cr.rank_history.size() - 8));
            }
            out.push_back(std::move(cr));
            ++rank;
        }
    };

    for (auto* region_str : kRegions) {
        std::string r(region_str);
        auto it = teams_by_region.find(r);
        if (it == teams_by_region.end()) continue;
        build_ranking_list(it->second, false, community_rankings_[r]);
    }

    // International: top 4 of each region by community score.
    std::vector<Team*> intl_pool;
    for (auto* region_str : kRegions) {
        std::string r(region_str);
        const auto& list = community_rankings_[r];
        int taken = 0;
        for (auto& cr : list) {
            if (!cr.team) continue;
            intl_pool.push_back(cr.team);
            if (++taken >= 4) break;
        }
    }
    build_ranking_list(intl_pool, true, community_rankings_["International"]);
}

const std::vector<PowerRanking>&
GameManager::power_rankings_for(const std::string& region) const {
    static const std::vector<PowerRanking> kEmpty;
    auto it = power_rankings_.find(region);
    if (it == power_rankings_.end()) return kEmpty;
    return it->second;
}

const std::vector<CommunityRanking>&
GameManager::community_rankings_for(const std::string& region) const {
    static const std::vector<CommunityRanking> kEmpty;
    auto it = community_rankings_.find(region);
    if (it == community_rankings_.end()) return kEmpty;
    return it->second;
}

int GameManager::power_rank_of(Team* t) const {
    if (!t) return 0;
    for (auto& kv : power_rankings_) {
        if (kv.first == "International") continue;  // prefer region rank
        for (auto& pr : kv.second) {
            if (pr.team == t) return pr.rank;
        }
    }
    return 0;
}

int GameManager::community_rank_of(Team* t) const {
    if (!t) return 0;
    for (auto& kv : community_rankings_) {
        if (kv.first == "International") continue;
        for (auto& cr : kv.second) {
            if (cr.team == t) return cr.rank;
        }
    }
    return 0;
}

int GameManager::next_power_tick_in_days() const {
    if (last_power_tick_day_ < 0) return 0;  // immediate
    int days_since = day_in_year - last_power_tick_day_;
    int remaining = 7 - days_since;
    return std::max(0, remaining);
}

// =====================================================================
// === News Emitters (10) ==============================================
// =====================================================================
namespace {

// Build a stable per-player ID for emitter dedup keys. We don't have a
// numeric player_id field exposed, so we use name+country_iso which is
// unique enough across a save.
std::string player_key(const Player& p) {
    return p.name + "/" + p.country_iso;
}

}  // namespace

void GameManager::emit_breakout_news() {
    // (1) Player season_rating ≥ 1.15 AND (season_rating - last_year_rating)
    // ≥ 0.15 AND career_seasons_played ≥ 2.
    for (auto& kv : leagues) {
        for (auto& t : kv.second->teams()) {
            if (!t) continue;
            for (auto& p : t->roster) {
                if (!p || p->is_retired) continue;
                if (p->career_seasons_played < 2) continue;
                if (p->season_matches < 5) continue;  // need at least some volume
                double season_rating = (p->season_matches > 0)
                    ? (p->season_rating_total / p->season_matches) : 0.0;
                if (season_rating < 1.15) continue;
                if (p->history.empty()) continue;
                double last_year_rating = p->history.back().rating;
                if (season_rating - last_year_rating < 0.15) continue;

                std::string key = "breakout/" + player_key(*p) + "/"
                                + std::to_string(year);
                if (news_emitted_keys_.count(key)) continue;

                NewsItem n;
                n.year = year; n.day_in_year = day_in_year;
                n.category = "Breakout";
                char hl[200];
                std::snprintf(hl, sizeof(hl),
                    "Breakout: %s explodes for %.2f this split",
                    p->name.c_str(), season_rating);
                n.headline = hl;
                char body[320];
                std::snprintf(body, sizeof(body),
                    "%s (%s) is outperforming their %.2f from last season by a wide margin.",
                    p->name.c_str(), t->name.c_str(), last_year_rating);
                n.body = body;
                n.player_name = p->name;
                n.team_name = t->name;
                push_news(n);
                news_emitted_keys_.insert(key);
            }
        }
    }
}

void GameManager::emit_slump_news() {
    // (2) Career rating ≥ 1.10 + season rating < 0.90 over 8+ season matches.
    for (auto& kv : leagues) {
        for (auto& t : kv.second->teams()) {
            if (!t) continue;
            for (auto& p : t->roster) {
                if (!p || p->is_retired) continue;
                if (p->season_matches < 8) continue;
                if (p->avg_match_rating() < 1.10) continue;
                double sr = (p->season_matches > 0)
                    ? (p->season_rating_total / p->season_matches) : 0.0;
                if (sr >= 0.90) continue;

                std::string key = "slump/" + player_key(*p) + "/"
                                + std::to_string(year);
                if (news_emitted_keys_.count(key)) continue;

                NewsItem n;
                n.year = year; n.day_in_year = day_in_year;
                n.category = "Slump";
                n.headline = "Star " + p->name + " mired in worst slump of career";
                char body[300];
                std::snprintf(body, sizeof(body),
                    "Career %.2f-rated %s sitting at %.2f across %d matches this season for %s.",
                    p->avg_match_rating(), p->name.c_str(),
                    sr, p->season_matches, t->name.c_str());
                n.body = body;
                n.player_name = p->name;
                n.team_name = t->name;
                push_news(n);
                news_emitted_keys_.insert(key);
            }
        }
    }
}

void GameManager::emit_hot_streak_news(Team* winner, Team* loser) {
    // (3) Threshold-based: emit at 5 / 8 / 12 wins. Counter is bumped by
    // the caller BEFORE this fires so the value we read reflects the
    // just-completed series.
    (void)loser;
    if (!winner) return;
    int streak = consecutive_series_wins_[winner];
    int threshold = 0;
    const char* tier = nullptr;
    if (streak == 12) { threshold = 12; tier = "historic"; }
    else if (streak == 8) { threshold = 8; tier = "elite"; }
    else if (streak == 5) { threshold = 5; tier = "hot"; }
    else return;

    std::string key = "streak/" + winner->name + "/" + std::to_string(threshold)
                    + "/" + std::to_string(year);
    if (news_emitted_keys_.count(key)) return;

    NewsItem n;
    n.year = year; n.day_in_year = day_in_year;
    n.category = "Hot Streak";
    char hl[200];
    if (threshold == 12) {
        std::snprintf(hl, sizeof(hl),
            "%s extends winning streak to %d — historic run.",
            winner->name.c_str(), threshold);
    } else {
        std::snprintf(hl, sizeof(hl),
            "%s on a %d-series %s streak.",
            winner->name.c_str(), threshold, tier);
    }
    n.headline = hl;
    n.body = "The grind continues — every opponent on the schedule is suddenly an upset risk.";
    n.team_name = winner->name;
    push_news(n);
    news_emitted_keys_.insert(key);
}

void GameManager::emit_milestone_news() {
    // (4) Year-end milestone scan. These are CAREER milestones (once-in-a-
    // lifetime achievements) — a player crosses 5000 career kills exactly
    // once in their life. Dedup keys live in news_emitted_career_keys_,
    // which is NEVER cleared at year rollover (unlike news_emitted_keys_
    // which is reset annually). Without the career-set the same milestone
    // would re-fire every subsequent year — see commit history.
    const int kKillsMilestones[]    = { 5000, 10000, 20000, 30000 };
    const int kMatchesMilestones[]  = { 500, 1000, 1500 };
    const int kTrophyMilestones[]   = { 3, 5, 10 };

    for (auto& kv : solo_qs) {
        for (auto& p : kv.second->global_ladder()) {
            if (!p) continue;
            // Career kills milestones.
            for (int m : kKillsMilestones) {
                if (p->career_kills < m) continue;
                // "milestone-career/..." prefix (vs the legacy
                // "milestone/..." prefix) makes it explicit at a glance
                // these keys live in the career set, not the per-year set.
                std::string key = "milestone-career/kills/" + std::to_string(m) + "/"
                                + player_key(*p);
                if (news_emitted_career_keys_.count(key)) continue;
                NewsItem n;
                n.year = year; n.day_in_year = day_in_year;
                n.category = "Milestone";
                char hl[160];
                std::snprintf(hl, sizeof(hl),
                    "%s crosses %d career kills", p->name.c_str(), m);
                n.headline = hl;
                n.body = "A career counter most pros never reach.";
                n.player_name = p->name;
                n.team_name = p->team_name;
                push_news(n);
                news_emitted_career_keys_.insert(key);
            }
            // Career matches milestones.
            for (int m : kMatchesMilestones) {
                if (p->career_matches < m) continue;
                std::string key = "milestone-career/matches/" + std::to_string(m) + "/"
                                + player_key(*p);
                if (news_emitted_career_keys_.count(key)) continue;
                NewsItem n;
                n.year = year; n.day_in_year = day_in_year;
                n.category = "Milestone";
                char hl[160];
                std::snprintf(hl, sizeof(hl),
                    "%s reaches %d career pro matches", p->name.c_str(), m);
                n.headline = hl;
                n.body = "Longevity is its own kind of greatness.";
                n.player_name = p->name;
                n.team_name = p->team_name;
                push_news(n);
                news_emitted_career_keys_.insert(key);
            }
            // Trophy count milestone — uses prefix-anchored awards counter.
            int trophies = p->award_count_by_prefix("[T] ")
                         + p->award_count_by_prefix("[M] ")
                         + p->award_count_by_prefix("[W] ");
            for (int m : kTrophyMilestones) {
                if (trophies < m) continue;
                std::string key = "milestone-career/trophies/" + std::to_string(m) + "/"
                                + player_key(*p);
                if (news_emitted_career_keys_.count(key)) continue;
                NewsItem n;
                n.year = year; n.day_in_year = day_in_year;
                n.category = "Milestone";
                char hl[160];
                std::snprintf(hl, sizeof(hl),
                    "%s wins %dth career trophy", p->name.c_str(), m);
                n.headline = hl;
                n.body = "Hall-of-Fame conversation just got louder.";
                n.player_name = p->name;
                n.team_name = p->team_name;
                push_news(n);
                news_emitted_career_keys_.insert(key);
            }
        }
    }
}

void GameManager::emit_retirement_countdown() {
    // (5) Year-end: age ≥ 30 AND season_rating < (career_rating - 0.10) AND
    // (last 2 years both below career_rating - 0.05).
    if (!historic_snapshot_initialized_) return;  // skip year 1
    for (auto& kv : solo_qs) {
        for (auto& p : kv.second->global_ladder()) {
            if (!p || p->is_retired) continue;
            if (p->age < 30) continue;
            double career_r = p->avg_match_rating();
            if (career_r < 0.01) continue;
            if (p->season_matches < 5) continue;
            double sr = p->season_rating_total / p->season_matches;
            if (sr >= career_r - 0.10) continue;

            auto ly_it = last_year_rating_snapshot_.find(p.get());
            auto ty_it = two_years_ago_rating_snapshot_.find(p.get());
            if (ly_it == last_year_rating_snapshot_.end()) continue;
            if (ty_it == two_years_ago_rating_snapshot_.end()) continue;
            if (ly_it->second >= career_r - 0.05) continue;
            if (ty_it->second >= career_r - 0.05) continue;

            std::string key = "retire_countdown/" + player_key(*p) + "/"
                            + std::to_string(year);
            if (news_emitted_keys_.count(key)) continue;

            NewsItem n;
            n.year = year; n.day_in_year = day_in_year;
            n.category = "Retirement Watch";
            n.headline = "Veteran " + p->name + " hints at possible final season";
            char body[260];
            std::snprintf(body, sizeof(body),
                "At %d with form dropping below their %.2f career average, %s could be approaching the end.",
                p->age, career_r, p->name.c_str());
            n.body = body;
            n.player_name = p->name;
            n.team_name = p->team_name;
            push_news(n);
            news_emitted_keys_.insert(key);
        }
    }
}

void GameManager::emit_transfer_rumor() {
    // (6) STAGE 1 + STAGE 3: FAs + contract-expiring players. Rate-limit by
    // restricting to one rumor per (player, team, year) via the dedup set.
    int rumors_this_pass = 0;
    const int kMaxPerPass = 3;  // cheap rate-limit
    for (auto& kv : solo_qs) {
        if (rumors_this_pass >= kMaxPerPass) break;
        for (auto& p : kv.second->global_ladder()) {
            if (rumors_this_pass >= kMaxPerPass) break;
            if (!p || p->is_retired) continue;
            bool fa = (p->team_name == "Free Agent");
            bool expiring = (p->contract.exp_year > 0
                          && p->contract.exp_year <= year + 1);
            if (!fa && !expiring) continue;
            if (p->ovr() < 70.0) continue;  // only star-tier rumor-worthy

            // Find a team in the same region with appetite + role need.
            auto it = leagues.find(p->region);
            if (it == leagues.end() || !it->second) continue;
            Team* best = nullptr;
            double best_app = 0.0;
            for (auto& t : it->second->teams()) {
                if (!t) continue;
                if (t->name == p->team_name) continue;  // not their own team
                double app = team_import_appetite(*t, p->primary_role, nullptr);
                if (app >= 0.18 && app > best_app) {
                    best = t.get();
                    best_app = app;
                }
            }
            if (!best) continue;

            std::string key = "rumor/" + player_key(*p) + "/" + best->name
                            + "/" + std::to_string(year);
            if (news_emitted_keys_.count(key)) continue;

            NewsItem n;
            n.year = year; n.day_in_year = day_in_year;
            n.category = "Rumor";
            const char* role_str = "starting";
            switch (p->primary_role) {
                case Role::Duelist:    role_str = "Duelist"; break;
                case Role::Controller: role_str = "Controller"; break;
                case Role::Sentinel:   role_str = "Sentinel"; break;
                case Role::Initiator:  role_str = "Initiator"; break;
                default: break;
            }
            char hl[220];
            std::snprintf(hl, sizeof(hl),
                "Rumor mill: %s eyeing %s for %s slot",
                best->name.c_str(), p->name.c_str(), role_str);
            n.headline = hl;
            n.body = "Sources close to the org suggest preliminary conversations are underway.";
            n.player_name = p->name;
            n.team_name = best->name;
            push_news(n);
            news_emitted_keys_.insert(key);
            ++rumors_this_pass;
        }
    }
}

void GameManager::emit_rivalry_match_night() {
    // (7) On matchdays only. Scan today's stage matchups via League's
    // weekly_matchups. We don't have a clean per-day fixture cursor exposed,
    // so we approximate by using the same week index that play_stage_round
    // just consumed (current_week was incremented at the end of that fn).
    // If we're in a tournament phase, the active_tournaments don't expose
    // a "today's matches" hook — leave that path stubbed.
    if (current_phase().find("STAGE") == std::string::npos) return;

    // play_stage_round increments current_week AFTER playing, so the round
    // that just played is current_week - 1. Index defensively.
    int played_week = current_week - 1;
    if (played_week < 0) return;

    for (auto& kv : leagues) {
        auto& lg = kv.second;
        if (played_week >= static_cast<int>(lg->weekly_matchups().size())) continue;
        for (auto& mu : lg->weekly_matchups()[played_week]) {
            if (!mu.a || !mu.b) continue;
            // Use the symmetric total across regular + playoff splits so the
            // rivalry-night gate still counts post-rebrand meetings the same
            // way the old single-map version did.
            int count = h2h_total(mu.a.get(), mu.b.get());
            if (count < 4) continue;

            std::string key = "rivalry/" + mu.a->name + "/" + mu.b->name
                            + "/" + std::to_string(year) + "/"
                            + std::to_string(day_in_year);
            if (news_emitted_keys_.count(key)) continue;

            NewsItem n;
            n.year = year; n.day_in_year = day_in_year;
            n.category = "Rivalry";
            n.headline = "Rivalry renewed: " + mu.a->name + " vs " + mu.b->name + " tonight";
            char body[280];
            std::snprintf(body, sizeof(body),
                "The %d-meeting head-to-head between %s and %s adds another chapter on the schedule.",
                count, mu.a->name.c_str(), mu.b->name.c_str());
            n.body = body;
            n.team_name = mu.a->name;
            push_news(n);
            news_emitted_keys_.insert(key);
        }
    }
}

void GameManager::emit_massive_upset(const Tournament& tour, Team* winner, Team* loser) {
    // (8) Winner's seed 5+ AND loser's seed 1-3. Use initial_seeding().
    if (!winner || !loser) return;
    const auto& seeds = tour.initial_seeding();
    int winner_seed = 0, loser_seed = 0;
    for (std::size_t i = 0; i < seeds.size(); ++i) {
        if (!seeds[i]) continue;
        if (seeds[i].get() == winner) winner_seed = static_cast<int>(i + 1);
        if (seeds[i].get() == loser)  loser_seed  = static_cast<int>(i + 1);
    }
    if (winner_seed < 5) return;
    if (loser_seed == 0 || loser_seed > 3) return;

    std::string key = "upset/" + tour.name() + "/" + std::to_string(year)
                    + "/" + winner->name + "/" + loser->name;
    if (news_emitted_keys_.count(key)) return;

    NewsItem n;
    n.year = year; n.day_in_year = day_in_year;
    n.category = "Upset";
    n.headline = "Massive upset! " + winner->name + " sends " + loser->name + " home";
    char body[300];
    std::snprintf(body, sizeof(body),
        "Seeded #%d, %s knocks out #%d-seeded %s at %s.",
        winner_seed, winner->name.c_str(), loser_seed, loser->name.c_str(),
        tour.name().c_str());
    n.body = body;
    n.team_name = winner->name;
    push_news(n);
    news_emitted_keys_.insert(key);
}

void GameManager::emit_historic_performance() {
    // (9) Year-end. Compare current career_max_* to year-start snapshot. Skip
    // year 1 (no snapshot yet — historic_snapshot_initialized_ guard).
    if (!historic_snapshot_initialized_) return;

    for (auto& kv : solo_qs) {
        for (auto& p : kv.second->global_ladder()) {
            if (!p) continue;
            int prev_max_kills = 0;
            int prev_max_kd    = 0;
            int prev_gf_clutch = 0;
            auto k_it = max_kills_snapshot_at_year_start_.find(p.get());
            auto d_it = max_kd_snapshot_at_year_start_.find(p.get());
            auto g_it = gf_clutches_snapshot_at_year_start_.find(p.get());
            if (k_it != max_kills_snapshot_at_year_start_.end()) prev_max_kills = k_it->second;
            if (d_it != max_kd_snapshot_at_year_start_.end()) prev_max_kd = d_it->second;
            if (g_it != gf_clutches_snapshot_at_year_start_.end()) prev_gf_clutch = g_it->second;

            // 30+ kill match (new this year)
            if (p->career_max_match_kills >= 30 && p->career_max_match_kills > prev_max_kills) {
                std::string key = "historic/kills/" + player_key(*p) + "/"
                                + std::to_string(year);
                if (!news_emitted_keys_.count(key)) {
                    NewsItem n;
                    n.year = year; n.day_in_year = day_in_year;
                    n.category = "Historic";
                    char hl[180];
                    std::snprintf(hl, sizeof(hl),
                        "%s drops %d in a pro match — career high",
                        p->name.c_str(), p->career_max_match_kills);
                    n.headline = hl;
                    n.body = "A single-match performance the highlight reels will be replaying for years.";
                    n.player_name = p->name;
                    n.team_name = p->team_name;
                    push_news(n);
                    news_emitted_keys_.insert(key);
                }
            }
            // 2.0+ KD match (new this year)
            if (p->career_max_match_kd_x100 >= 200 && p->career_max_match_kd_x100 > prev_max_kd) {
                std::string key = "historic/kd/" + player_key(*p) + "/"
                                + std::to_string(year);
                if (!news_emitted_keys_.count(key)) {
                    NewsItem n;
                    n.year = year; n.day_in_year = day_in_year;
                    n.category = "Historic";
                    char hl[180];
                    std::snprintf(hl, sizeof(hl),
                        "%s posts a %.2f KD performance — career best",
                        p->name.c_str(), p->career_max_match_kd_x100 / 100.0);
                    n.headline = hl;
                    n.body = "Two-to-one frags-to-deaths — the kind of game that closes a series.";
                    n.player_name = p->name;
                    n.team_name = p->team_name;
                    push_news(n);
                    news_emitted_keys_.insert(key);
                }
            }
            // Grand-final clutch (incremented this year)
            if (p->career_grand_final_clutches > prev_gf_clutch) {
                std::string key = "historic/gf_clutch/" + player_key(*p) + "/"
                                + std::to_string(year);
                if (!news_emitted_keys_.count(key)) {
                    NewsItem n;
                    n.year = year; n.day_in_year = day_in_year;
                    n.category = "Historic";
                    n.headline = p->name + " clutches a Grand Final round";
                    n.body = "The kind of moment that gets archived in the highlight reels forever.";
                    n.player_name = p->name;
                    n.team_name = p->team_name;
                    push_news(n);
                    news_emitted_keys_.insert(key);
                }
            }
        }
    }
}

void GameManager::emit_dynasty_watch() {
    // (10) Year-end. Team won >= 3 of {regional, masters, world} in last 5 years.
    // Read trophy_case().ordered (Agent B contract: vector<pair<int,string>>).
    for (auto& kv : leagues) {
        for (auto& t : kv.second->teams()) {
            if (!t) continue;
            const auto& tc = t->trophy_case();
            int recent_titles = 0;
            for (auto& entry : tc.ordered) {
                if (entry.first <= year && entry.first >= year - 4) {
                    ++recent_titles;
                }
            }
            if (recent_titles < 3) continue;

            std::string key = "dynasty/" + t->name + "/" + std::to_string(year);
            if (news_emitted_keys_.count(key)) continue;

            NewsItem n;
            n.year = year; n.day_in_year = day_in_year;
            n.category = "Dynasty";
            char hl[200];
            std::snprintf(hl, sizeof(hl),
                "Dynasty watch: %s chasing fourth championship in five years",
                t->name.c_str());
            n.headline = hl;
            char body[260];
            std::snprintf(body, sizeof(body),
                "%d titles in 5 years — %s is officially in the dynasty conversation.",
                recent_titles, t->name.c_str());
            n.body = body;
            n.team_name = t->name;
            push_news(n);
            news_emitted_keys_.insert(key);
        }
    }
}

void GameManager::snapshot_year_end_state() {
    max_kills_snapshot_at_year_start_.clear();
    max_kd_snapshot_at_year_start_.clear();
    gf_clutches_snapshot_at_year_start_.clear();

    // Shift retirement-countdown ratings: 2yo <- 1yo, 1yo <- current season rating.
    std::unordered_map<Player*, double> new_two_years_ago = last_year_rating_snapshot_;
    std::unordered_map<Player*, double> new_last_year;
    for (auto& kv : solo_qs) {
        for (auto& p : kv.second->global_ladder()) {
            if (!p) continue;
            max_kills_snapshot_at_year_start_[p.get()] = p->career_max_match_kills;
            max_kd_snapshot_at_year_start_[p.get()] = p->career_max_match_kd_x100;
            gf_clutches_snapshot_at_year_start_[p.get()] = p->career_grand_final_clutches;
            // Use p->history.back() — save_history_and_progress already
            // appended this year's MatchHistoryEntry (and reset season_*),
            // so reading p->history.back().rating gives the just-completed
            // season's rating without relying on the now-cleared counters.
            if (!p->history.empty() && p->history.back().year == year) {
                new_last_year[p.get()] = p->history.back().rating;
            }
        }
    }
    last_year_rating_snapshot_ = std::move(new_last_year);
    two_years_ago_rating_snapshot_ = std::move(new_two_years_ago);
    historic_snapshot_initialized_ = true;
}

}  // namespace vlr
