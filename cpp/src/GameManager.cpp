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
    // Stamp the global world year up front so every contract stamped during
    // world generation (draft, auto-fill) resolves a correct exp_year even on
    // paths that don't thread a year explicitly.
    set_current_world_year(year);
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

            team->head_scout = generate_scout(r);   // WS-A: every club starts with a scout
            team->head_scout->team_name = team->name;
            team->head_scout->contract_exp_year = year + team->head_scout->contract_years - 1;
            team->head_analyst = generate_analyst(r);   // Match-Prep: + an analyst
            team->head_analyst->team_name = team->name;
            team->head_analyst->contract_exp_year = year + team->head_analyst->contract_years - 1;

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

        auto league = std::make_shared<League>("VCT " + r, region_teams, r, 1, "VCT");
        league->generate_schedule();
        leagues[r] = league;
        tier_leagues_[r].push_back(league);   // index 0 aliases leagues[r]
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
            t->identity = compute_team_identity(*t);   // WS-B: derive at world init
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

    // Generate the lower-division pyramid (Challengers, ...) from the leftover
    // regional FA pool now that tier-1 rosters are drafted. Lives in
    // tier_leagues_ (NOT `leagues`), so every region-keyed tier-1 call site is
    // untouched. tier_leagues_[region][0] already aliases leagues[region].
    generate_lower_tiers(draft_log);

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

    // ROLE LOCK migration: stamp contract_role on every currently-rostered
    // tier-1 player whose role isn't yet locked (contract_role == Count) so it
    // is fixed to the role they were generated/drafted into. Idempotent —
    // players placed via sign_player already carry a stamped role. Lower-tier
    // players self-heal on their next sign_player/resign_player.
    for (auto& kv : leagues) {
        if (!kv.second) continue;
        for (auto& t : kv.second->teams()) {
            if (!t) continue;
            for (auto& p : t->roster)
                if (p && p->contract_role == Role::Count)
                    p->contract_role = p->primary_role;
        }
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
    intl_qualifiers_.clear();   // WS-C: per-world regional qualifier placements
    tier_leagues_.clear();
    last_promo_rel_.clear();
    t2_week = 0;

    user_team.reset();
    user_auto_manage = false;
    user_team_talk = 0;
    user_philosophy = ClubPhilosophy::Emergent;   // WS-B
    scout_assignments_.clear();                    // scouting queue is per-world
    scout_briefs_.clear();                         // filter briefs are per-world
    scout_report_accuracy_.clear();

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
    watchlist_.clear();          // increment C: transfer/scouting targets
    scout_focus_ = ScoutFocus::None;   // increment D: clear scout assignment
    scout_focus_region_.clear();
    scout_focus_role_ = Role::Count;
    hall_of_fame.clear();
    recent_progression_reports.clear();

    // Awards / news / race state.
    awards_history.clear();
    last_season_awards.clear();
    news_feed.clear();
    news_pushed_events_.clear();
    mvp_race.clear();

    // Mail / inbox — a brand-new career starts with an empty inbox.
    mailbox.clear();
    mail_emitted_keys_.clear();
    next_mail_id_ = 1;
    current_board_objective_.clear();
    board_target_placement_ = 0;
    board_ambition_ = 0.0;
    board_objectives_.clear();   // dynamic objective set (Group E)
    free_coaches_.clear();       // don't leak the prior world's FA staff pools
    free_scouts_.clear();        // WS-A: (free_coaches_ was a pre-existing leak)
    free_analysts_.clear();      // Match-Prep: analyst pool
    retired_coaches_.clear();    // WS-B never-free retention pools (per-world)
    retired_scouts_.clear();
    retired_analysts_.clear();
    // Preseason buffer + sponsor-choice state (items 2 + 4).
    in_preseason_buffer_ = false;
    preseason_days_left_ = 0;
    sponsor_choice_pending_ = false;
    pending_sponsor_offers_.clear();

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
        // Stamp EVERY head staffer, not just the coach — a stale scout/analyst
        // team_name made them read as "not your club" (blocking god-mode edits).
        if (user_team->head_coach)   user_team->head_coach->team_name   = user_team->name;
        if (user_team->head_scout)   user_team->head_scout->team_name   = user_team->name;
        if (user_team->head_analyst) user_team->head_analyst->team_name = user_team->name;
        // The typed name is EXCLUSIVE — a generated club anywhere in the pyramid
        // that already carries it gets a fresh identity (else brackets show two
        // clubs with one name, e.g. "playing Mirage in Americas AND EMEA").
        resolve_user_name_collision();
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

    // 9. Preseason onboarding (items 2-4): open a ~1.5-week no-match buffer so the
    //    user can review their squad, staff, strategy and the league table, and
    //    make roster/transfer changes before match 1; then fire the board email +
    //    the 3-offer sponsor choice so both are waiting the moment they land.
    in_preseason_buffer_ = true;
    preseason_days_left_ = 10;
    start_of_season_setup(log);

    // 10. Seed a staff FREE-AGENT market so the user can hire coaches/scouts/analysts
    //     from day 1. The AI staff markets only refill at YEAR-END (run_*_market), so
    //     a fresh career otherwise opens with an empty market. Wizard path only — the
    //     default/test/dynasty world (initialize_world) is untouched, preserving the
    //     --dynasty stream.
    for (const std::string rg : {std::string("Americas"), std::string("EMEA"), std::string("Pacific")}) {
        for (int i = 0; i < 12; ++i) {
            free_coaches_.push_back(generate_coach(rg));
            free_scouts_.push_back(generate_scout(rg));
            free_analysts_.push_back(generate_analyst(rg));
        }
    }

    log.emplace_back("[NewGame] World seeded for year " + std::to_string(year)
                     + " — user_team '" + user_team->name + "' in " + region
                     + ", budget $" + std::to_string(budget / 1000) + "K"
                     + ", prestige " + std::to_string(prestige)
                     + ", difficulty " + std::to_string(world_difficulty_));
    return true;
}

// === Tournament prize purses ============================================
// {champion, runner-up} payout in $K by event tier. Matched on the same
// name fragments every other name consumer uses (push_tournament_news,
// award_event_titles): international phase names are ALL-CAPS ("MASTERS 1",
// "CHAMPIONS"), regionals read "<Region> Regionals <n>". Fixed world rule —
// applied to every team identically, no RNG.
static std::pair<int, int> tournament_purse_k(const std::string& name) {
    if (name.find("CHAMPIONS") != std::string::npos) return {400, 180};
    if (name.find("MASTERS")   != std::string::npos) return {250, 110};
    return {120, 50};   // regional playoffs
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
        // Prize money — backstop for brackets that only finish here. The
        // prize_paid fuse makes this a no-op for anything already paid in
        // play_tournament_round's natural finished block. Both finalists
        // are identifiable on this path (champion() / runner_up()).
        if (!tour->prize_paid && tour->champion()) {
            auto purse = tournament_purse_k(tour->name());
            const TeamPtr& pc = tour->champion();
            const TeamPtr& pr = tour->runner_up();
            pc->budget += static_cast<long long>(purse.first) * 1000LL;
            mail_user(pc->name,
                      "mail/prize/champ/" + tour->name() + "/" + std::to_string(year),
                      MailCategory::Result,
                      "Prize money: $" + std::to_string(purse.first) + "K \xE2\x80\x94 " + tour->name(),
                      "The $" + std::to_string(purse.first) + "K champion's purse from " +
                          tour->name() + " has been credited to the club budget.",
                      MailLink::Finance, "", purse.first, false);
            if (pr) {
                pr->budget += static_cast<long long>(purse.second) * 1000LL;
                mail_user(pr->name,
                          "mail/prize/runnerup/" + tour->name() + "/" + std::to_string(year),
                          MailCategory::Result,
                          "Prize money: $" + std::to_string(purse.second) + "K \xE2\x80\x94 " + tour->name(),
                          "The $" + std::to_string(purse.second) + "K runner-up purse from " +
                              tour->name() + " has been credited to the club budget.",
                          MailLink::Finance, "", purse.second, false);
            }
            tour->prize_paid = true;
        }
        // Record trophy on the champion's team. Defensive against double-
        // recording: if the same tournament was already finished and news-
        // pushed in play_tournament_round, push_tournament_news's dedup
        // short-circuits, but record_trophy has no built-in dedup —
        // Agent B's storage layer is expected to dedup on (year, name).
        if (tour->champion() && tour->awards_title()) {
            tour->champion()->record_trophy(year, tour->name());
        }
        // Dynasty tiers (P6.1): record a FINALS APPEARANCE for BOTH finalists,
        // gated like the trophy so non-awarding regionals (2/3) don't double-
        // count. Idempotent on (year, name) -> unlocks ChampionEra/Contender.
        if (tour->awards_title()) {
            if (tour->champion())  tour->champion()->record_finals_appearance(year, tour->name());
            if (tour->runner_up()) tour->runner_up()->record_finals_appearance(year, tour->name());
        }
        // WS-C: record regional playoff PLACEMENT for international seeding. NOT
        // awards-gated — REGIONALS 2/3 don't crown a titled champ but still
        // qualify teams for Masters 2 / Champions.
        capture_regional_qualifiers(*tour);
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

void GameManager::capture_regional_qualifiers(const Tournament& tour) {
    if (tour.name().find("Regionals") == std::string::npos) return;
    const TeamPtr& champ = tour.champion();
    if (!champ) return;
    // Placement order [1st, 2nd, 3rd, 4th]. semifinalists() holds the two SF
    // losers in ELIMINATION order = [4th, 3rd] (Tournament.cpp:288-295), so
    // reverse-iterate to append 3rd then 4th.
    std::vector<TeamPtr> placed;
    placed.push_back(champ);
    const TeamPtr& ru = tour.runner_up();
    if (ru) placed.push_back(ru);
    const auto& sf = tour.semifinalists();
    for (auto it = sf.rbegin(); it != sf.rend(); ++it)
        if (*it && *it != champ && *it != ru) placed.push_back(*it);
    intl_qualifiers_[champ->region] = std::move(placed);
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
            // Append the split number ("Americas Regionals 1") so per-tournament
            // stat scoping (item 1) collapses this regional PLAYOFF with its
            // STAGE league phase ("VCT Americas Stage 1") into ONE tournament
            // bucket. Every name consumer uses find("Regionals"), so the suffix
            // is matcher-safe; it also reads cleaner in brackets/news.
            std::string reg_split;
            for (auto rit = phase.rbegin(); rit != phase.rend(); ++rit)
                if (*rit >= '0' && *rit <= '9') { reg_split = std::string(1, *rit); break; }
            std::string reg_name = kv.first + " Regionals"
                                 + (reg_split.empty() ? std::string() : " " + reg_split);
            auto reg_tour = std::make_shared<Tournament>(reg_name, top, fmt);
            // "Regional Champs" only applies to the FIRST regional of the year.
            // REGIONALS 2 / REGIONALS 3 still run (seeding for Masters/Champions)
            // but crown no titled champion — no award, no team trophy.
            reg_tour->set_awards_title(phase.find("REGIONALS 1") != std::string::npos);
            active_tournaments.push_back(std::move(reg_tour));
        }
    } else if ((phase.find("MASTERS") != std::string::npos || phase.find("CHAMPIONS") != std::string::npos)
               && active_tournaments.empty()) {
        // WS-C international qualification: seeded ENTIRELY by REGIONAL PLAYOFF
        // PLACEMENT (captured in intl_qualifiers_ when each region's regionals
        // finished). Masters = top 3 per region (9 teams, 2 groups 5+4 -> 4-team
        // bracket); Champions = top 4 per region (12 teams, 4 groups of 3 ->
        // 8-team bracket). No more "top-2-by-season-wins + best-OVR fill" — a
        // team must EARN it in the regional playoff.
        const bool is_champs = (phase.find("CHAMPIONS") != std::string::npos);
        const std::size_t per_region = is_champs ? 4 : 3;
        std::vector<TeamPtr> globals;
        for (auto& kv : leagues) {
            auto qit = intl_qualifiers_.find(kv.first);
            if (qit != intl_qualifiers_.end() && !qit->second.empty()) {
                for (std::size_t i = 0; i < std::min(per_region, qit->second.size()); ++i)
                    if (qit->second[i]) globals.push_back(qit->second[i]);
            } else {
                // Fallback (no regional captured yet — shouldn't happen since a
                // REGIONALS phase always precedes the international): top-N by
                // season wins, so the field is never empty.
                std::vector<TeamPtr> sorted_teams;
                for (auto& t : kv.second->teams()) if (t) sorted_teams.push_back(t);
                std::sort(sorted_teams.begin(), sorted_teams.end(),
                          [](const TeamPtr& a, const TeamPtr& b) { return a->wins > b->wins; });
                for (std::size_t i = 0; i < std::min(per_region, sorted_teams.size()); ++i)
                    globals.push_back(sorted_teams[i]);
            }
        }
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
        // World difficulty scales the AI side(s); the user's team stays at 1.0
        // so higher difficulty makes the user's opponents tougher. AI-vs-AI
        // matches scale both sides equally -> no relative change. This is the
        // callsite that finally makes world_difficulty_ (set by the New Game
        // wizard) actually affect play.
        m.set_strength_mults(a == user_team ? 1.0 : world_difficulty_,
                             b == user_team ? 1.0 : world_difficulty_);
        // Increment E: the user's per-map PREP edge applies ONLY to their own
        // competitive matches. AI-vs-AI (and the dynasty sim) never set this, so
        // prep tilt stays a strict 1.0 there and the balance bands are untouched.
        if (user_team && (a == user_team || b == user_team)) {
            double aq = user_team->head_analyst ? user_team->head_analyst->quality01() : 0.0;
            m.set_prep_context(user_team.get(), aq);
            // TEAM TALK (M3c): a one-shot pre-match edge whose size depends on
            // reading the matchup right — Calm pays when you're the favorite,
            // Fire-up when you're the underdog, Focus is a safe small edge.
            // User-only (dynasty never sets user_team_talk → stream untouched).
            if (user_team_talk != 0) {
                const TeamPtr& opp = (a == user_team) ? b : a;
                bool favored = user_team->ovr() >= opp->ovr();
                double talk = 1.0;
                if      (user_team_talk == 1) talk = favored ? 1.025 : 1.005;
                else if (user_team_talk == 2) talk = favored ? 1.005 : 1.025;
                else if (user_team_talk == 3) talk = 1.015;
                m.set_strength_mults((a == user_team ? talk : world_difficulty_),
                                     (b == user_team ? talk : world_difficulty_));
            }
        }
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
    // Team talk is ONE-SHOT: consumed by the user's series (all maps), then cleared.
    if (user_team && (a == user_team || b == user_team)) user_team_talk = 0;
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
        // WS-C bug-2: track whether the USER won a series THIS round (and whom
        // they beat) so we can fire a "reached the Grand Final" heads-up the
        // moment they qualify, before the final is played.
        bool user_won_series = false;
        std::string user_beat;
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
                if (s.winner() && s.winner().get() == user_team.get()) {
                    user_won_series = true;
                    user_beat = out_user_opp;
                }
                char dbg[160];
                std::snprintf(dbg, sizeof(dbg),
                    "  >> Your match: vs %s -> %d-%d (%zu maps captured)",
                    out_user_opp.c_str(), s.t1_wins(), s.t2_wins(),
                    out_user_series.size());
                log.emplace_back(dbg);
            }
        }
        // WS-C bug-2: "reached the Grand Final" heads-up the MOMENT the user wins
        // the qualifying series — before the final is played. The bracket sets up
        // the GF (in_grand_final true) one round BEFORE playing it, so this window
        // exists. If the user won this round, the tournament is now in the GF and
        // not yet decided, and the user isn't eliminated, they're a finalist.
        if (user_team && user_won_series && tour->in_grand_final() && !tour->finished()) {
            bool user_eliminated = false;
            for (auto& e : tour->eliminated())
                if (e.get() == user_team.get()) { user_eliminated = true; break; }
            if (!user_eliminated) {
                std::string ev   = tour->name();
                std::string head = "Through to the Grand Final of " + ev + "!";
                std::string body = "Your club beat " + user_beat + " to reach the Grand "
                                   "Final of " + ev + " \xE2\x80\x94 one series from the title.";
                NewsItem n;
                n.year = year; n.day_in_year = day_in_year;
                n.category = "Result";
                n.headline = head;
                n.body = body;
                n.team_name = user_team->name;
                push_news(n);
                mail_user(user_team->name,
                          "mail/gf_reached/" + ev + "/" + std::to_string(year),
                          MailCategory::Result, head, body,
                          MailLink::None, "", 0, true);
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
            // Prize money — paid exactly once per tournament. The prize_paid
            // fuse also guards the force_finish_stale_tournaments backstop,
            // which re-walks finished brackets at the phase boundary.
            if (!tour->prize_paid && champ) {
                auto purse = tournament_purse_k(tour->name());
                champ->budget += static_cast<long long>(purse.first) * 1000LL;
                mail_user(champ->name,
                          "mail/prize/champ/" + tour->name() + "/" + std::to_string(year),
                          MailCategory::Result,
                          "Prize money: $" + std::to_string(purse.first) + "K \xE2\x80\x94 " + tour->name(),
                          "The $" + std::to_string(purse.first) + "K champion's purse from " +
                              tour->name() + " has been credited to the club budget.",
                          MailLink::Finance, "", purse.first, false);
                if (ru) {
                    ru->budget += static_cast<long long>(purse.second) * 1000LL;
                    mail_user(ru->name,
                              "mail/prize/runnerup/" + tour->name() + "/" + std::to_string(year),
                              MailCategory::Result,
                              "Prize money: $" + std::to_string(purse.second) + "K \xE2\x80\x94 " + tour->name(),
                              "The $" + std::to_string(purse.second) + "K runner-up purse from " +
                                  tour->name() + " has been credited to the club budget.",
                              MailLink::Finance, "", purse.second, false);
                }
                tour->prize_paid = true;
            }
            // Record the trophy on the champion's team trophy_case so the
            // dynasty / community-rankings emitters can read a coherent
            // history. Agent B is implementing Team::record_trophy + the
            // backing storage; we just have to call it once per finish.
            if (champ && tour->awards_title()) {
                champ->record_trophy(year, tour->name());
            }
            // Dynasty tiers (P6.1): finals appearance for BOTH finalists,
            // awards-gated + idempotent (mirrors the force-finish path).
            if (tour->awards_title()) {
                if (champ) champ->record_finals_appearance(year, tour->name());
                if (ru)    ru->record_finals_appearance(year, tour->name());
            }
            // WS-C: record regional playoff PLACEMENT for international seeding
            // (not awards-gated; mirrors the force-finish path).
            capture_regional_qualifiers(*tour);
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

    // Map each player to their team's coach (if any). Walk tier_leagues_ (ALL
    // divisions; index 0 aliases tier-1) so lower-tier players also get their
    // coach's development bonus instead of the no-coach 0.95x default.
    std::unordered_map<Player*, const Coach*> coach_for;
    for (auto& kv : tier_leagues_) {
        for (auto& lg : kv.second) {
            if (!lg) continue;
            for (auto& t : lg->teams()) {
                if (!t) continue;
                const Coach* c = t->head_coach.get();
                for (auto& p : t->roster) coach_for[p.get()] = c;
            }
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

    // Progress any commissioned scouting assignments by a day (user-only queue;
    // a no-op on the dynasty/test path where the queue is always empty).
    tick_scout_assignments();
    tick_scout_briefs();     // FM-style filter briefs (also empty/no-op on dynasty path)

    // === Preseason buffer (item 2) — boot-only no-match onboarding days ===
    // After team creation the user gets a few days to read the board email,
    // pick a sponsor, and finalize the roster before Stage 1. We do NOT advance
    // day_in_year here (the season clock starts at day 0 once Stage 1 begins),
    // so the 200-day calendar is untouched; only day_total ticks.
    if (in_preseason_buffer_) {
        r.was_matchday = false;
        r.label = "Preseason — review board expectations, choose a sponsor, set your roster";
        if (preseason_days_left_ > 0) --preseason_days_left_;
        if (preseason_days_left_ <= 0) {
            in_preseason_buffer_ = false;
            // Safety net: enter Stage 1 with a full starting 5 (same AI fallback
            // the offseason uses) if the user left the roster short.
            if (user_team && user_team->roster.size() < 5) {
                auto sit = solo_qs.find(user_team->region);
                if (sit != solo_qs.end() && sit->second) {
                    auto fa_pool = sit->second->global_ladder();
                    user_team->ai_full_offseason_pass(fa_pool, year, log);
                }
            }
        }
        ++day_total;
        return r;
    }

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
                    // per-move logging. Same depth all AI teams now use. `year`
                    // is already the upcoming season here (run_end_of_year did
                    // year += 1 at the AWARDS tick that precedes OFFSEASON), so
                    // signings get a correct exp_year.
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
            t2_week = 0;
            day_in_year = 0;
            r.label = "New season starting!";
            // Season start (seasons 2+): board objective + expiry warnings AND
            // the new 3-offer sponsor choice (year is already the upcoming
            // season here). Season 1 is handled by the preseason buffer.
            start_of_season_setup(log);
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
            // Simulate the lower-tier (Challengers) round-robin on the same
            // stage matchday cadence (separate t2_week cursor).
            play_tier_stage_round();
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
            t2_week = 0;   // lower tiers replay their round-robin each stage
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
            if (!t) continue;
            // The USER's team is never auto-managed mid-season unless they opted
            // into auto-manage (this guard also covers the cut/replace logic
            // below, which previously had NO user gate — user rosters got
            // auto-cut without consent).
            const bool manage = (t != user_team || user_auto_manage);
            // Mid-season MERIT benching: promote a clearly-better reserve over a
            // slumping same-role starter (optimize_starting_five enforces a form
            // sample, prospect protection, and a hysteresis margin so it won't
            // thrash on monthly noise). This is what makes the AI actually
            // START its best 5 in-season, not just at the offseason.
            if (manage) t->optimize_starting_five(year);
            if (!t->head_coach) continue;   // the cut/replace logic needs a coach
            if (!manage) continue;          // never auto-cut the user's team
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
    // Mail dedup clears at year rollover too, so the same event (e.g. a board
    // objective, a transfer request) can mail again in a future season. Board
    // keys embed the year, so this never resurrects a stale objective.
    mail_emitted_keys_.clear();

    // Compute awards FIRST, before save_history_and_progress resets the
    // season counters. Awards are computed against season_rating_total etc.
    compute_season_awards(log);

    // Year-end finance + coach growth/aging for EVERY division (tier_leagues_;
    // index 0 aliases tier-1). Previously this walked `leagues` (tier-1 only),
    // so lower-tier teams never paid payroll nor collected sponsorship — their
    // budgets only grew, an economic asymmetry vs tier-1. Walking tier_leagues_
    // processes each team exactly once and gives lower tiers the same economy.
    for (auto& region_kv : tier_leagues_) {
        for (std::size_t ti = 0; ti < region_kv.second.size(); ++ti) {
            auto& lg = region_kv.second[ti];
            if (!lg) continue;
            // Tier index: ti==0 = tier-1 (VCT), 1 = Challengers, 2 = Open. Lower
            // tiers run smaller operations, so scale op-cost down to match their
            // (lower-prestige) income and give them a soft budget floor.
            int tier = static_cast<int>(ti) + 1;
            static const double kTierOpMult[3] = { 1.0, 0.55, 0.35 };
            double op_mult = kTierOpMult[std::min(tier - 1, 2)];
            for (auto& t : lg->teams()) {
                if (!t) continue;
                // Sponsorship + base revenue paid at year-start. Sponsorship
                // scales with prestige; high-prestige orgs net more income.
                long long revenue = static_cast<long long>(t->sponsorship_k) * 1000LL;
                // P3.2: difficulty gives the USER ONLY a small economic head/tail-
                // wind (Easy = more income, Hard = less). Clamped [0.9,1.1] so it
                // never warps the economy; AI income is untouched; no-op at Normal.
                if (t == user_team) {
                    double econ = clamp_v(2.0 - world_difficulty(), 0.9, 1.1);
                    revenue = static_cast<long long>(revenue * econ);
                }
                t->budget += revenue;
                t->pay_payroll(year);
                // === Operating costs + soft budget cap (economy balance) ====
                // Without a recurring sink the only expense is payroll, while
                // income (sponsorship + per-round stage stipends) far exceeds
                // it — so every team's budget grew unbounded over a long career
                // and the budget-tier signing AI collapsed to "everyone can
                // afford anyone". Operating costs (facilities, staff, travel,
                // bootcamps) scale with prestige so running a big-name org is
                // genuinely expensive and roughly offsets sponsorship; the soft
                // cap shaves runaway cash so a dynasty can't snowball forever.
                // Tuned so a stable org nets modestly POSITIVE each year (cash
                // to chase upgrades) while prestige still buys a real edge
                // (sponsorship grows 8/prestige, op-cost only 4/prestige), and
                // the soft cap below stops runaway accumulation. Earlier
                // (180+7*prestige) over-corrected — teams bled into the red.
                long long op_cost = static_cast<long long>(
                    (150LL + static_cast<long long>(t->prestige) * 4LL) * 1000LL * op_mult);
                t->budget -= op_cost;
                const long long kBudgetSoftCap = 3000000LL;
                if (t->budget > kBudgetSoftCap)
                    t->budget = kBudgetSoftCap + (t->budget - kBudgetSoftCap) / 2;
                // Lower-tier soft FLOOR (tier-1 left UNCHANGED so its economics
                // and smoke [23] tier-1 band are unaffected): a bad Challengers/
                // Open year hurts but can't cascade into runaway insolvency.
                if (tier > 1) {
                    const long long kLowerFloor = -200000LL;
                    if (t->budget < kLowerFloor)
                        t->budget = kLowerFloor + (t->budget - kLowerFloor) / 2;
                }
                if (t->head_coach) {
                    Coach& c = *t->head_coach;
                    // FM-depth: evolve the coach's REPUTATION from this season's
                    // silverware + win record, with a slow baseline decay (a name
                    // fades without results). Recompute salary off the new rep.
                    int titles = t->titles_in_year(year);
                    int net_w  = t->wins - t->losses;
                    c.reputation = clamp_v(c.reputation + titles * 6 + (net_w > 0 ? 2 : -2) - 1, 1, 99);
                    c.career_titles += titles;
                    c.career_seasons += 1;
                    c.salary_k = c.requested_salary_k();
                    // Player development, weighted by the coach's SIGNATURE
                    // dev_focus (a DevelopmentCoach grows youth harder than a
                    // win-now Pragmatist).
                    //
                    // WS-B INC-3 hardening:
                    //  * POTENTIAL-CEILING BUG FIX — gate every roll on
                    //    `ovr() < potential`. apply_attribute_delta only clamps
                    //    to [0,99], so the old un-gated roll silently grew
                    //    players PAST their scouted potential over a career.
                    //  * Chance clamped to [0,0.45] and total gains capped at 3
                    //    attribute points / player / season so even a monster
                    //    dev coach helps without runaway roster inflation.
                    double dev_focus = t->coach_lean().dev_focus;
                    double base = c.dev_growth_factor();
                    for (auto& p : t->roster) {
                        if (!p) continue;
                        // POTENTIAL-CEILING BUG FIX (kept): apply_attribute_delta
                        // only clamps to [0,99], so an un-gated roll silently grew
                        // players PAST their scouted potential over a career. Gate
                        // the roll on real headroom.
                        if (p->ovr() >= p->potential) continue;
                        double ch = base * (1.0 + 0.4 * dev_focus);
                        if (p->age <= 23 && dev_focus > 0.0) ch *= (1.0 + 0.3 * dev_focus);
                        ch = clamp_v(ch, 0.0, 0.45);
                        // SINGLE roll per player per season (one attribute max).
                        // A multi-roll version (up to 3/yr) tripled youth growth
                        // and let the best-run clubs compound into entrenched
                        // dynasties (--dynasty back-to-back blew out 5%->35%), so
                        // dev throughput is held at the tuned baseline. The coach
                        // still matters via dev_growth_factor + the potential gate;
                        // their value is surfaced in the readout, not inflated here.
                        if (rng().chance(ch)) {
                            Attr a = static_cast<Attr>(rng().irange(0, static_cast<int>(kAttrCount) - 1));
                            p->apply_attribute_delta(a, 1);
                        }
                    }
                    c.experience = std::min(99, c.experience + 1);
                    c.age += 1;
                    if (c.age >= 60 && rng().chance(0.25)) {
                        c.is_retired = true;
                        // NEVER-FREE: retain the retiring coach (career_titles /
                        // career_coty history) before dropping the team link, so
                        // its last shared_ptr is never released mid-world. Mirrors
                        // retired players persisting in the solo_q ladders.
                        c.team_name = "Retired";
                        retired_coaches_.push_back(t->head_coach);
                        t->head_coach.reset();
                    }
                }
                // WS-A: scout reputation/aging, parallel to the coach above. A
                // winning club's scout slowly gains standing; all fade a touch
                // without results (career_finds boosts come at discovery time).
                if (t->head_scout) {
                    Scout& sc = *t->head_scout;
                    int net_w = t->wins - t->losses;
                    sc.reputation = clamp_v(sc.reputation + (net_w > 0 ? 2 : -2) - 1, 1, 99);
                    sc.career_seasons += 1;
                    sc.salary_k = sc.requested_salary_k();
                    sc.experience = std::min(99, sc.experience + 1);
                    sc.age += 1;
                    if (sc.age >= 60 && rng().chance(0.25)) {
                        sc.is_retired = true;
                        // NEVER-FREE: retain the retiring scout before dropping the
                        // team link (parallel to the coach path above).
                        sc.team_name = "Retired";
                        retired_scouts_.push_back(t->head_scout);
                        t->head_scout.reset();
                    }
                }
                // Match-Prep: analyst reputation/aging, parallel to the scout above.
                if (t->head_analyst) {
                    Analyst& an = *t->head_analyst;
                    int net_w = t->wins - t->losses;
                    an.reputation = clamp_v(an.reputation + (net_w > 0 ? 2 : -2) - 1, 1, 99);
                    an.career_seasons += 1;
                    an.career_reports += std::max(0, t->wins + t->losses);  // a season of prep filed
                    an.salary_k = an.requested_salary_k();
                    an.experience = std::min(99, an.experience + 1);
                    an.age += 1;
                    if (an.age >= 60 && rng().chance(0.25)) {
                        an.is_retired = true;
                        an.team_name = "Retired";
                        retired_analysts_.push_back(t->head_analyst);
                        t->head_analyst.reset();
                    }
                }
                // FM-depth finance: book this pass's income as last_revenue_k.
                t->last_revenue_k = t->sponsorship_k;
                // Per-season net-transfer spend resets here — the offseason's
                // scouting-pass fees (which run later in run_end_of_year)
                // accumulate into it, so "NET XFER" reflects ONE season.
                t->net_transfer_k = 0;
                // Prestige + DYNAMIC sponsorship for next season, applied to ALL
                // tiers here (lower tiers were previously static forever, and the
                // tier-1 path used to do this in the offseason loop). Uses this
                // season's W/L (reset later in the offseason loop).
                t->prestige = clamp_v(t->prestige + (t->wins - t->losses) / 3, 10, 99);
                apply_dynamic_sponsorship(*t, tier, year);
                // Project next season's committed payroll / income / wage envelope
                // / wealth tier (read-only signals the AI spend paths gate on).
                project_team_finances(*t, tier, year);
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

    int contracts_expired = 0;
    for (auto& kv : solo_qs) {
        for (auto& p : kv.second->global_ladder()) {
            p->save_history_and_progress(year);
            // Transfer-request news: a player who just handed one in this year
            // is shopped. (The AI release of the most disgruntled players, which
            // routes them to FA for the B8 market, happens in the team cleanup
            // pass below where the Team object is in hand.)
            if (p->transfer_requested && p->transfer_request_year == year
                && p->team_name != "Free Agent" && p->team_name != "Retired") {
                NewsItem n;
                n.year = year; n.day_in_year = day_in_year;
                n.category = "Roster Move";
                n.headline = p->name + " hands in a transfer request at " + p->team_name;
                n.body = "Unhappy with playing time / results, the player wants a move.";
                n.team_name = p->team_name; n.player_name = p->name;
                push_news(n);
                mail_user(p->team_name,
                          "mail/transfer_req/" + p->name + "/" + std::to_string(year),
                          MailCategory::Squad,
                          p->name + " has requested a transfer",
                          p->name + " has formally asked to leave. " +
                              (p->restlessness >= 0.55
                                  ? "Despite the team's results, they feel they've "
                                    "outgrown the project and crave a bigger stage \xE2\x80\x94 "
                                    "the kind of itch that breaks up winning cores. "
                                  : "Frustrated by playing time and a difficult run, "
                                    "they want a fresh start. ") +
                              "Open their profile to act: win them back with a new deal + "
                              "a guaranteed-starter promise, listen to bids from rivals, or "
                              "let it fester and risk the dressing room.",
                          MailLink::Player, p->name, 0, true, p->id);
            }
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
                    // Capture the club they're leaving BEFORE flipping
                    // team_name, so the send-off can credit their final team.
                    std::string old_team = p->team_name;
                    p->is_retired = true;
                    p->team_name = "Retired";

                    // === Retirement send-off ==============================
                    // The retirement event itself previously vanished silently
                    // (only the earlier "Retirement Watch" countdown surfaced).
                    // Per design: the send-off is reserved for LEGENDS — players
                    // who are Hall-of-Fame bound or just short of it. We reuse
                    // the exact HoF criteria so the bar is consistent: >= 5 is
                    // an inductee, == 4 is a near-miss "close to it" career.
                    // Both get a proper tribute; quiet role-player retirements
                    // pass without feed/mail spam.
                    int hof_pts = hof_criteria_count(*p);
                    int intl_titles = 0;
                    for (auto& a : p->awards)
                        if (a.find("[M]") != std::string::npos ||
                            a.find("[W]") != std::string::npos) ++intl_titles;
                    int honours = (int)p->awards.size();
                    bool notable = (p->career_seasons_played >= 3 && hof_pts >= 4);

                    if (notable) {
                        std::string honours_str;
                        if (intl_titles > 0)
                            honours_str = ", " + std::to_string(intl_titles)
                                        + "x international champion";
                        else if (honours > 0)
                            honours_str = ", " + std::to_string(honours)
                                        + " career honours";
                        NewsItem n;
                        n.year = year; n.day_in_year = day_in_year;
                        n.category = "Retirement";
                        n.headline = p->name + " retires after "
                                   + std::to_string(p->career_seasons_played)
                                   + " seasons";
                        const char* legacy_tail = (hof_pts >= 5)
                            ? " A Hall-of-Fame-bound career."
                            : " Fell just short of the Hall of Fame.";
                        char rb[360];
                        std::snprintf(rb, sizeof(rb),
                            "%s calls time on a %d-season career at %d years old "
                            "(%d pro matches, %.2f career rating%s). The %s scene "
                            "salutes one of its own.%s",
                            p->name.c_str(), p->career_seasons_played, p->age,
                            p->career_matches, p->avg_match_rating(),
                            honours_str.c_str(), p->region.c_str(), legacy_tail);
                        n.body = rb;
                        n.player_name = p->name;
                        n.team_name = old_team;
                        push_news(n);
                    }

                    // User mail — same legend bar as the public tribute, and
                    // only lands if it was their player (mail_user is a no-op
                    // for non-user clubs). Skip bare FAs with no club to credit.
                    if (notable && old_team != "Free Agent" && old_team != "Retired") {
                        mail_user(old_team,
                            "mail/retire/" + p->name + "/" + std::to_string(year),
                            MailCategory::Squad,
                            p->name + " has announced their retirement",
                            p->name + " is hanging up the mouse after "
                                + std::to_string(p->career_seasons_played)
                                + " seasons. Their locker is now open \xE2\x80\x94 "
                                "plan a replacement at the position before next "
                                "season.",
                            MailLink::Player, p->name, 0, true, p->id);
                    }

                    // Non-legend farewell — YOUR OWN quiet role players used
                    // to vanish silently below the legend bar. Ownership by
                    // roster id (player_on_user_team), never name comparison;
                    // same per-season dedup key family as the tribute above
                    // (the two branches are mutually exclusive).
                    if (!notable && player_on_user_team(p->id)) {
                        mail_user(old_team,
                            "mail/retire/" + p->name + "/" + std::to_string(year),
                            MailCategory::Squad,
                            p->name + " announces retirement",
                            p->name + " calls time after "
                                + std::to_string(p->career_seasons_played)
                                + " seasons. " + std::to_string(p->age)
                                + " years old. Their roster spot is now open.",
                            MailLink::Player, p->name, 0, false, p->id);
                    }
                }
            }
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
            // Stricter career floor: a HoF legend needs a sustained career,
            // not a 3-season flash (was < 3).
            if (p->career_seasons_played < 4) continue;
            if (std::find(hall_of_fame.begin(), hall_of_fame.end(), p) != hall_of_fame.end()) continue;

            // Criteria 1-10 computed by the shared helper so the induction bar
            // and the retirement send-off can never drift apart.
            int criteria = hof_criteria_count(*p);

            // Stricter gate: 5 of 10 accomplishments (was 4). Genuine stars
            // clear this comfortably; it trims borderline 4-criteria careers.
            if (criteria >= 5) {
                hall_of_fame.push_back(p);
                NewsItem n;
                n.year = year; n.day_in_year = day_in_year;
                n.category = "Awards";
                n.headline = p->name + " inducted into the Hall of Fame";
                char body[200];
                std::snprintf(body, sizeof(body),
                    "After %d seasons, %s meets %d of the HoF criteria (5 now required).",
                    p->career_seasons_played, p->name.c_str(), criteria);
                n.body = body;
                n.player_name = p->name;
                push_news(n);
                // HoF inductee whose LAST club was the user's -> a legacy mail.
                // Retired players carry team_name=="Retired", so resolve the
                // final club from the most recent season-history entry instead.
                std::string last_club = p->history.empty()
                    ? p->team_name : p->history.back().team;
                mail_user(last_club,
                          "mail/hof/" + p->name + "/" + std::to_string(year),
                          MailCategory::Award,
                          p->name + " inducted into the Hall of Fame",
                          std::string(body) + " A franchise legend who wore your "
                              "colors earns the game's highest honor.",
                          MailLink::Player, p->name, 0, true, p->id);
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
        mail_user(t->name, "mail/resign/" + p->name + "/" + std::to_string(year),
                  MailCategory::Contract,
                  "Deal done — " + p->name + " extends with " + t->name,
                  "Contract extension finalized: " + std::to_string(years) +
                      "y at $" + std::to_string(amount_k) + "K/yr. A key piece of "
                      "your core stays in place for another run.",
                  MailLink::Player, p->name, amount_k, false, p->id);
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
        mail_user(t->name, "mail/walk/" + p->name + "/" + std::to_string(year),
                  MailCategory::Contract,
                  p->name + " has left for free agency",
                  "Re-sign talks with " + p->name + " broke down and the player "
                      "has walked into free agency. You'll need to fill the gap "
                      "from the transfer market.",
                  MailLink::Market, p->name, 0, true, p->id);
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
        // Two guarded one-liners: fires for whichever side (if either) is the
        // user club. mail_user no-ops for the non-user side.
        mail_user(winner->name, "mail/poach_in/" + p->name + "/" + std::to_string(year),
                  MailCategory::Transfer,
                  "Signed: " + p->name + " joins " + winner->name,
                  "Your front office won the open-market race for " + p->name +
                      ". The new arrival slots into your depth chart.",
                  MailLink::Roster, p->name, 0, false, p->id);
        mail_user(prev_team_name, "mail/poach_out/" + p->name + "/" + std::to_string(year),
                  MailCategory::Transfer,
                  winner->name + " poach " + p->name + " from you",
                  p->name + " has signed with " + winner->name + " on the open "
                      "market. Scan the market for a replacement before the season.",
                  MailLink::Market, p->name, 0, true, p->id);
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

        // === Star retention =================================================
        // A front office fights harder to keep a franchise player. If p is among
        // the team's top 2 by overall, sweeten the money and tolerate a slightly
        // negative eval / dip into reserves so a marquee name doesn't walk over
        // a marginal shortfall (combined with the economy fix, budget now
        // genuinely constrains who can be kept).
        {
            int better = 0;
            for (auto& rp : t->roster)
                if (rp && rp.get() != p.get() &&
                    (rp->ovr() > p->ovr() ||
                     (rp->ovr() == p->ovr() && rp.get() < p.get()))) ++better;
            if (better < 2) {   // top-2 on the roster
                // P3.1: harder AI sweetens HARDER to keep its stars (and lets
                // them walk on Easy). No-op at Normal (1.12). User team never
                // re-tuned. d is the difficulty scalar 0.7..1.3.
                double d = (t == user_team) ? 1.0 : world_difficulty();
                counter    = std::min(kSalaryCapK, static_cast<int>(counter * (1.0 + 0.12 * d)) + 5);
                deal_cost  = static_cast<long long>(counter) * 1000LL;
                team_wants = (eval >= -0.6) && (t->budget + 400000LL >= deal_cost);
            }
        }

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
            // DETAILED AI OFFER (parity with the user's negotiation modal): try
            // the straight salary first; if the player would reject but the org
            // can afford it, SWEETEN with a guaranteed-starter promise (for
            // likely starters) + a signing bonus up to ~25% of salary — exactly
            // the levers the user has. Personality is already baked into the
            // 6-arg evaluator, so a loyal player needs little and a greedy/ego
            // player needs the full sweetener.
            // Only promise a STARTER slot to a player who actually starts
            // (roster[0..4]) — promising a bench/depth re-sign a starting role
            // they won't get would spuriously sour their mood at year-end.
            bool is_starter = false;
            for (std::size_t i = 0; i < t->roster.size() && i < 5; ++i)
                if (t->roster[i] == p) { is_starter = true; break; }
            bool ai_promise = (p->ovr() >= 72.0) && is_starter;
            int  ai_bonus   = 0;
            bool accept = p->accepts_resign_offer(counter, team_years, *t);
            if (!accept) {
                ai_bonus = std::min(static_cast<int>(counter * 0.25),
                                    static_cast<int>(std::max(0LL, t->budget / 1000LL - counter)));
                if (ai_bonus < 0) ai_bonus = 0;
                accept = p->evaluate_resign_offer(counter, team_years, *t,
                            p->primary_role, ai_bonus, ai_promise).will_accept;
                if (!accept) { ai_bonus = 0; ai_promise = false; }
            }
            if (accept) {
                // CONTRACT YEAR FIX (2026-05-29): the year-end re-sign loop
                // runs INSIDE run_end_of_year, BEFORE the `year += 1` step.
                // New contracts start in the upcoming season, so pass year + 1.
                if (t->resign_player(p, team_years, counter, year + 1)) {
                    p->contract.signing_bonus_k  = ai_bonus;
                    p->contract.promised_starter = ai_promise;
                    p->contract.promise_active   = ai_promise;
                    if (ai_bonus > 0) t->budget -= static_cast<long long>(ai_bonus) * 1000LL;
                    push_resign_news(t, p, team_years, counter);
                    log.emplace_back(std::string("[RE-SIGN] ")
                                     + t->name + " extends "
                                     + p->name + " ("
                                     + std::to_string(team_years) + "y / $"
                                     + std::to_string(counter) + "K"
                                     + (ai_bonus > 0 ? " +bonus" : "")
                                     + (ai_promise ? " +starter" : "") + ")");
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
        // DETAILED AI SIGNING (parity with the user): a poached starter is
        // promised their slot (benching them later sours the relationship), and
        // a marquee signing is sweetened with a one-time bonus if affordable.
        {
            // Promise a starter slot only if the poached player landed in the
            // starting 5 (sign_player appends; a 6th/7th-man depth signing must
            // not be promised a start they won't get).
            bool is_starter = false;
            for (std::size_t i = 0; i < win.team->roster.size() && i < 5; ++i)
                if (win.team->roster[i] == w.player) { is_starter = true; break; }
            bool starter_promise = (w.player->ovr() >= 72.0) && is_starter;
            int  bonus_k = 0;
            if (w.player->ovr() >= 78.0) {
                bonus_k = std::min(static_cast<int>(win.amount_k * 0.15),
                                   static_cast<int>(std::max(0LL, win.team->budget / 1000LL)));
            }
            w.player->contract.promised_starter = starter_promise;
            w.player->contract.promise_active   = starter_promise;
            w.player->contract.signing_bonus_k  = bonus_k;
            if (bonus_k > 0) win.team->budget -= static_cast<long long>(bonus_k) * 1000LL;
        }

        push_poach_news(win.team, w.player, prev_name);
        log.emplace_back(std::string("[FA POACH] ")
                         + win.team->name + " signs "
                         + w.player->name + " away from "
                         + prev_name + " ("
                         + std::to_string(win.years) + "y / $"
                         + std::to_string(win.amount_k) + "K)");
    }

    // Board verdict + season sponsor payout. MUST run here: AFTER the season's
    // standings are final but BEFORE the roster-cleanup pass below drops
    // expired/retired players — otherwise settle_sponsor's IndividualMilestone
    // scan would miss a qualifying player who left the roster this year-end
    // (rev. finding). Both read wins/standings, which stay intact until the
    // wins reset further down.
    judge_board_objective();
    settle_sponsor();

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
            int restless_dropped = 0;   // anti-dynasty: cap forced exits/org/yr
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
                // AI teams (NEVER the user's) cut a deeply disgruntled player
                // who has demanded out — they walk to free agency where the B8
                // market signs them elsewhere. This is the "force a move off
                // the team" outcome; the user's unhappy players instead surface
                // a badge the user acts on themselves.
                bool agitating = (t != user_team) && p->transfer_requested
                              && p->discontent >= 0.85;
                // Anti-dynasty (D3 hybrid): a RESTLESS star (success+ambition
                // itch) forces a move regardless of owner OR management — this
                // is the mechanism that breaks up winning cores. Capped at 2
                // per org per year so a whole core isn't dumped at once (the
                // remaining restless stars carry their itch into next year).
                bool restless_exit = p->transfer_requested
                                  && p->restlessness >= 0.55
                                  && p->ovr() >= 75.0
                                  && restless_dropped < 2;
                if (restless_exit) ++restless_dropped;
                if (expired || retired || agitating || restless_exit) drops.push_back(p);
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
            if (t->head_scout && t->head_scout->is_retired) {
                t->head_scout.reset();
            }
            if (t->head_analyst && t->head_analyst->is_retired) {
                t->head_analyst.reset();
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

    // === Promotion / relegation ===
    // Resolve the Ascension bracket + relegation gauntlet per region BEFORE
    // the tier-1 rebuild loop below — it reads season `wins` for seeding (the
    // loop resets wins), and the team swaps must land before generate_schedule
    // regenerates next year's fixtures. Swaps are pure TeamPtr moves between
    // tier_leagues_ vectors (no team destroyed -> never-free invariant held).
    resolve_promotion_relegation(log);

    // FM-depth COACH MARKET: expire AI coaches into the FA pool + wealth-based
    // hire/poach (rich orgs land decorated names, poor orgs settle). Runs before
    // the offseason loop so ai_full_offseason_pass sees the new staff. Skips the
    // user team (user-managed).
    run_coach_market(log);
    run_scout_market(log);   // WS-A: hire/expire/poach scouts alongside coaches
    run_analyst_market(log); // Match-Prep: hire/expire/poach analysts alongside scouts

    for (auto& kv : leagues) {
        for (auto& t : kv.second->teams()) {
            if (!t) continue;
            t->save_history(year);
            t->wins = 0; t->losses = 0;
            t->phase_wins = 0; t->phase_losses = 0;
            t->recent_results.clear();   // season-scope the FORM sparkline (Group F)

            // Safety net: any coachless club (chiefly the user's, which the coach
            // market leaves alone) gets a staff so it's never left without one.
            // AI hire/expire is handled by run_coach_market above.
            if (!t->head_coach) {
                t->head_coach = generate_coach(t->region);
                t->head_coach->team_name = t->name;
                t->head_coach->contract_exp_year =
                    year + t->head_coach->contract_years - 1;
            }
            // WS-A: AI clubs are never left without a scout; the USER may CHOOSE
            // to be scoutless (the fog-of-war has a no-scout floor), so skip them.
            if (t != user_team && !t->head_analyst) {
                t->head_analyst = generate_analyst(t->region);
                t->head_analyst->team_name = t->name;
                t->head_analyst->contract_exp_year =
                    year + t->head_analyst->contract_years - 1;
            }
            if (t != user_team && !t->head_scout) {
                t->head_scout = generate_scout(t->region);
                t->head_scout->team_name = t->name;
                t->head_scout->contract_exp_year =
                    year + t->head_scout->contract_years - 1;
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
            // The user team is normally excluded (the user manages it during
            // OFFSEASON), UNLESS they opted into auto-manage / simulate mode,
            // in which case the AI GM runs their offseason like everyone else.
            if (t != user_team || user_auto_manage) {
                // CONTRACT YEAR FIX (2026-05-29): runs inside run_end_of_year
                // before `year += 1`. Signings here are for the UPCOMING
                // season, so pass year + 1 so internal sign_player calls
                // set exp_year correctly. The OFFSEASON-end user fallback
                // path (advance_day) already passes the post-incremented
                // year so this is the only callsite that needs the bump.
                t->ai_full_offseason_pass(fa_pool, year + 1, log);
            }
            t->refresh_target_comp();
            // Prestige + dynamic sponsorship are now updated for ALL tiers in the
            // year-end FINANCE PASS (above), so no per-tier-1 recompute here.
            // Strategy Inertia — orgs RESIST snapping to a new philosophy
            // on a single year's evidence. classify_team_strategy still
            // computes the data-driven suggestion; commit_strategy_with_inertia
            // probabilistically decides whether to actually adopt it or
            // stick with last year's strategy. Year-1 fast-path is
            // guaranteed by the initialize_world seed (previous_strategy
            // == strategy at world init).
            Team::Strategy suggested = classify_team_strategy(*t);
            t->strategy = commit_strategy_with_inertia(*t, suggested);
            // WS-B: refresh identity each year; the user's CHOSEN philosophy
            // overrides the emergent derivation for their club.
            t->identity = (t == user_team && user_philosophy != ClubPhilosophy::Emergent)
                        ? philosophy_to_identity(user_philosophy, *t)
                        : compute_team_identity(*t);

            // Re-sign loop hoisted ABOVE the cleanup pass — see the
            // "Re-sign attempt pass" earlier in run_end_of_year. Players
            // whose Desire rejects the auto-extend offer were released
            // by the cleanup pass; remaining roster members had their
            // exp_year bumped and are good to go.
        }
        kv.second->generate_schedule();
    }

    // SCOUTING PIPELINE — after tier-1 rosters settle, tier-1 AI teams scout
    // tier-2 risers + top solo-Q FAs and sign one ONLY on a genuine need +
    // upgrade. Runs BEFORE run_lower_tier_offseason so any poached tier-2 hole
    // gets refilled in the same year-end tick.
    run_scouting_pass(year + 1, log);

    // Lower-tier offseason: release retired/expired players, reset season W/L,
    // re-fill rosters, regenerate schedules for every Challengers league. Runs
    // AFTER the tier-1 loop (so the promoted team got tier-1 maintenance and
    // the relegated team gets tier-2 maintenance here). Fixes feasibility
    // defects B1 (retired tier-2 players never released) + B2 (tier-2 wins
    // never reset -> Ascension would seed on all-time wins).
    run_lower_tier_offseason(log);
    announce_prospect_discovery(log);   // WS-A INC-6: surface first-pro-deal phenoms

    // === Contract-floor invariant enforcer ================================
    // After ALL offseason machinery (cleanup, re-sign, poach, deep GM pass,
    // scouting, promotion/relegation tier swaps, lower-tier maintenance), every
    // player STILL on a roster is, by definition, being kept — so guarantee
    // their contract runs through at least next season. This closes a latent
    // edge where an Ascension-promoted team carries an expiring contract into
    // tier-1 AFTER the tier-1 offseason loop already passed (the promoted roster
    // never got the re-sign/cleanup sweep), leaving a "0 years left but still
    // rostered" player. Bump (don't release) so promoted/retooled rosters stay
    // full. This now covers the USER team too: a rostered player with an expired
    // contract is a bookkeeping bug regardless of owner, and flooring only fixes
    // the contract record (it never signs or releases anyone, so it does not
    // auto-manage roster CHOICES). Normally a no-op for the user (cleanup already
    // released their genuinely-expired players) — it's a belt-and-braces backstop.
    {
        int floored = 0, reattached = 0;
        for (auto& kv : tier_leagues_) {
            for (auto& tier_lg : kv.second) {
                if (!tier_lg) continue;
                for (auto& t : tier_lg->teams()) {
                    if (!t) continue;
                    for (auto& p : t->roster) {
                        if (!p || p->is_retired) continue;
                        // GHOST re-attach: a player ON a roster IS that club's
                        // player. If their team_name says otherwise (e.g. still
                        // "Free Agent" because a prior club's release set the flag
                        // while they remained on this roster — a never-free edge
                        // that years_left treats as 0 years left), re-sync it so
                        // the contract reads correctly.
                        if (p->team_name != t->name) { p->team_name = t->name; ++reattached; }
                        if (p->contract.exp_year <= year) {
                            p->contract.exp_year = year + 1;
                            if (p->contract_years < 1) p->contract_years = 1;
                            ++floored;
                        }
                    }
                }
            }
        }
        if (floored > 0 || reattached > 0) {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                "[OFFSEASON] Contract-floor invariant: committed %d player(s) "
                "through next season, re-attached %d roster ghost(s).",
                floored, reattached);
            log.emplace_back(buf);
        }
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

    // Rookie generation: top each region's *active* ladder back up toward the
    // ~900 target so the ranked scene stays competitive and produces 15+
    // Radiants/year.
    //
    // CRITICAL FIX (was a geometric memory/CPU explosion): the previous formula
    // `55 + retired_this_year/3` looked bounded, but retired_this_year counted
    // CUMULATIVE retired players, not this year's new retirements — and retired
    // players are never erased from the ladder (never-free invariant). So the
    // count ratcheted up every year, the rookie injection grew with it, those
    // rookies later retired and inflated the count further... a positive
    // feedback loop that DOUBLED the ladder yearly (measured +525 -> +1248 ->
    // +2484), ballooning RAM and the O(ladder)/day ranked sim into an effective
    // hang by ~season 8. Targeting the LIVE population is self-correcting and
    // immune to any miscount: we generate only enough to replace players who
    // actually left the active pool, with a generous ceiling against spikes.
    constexpr int kActiveTarget = 900;       // matches populate_initial_ladder(900)
    // Per-year ceiling on fresh rookies. Measured steady-state attrition runs
    // ~130-145/region/yr, so this cap is roughly the binding constraint: the
    // live pool settles a little below kActiveTarget (~760-800/region) at a
    // stable equilibrium rather than pinned at 900 — healthy and verified over a
    // 20-season sim. Raise it if a fuller ranked pool is wanted (and re-verify
    // dynasty/KD balance, since pool size shifts the RNG stream & competition).
    constexpr int kMaxRookiesPerYear = 140;
    for (auto& kv : solo_qs) {
        if (!kv.second) continue;
        int active = 0;
        for (auto& p : kv.second->global_ladder())
            if (p && !p->is_retired) ++active;
        int need = std::min(kMaxRookiesPerYear, std::max(0, kActiveTarget - active));
        for (int i = 0; i < need; ++i) kv.second->generate_rookie();
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
    t2_week = 0;
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

// === Mail / Inbox =====================================================
// Bounded user inbox. Unlike news_feed (a blunt resize(200) that can drop
// anything), mail must never silently lose an UNREAD or IMPORTANT item the
// user has not yet actioned — so the drop policy prefers the oldest read &&
// !important item, falling back to the absolute oldest only when every item
// is protected.
void GameManager::push_mail(MailItem m) {
    static constexpr int kMailCap = 150;
    m.id = next_mail_id_++;
    if (m.year == 0) m.year = year;
    if (m.day  == 0) m.day  = day_in_year;
    mailbox.insert(mailbox.begin(), std::move(m));
    if (static_cast<int>(mailbox.size()) > kMailCap) {
        // Prefer dropping the OLDEST read && !important item (scan from back).
        int drop = -1;
        for (int i = static_cast<int>(mailbox.size()) - 1; i >= 0; --i) {
            if (mailbox[i].read && !mailbox[i].important) { drop = i; break; }
        }
        // None qualifies (all unread or important) -> drop absolute oldest.
        if (drop < 0) drop = static_cast<int>(mailbox.size()) - 1;
        mailbox.erase(mailbox.begin() + drop);
    }
}

int GameManager::unread_mail_count() const {
    return static_cast<int>(std::count_if(
        mailbox.begin(), mailbox.end(),
        [](const MailItem& m) { return !m.read; }));
}

bool GameManager::mail_seen_this_season(const std::string& key) {
    // First sight this season -> record + allow. Repeat -> suppress.
    return mail_emitted_keys_.insert(key).second;
}

void GameManager::mail_user(const std::string& team, const std::string& dedup_key,
                            MailCategory cat, const std::string& subject,
                            const std::string& body, MailLink link,
                            const std::string& player_name,
                            int amount_k, bool important,
                            std::uint64_t player_id) {
    // The user-team-ONLY invariant + per-season dedup live HERE so the ~18
    // event sites stay one guarded line each. Pre-game (no club) -> no mail.
    if (!user_team) return;
    if (team != user_team->name) return;
    if (!mail_seen_this_season(dedup_key)) return;
    MailItem m;
    m.category    = cat;
    m.subject     = subject;
    m.body        = body;
    m.link        = link;
    m.player_name = player_name;
    m.player_id   = player_id;
    m.team_name   = team;
    m.amount_k    = amount_k;
    m.important   = important;
    push_mail(std::move(m));
}

void GameManager::issue_board_objective(std::vector<std::string>& log) {
    (void)log;
    if (!user_team) return;
    // Find the user's tier-1 league + size to scale the placement target.
    int league_size = 0;
    for (auto& kv : leagues) {
        if (!kv.second) continue;
        auto& teams = kv.second->teams();
        for (auto& t : teams)
            if (t == user_team) { league_size = static_cast<int>(teams.size()); break; }
        if (league_size) break;
    }
    const int sz = league_size > 0 ? league_size : 10;

    // === Input-scaled board ambition (item 3) ===========================
    // Scale expectations on the inputs the user listed: difficulty, roster
    // overall + potential, the best individual ceiling + overall, and the head
    // coach (overall + how aggressive their archetype is). A stacked roster on
    // EASY difficulty earns title expectations; a weak roster on HARD
    // difficulty gets a modest, realistic mandate.
    double roster_ovr = user_team->ovr();
    double best_ovr = 0.0, best_pot = 0.0, pot_sum = 0.0;
    int pn = 0;
    for (auto& p : user_team->roster) {
        if (!p) continue;
        best_ovr = std::max(best_ovr, p->ovr());
        best_pot = std::max(best_pot, static_cast<double>(p->potential));
        pot_sum += p->potential; ++pn;
    }
    double roster_pot_avg = (pn > 0) ? pot_sum / pn : 74.0;
    double coach_ovr = 0.0, coach_amb = 0.0;
    bool   has_coach = (user_team->head_coach != nullptr);
    if (has_coach) {
        const Coach& c = *user_team->head_coach;
        coach_ovr = (c.tactical + c.development + c.leadership + c.experience) / 4.0;
        coach_amb = user_team->coach_lean().risk_appetite;   // [-1,1] archetype tilt
    }
    double difficulty = world_difficulty();   // 0.7 (easy) .. 1.3 (hard)

    double ambition = 0.0;
    ambition += (roster_ovr     - 72.0) * 1.0;   // strong avg roster raises the bar
    ambition += (best_ovr       - 80.0) * 0.6;   // a star raises the bar
    ambition += (best_pot       - 85.0) * 0.4;   // a high-ceiling prospect, a bit
    ambition += (roster_pot_avg - 74.0) * 0.3;
    ambition += has_coach ? (coach_ovr - 60.0) * 0.2 : -3.0;  // no coach -> lower bar
    ambition += coach_amb * 4.0;                 // aggressive archetype raises the bar
    ambition += (1.0 - difficulty) * 25.0;       // EASY -> raise; HARD -> lower
    board_ambition_ = ambition;

    // Map the ambition score -> placement target + tone. Higher ambition means
    // the board demands a higher finish (a smaller target placement).
    std::string obj, tone;
    int target;
    if (ambition >= 12.0) {
        target = 1;
        obj  = "Win the title — anything less is a disappointment";
        tone = "With a roster this strong, the board expects silverware this year.";
    } else if (ambition >= 4.0) {
        target = 2;
        obj  = "Finish top 2 and contend for a championship";
        tone = "This is a title-contender squad — the board wants you in the hunt.";
    } else if (ambition >= -4.0) {
        target = std::max(3, sz / 2);
        obj  = "Reach the playoffs (finish top " + std::to_string(target) + ")";
        tone = "A solid season with a real playoff run is the expectation.";
    } else if (ambition >= -12.0) {
        target = std::max(4, (sz * 6) / 10);
        obj  = "Finish mid-table or better (top " + std::to_string(target) + ")";
        tone = "The board wants steady, competitive progress this season.";
    } else {
        target = std::max(1, sz - 2);
        obj  = "Stay up — keep the club out of the relegation zone";
        tone = "Given the circumstances, survival and development are enough.";
    }
    // Keep the target a valid placement [1, league_size] even for an unusually
    // small league (the std::max floors above could otherwise exceed sz).
    target = std::min(std::max(1, target), sz);
    current_board_objective_ = obj;
    board_target_placement_  = target;
    generate_board_objectives();   // dynamic stretch-goal set (Group E)
    std::string goals_extra;
    for (auto& bo : board_objectives_) {
        if (bo.mandatory) continue;
        goals_extra += "\n   \xE2\x80\xA2 " + bo.text;
    }
    std::string body_obj = tone + " The primary objective: " + obj + ".";
    if (!goals_extra.empty())
        body_obj += "\n\nThe board has also set these targets for the season:" + goals_extra;
    body_obj += "\n\nTrack your progress on the Dashboard. Meet them to keep the board's "
                "confidence and unlock backing for next year.";
    mail_user(user_team->name,
              "mail/board_obj/" + std::to_string(year),
              MailCategory::Board,
              "Board expectations for " + std::to_string(year),
              body_obj,
              MailLink::Manager, "", 0, true);

    // Season-start contract-expiry warnings: owned starter-caliber players
    // whose deal expires at the END of this season (final contract year). The
    // Negotiation deep-link works here because the player is still rostered.
    for (auto& p : user_team->roster) {
        if (!p || p->is_retired) continue;
        if (p->team_name != user_team->name) continue;   // ghost guard
        if (p->years_left(year) != 1) continue;          // expires this season
        if (p->ovr() < 70.0) continue;                   // only meaningful pieces
        mail_user(user_team->name,
                  "mail/expiry/" + p->name + "/" + std::to_string(year),
                  MailCategory::Contract,
                  p->name + "'s contract expires this season",
                  p->name + " is entering the final year of their deal. Re-sign "
                      "them now or risk losing them to free agency at season's end.",
                  MailLink::Negotiation, p->name, 0, true, p->id);
    }
}

// === Dynamic board objectives (Group E) ==================================
void GameManager::generate_board_objectives() {
    board_objectives_.clear();
    if (!user_team) return;
    Team& T = *user_team;

    // 1) Mandatory league placement (mirrors board_target_placement_).
    {
        BoardObjective o;
        o.kind = BoardObjective::Kind::Placement;
        o.target = board_target_placement_ > 0 ? board_target_placement_ : 8;
        o.mandatory = true;
        o.text = "Finish top " + std::to_string(o.target) + " in the league";
        board_objectives_.push_back(o);
    }

    // Roster snapshots for sensible stretch targets.
    double best_ovr = 0.0, best_u22 = 0.0;
    for (auto& p : T.roster) {
        if (!p || p->is_retired) continue;
        best_ovr = std::max(best_ovr, p->ovr());
        if (p->age <= 21) best_u22 = std::max(best_u22, p->ovr());
    }
    auto add = [&](BoardObjective::Kind k, int target, const std::string& text) {
        BoardObjective o; o.kind = k; o.target = target; o.text = text;
        board_objectives_.push_back(o);
    };
    int floor_k = std::max(300, static_cast<int>(T.budget / 1000 / 2));

    // 2-3) Stretch goals chosen by strategy + wealth.
    using S = Team::Strategy;
    bool rich = (T.wealth_tier == WealthTier::Rich || T.wealth_tier == WealthTier::SuperRich);
    bool winnow = (T.strategy == S::Contender || T.strategy == S::WinNow);
    bool youthful = (T.strategy == S::Rebuilding || T.strategy == S::DevelopmentFocus ||
                     T.strategy == S::TalentFarm);

    if (winnow || rich) {
        int star = clamp_v(static_cast<int>(std::lround(best_ovr)) + 2, 80, 92);
        add(BoardObjective::Kind::SignStar, star,
            "Field a star rated OVR " + std::to_string(star) + "+");
        add(BoardObjective::Kind::WinTrophy, 1, "Win a trophy this season");
    } else if (youthful) {
        int yt = clamp_v(static_cast<int>(std::lround(best_u22 > 0 ? best_u22 + 4 : 70.0)), 66, 86);
        add(BoardObjective::Kind::DevelopYouth, yt,
            "Develop a U22 talent to OVR " + std::to_string(yt) + "+");
        add(BoardObjective::Kind::Finance, floor_k,
            "Stay solvent \xE2\x80\x94 end above $" + std::to_string(floor_k) + "K");
    } else {
        add(BoardObjective::Kind::WinTrophy, 1, "Win a trophy this season");
        add(BoardObjective::Kind::Finance, floor_k,
            "Stay solvent \xE2\x80\x94 end above $" + std::to_string(floor_k) + "K");
    }
}

ObjectiveStatus GameManager::evaluate_objective(const BoardObjective& o) const {
    ObjectiveStatus st;
    if (!user_team) return st;
    const Team& T = *user_team;
    char buf[72];
    switch (o.kind) {
        case BoardObjective::Kind::Placement: {
            auto pl = user_league_placement();
            int pos = pl.first;
            if (pos <= 0) { st.progress = "season not started"; break; }
            st.met = pos <= o.target;
            st.pct = st.met ? 1.0
                   : clamp_v(static_cast<double>(o.target) / std::max(1, pos), 0.0, 1.0);
            std::snprintf(buf, sizeof(buf), "currently #%d  (need top %d)", pos, o.target);
            st.progress = buf;
            break;
        }
        case BoardObjective::Kind::SignStar: {
            double best = 0.0;
            for (auto& p : T.roster) if (p && !p->is_retired) best = std::max(best, p->ovr());
            st.met = best >= o.target;
            st.pct = clamp_v(best / std::max(1, o.target), 0.0, 1.0);
            std::snprintf(buf, sizeof(buf), "best OVR %.0f / %d", best, o.target);
            st.progress = buf;
            break;
        }
        case BoardObjective::Kind::DevelopYouth: {
            double best = 0.0;
            for (auto& p : T.roster)
                if (p && !p->is_retired && p->age <= 21) best = std::max(best, p->ovr());
            st.met = best >= o.target;
            st.pct = clamp_v(best / std::max(1, o.target), 0.0, 1.0);
            if (best <= 0) std::snprintf(buf, sizeof(buf), "no U22 yet (need %d)", o.target);
            else           std::snprintf(buf, sizeof(buf), "best U22 OVR %.0f / %d", best, o.target);
            st.progress = buf;
            break;
        }
        case BoardObjective::Kind::Finance: {
            int bk = static_cast<int>(T.budget / 1000);
            st.met = bk >= o.target;
            st.pct = clamp_v(static_cast<double>(bk) / std::max(1, o.target), 0.0, 1.0);
            std::snprintf(buf, sizeof(buf), "$%dK / $%dK", bk, o.target);
            st.progress = buf;
            break;
        }
        case BoardObjective::Kind::WinTrophy: {
            int won = 0;
            auto tc = T.trophy_case();
            for (auto& e : tc.ordered) if (e.first == year) ++won;
            st.met = won >= o.target;
            st.pct = clamp_v(static_cast<double>(won) / std::max(1, o.target), 0.0, 1.0);
            std::snprintf(buf, sizeof(buf), "%d / %d this season", won, o.target);
            st.progress = buf;
            break;
        }
    }
    return st;
}

int GameManager::board_objectives_met() const {
    int n = 0;
    for (auto& o : board_objectives_) if (evaluate_objective(o).met) ++n;
    return n;
}

void GameManager::judge_board_objective() {
    if (!user_team) return;
    if (current_board_objective_.empty()) return;   // none was issued
    // Final placement in the user's tier-1 league (wins desc, losses asc).
    // MUST run before the year-end wins reset.
    int rank = 0, size = 0;
    for (auto& kv : leagues) {
        if (!kv.second) continue;
        auto& teams = kv.second->teams();
        bool has = false;
        for (auto& t : teams) if (t == user_team) { has = true; break; }
        if (!has) continue;
        rank = 1;
        for (auto& t : teams) {
            if (!t || t == user_team) continue;
            if (t->wins > user_team->wins ||
                (t->wins == user_team->wins && t->losses < user_team->losses))
                ++rank;
        }
        size = static_cast<int>(teams.size());
        break;
    }
    std::string subject, body;
    bool met = false;
    if (rank == 0) {
        // Not in a tier-1 league this season (e.g. relegated) — judge on the
        // rebuild rather than a top-flight placement.
        subject = "Board review: a season in transition";
        body = "The board reviewed a campaign outside the top flight. The "
               "objective was: " + current_board_objective_ + ". Steady the "
               "rebuild and push for promotion.";
    } else {
        met = (rank <= board_target_placement_);
        subject = met ? "Board review: objective MET"
                      : "Board review: objective missed";
        body = "Final placement: " + std::to_string(rank) + " of " +
               std::to_string(size) + ". Objective was: " + current_board_objective_ +
               ". " + (met ? "The board is pleased — expectations were met."
                           : "The board is disappointed and will be watching "
                             "next season closely.");
    }
    // Append the full dynamic-objective scorecard (Group E/G integration) so the
    // year-end review reads as a real performance review, not one placement line.
    if (!board_objectives_.empty()) {
        int met_n = 0;
        std::string lines;
        for (auto& o : board_objectives_) {
            auto st = evaluate_objective(o);
            if (st.met) ++met_n;
            lines += std::string("\n   ") + (st.met ? "[MET]  " : "[miss] ") +
                     o.text + "  (" + st.progress + ")";
        }
        body += "\n\nSeason scorecard \xE2\x80\x94 " + std::to_string(met_n) + " of " +
                std::to_string(board_objectives_.size()) + " objectives met:" + lines;
        if (met_n == static_cast<int>(board_objectives_.size()))
            body += "\n\nEvery target hit. The board is delighted and backing you fully.";
        else if (met_n == 0)
            body += "\n\nNot one target met. Serious questions hang over the offseason.";
    }
    mail_user(user_team->name,
              "mail/board_verdict/" + std::to_string(year),
              MailCategory::Board, subject, body,
              MailLink::Manager, "", 0, true);

    // Sponsor outlook — reports the club's standing sponsorship (no economy
    // mutation; the finance pass already books revenue). Finance deep-link.
    if (user_team->sponsorship_k > 0) {
        mail_user(user_team->name,
                  "mail/sponsor/" + std::to_string(year),
                  MailCategory::Media,
                  "Sponsor outlook for next season",
                  std::string("Your commercial team has firmed up sponsorship "
                      "backing for the coming campaign") +
                      (met ? " — a strong result has the partners optimistic."
                           : "; this season's results will shape future deals.") +
                      " See the Finance dashboard for the full breakdown.",
                  MailLink::Finance, "", user_team->sponsorship_k, false);
    }

    // Objective consumed; next season issues a fresh one.
    current_board_objective_.clear();
    board_target_placement_ = 0;
}

void GameManager::diagnose_season_kd(std::vector<std::string>& out) const {
    // Percentile on a SORTED vector (nearest-rank). q in [0,1].
    auto pct = [](const std::vector<double>& v, double q) -> double {
        if (v.empty()) return 0.0;
        std::size_t idx = static_cast<std::size_t>(q * (v.size() - 1) + 0.5);
        if (idx >= v.size()) idx = v.size() - 1;
        return v[idx];
    };
    auto mean = [](const std::vector<double>& v) -> double {
        if (v.empty()) return 0.0;
        double s = 0.0; for (double x : v) s += x; return s / v.size();
    };

    std::vector<double> all_kd, gated_kd, intl_kd;   // KD over deaths>0 only
    int undefeated = 0;                              // 0-death samples (excluded)
    struct Outlier { std::string name, team; double kd; int im; };
    std::vector<Outlier> outliers;

    for (auto& kv : leagues) {
        if (!kv.second) continue;
        for (auto& t : kv.second->teams()) {
            if (!t) continue;
            for (auto& p : t->roster) {
                if (!p) continue;
                if (p->season_matches <= 0) continue;
                if (p->season_deaths <= 0) { ++undefeated; continue; }
                double kd = static_cast<double>(p->season_kills) / p->season_deaths;
                all_kd.push_back(kd);
                if (p->season_matches >= 4) gated_kd.push_back(kd);
                if (p->season_intl_matches > 0) intl_kd.push_back(kd);
                if (p->season_intl_matches > 20 && kd > 1.5)
                    outliers.push_back({p->name, t->name, kd, p->season_intl_matches});
            }
        }
    }
    std::sort(all_kd.begin(),   all_kd.end());
    std::sort(gated_kd.begin(), gated_kd.end());
    std::sort(intl_kd.begin(),  intl_kd.end());

    char buf[224];
    std::snprintf(buf, sizeof(buf),
        "TIER1 SEASON KD  n=%d gated_n=%d  mean=%.3f p50=%.3f p90=%.3f max=%.3f",
        static_cast<int>(all_kd.size()), static_cast<int>(gated_kd.size()),
        mean(all_kd), pct(all_kd, 0.5), pct(all_kd, 0.9),
        all_kd.empty() ? 0.0 : all_kd.back());
    out.emplace_back(buf);
    std::snprintf(buf, sizeof(buf),
        "TIER1 KD (>=4 maps) n=%d  mean=%.3f p50=%.3f p90=%.3f p97=%.3f max=%.3f",
        static_cast<int>(gated_kd.size()), mean(gated_kd),
        pct(gated_kd, 0.5), pct(gated_kd, 0.9), pct(gated_kd, 0.97),
        gated_kd.empty() ? 0.0 : gated_kd.back());
    out.emplace_back(buf);
    std::snprintf(buf, sizeof(buf),
        "INTL-EXPOSED KD  n=%d  mean=%.3f p50=%.3f p90=%.3f max=%.3f",
        static_cast<int>(intl_kd.size()), mean(intl_kd),
        pct(intl_kd, 0.5), pct(intl_kd, 0.9),
        intl_kd.empty() ? 0.0 : intl_kd.back());
    out.emplace_back(buf);
    std::snprintf(buf, sizeof(buf),
        "  (excluded %d undefeated/0-death samples from the distribution)", undefeated);
    out.emplace_back(buf);

    std::sort(outliers.begin(), outliers.end(),
              [](const Outlier& a, const Outlier& b) { return a.kd > b.kd; });
    for (auto& o : outliers) {
        std::snprintf(buf, sizeof(buf),
            "  INTL OUTLIER  %s (%s)  kd=%.2f  intl_maps=%d",
            o.name.c_str(), o.team.c_str(), o.kd, o.im);
        out.emplace_back(buf);
    }
}

void GameManager::diagnose_dynasty_rate(std::vector<std::string>& out) const {
    // Build {year -> world champion} + {team -> all title years} from the
    // persistent tier-1 trophy_cases (world champions are always tier-1).
    std::unordered_map<int, std::string> world_champ;
    std::unordered_map<std::string, std::vector<int>> team_titles;       // all
    std::unordered_map<std::string, std::vector<int>> team_intl_titles;  // Masters+Champions
    int min_y = 0, max_y = 0; bool any = false;
    for (auto& kv : leagues) {
        if (!kv.second) continue;
        for (auto& t : kv.second->teams()) {
            if (!t) continue;
            for (auto& e : t->trophy_case().ordered) {
                int yr = e.first;
                const std::string& ev = e.second;
                team_titles[t->name].push_back(yr);
                if (!any) { min_y = max_y = yr; any = true; }
                else { if (yr < min_y) min_y = yr; if (yr > max_y) max_y = yr; }
                bool world = (ev.find("CHAMPIONS") != std::string::npos);
                if (world) world_champ[yr] = t->name;
                if (world || ev.find("MASTERS") != std::string::npos)
                    team_intl_titles[t->name].push_back(yr);
            }
        }
    }
    if (!any) { out.emplace_back("DYNASTY: no trophies recorded yet"); return; }
    const int span = max_y - min_y + 1;

    // (1) back-to-back world champions.
    int bb = 0, bb_pairs = 0;
    for (int y = min_y + 1; y <= max_y; ++y) {
        auto a = world_champ.find(y), b = world_champ.find(y - 1);
        if (a != world_champ.end() && b != world_champ.end()) {
            ++bb_pairs;
            if (a->second == b->second) ++bb;
        }
    }
    // (2) 3-titles-in-5y windows: any team with >=3 titles in [y-4, y].
    // ALL titles (regional dominance) AND international-only (true global dynasty).
    auto three_in_5_for = [&](const std::unordered_map<std::string, std::vector<int>>& tt) {
        int hits = 0, max_in_window = 0;
        for (int y = min_y; y <= max_y; ++y) {
            std::unordered_map<std::string, int> cnt;
            for (auto& kv : tt)
                for (int ty : kv.second)
                    if (ty >= y - 4 && ty <= y) ++cnt[kv.first];
            bool hit = false;
            for (auto& c : cnt) {
                if (c.second > max_in_window) max_in_window = c.second;
                if (c.second >= 3) hit = true;
            }
            if (hit) ++hits;
        }
        return std::make_pair(hits, max_in_window);
    };
    auto all3 = three_in_5_for(team_titles);
    auto intl3 = three_in_5_for(team_intl_titles);
    // (3) world-champ parity (distinct champions / years with a world champ).
    std::unordered_map<std::string, int> wc_count;
    for (auto& wc : world_champ) ++wc_count[wc.second];
    const int wc_years = static_cast<int>(world_champ.size());
    const double parity = wc_years > 0
        ? static_cast<double>(wc_count.size()) / wc_years : 0.0;

    char buf[256];
    std::snprintf(buf, sizeof(buf),
        "DYNASTY over %dy (%d-%d): back-to-back world champ=%d/%d  parity=%.2f "
        "(%d distinct / %d world titles)",
        span, min_y, max_y, bb, bb_pairs, parity,
        static_cast<int>(wc_count.size()), wc_years);
    out.emplace_back(buf);
    std::snprintf(buf, sizeof(buf),
        "  3-in-5y windows: ALL-titles=%d/%d (max %d, regional dominance)  "
        "INTERNATIONAL-only=%d/%d (max %d, true dynasty)",
        all3.first, span, all3.second, intl3.first, span, intl3.second);
    out.emplace_back(buf);
    for (auto& c : wc_count) if (c.second >= 2) {
        std::snprintf(buf, sizeof(buf), "  multi-world-champ: %s x%d",
                      c.first.c_str(), c.second);
        out.emplace_back(buf);
    }
}

void GameManager::diagnose_intl_kd(std::vector<std::string>& out) const {
    // Per (player, year) intl-map K/D from the Masters/Champions tourn_stats
    // buckets — the TRUE international performance, not the season KD diluted by
    // domestic maps. Gate on >=5 intl maps so a 1-map cameo isn't counted.
    std::vector<double> kds;
    int ge15 = 0, ge14 = 0;
    double top = 0.0; std::string top_name; int top_year = 0;
    for (auto& kv : solo_qs) {
        if (!kv.second) continue;
        for (auto& p : kv.second->global_ladder()) {
            if (!p) continue;
            std::unordered_map<int, long long> k_by_y, d_by_y, m_by_y;
            for (auto& tb : p->tourn_stats) {
                const std::string& key = tb.first;
                if (key.find("Masters") == std::string::npos &&
                    key.find("Champions") == std::string::npos) continue;
                std::size_t bar = key.rfind('|');
                if (bar == std::string::npos) continue;
                int yr = std::atoi(key.c_str() + bar + 1);
                k_by_y[yr] += tb.second.kills;
                d_by_y[yr] += tb.second.deaths;
                m_by_y[yr] += tb.second.maps;
            }
            for (auto& mk : m_by_y) {
                if (mk.second < 5) continue;
                long long dn = d_by_y[mk.first];
                if (dn <= 0) continue;
                double kd = static_cast<double>(k_by_y[mk.first]) / dn;
                kds.push_back(kd);
                if (kd >= 1.5) ++ge15;
                if (kd >= 1.4) ++ge14;
                if (kd > top) { top = kd; top_name = p->name; top_year = mk.first; }
            }
        }
    }
    std::sort(kds.begin(), kds.end());
    auto pct = [&](double q) -> double {
        if (kds.empty()) return 0.0;
        std::size_t i = static_cast<std::size_t>(q * (kds.size() - 1) + 0.5);
        if (i >= kds.size()) i = kds.size() - 1;
        return kds[i];
    };
    double mean = 0.0; for (double x : kds) mean += x;
    if (!kds.empty()) mean /= kds.size();
    char buf[256];
    std::snprintf(buf, sizeof(buf),
        "INTL-MAP KD (>=5 intl maps): n=%d  mean=%.3f p50=%.3f p90=%.3f max=%.3f",
        static_cast<int>(kds.size()), mean, pct(0.5), pct(0.9),
        kds.empty() ? 0.0 : kds.back());
    out.emplace_back(buf);
    std::snprintf(buf, sizeof(buf),
        "  intl seasons >=1.5 KD: %d   >=1.4: %d   (%d player-seasons; %.1f%% hit 1.5)",
        ge15, ge14, static_cast<int>(kds.size()),
        kds.empty() ? 0.0 : 100.0 * ge15 / kds.size());
    out.emplace_back(buf);
    if (!top_name.empty()) {
        std::snprintf(buf, sizeof(buf), "  top intl-map season: %s (%d) kd=%.2f",
                      top_name.c_str(), top_year, top);
        out.emplace_back(buf);
    }
}

std::pair<int, int> GameManager::user_league_placement() const {
    if (!user_team) return {0, 0};
    for (auto& kv : leagues) {
        if (!kv.second) continue;
        auto& teams = kv.second->teams();
        bool has = false;
        for (auto& t : teams) if (t == user_team) { has = true; break; }
        if (!has) continue;
        int rank = 1;
        for (auto& t : teams) {
            if (!t || t == user_team) continue;
            if (t->wins > user_team->wins ||
                (t->wins == user_team->wins && t->losses < user_team->losses))
                ++rank;
        }
        return {rank, static_cast<int>(teams.size())};
    }
    return {0, 0};
}

std::vector<SponsorOffer> GameManager::generate_sponsor_offers() const {
    std::vector<SponsorOffer> offers;
    if (!user_team) return offers;
    int sz = 0;
    for (auto& kv : leagues) {
        if (!kv.second) continue;
        auto& teams = kv.second->teams();
        for (auto& t : teams) if (t == user_team) { sz = static_cast<int>(teams.size()); break; }
        if (sz) break;
    }
    if (sz <= 0) sz = 10;
    const double diff = world_difficulty();
    auto reward = [&](int type_bonus) {
        int r = 90 + type_bonus + static_cast<int>((diff - 1.0) * 40.0);
        return std::min(200, std::max(75, r));   // modest one-time lump [75,200]
    };
    // (a) Prestige — finish top 2 (demanding, biggest payout).
    offers.push_back({ "Apex Performance",
        "Finish top 2 in the league",
        SponsorReqType::Placement, 2, reward(50) });
    // (b) Performance — a strong regular-season win haul.
    int wins_target = std::max(8, sz);
    offers.push_back({ "Velocity Energy",
        "Win " + std::to_string(wins_target) + "+ regular-season matches",
        SponsorReqType::WinCount, wins_target, reward(30) });
    // (c) Growth — develop a standout (a rostered player ends >= 1.15 rating).
    offers.push_back({ "Ascend Apparel",
        "Have a rostered player finish the season rated 1.15+",
        SponsorReqType::IndividualMilestone, 115, reward(35) });
    return offers;
}

void GameManager::choose_sponsor(const SponsorOffer& offer) {
    if (!user_team) return;
    user_team->sponsor_name      = offer.name;
    user_team->sponsor_req_type  = offer.type;
    user_team->sponsor_req_value = offer.requirement_value;
    user_team->sponsor_reward_k  = offer.reward_k;
    user_team->sponsor_active    = !offer.name.empty();
    user_team->sponsor_credited  = false;   // fresh season -> re-evaluate
    sponsor_choice_pending_      = false;   // choice made; close the modal
}

void GameManager::settle_sponsor() {
    if (!user_team) return;
    Team* t = user_team.get();
    if (!t->sponsor_active || t->sponsor_credited) return;

    bool met = false;
    switch (t->sponsor_req_type) {
        case SponsorReqType::Placement:
        case SponsorReqType::TitleBerth: {
            auto pl = user_league_placement();
            if (pl.first > 0) met = (pl.first <= t->sponsor_req_value);
            break;
        }
        case SponsorReqType::WinCount:
            met = (t->wins >= t->sponsor_req_value);
            break;
        case SponsorReqType::IndividualMilestone: {
            const double thresh = t->sponsor_req_value / 100.0;   // 115 -> 1.15
            for (auto& p : t->roster) {
                if (!p || p->season_matches < 8) continue;
                if (p->season_rating_total / p->season_matches >= thresh) { met = true; break; }
            }
            break;
        }
    }
    // Mark resolved either way so it can't re-fire this season; the next
    // preseason pick resets the flag via choose_sponsor.
    t->sponsor_credited = true;
    if (!met) {
        mail_user(t->name, "mail/sponsor_miss/" + std::to_string(year),
                  MailCategory::Media,
                  "Sponsor target missed: " + t->sponsor_name,
                  "You fell short of " + t->sponsor_name + "'s goal this season — "
                      "no performance bonus. A fresh deal awaits next preseason.",
                  MailLink::Finance, "", 0, false);
        return;
    }
    // Met -> credit the modest one-time lump straight to the budget (no EMA).
    t->budget += static_cast<long long>(t->sponsor_reward_k) * 1000LL;
    mail_user(t->name, "mail/sponsor_paid/" + std::to_string(year),
              MailCategory::Media,
              "Sponsor bonus: +$" + std::to_string(t->sponsor_reward_k) + "K from " + t->sponsor_name,
              t->sponsor_name + " is delighted — you hit the target and the club "
                  "banks a $" + std::to_string(t->sponsor_reward_k) + "K performance bonus.",
              MailLink::Finance, "", t->sponsor_reward_k, true);
}

void GameManager::start_of_season_setup(std::vector<std::string>& log) {
    // Board objective (item 3) + the 3-offer sponsor choice (item 4) land at the
    // start of every season. Year-keyed dedup inside issue_board_objective makes
    // a same-year second call a no-op.
    issue_board_objective(log);
    reset_scout_credits();
    run_scout_assignment(log);   // increment D: auto-reveal on the assigned focus
    if (user_team) {
        pending_sponsor_offers_ = generate_sponsor_offers();
        sponsor_choice_pending_ = !pending_sponsor_offers_.empty();
    }
}

// === Scouting fog-of-war (Group C) =======================================
void GameManager::reset_scout_credits() {
    if (!user_team) { scout_credits_ = 0; return; }
    if (scout_credits_year_ == year) return;   // already refilled this season
    scout_credits_year_ = year;
    // WS-A INC-3: the head scout drives how much of the market the club sees.
    // Floor 3 when scoutless (never fully blind); an elite department ~10-12.
    scout_credits_ = user_team->head_scout
                   ? user_team->head_scout->reveal_credits() : 3;
}

// WS-B: map a chosen club philosophy to a fixed identity (user_chosen=true);
// Emergent falls back to the derived identity. Single mapping point (B1).
TeamIdentity GameManager::philosophy_to_identity(ClubPhilosophy p, const Team& base) {
    TeamIdentity id = compute_team_identity(base);   // emergent baseline
    id.user_chosen = (p != ClubPhilosophy::Emergent);
    switch (p) {
        case ClubPhilosophy::AggressiveAcademy:
            id.aggression = 0.80; id.dev_youth = 0.75;
            id.comp_lean = CompTag::DoubleDuelist;   id.brand = BrandTag::AcademyPowerhouse; break;
        case ClubPhilosophy::TacticalMethodical:
            id.aggression = 0.25; id.dev_youth = 0.50;
            id.comp_lean = CompTag::DoubleInitiator; id.brand = BrandTag::TacticalMasterminds; break;
        case ClubPhilosophy::WinNowSpender:
            id.aggression = 0.65; id.dev_youth = 0.15;
            id.comp_lean = CompTag::DoubleDuelist;   id.brand = BrandTag::WinNowSpenders; break;
        case ClubPhilosophy::DefensiveStructured:
            id.aggression = 0.20; id.dev_youth = 0.45;
            id.comp_lean = CompTag::DoubleSentinel;  id.brand = BrandTag::DefensiveWall; break;
        case ClubPhilosophy::BalancedFlexible:
            id.aggression = 0.50; id.dev_youth = 0.55;
            id.comp_lean = CompTag::DoubleInitiator; id.brand = BrandTag::BalancedContenders; break;
        case ClubPhilosophy::Emergent: default: break;
    }
    return id;
}

void GameManager::scouted_band(const Player& p, int& lo, int& hi) const {
    // Deterministic id-seeded base band — the SINGLE source of this hash (the GUI
    // calls here) so Market + Profile + every frame agree and it never flickers.
    std::uint64_t h = p.id * 2654435761ull + 0x9E3779B97F4A7C15ull;
    int below = 3 + static_cast<int>(h % 7u);
    int above = 3 + static_cast<int>((h >> 8) % 7u);
    // A sharper user scout reads the ceiling tighter — shrink toward the true
    // value. Re-floor each offset at >=1 so an unscouted player is NEVER pinned
    // exactly; the true potential always stays inside [lo,hi].
    double t = (user_team && user_team->head_scout)
                 ? user_team->head_scout->band_tighten_mult() : 0.0;
    below = std::max(1, static_cast<int>(std::lround(below * (1.0 - t))));
    above = std::max(1, static_cast<int>(std::lround(above * (1.0 - t))));
    lo = std::max(1,  p.potential - below);
    hi = std::min(99, p.potential + above);
}

bool GameManager::scout_player(const PlayerPtr& p) {
    if (!user_team || !p || p->potential_scouted) return false;
    if (scout_credits_ <= 0) return false;
    p->potential_scouted = true;
    --scout_credits_;
    // Scouting report mail — flag a genuine gem so the reveal pays off.
    int cur = static_cast<int>(std::lround(p->ovr()));
    if (p->potential >= 80 && p->potential - cur >= 6) {
        mail_user(user_team->name,
                  "mail/scout_gem/" + p->name + "/" + std::to_string(year),
                  MailCategory::Squad,
                  "Scouting report: " + p->name + " is one to watch",
                  "Our scouts are excited about " + p->name + " \xE2\x80\x94 a " +
                      std::to_string(p->age) + "-year-old " +
                      position_name(position_of(*p)) + " with potential " +
                      std::to_string(p->potential) + " off a current " +
                      std::to_string(cur) + " OVR. Worth a move before rivals notice.",
                  MailLink::Player, p->name, 0, false, p->id);
    }
    return true;
}

void GameManager::run_scout_assignment(std::vector<std::string>& log) {
    if (!user_team || scout_focus_ == ScoutFocus::None || scout_credits_ <= 0) return;

    // Focus-matching candidate pool: unscouted, non-retired prospects (or, for
    // the Watchlist focus, the watched players regardless of ceiling).
    std::vector<PlayerPtr> cands;
    if (scout_focus_ == ScoutFocus::Watchlist) {
        for (auto& w : watchlist_)
            if (w.player && !w.player->is_retired && !w.player->potential_scouted)
                cands.push_back(w.player);
    } else {
        for (auto& kv : solo_qs) {
            if (!kv.second) continue;
            for (auto& p : kv.second->global_ladder()) {
                if (!p || p->is_retired || p->potential_scouted) continue;
                if (p->potential < 70) continue;   // direct the eye at real prospects
                if (scout_focus_ == ScoutFocus::Region && p->region != scout_focus_region_) continue;
                if (scout_focus_ == ScoutFocus::Role   && p->primary_role != scout_focus_role_) continue;
                cands.push_back(p);
            }
        }
    }
    std::sort(cands.begin(), cands.end(),
              [](const PlayerPtr& a, const PlayerPtr& b) { return a->potential > b->potential; });

    int spent = 0;
    for (auto& p : cands) {
        if (scout_credits_ <= 0) break;
        if (scout_player(p)) ++spent;   // reveal + credit decrement + gem mail
    }
    if (spent > 0) {
        const char* fl = (scout_focus_ == ScoutFocus::Region)   ? scout_focus_region_.c_str()
                       : (scout_focus_ == ScoutFocus::Role)     ? role_name(scout_focus_role_)
                       : "your watchlist";
        char buf[176];
        std::snprintf(buf, sizeof(buf),
            "[SCOUT] Assignment revealed %d prospect%s (%s focus); %d credit%s left.",
            spent, spent == 1 ? "" : "s", fl, scout_credits_, scout_credits_ == 1 ? "" : "s");
        log.emplace_back(buf);
    }
}

// === User Make-Offer for contracted players (Group D) ====================
GameManager::TransferBidOutcome GameManager::user_transfer_bid(
        const TeamPtr& seller, const PlayerPtr& target, int fee_offer_k, int years) {
    TransferBidOutcome o;
    o.fee_k = fee_offer_k;
    if (!user_team || !seller || !target) { o.message = "Invalid offer."; return o; }
    if (target->team_name == "Free Agent" || target->team_name == "Retired" ||
        target->team_name == user_team->name || target->team_name != seller->name) {
        o.message = target->name + " isn't available to buy from " + seller->name + ".";
        return o;
    }
    if (static_cast<int>(user_team->roster.size()) >= 7) {
        o.message = "Your roster is full (7) — free a slot before buying.";
        return o;
    }
    o.asking_k = seller->sell_threshold_k(*target, year);

    // A club won't sell down to a sub-6 roster mid-season. There is no
    // mid-season auto-fill to heal a short AI side (unlike the offseason poach
    // path), so a <5 team would forfeit 13-0 every series until the offseason.
    // Refusing here both prevents that and matches how a real club guards its
    // last starters.
    if (static_cast<int>(seller->roster.size()) <= 5) {
        o.code = TransferBidOutcome::Rejected;
        o.message = seller->name + " won't sell " + target->name +
                    " mid-season \xE2\x80\x94 it would leave them short of a full roster.";
        return o;
    }

    // Affordability: the fee PLUS the wage you'd inherit must fit the envelope.
    if (!user_team->within_wage_envelope(target->contract.amount_k, fee_offer_k)) {
        o.code = TransferBidOutcome::CantAfford;
        o.message = "Your club can't responsibly afford that fee + $" +
                    std::to_string(target->contract.amount_k) + "K/yr wage right now.";
        return o;
    }
    if (!seller->will_sell(*target, fee_offer_k, year)) {
        o.code = TransferBidOutcome::Rejected;
        o.message = seller->name + " rejected the bid \xE2\x80\x94 they value " +
                    target->name + " at about $" + std::to_string(o.asking_k) + "K.";
        // Persist the asking figure beyond the transient toast (accepted bids
        // already mail a receipt; rejections used to leave no record). Fee in
        // the dedup key so each distinct rejected offer lands its own mail.
        mail_user(user_team->name,
                  "mail/bid_rejected/" + target->name + "/" +
                      std::to_string(fee_offer_k) + "/" + std::to_string(year),
                  MailCategory::Transfer,
                  "Bid rejected: " + target->name,
                  seller->name + " turned down your $" + std::to_string(fee_offer_k) +
                      "K bid for " + target->name + " \xE2\x80\x94 they value them at "
                      "about $" + std::to_string(o.asking_k) + "K.",
                  MailLink::Player, target->name, o.asking_k, false, target->id);
        return o;
    }
    // WS-A INC-4: respect the transfer window (open today; gated later).
    if (!transfers_open()) {
        o.code = TransferBidOutcome::Rejected;
        o.message = "The transfer window is closed right now.";
        return o;
    }
    // Accepted: execute the move. Mirror the AI poach order — release from the
    // seller, move the cash + log, then sign to the user club. Never-free safe
    // (the player MOVES; nothing is destroyed).
    seller->release_player(target);
    user_team->pay_transfer_fee(seller, *target, year, fee_offer_k);
    int yrs = years > 0 ? years
            : std::max(1, user_team->decide_contract_years(*target, year));
    user_team->sign_player(target, yrs, year);
    o.code = TransferBidOutcome::Accepted;
    o.message = target->name + " joins " + user_team->name + " for $" +
                std::to_string(fee_offer_k) + "K!";
    mail_user(user_team->name,
              "mail/transfer_in/" + target->name + "/" + std::to_string(year),
              MailCategory::Transfer,
              "Signing complete: " + target->name,
              target->name + " (" +
                  std::to_string(static_cast<int>(std::lround(target->ovr()))) +
                  " OVR, age " + std::to_string(target->age) + ", " +
                  position_name(position_of(*target)) + ") has signed from " +
                  seller->name + " for a $" + std::to_string(fee_offer_k) +
                  "K fee on $" + std::to_string(target->contract.amount_k) +
                  "K/yr wages. Welcome them on the Squad page.",
              MailLink::Roster, target->name, fee_offer_k, true, target->id);
    return o;
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
        n.headline = champion->name + " win " + tour.name();
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
        // Champion / runner-up mail when the user club is involved. yr is the
        // tournament's year; mail_user dedups per season on (tour, year).
        mail_user(champion->name,
                  "mail/champ/" + tour.name() + "/" + std::to_string(yr),
                  MailCategory::Result,
                  "CHAMPIONS — " + champion->name + " win " + tour.name() + "!",
                  "Your club is crowned " + tier_label + " " + std::to_string(yr) + "." +
                      (runner_up ? std::string(" A grand-final win over ") +
                          runner_up->name + " seals the title." : std::string()),
                  MailLink::Manager, "", 0, true);
        if (runner_up) {
            mail_user(runner_up->name,
                      "mail/runnerup/" + tour.name() + "/" + std::to_string(yr),
                      MailCategory::Result,
                      "Runner-up at " + tour.name(),
                      "So close — your club reached the grand final of " + tour.name() +
                          " but fell to " + champion->name + ". A strong run to build on.",
                      MailLink::Manager, "", 0, false);
        }
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

int GameManager::team_wins_for_player(const Player& p) const {
    for (auto& kv : leagues)
        if (kv.second) for (auto& t : kv.second->teams())
            if (t) for (auto& tp : t->roster)
                if (tp.get() == &p) return t->wins;
    return 0;
}

int GameManager::hof_criteria_count(const Player& p) const {
    int criteria = 0;
    // 1. Won an international event (Masters or Champions)
    for (auto& a : p.awards)
        if (a.find("[M]") != std::string::npos ||
            a.find("[W]") != std::string::npos) { ++criteria; break; }
    // 2. Won a role award (Duelist/Initiator/Controller/Sentinel of the Year)
    for (auto& a : p.awards)
        if (a.find("of the Year") != std::string::npos &&
            a.find("MVP") == std::string::npos &&
            a.find("IGL") == std::string::npos) { ++criteria; break; }
    // 3. Won an MVP
    for (auto& a : p.awards)
        if (a.find("MVP") != std::string::npos) { ++criteria; break; }
    // 4. Maintained 1.2+ rating across an entire season
    for (auto& h : p.history)
        if (h.rating >= 1.20 && p.career_seasons_played >= 3) { ++criteria; break; }
    // 5. Reached top 20 in solo queue rankings
    if (p.ever_top20_solo) ++criteria;
    // 6. Earned over $1,000,000 career
    int total_earnings = 0;
    for (auto& s : p.salary_log) total_earnings += s.second;
    if (total_earnings >= 1000) ++criteria;  // 1000 K = $1M
    // 7. Dropped 30 kills in a pro match
    if (p.career_max_match_kills >= 30) ++criteria;
    // 8. Achieved 2.0 KD in a pro match
    if (p.career_max_match_kd_x100 >= 200) ++criteria;
    // 9. Clutched the final round of a Grand Final
    if (p.career_grand_final_clutches >= 1) ++criteria;
    // 10. (IGLs only) Career IGL strategic impact > 50 — a HoF lane for IGLs
    //     who win by calling rather than frag-carrying. Additional criterion,
    //     not a replacement, so the gate fires for everyone else as before.
    if (p.is_igl && p.igl_impact_total > 50.0) ++criteria;
    return criteria;
}

GameManager::OppositionReport GameManager::scout_opposition(const Team& opp) const {
    OppositionReport r;
    r.opp_name   = opp.name;
    r.opp_region = opp.region;
    if (opp.roster.empty()) return r;   // r.valid stays false
    r.valid = true;

    // The USER's head analyst gates section depth + sharpens figures. AI clubs
    // never consume this (it's the user's pre-match prep view only).
    const Analyst* an = (user_team && user_team->head_analyst)
                          ? user_team->head_analyst.get() : nullptr;
    r.has_analyst  = (an != nullptr);
    r.detail_level = an ? an->report_depth_sections() : 1;
    r.accuracy     = an ? an->report_accuracy_mult()  : 0.0;

    // Section 1 (always): headline OVR estimate (band shrinks with accuracy) + form.
    r.opp_ovr_est = static_cast<int>(std::lround(opp.ovr()));
    r.ovr_band    = static_cast<int>(std::lround((1.0 - r.accuracy) * 8.0));  // +/-8 .. +/-3
    {
        std::string f; int shown = 0;
        for (auto it = opp.recent_results.rbegin();
             it != opp.recent_results.rend() && shown < 5; ++it, ++shown) {
            f += (*it ? 'W' : 'L');
        }
        r.form = f.empty() ? "no recent games" : f;
    }

    // Section 2 (depth>=2): key players — top 2 by OVR.
    if (r.detail_level >= 2) {
        std::vector<const Player*> ros;
        for (auto& p : opp.roster) if (p) ros.push_back(p.get());
        std::sort(ros.begin(), ros.end(),
                  [](const Player* a, const Player* b) { return a->ovr() > b->ovr(); });
        for (std::size_t i = 0; i < ros.size() && i < 2; ++i) {
            char b[96];
            std::snprintf(b, sizeof(b), "%s (%s, %d OVR)",
                          ros[i]->name.c_str(), role_name(ros[i]->primary_role),
                          static_cast<int>(std::lround(ros[i]->ovr())));
            r.key_players.emplace_back(b);
        }
    }

    // Section 3 (depth>=3): comp tendency (what they like to run).
    if (r.detail_level >= 3) {
        r.comp_tendency = comp_tag_name(comp_tag_of(opp.target_comp));
    }

    // Section 4 (depth>=4): role strength read + (depth>=5) recommended counter.
    if (r.detail_level >= 4) {
        RoleNeed need = opp.compute_role_need();
        int strong_r = 0, soft_r = 0; double hi = -1.0, lo = 1e9;
        for (int rr = 0; rr < 4; ++rr) {
            double bo = (need.count[rr] == 0) ? 0.0 : need.best_ovr[rr];
            if (bo > hi) { hi = bo; strong_r = rr; }
            if (bo < lo) { lo = bo; soft_r  = rr; }
        }
        char b[120];
        std::snprintf(b, sizeof(b), "Strongest: %s  |  Softest: %s",
                      role_name(static_cast<Role>(strong_r)),
                      role_name(static_cast<Role>(soft_r)));
        r.role_read = b;
        if (r.detail_level >= 5) {
            std::string comp = r.comp_tendency.empty() ? "flexible" : r.comp_tendency;
            r.recommendation = "Pressure their "
                             + std::string(role_name(static_cast<Role>(soft_r)))
                             + " and prepare for a " + comp + " look.";
        }
    }
    return r;
}

GameManager::MvpFactorBreakdown GameManager::mvp_score_breakdown(const Player& p) const {
    // Single source of truth for the MVP/POTY composite — emits every factor so
    // the UI can show the user WHY a player tops the chart. mvp_composite_score
    // is now a thin wrapper returning .total, keeping every caller unchanged.
    MvpFactorBreakdown b;
    b.rating = p.season_matches > 0 ? p.season_rating_total / p.season_matches : 0.0;
    if (b.rating < 0.95) { b.qualified = false; b.total = 0.0; return b; }
    b.qualified = true;

    b.clutch = 1.0 + std::min(0.30, p.season_clutch_pts / 30.0);

    b.is_igl = p.is_igl;
    if (p.is_igl) b.igl = 1.0 + std::min(0.35, p.igl_impact_season / 40.0);

    b.attended_intl = p.season_intl_matches > 0;
    {
        std::string yr_str = std::to_string(year);
        for (auto& a : p.awards) {
            if (a.find(yr_str) == std::string::npos) continue;
            if (a.find("[M]") != std::string::npos ||
                a.find("[W]") != std::string::npos) { b.won_intl = true; break; }
        }
    }
    b.sos = 1.0;
    if (b.attended_intl) b.sos *= 1.08;
    if (b.won_intl)      b.sos *= 1.15;

    b.playoff = 1.0 + std::min(0.20, p.season_pressure_matches / 10.0);

    int tw = team_wins_for_player(p);
    if      (tw >= 28) b.team = 1.25;
    else if (tw >= 22) b.team = 1.15;
    else if (tw >= 16) b.team = 1.05;
    else if (tw <  10) b.team = 0.85;

    b.clutch_pts = p.season_clutch_pts;
    b.pressure_matches = p.season_pressure_matches;
    b.intl_matches = p.season_intl_matches;
    b.team_wins = tw;

    // Compact factor list — only the levers that actually moved the score.
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.2f rating", b.rating);
    b.factors.emplace_back(buf, b.rating);
    if (b.clutch  > 1.001) b.factors.emplace_back("clutch", b.clutch);
    if (b.igl     > 1.001) b.factors.emplace_back("IGL impact", b.igl);
    if (b.attended_intl)   b.factors.emplace_back("intl attendee", 1.08);
    if (b.won_intl)        b.factors.emplace_back("intl title", 1.15);
    if (b.playoff > 1.001) b.factors.emplace_back("playoff run", b.playoff);
    if (b.team != 1.0)     b.factors.emplace_back(b.team >= 1.0 ? "team success" : "weak team", b.team);

    b.total = b.rating * b.clutch * b.igl * b.sos * b.playoff * b.team;
    return b;
}

double GameManager::mvp_composite_score(const Player& p) const {
    return mvp_score_breakdown(p).total;
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

    // Gather qualifying players. TIER-1 ONLY: lower-tier (Challengers/Open)
    // players now accumulate season_matches from their own stage play, so we
    // must restrict the MVP race to players on a tier-1 (leagues) roster —
    // otherwise a Challengers star could enter the tier-1 MVP race.
    std::unordered_set<Player*> tier1_players;
    for (auto& kv : leagues)
        if (kv.second) for (auto& t : kv.second->teams())
            if (t) for (auto& p : t->roster) if (p) tier1_players.insert(p.get());
    std::vector<PlayerPtr> qualified;
    for (auto& kv : solo_qs) {
        for (auto& p : kv.second->global_ladder()) {
            if (!p) continue;
            if (p->season_matches < 4) continue;
            if (!tier1_players.count(p.get())) continue;
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
                       std::function<double(const PlayerPtr&)> scorer) {
        RaceLeaderboard lb;
        lb.category = cat;
        std::vector<std::pair<PlayerPtr, double>> ranked;
        for (auto& p : qualified) {
            if (!gate(p)) continue;
            ranked.emplace_back(p, scorer(p));
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
            // Populate the per-factor breakdown for the headline MVP Race so the
            // UI can show WHY (role/IGL races use hybrid_score — leave empty).
            if (cat == "MVP Race" && c.player)
                c.breakdown = mvp_score_breakdown(*c.player);
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

    // The headline MVP Race ranks by the SAME composite that decides the MVP
    // award (mvp_composite_score), so the season-long race actually predicts
    // the winner. Role / IGL races keep the lighter hybrid_score (they loosely
    // mirror the role awards, not the MVP).
    auto mvp_scorer    = [&](const PlayerPtr& p) { return mvp_composite_score(*p); };
    auto hybrid_09     = [&](const PlayerPtr& p) { return hybrid_score(p, 0.9); };
    auto hybrid_07     = [&](const PlayerPtr& p) { return hybrid_score(p, 0.7); };
    build_lb("MVP Race",
        [&](const PlayerPtr& p) { return attended_intl(p) || season_rating(*p) >= 1.10; },
        mvp_scorer);
    auto role_gate = [](Role r) {
        return [r](const PlayerPtr& p) { return p->primary_role == r; };
    };
    build_lb("Duelist Race",   role_gate(Role::Duelist),    hybrid_09);
    build_lb("Initiator Race", role_gate(Role::Initiator),  hybrid_09);
    build_lb("Controller Race",role_gate(Role::Controller), hybrid_09);
    build_lb("Sentinel Race",  role_gate(Role::Sentinel),   hybrid_09);
    build_lb("IGL Race",
        [](const PlayerPtr& p) {
            if (!p->is_igl) return false;
            if (p->is_retired) return false;
            if (p->team_name == "Free Agent" || p->team_name == "Retired") return false;
            return true;
        },
        hybrid_07);

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
    // Hard gate: 8+ season matches AND on a tier-1 (leagues) roster — lower-tier
    // players now accumulate season_matches from their own stage play, so they
    // must be excluded or a Challengers player could win a tier-1 award.
    std::unordered_set<Player*> tier1_players;
    for (auto& kv : leagues)
        if (kv.second) for (auto& t : kv.second->teams())
            if (t) for (auto& p : t->roster) if (p) tier1_players.insert(p.get());
    std::vector<PlayerPtr> qualified;
    for (auto& kv : solo_qs) {
        for (auto& p : kv.second->global_ladder()) {
            if (!p) continue;
            if (p->season_matches < 8) continue;
            if (!tier1_players.count(p.get())) continue;
            qualified.push_back(p);
        }
    }
    // Player-award pool can be empty in an edge season (very short world / thin
    // schedule). Do NOT early-return here: every player add_award() below safely
    // no-ops on an empty pool, and the self-contained Coach of the Year block at
    // the END of this function must still mint (it never reads `qualified`).
    // (WS-B review fix: COTY was previously skipped whenever no player qualified.)

    auto season_rating = [](const Player& p) {
        return p.season_matches > 0
            ? p.season_rating_total / p.season_matches : 0.0;
    };

    // Flat player -> team-wins map (built once; replaces the old per-call triple
    // loop and is shared by every scorer + the explanation builder).
    std::unordered_map<const Player*, int> team_wins_map;
    for (auto& kv : leagues)
        if (kv.second) for (auto& t : kv.second->teams())
            if (t) for (auto& tp : t->roster) if (tp) team_wins_map[tp.get()] = t->wins;
    auto team_wins_for = [&](const Player& p) -> int {
        auto it = team_wins_map.find(&p);
        return it != team_wins_map.end() ? it->second : 0;
    };

    // REAL international ATTENDANCE (maps played at Masters/Champions this
    // season) vs WINNING a title (an [M]/[W] award tag). These were conflated
    // before — "attended" actually meant "won", so a losing international
    // finalist got zero credit and could even be gated out of MVP.
    auto attended_intl = [](const Player& p) { return p.season_intl_matches > 0; };
    auto won_intl = [yr = year](const Player& p) {
        std::string yr_str = std::to_string(yr);
        for (auto& a : p.awards) {
            if (a.find(yr_str) == std::string::npos) continue;
            if (a.find("[M]") != std::string::npos || a.find("[W]") != std::string::npos)
                return true;
        }
        return false;
    };

    // Format a winner's headline season stats for the award explanation.
    auto fmt_player_line = [&](const Player& p) -> std::string {
        double rat = season_rating(p);
        double kd  = (p.season_deaths > 0)
                     ? static_cast<double>(p.season_kills) / p.season_deaths
                     : static_cast<double>(p.season_kills);
        int kast = (p.season_rounds > 0)
                   ? static_cast<int>(std::round(100.0 * p.season_rounds_with_kast / p.season_rounds))
                   : 0;
        char b[224];
        std::snprintf(b, sizeof(b),
            "%.2f rating, %.2f K/D, %d%% KAST, %d clutches over %d maps (%dW season%s)",
            rat, kd, kast, p.season_clutch_pts, p.season_matches, team_wins_for(p),
            p.season_intl_matches > 0 ? ", intl attendee" : "");
        return std::string(b);
    };

    auto add_award = [&](const std::string& cat,
                          std::function<double(const Player&)> score,
                          std::function<bool(const PlayerPtr&)> gate,
                          const std::string& category_note) {
        std::vector<std::pair<PlayerPtr, double>> ranked;
        for (auto& p : qualified) {
            if (!gate(p)) continue;
            ranked.emplace_back(p, score(*p));
        }
        if (ranked.empty()) return;
        // Deterministic: stable_sort + explicit tie-break so the permanently
        // pinned (GOAT-feeding) trophy never depends on ladder iteration order.
        std::stable_sort(ranked.begin(), ranked.end(),
            [](const std::pair<PlayerPtr, double>& a,
               const std::pair<PlayerPtr, double>& b) {
                if (a.second != b.second) return a.second > b.second;
                if (a.first->career_matches != b.first->career_matches)
                    return a.first->career_matches > b.first->career_matches;
                return a.first->name < b.first->name;
            });
        // Never mint a trophy from an all-zero pool (e.g. a thin role pool
        // where every qualifier scored 0).
        if (ranked.front().second <= 0.0) return;

        SeasonAward aw;
        aw.year = year;
        aw.category = cat;
        aw.winner = ranked.front().first;
        for (std::size_t i = 0; i < std::min<std::size_t>(3, ranked.size()); ++i) {
            aw.finalists.push_back(ranked[i].first);
            aw.scores.push_back(ranked[i].second);
        }

        // Rich, stat-cited explanation + the exact margin over the runner-up.
        std::string expl = aw.winner->name + " (" + aw.winner->team_name + "): "
                         + fmt_player_line(*aw.winner) + ".";
        if (ranked.size() > 1 && ranked[1].second > 0.0) {
            char mb[200];
            std::snprintf(mb, sizeof(mb), " Edged %s (%.2f index) by +%.2f%s%s.",
                ranked[1].first->name.c_str(), ranked[1].second,
                ranked.front().second - ranked[1].second,
                category_note.empty() ? "" : " — ", category_note.c_str());
            expl += mb;
        } else if (!category_note.empty()) {
            expl += "  " + category_note;
        }
        aw.explanation = expl;

        last_season_awards.push_back(aw);
        awards_history.push_back(aw);

        // Pin the award to the winner's career awards.
        std::string award_str = "[A] " + cat + " " + std::to_string(year);
        bool exists = false;
        for (auto& a : aw.winner->awards) if (a == award_str) { exists = true; break; }
        if (!exists) aw.winner->awards.push_back(award_str);

        char buf[224];
        std::snprintf(buf, sizeof(buf), "[AWARD] %s %d: %s (%.2f)",
                      cat.c_str(), year, aw.winner->name.c_str(), ranked.front().second);
        log.emplace_back(buf);

        NewsItem n;
        n.year = year; n.day_in_year = day_in_year;
        n.category = "Awards";
        n.headline = aw.winner->name + " wins " + cat + " " + std::to_string(year);
        n.body = expl;
        n.player_name = aw.winner->name;
        n.team_name = aw.winner->team_name;
        push_news(n);
        // One award chokepoint -> one mail. Fires for MVP, every role-of-the-
        // year, IGL, ROTY, Most Improved when the winner is on the user club.
        // Grammar: "... of the Year" categories already carry their own tail —
        // the generic suffix read "Duelist of the Year of the season!". Only
        // plain categories (MVP, Most Improved) keep " of the season!".
        std::string mail_subject = (cat.find("of the Year") != std::string::npos)
            ? aw.winner->name + " named " + cat + "!"
            : aw.winner->name + " named " + cat + " of the season!";
        mail_user(aw.winner->team_name,
                  "mail/award/" + cat + "/" + aw.winner->name + "/" + std::to_string(year),
                  MailCategory::Award,
                  mail_subject,
                  aw.winner->name + " of " + aw.winner->team_name + " has been "
                      "voted " + cat + " for " + std::to_string(year) + ". " + expl,
                  MailLink::Player, aw.winner->name, 0, true, aw.winner->id);
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
    // The formula now lives in GameManager::mvp_composite_score so the MVP
    // award and the season-long MVP Race (update_mvp_race) rank by the EXACT
    // same math — they used to diverge (the race ranked by a separate additive
    // hybrid_score, so the race leader often was not the eventual MVP). The
    // documented formula above is implemented there; this is a thin alias kept
    // so the award/role/IGL scorers below read uniformly.
    auto mvp_score = [&](const Player& p) -> double {
        return mvp_composite_score(p);
    };
    add_award("MVP",
        mvp_score,
        [&](const PlayerPtr& p) {
            // Soft attendance gate: international attendees qualify, and a
            // domestic monster (rating >= 1.20) can still slip through so a
            // complete carry season on a weak team can win.
            if (attended_intl(*p)) return true;
            return season_rating(*p) >= 1.20;
        },
        "rating x clutch x IGL x international (attend + win) x playoff x team success");

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
        double f = 1.0;
        if (attended_intl(p)) f *= 1.05;   // attended a Masters/Champions
        if (won_intl(p))      f *= 1.08;   // won a title (stacks)
        return f;
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

            double team_w = static_cast<double>(team_wins_for(p));
            double raw = 0.35 * rat
                       + 0.25 * per_match_impact
                       + 0.20 * team_finals
                       + 0.20 * (team_w / 40.0);
            // Scale to roughly the same ~1.0-1.6 range as the MVP/role indices
            // so the "Score" shown side-by-side in the Awards UI is comparable
            // (the raw composite sat near ~0.7 and read as a far weaker season).
            return raw * 2.0;
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
        "rating + per-match IGL impact + finals reach + season wins (scaled)");

    // === Rookie of the Year ===
    // Best DEBUT-season pro by rating x team success. Two fixes vs the old gate
    // (which let the same player win every year): (1) ONCE-EVER — scan p->awards
    // and exclude anyone who has ever won ROTY (the pinned "[A] Rookie of the
    // Year <year>" string persists across seasons); (2) strictly the debut
    // season (career_seasons_played == 0 — the counter is incremented later in
    // save_history_and_progress, AFTER awards run, so a genuine first-year pro
    // reads 0 here).
    add_award("Rookie of the Year",
        [&](const Player& p) { return season_rating(p) * role_team_factor(p); },
        [&](const PlayerPtr& p) {
            for (const auto& a : p->awards)
                if (a.find("Rookie of the Year") != std::string::npos) return false;
            return p->career_seasons_played == 0 && season_rating(*p) >= 0.90;
        },
        "best debut-season pro: rating x team success");

    // === Most Improved Player ===
    // Biggest jump in season rating vs the player's PRIOR season. At this point
    // (before save_history_and_progress runs) history.back() is last season, so
    // history holds prior years only — exactly the baseline we want.
    auto prior_rating = [&](const Player& p) -> double {
        double last = -1.0;
        for (auto& h : p.history) {
            if (h.year == year - 1) return h.rating;       // exact prior season
            if (h.year < year && h.rating > 0.0) last = h.rating;  // fallback: last known
        }
        return last;
    };
    add_award("Most Improved",
        [&](const Player& p) {
            double pr = prior_rating(p);
            if (pr < 0.0) return 0.0;          // no prior season -> not a candidate
            return season_rating(p) - pr;       // the improvement delta
        },
        [&](const PlayerPtr& p) {
            double pr = prior_rating(*p);
            return pr >= 0.0 && (season_rating(*p) - pr) > 0.05;
        },
        "biggest rating jump vs last season");

    // === Flex of the Year (P5.2) ==========================================
    // The best player who MEANINGFULLY played off his primary role this season.
    // flex_index = off-role share * off-role quality * role-breadth bonus,
    // consuming the P4.0 per-map counters. Reuses add_award's all-zero guard +
    // deterministic stable_sort + news/mail/career-pin; an empty pool mints
    // nothing (the UI placeholder covers that gracefully).
    add_award("Flex of the Year",
        [&](const Player& p) -> double {
            int total = 0, breadth = 0;
            for (int rm : p.season_role_maps) { total += rm; if (rm > 0) ++breadth; }
            if (total <= 0 || p.season_offrole_matches <= 0) return 0.0;
            double share   = static_cast<double>(p.season_offrole_matches) / total;
            double quality = p.season_offrole_rating_total / p.season_offrole_matches;
            return share * quality * (1.0 + 0.15 * std::max(0, breadth - 1));
        },
        [&](const PlayerPtr& p) -> bool {
            int total = 0;
            for (int rm : p->season_role_maps) total += rm;
            if (total <= 0 || p->season_offrole_matches <= 0) return false;
            double share   = static_cast<double>(p->season_offrole_matches) / total;
            double quality = p->season_offrole_rating_total / p->season_offrole_matches;
            return p->season_matches >= 8 && share >= 0.25 && quality >= 0.95;
        },
        "most impactful off-role / multi-role campaign");

    // === Coach of the Year (WS-B INC-3) ===================================
    // Cosmetic award — no match/dev effect. Rewards the tier-1 head coach whose
    // season most combined silverware + win record + craft. Stored as a
    // SeasonAward with coach_winner set (winner stays null) so the recap/history
    // UI can render it alongside the player trophies.
    {
        struct CoachCand { CoachPtr coach; Team* team; double score; };
        std::vector<CoachCand> cc;
        for (auto& kv : leagues) {
            if (!kv.second) continue;
            for (auto& t : kv.second->teams()) {
                if (!t || !t->head_coach) continue;
                Coach& c = *t->head_coach;
                int titles = t->titles_in_year(year);
                int net_w  = t->wins - t->losses;
                double dev01 = clamp_v(c.development / 99.0, 0.0, 1.0);
                double score = titles * 6.0
                             + (net_w > 0 ? net_w * 0.6 : net_w * 0.2)
                             + dev01 * 2.5
                             + (c.match_synergy_mult() - 1.0) * 6.0;
                if (score <= 0.0) continue;
                cc.push_back({t->head_coach, t.get(), score});
            }
        }
        if (!cc.empty()) {
            std::stable_sort(cc.begin(), cc.end(),
                [](const CoachCand& a, const CoachCand& b) {
                    if (a.score != b.score) return a.score > b.score;
                    if (a.coach->career_titles != b.coach->career_titles)
                        return a.coach->career_titles > b.coach->career_titles;
                    return a.coach->name < b.coach->name;
                });
            if (cc.front().score > 0.0) {
                SeasonAward aw;
                aw.year = year;
                aw.category = "Coach of the Year";
                aw.coach_winner = cc.front().coach;
                for (std::size_t i = 0; i < std::min<std::size_t>(3, cc.size()); ++i) {
                    aw.coach_finalists.push_back(cc[i].coach);
                    aw.scores.push_back(cc[i].score);
                }
                Coach& wc = *cc.front().coach;
                Team*  wt = cc.front().team;
                char eb[288];
                std::snprintf(eb, sizeof(eb),
                    "%s (%s): %d title(s), %d-%d record this season, %d career "
                    "title(s), rep %d. Tac %d / Dev %d / Lead %d.",
                    wc.name.c_str(), wt->name.c_str(),
                    wt->titles_in_year(year), wt->wins, wt->losses,
                    wc.career_titles, wc.reputation,
                    wc.tactical, wc.development, wc.leadership);
                aw.explanation = eb;
                wc.career_coty += 1;
                last_season_awards.push_back(aw);
                awards_history.push_back(aw);

                char buf[224];
                std::snprintf(buf, sizeof(buf),
                    "[AWARD] Coach of the Year %d: %s (%.2f)",
                    year, wc.name.c_str(), cc.front().score);
                log.emplace_back(buf);

                NewsItem n;
                n.year = year; n.day_in_year = day_in_year;
                n.category = "Awards";
                n.headline = wc.name + " wins Coach of the Year " + std::to_string(year);
                n.body = aw.explanation;
                n.team_name = wt->name;
                push_news(n);
                mail_user(wt->name,
                          "mail/award/CoachOfTheYear/" + wc.name + "/" + std::to_string(year),
                          MailCategory::Award,
                          wc.name + " named Coach of the Year!",
                          wc.name + " of " + wt->name + " has been voted Coach of the "
                              "Year for " + std::to_string(year) + ". " + aw.explanation,
                          MailLink::Manager, "", 0, true);
            }
        }
    }
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

// === Watchlist (increment C) — transfer/scouting targets, distinct from favs ===
void GameManager::watch_player(const PlayerPtr& p) {
    if (!p) return;
    for (auto& w : watchlist_) if (w.player == p) return;   // dedup by identity
    watchlist_.push_back(WatchEntry{p, "", WatchStatus::Watching});
}
void GameManager::unwatch_player(const PlayerPtr& p) {
    watchlist_.erase(
        std::remove_if(watchlist_.begin(), watchlist_.end(),
                       [&](const WatchEntry& w) { return w.player == p; }),
        watchlist_.end());
}
bool GameManager::is_watched(const PlayerPtr& p) const {
    for (auto& w : watchlist_) if (w.player == p) return true;
    return false;
}
GameManager::WatchEntry* GameManager::watch_entry(const PlayerPtr& p) {
    for (auto& w : watchlist_) if (w.player == p) return &w;
    return nullptr;
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
    // "last 30 days" on a monotonic absolute-day timeline so the window spans
    // the year boundary cleanly (year*kDaysPerYear + day_in_year). The old
    // same-year/prev-year-tail wrap only caught a single prior year and
    // dropped legitimate news whenever the 30-day window straddled Jan 1.
    long long now_abs    = static_cast<long long>(year) * kDaysPerYear + day_in_year;
    long long cutoff_abs = now_abs - 30;
    for (auto& n : news_feed) {
        long long n_abs = static_cast<long long>(n.year) * kDaysPerYear + n.day_in_year;
        if (n_abs < cutoff_abs || n_abs > now_abs) continue;
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
                mail_user(t->name, "mail/" + key,
                          MailCategory::Squad,
                          "Breakout: " + p->name + " is having a career year",
                          std::string(body) + " One of your own is taking a real "
                              "step up — a piece to build around.",
                          MailLink::Player, p->name, 0, false, p->id);
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
                mail_user(t->name, "mail/" + key,
                          MailCategory::Squad,
                          p->name + " is in a worrying slump",
                          std::string(body) + " A struggling starter — a coaching "
                              "or roster question worth weighing.",
                          MailLink::Player, p->name, 0, false, p->id);
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
    mail_user(winner->name, "mail/" + key,
              MailCategory::Media,
              winner->name + " on a " + std::to_string(threshold) + "-series win streak",
              std::string("The press is taking notice: your club has reeled off ") +
                  std::to_string(threshold) + " series wins in a row. Momentum is "
                  "firmly on your side.",
              MailLink::Manager, "", 0, false);
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
                mail_user(p->team_name, "mail/" + key,
                          MailCategory::Media,
                          p->name + " hits " + std::to_string(m) + " career kills",
                          p->name + " just crossed " + std::to_string(m) + " career "
                              "kills in your colors — a counter most pros never reach.",
                          MailLink::Player, p->name, 0, false, p->id);
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
                mail_user(p->team_name, "mail/" + key,
                          MailCategory::Media,
                          p->name + " reaches " + std::to_string(m) + " career matches",
                          p->name + " has now played " + std::to_string(m) + " career "
                              "pro matches for your club — longevity is its own greatness.",
                          MailLink::Player, p->name, 0, false, p->id);
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
                mail_user(p->team_name, "mail/" + key,
                          MailCategory::Media,
                          p->name + " wins career trophy #" + std::to_string(m),
                          p->name + " has now won " + std::to_string(m) + " career "
                              "trophies with your club — the Hall-of-Fame conversation "
                              "gets louder.",
                          MailLink::Player, p->name, 0, false, p->id);
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
            mail_user(p->team_name, "mail/" + key,
                      MailCategory::Squad,
                      p->name + " may be nearing retirement",
                      std::string(body) + " Now is the time to plan a succession at "
                          "the position before the drop-off forces your hand.",
                      MailLink::Player, p->name, 0, true, p->id);
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
    mail_user(winner->name, "mail/" + key,
              MailCategory::Result,
              "Upset! " + winner->name + " take down #" + std::to_string(loser_seed) +
                  " " + loser->name,
              std::string(body) + " A statement result — your run continues.",
              MailLink::Manager, "", 0, false);
    mail_user(loser->name,
              "mail/upsetloss/" + tour.name() + "/" + std::to_string(year) + "/" + winner->name,
              MailCategory::Result,
              "Upset: " + winner->name + " knock you out",
              std::string(body) + " A costly early exit as a top seed — the board "
                  "will want a response.",
              MailLink::Manager, "", 0, true);
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
            mail_user(t->name, "mail/" + key,
                      MailCategory::Media,
                      "Dynasty watch: " + t->name + " among the era's best",
                      std::string(body) + " The press is calling your club a dynasty "
                          "in the making — protect the window while it's open.",
                      MailLink::Manager, "", 0, false);
        }
    }
}

void GameManager::snapshot_year_end_state() {
    max_kills_snapshot_at_year_start_.clear();
    max_kd_snapshot_at_year_start_.clear();
    gf_clutches_snapshot_at_year_start_.clear();

    // Shift retirement-countdown ratings: 2yo <- 1yo, 1yo <- current season rating.
    // Build the new maps ONLY over currently-active ladder players. The old
    // 2yo slot inherited the full prior 1yo map (`= last_year_rating_
    // snapshot_`), carrying every retired Player* forward forever and growing
    // unbounded across seasons. The sole consumer (emit_retirement_countdown)
    // only ever .find()s by an active Player*, so dropping retired entries is
    // behavior-preserving.
    std::unordered_set<Player*> active;
    for (auto& kv : solo_qs) {
        for (auto& p : kv.second->global_ladder()) {
            if (p) active.insert(p.get());
        }
    }
    std::unordered_map<Player*, double> new_two_years_ago;
    new_two_years_ago.reserve(last_year_rating_snapshot_.size());
    for (auto& kv : last_year_rating_snapshot_) {
        if (active.count(kv.first)) new_two_years_ago.insert(kv);
    }
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

// ======================================================================
//  Multi-tier esports pyramid (Challengers + promotion/relegation)
// ======================================================================
namespace {
struct TierConfig {
    int teams;                  // teams per region at this tier
    int budget_lo, budget_hi;   // starting budget range
    int prestige_lo, prestige_hi;
    int ovr_cap;                // FA-pool quality cap for auto_fill (0 = uncapped)
    const char* label;          // division name shown in UI ("VCT", "Challengers", ...)
};
// Index = tier - 1. Tier-1 numbers mirror initialize_world's tier-1 loop so
// build_tier_teams(region,1) would reproduce it; lower tiers are weaker.
const std::vector<TierConfig> kTiers = {
    /*T1*/ {12, 480000, 720000, 35, 80,  0, "VCT"},
    /*T2*/ {10, 200000, 360000, 15, 45, 62, "Challengers"},
    /*T3*/ {12,  90000, 180000,  5, 25, 50, "Open Circuit"},
};

// Region's main headquarter countries — same spec the tier-1 loop uses,
// promoted to a file-static so build_tier_teams can share it.
std::vector<const Country*> region_main_countries(Region r) {
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
    for (auto& n : wanted) { const Country* c = find_country(n); if (c) out.push_back(c); }
    return out;
}
}  // namespace

int GameManager::tier_count(const std::string& region) const {
    auto it = tier_leagues_.find(region);
    return it == tier_leagues_.end() ? 0 : static_cast<int>(it->second.size());
}

std::shared_ptr<League> GameManager::league_at(const std::string& region, int tier) const {
    auto it = tier_leagues_.find(region);
    if (it == tier_leagues_.end()) return nullptr;
    int idx = tier - 1;
    if (idx < 0 || idx >= static_cast<int>(it->second.size())) return nullptr;
    return it->second[idx];
}

std::shared_ptr<League> GameManager::league_for(const TeamPtr& t) const {
    if (!t) return nullptr;
    auto it = tier_leagues_.find(t->region);
    if (it == tier_leagues_.end()) return nullptr;
    for (auto& lg : it->second) {
        if (!lg) continue;
        for (auto& mt : lg->teams()) if (mt == t) return lg;
    }
    return nullptr;
}

int GameManager::tier_of(const TeamPtr& t) const {
    auto lg = league_for(t);
    return lg ? lg->tier() : 1;
}

std::vector<TeamPtr> GameManager::build_tier_teams(const std::string& region, int tier) {
    std::vector<TeamPtr> out;
    if (tier < 1 || tier > static_cast<int>(kTiers.size())) return out;
    const TierConfig& tc = kTiers[tier - 1];
    Region reg = region_from_str(region);
    auto mains = region_main_countries(reg);
    out.reserve(tc.teams);
    for (int i = 0; i < tc.teams; ++i) {
        std::string tn = take_team_name();
        long long budget = rng().irange(tc.budget_lo, tc.budget_hi);
        auto team = std::make_shared<Team>(std::move(tn), budget, region);
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
        team->head_coach = generate_coach(region);
        team->head_coach->team_name = team->name;
        team->head_coach->contract_exp_year = year + team->head_coach->contract_years - 1;
        team->head_scout = generate_scout(region);   // WS-A: lower-tier clubs get a scout too
        team->head_scout->team_name = team->name;
        team->head_scout->contract_exp_year = year + team->head_scout->contract_years - 1;
        team->head_analyst = generate_analyst(region);   // Match-Prep: + an analyst
        team->head_analyst->team_name = team->name;
        team->head_analyst->contract_exp_year = year + team->head_analyst->contract_years - 1;
        team->prestige = rng().irange(tc.prestige_lo, tc.prestige_hi);
        team->sponsorship_k = 200 + team->prestige * 8;
        team->strategy = classify_team_strategy(*team);
        team->identity = compute_team_identity(*team);   // WS-B: lower-tier init
        out.push_back(team);
    }
    return out;
}

void GameManager::generate_lower_tiers(std::vector<std::string>& log) {
    for (auto* region_str : kRegions) {
        std::string r(region_str);
        for (int tier = 2; tier <= static_cast<int>(kTiers.size()); ++tier) {
            const TierConfig& tc = kTiers[tier - 1];
            auto teams = build_tier_teams(r, tier);
            auto lg = std::make_shared<League>(
                std::string(tc.label) + " " + r, teams, r, tier, tc.label);

            // Cheap roster fill from the LEFTOVER regional FA pool (tier-1 has
            // already drafted). ovr_cap keeps lower tiers genuinely weaker.
            std::vector<PlayerPtr> sub;
            auto sqit = solo_qs.find(r);
            if (sqit != solo_qs.end() && sqit->second) {
                for (auto& p : sqit->second->global_ladder()) {
                    if (!p || p->is_retired) continue;
                    if (p->team_name != "Free Agent") continue;
                    if (tc.ovr_cap == 0 || p->ovr() < tc.ovr_cap) sub.push_back(p);
                }
            }
            for (auto& t : lg->teams()) {
                if (!t) continue;
                t->auto_fill_roster(sub, year);    // enforces import cap + 1-IGL
                t->strategy = classify_team_strategy(*t);
                t->identity = compute_team_identity(*t);   // WS-B
                t->previous_strategy = t->strategy;
                t->enforce_one_igl();
                t->enforce_one_flex();
            }
            lg->generate_schedule();
            tier_leagues_[r].push_back(lg);
        }
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "[Tier] %s: generated %d lower-division league(s).",
                      r.c_str(), static_cast<int>(kTiers.size()) - 1);
        log.emplace_back(buf);
    }
}

// Tally the series winner from the per-map recordings (mirrors the
// narrative-winner derivation in play_stage_round). Returns +1 if `a` won
// more maps, -1 if `b` did, 0 on a tie/ambiguous.
static int series_winner_sign(const std::vector<RecordedMatchPtr>& recs,
                              const TeamPtr& a, const TeamPtr& b) {
    int aw = 0, bw = 0;
    for (auto& rec : recs) {
        if (!rec) continue;
        auto t1 = rec->team1.lock();
        auto t2 = rec->team2.lock();
        if (rec->blue_score > rec->red_score) {
            if (t1 == a) ++aw; else if (t1 == b) ++bw;
        } else if (rec->red_score > rec->blue_score) {
            if (t2 == a) ++aw; else if (t2 == b) ++bw;
        }
    }
    return (aw > bw) ? 1 : (bw > aw ? -1 : 0);
}

void GameManager::play_tier_stage_round() {
    for (auto& kv : tier_leagues_) {
        auto& vec = kv.second;
        // index 0 aliases the tier-1 league (played by play_stage_round); skip.
        for (std::size_t ti = 1; ti < vec.size(); ++ti) {
            auto& lg = vec[ti];
            if (!lg) continue;
            if (t2_week >= static_cast<int>(lg->weekly_matchups().size())) continue;
            std::string event_label = lg->division_name() + " " + kv.first
                                    + " Stage " + std::to_string(phase_idx + 1);
            for (auto& mu : lg->weekly_matchups()[t2_week]) {
                if (!mu.a || !mu.b) continue;
                // is_league_play=true records wins/phase_wins for both teams.
                sim_series_returning_all_matches(mu.a, mu.b, /*bo=*/3,
                                                 event_label, /*league_play=*/true);
            }
            for (auto& t : lg->teams()) if (t) t->budget += 5000;  // small stage stipend
        }
    }
    t2_week += 1;
}

TeamPtr GameManager::run_bracket_to_completion(std::vector<TeamPtr> field,
                                               const std::string& event_name) {
    std::vector<TeamPtr> alive;
    for (auto& t : field) if (t) alive.push_back(t);
    if (alive.empty()) return nullptr;
    // Synchronous single-elimination: highest seed gets the bye on odd rounds.
    // Always halves -> always terminates. is_league_play=false -> no phantom W/L.
    // `field` arrives sorted best-seed-first. Seed properly each round
    // (top vs bottom: 1vN, 2v(N-1), ...) so the two best teams can only meet in
    // the final, and the top seed byes on an odd count.
    int guard = 0;
    while (alive.size() > 1 && guard++ < 16) {
        std::vector<TeamPtr> next;
        std::size_t lo = 0, hi = alive.size() - 1;
        if (alive.size() % 2 == 1) { next.push_back(alive[0]); lo = 1; }
        while (lo < hi) {
            TeamPtr a = alive[lo], b = alive[hi];
            auto recs = sim_series_returning_all_matches(a, b, /*bo=*/3,
                                                         event_name, /*league_play=*/false);
            int sgn = series_winner_sign(recs, a, b);
            next.push_back(sgn >= 0 ? a : b);   // higher seed (a) wins ties
            ++lo; --hi;
        }
        alive = next;
    }
    return alive.empty() ? nullptr : alive.front();
}

void GameManager::apply_promotion(const TeamPtr& t, const std::string& region, int to_tier) {
    auto hi = league_at(region, to_tier);       // destination (higher division)
    auto lo = league_at(region, to_tier + 1);   // source (lower division)
    if (!t || !hi || !lo) return;
    auto& src = lo->teams();
    src.erase(std::remove(src.begin(), src.end(), t), src.end());
    hi->teams().push_back(t);   // for to_tier==1 this is also leagues[region]
    // Promotion windfall: bigger budget + a prestige bump (so the org can sign
    // higher-tier talent and not be instant relegation fodder). Sponsorship is
    // NOT hard-reset here — the year-end dynamic-sponsorship EMA grows it toward
    // the new prestige over the coming seasons (a static reset wiped the
    // accumulated rich/poor divergence).
    t->budget += 800000;
    t->prestige = clamp_v(t->prestige + 15, 1, 99);
}

void GameManager::apply_relegation(const TeamPtr& t, const std::string& region, int from_tier) {
    auto hi = league_at(region, from_tier);     // source (higher division)
    auto lo = league_at(region, from_tier + 1); // destination (lower division)
    if (!t || !hi || !lo) return;
    auto& src = hi->teams();
    src.erase(std::remove(src.begin(), src.end(), t), src.end());
    lo->teams().push_back(t);
    t->budget = std::max(0LL, t->budget - 200000);
    t->prestige = clamp_v(t->prestige - 12, 1, 99);
    // Sponsorship is NOT hard-reset — the dynamic-sponsorship EMA erodes it
    // toward the lower prestige over the coming seasons (a static reset wiped
    // the accumulated divergence).
    // Relegation shock: high-OVR players don't want to drop a division — they
    // grow disgruntled and many demand a move (ties into the discontent /
    // transfer-request system), so a relegated org tends to lose its stars to
    // teams still in the higher tier.
    for (auto& p : t->roster) {
        if (!p) continue;
        if (p->ovr() >= 75.0) {
            p->discontent = clamp_v(p->discontent + 0.40, 0.0, 1.0);
            if (p->discontent >= 0.6 && !p->transfer_requested) {
                p->transfer_requested    = true;
                p->transfer_request_year = year;
            }
        }
    }
}

void GameManager::resolve_promotion_relegation(std::vector<std::string>& log) {
    last_promo_rel_.clear();
    for (auto* region_str : kRegions) {
        std::string r(region_str);
        int tiers = tier_count(r);
        if (tiers < 2) continue;
        PromoRelResult result;
        result.region = r;

        auto sorted_by_wins = [](const std::shared_ptr<League>& lg) {
            std::vector<TeamPtr> v;
            if (lg) for (auto& t : lg->teams()) if (t) v.push_back(t);
            std::sort(v.begin(), v.end(),
                      [](const TeamPtr& a, const TeamPtr& b) { return a->wins > b->wins; });
            return v;
        };
        auto t1 = league_at(r, 1);
        auto t2 = league_at(r, 2);
        auto t3 = (tiers >= 3) ? league_at(r, 3) : nullptr;
        std::vector<TeamPtr> t1s = sorted_by_wins(t1);
        std::vector<TeamPtr> t2s = sorted_by_wins(t2);
        std::vector<TeamPtr> t3s = sorted_by_wins(t3);

        // === Decide ALL moves on PRE-swap standings ========================
        // Teams are distinct across leagues, so applying the moves afterward in
        // any order is safe (each team is erased from its source + appended to
        // its destination exactly once). Tier sizes are conserved per region:
        // T1: -1 releg +1 (best T2) = 0; T2: -1 (best up) +1 (T1 releg in)
        // +2 (T3 up) -2 (bottom-2 down) = 0; T3: -2 (top-2 up) +2 (T2 down) = 0.

        // --- T1<->T2: best Tier-2 promotes; bottom-3 Tier-1 gauntlet drops 1 ---
        TeamPtr best_t2 = t2s.empty() ? nullptr : t2s.front();
        TeamPtr relegated_t1;
        if (t1s.size() >= 3) {
            std::size_t n = t1s.size();
            TeamPtr s14 = t1s[n - 3];   // best of the bottom three
            TeamPtr s15 = t1s[n - 2];
            TeamPtr s16 = t1s[n - 1];   // LAST place — hardest road
            // Climbing ladder: the last seed must win BOTH BO3s to survive; the
            // other two need just one. Exactly ONE team relegates.
            //  - s16 loses the play-in (vs s15)      -> s16 relegated.
            //  - s16 wins play-in, loses decider(s14) -> s16 relegated.
            //  - s16 wins BOTH                        -> s16 stays, s15 (whom it
            //    knocked out in the play-in) relegates.
            auto pin = sim_series_returning_all_matches(
                s16, s15, /*bo=*/3, "Relegation Play-in " + r, /*league_play=*/false);
            if (series_winner_sign(pin, s16, s15) < 0) {
                relegated_t1 = s16;
            } else {
                auto dec = sim_series_returning_all_matches(
                    s16, s14, /*bo=*/3, "Relegation Decider " + r, /*league_play=*/false);
                relegated_t1 = (series_winner_sign(dec, s16, s14) < 0) ? s16 : s15;
            }
        } else if (!t1s.empty()) {
            relegated_t1 = t1s.back();   // tiny-league fallback: worst drops
        }

        // --- T2<->T3: top-2 Tier-3 promote; bottom-2 Tier-2 demote ---------
        std::vector<TeamPtr> promote_t3, demote_t2;
        if (t3) {
            for (std::size_t i = 0; i < std::min<std::size_t>(2, t3s.size()); ++i)
                promote_t3.push_back(t3s[i]);
            for (std::size_t i = 0; i < t2s.size() && demote_t2.size() < 2; ++i) {
                const TeamPtr& cand = t2s[t2s.size() - 1 - i];   // from the bottom up
                if (cand == best_t2) continue;                   // never demote the promoting team
                demote_t2.push_back(cand);
            }
        }

        // === Apply + announce ==============================================
        auto announce = [&](const TeamPtr& t, bool up, int dest_tier, const std::string& why) {
            if (!t) return;
            auto dl = league_at(r, dest_tier);
            std::string div = (dl && !dl->division_name().empty())
                ? dl->division_name() : ("Tier " + std::to_string(dest_tier));
            push_news(NewsItem{year, day_in_year, up ? "Promotion" : "Relegation",
                t->name + (up ? " promoted to " : " relegated to ") + r + " " + div, why,
                t->name, ""});
            mail_user(t->name,
                "mail/" + std::string(up ? "promoted" : "relegated") + "/" + t->name
                    + "/" + std::to_string(year),
                MailCategory::Board,
                up ? (t->name + " PROMOTED to " + r + " " + div + "!")
                   : (t->name + " relegated to " + r + " " + div),
                why, MailLink::Manager, "", 0, true);
        };

        if (best_t2 && relegated_t1) {
            apply_relegation(relegated_t1, r, 1);   // T1 -> T2
            apply_promotion(best_t2, r, 1);         // T2 -> T1
            result.relegated.push_back(relegated_t1);
            result.promoted.push_back(best_t2);
            result.ascension_champion = best_t2;
            announce(best_t2, true, 1,
                best_t2->name + " finished top of Tier 2 and earns promotion to the top flight.");
            announce(relegated_t1, false, 2,
                relegated_t1->name + " lost the Tier-1 relegation gauntlet and drops a division.");
        }
        for (auto& d : demote_t2) {
            apply_relegation(d, r, 2);              // T2 -> T3
            result.relegated.push_back(d);
            announce(d, false, 3, d->name + " finished in the Tier-2 drop zone.");
        }
        for (auto& p : promote_t3) {
            apply_promotion(p, r, 2);               // T3 -> T2
            result.promoted.push_back(p);
            announce(p, true, 2, p->name + " finished top-2 of Tier 3 and is promoted.");
        }

        last_promo_rel_.push_back(result);
        log.emplace_back("[Pro/Rel] " + r + ": best T2 up + 1 T1 relegation gauntlet; "
            + std::to_string(promote_t3.size()) + " T3 up / "
            + std::to_string(demote_t2.size()) + " T2 down.");
    }
}

void GameManager::run_scouting_pass(int sign_year, std::vector<std::string>& log) {
    // For each region: build the candidate pool ONCE (tier-2/3 RISERS rostered
    // on Challengers teams + top SOLO-Q free agents), then let each tier-1 AI
    // team scout it on genuine need. Team::score_scout_target is the gate — it
    // returns 0 unless the player fills a real role hole/weakness AND beats the
    // incumbent, so teams only act when a target genuinely fits a need.
    int scouted = 0;
    for (auto& kv : leagues) {
        const std::string& region = kv.first;

        // (1) Tier-2/3 risers — high ceiling OR strong recent form. Paired with
        // their current lower-tier team so a poach can release there first.
        std::vector<std::pair<TeamPtr, PlayerPtr>> risers;
        for (int tier = 2; tier <= tier_count(region); ++tier) {
            auto lg = league_at(region, tier);
            if (!lg) continue;
            for (auto& tt : lg->teams()) {
                if (!tt) continue;
                for (auto& p : tt->roster) {
                    if (!p || p->is_retired) continue;
                    bool rising = (p->potential >= 72)
                               || (p->season_matches >= 6 && p->avg_match_rating() >= 1.05);
                    if (rising) risers.emplace_back(tt, p);
                }
            }
        }

        // (2) Top solo-Q free agents in the region.
        std::vector<PlayerPtr> soloq;
        auto sit = solo_qs.find(region);
        if (sit != solo_qs.end() && sit->second) {
            for (auto& p : sit->second->global_ladder()) {
                if (!p || p->is_retired) continue;
                if (p->team_name != "Free Agent") continue;
                if (p->solo_mmr >= 1100 && p->potential >= 68) soloq.push_back(p);
            }
        }
        if (risers.empty() && soloq.empty()) continue;

        for (auto& t : kv.second->teams()) {
            if (!t) continue;
            if (t == user_team && !user_auto_manage) continue;

            RoleNeed need = t->compute_role_need();
            if (need.most_need_score < 0.15) continue;   // matches score_scout_target's gate

            int cur_imports = 0;
            for (auto& x : t->roster) if (x && x->region != t->region) ++cur_imports;
            bool import_ok = (cur_imports < config().max_imports);

            // WS-A INC-5: the club's scouting NETWORK sets how well it reads
            // FOREIGN talent — a thin network down-weights out-of-region targets
            // (a soft SCORE bias, never a hard cut, so small markets aren't starved).
            double reach = t->head_scout
                ? 0.75 + 0.25 * clamp_v((t->head_scout->network - 40) / 59.0, 0.0, 1.0)
                : 0.75;
            // P3.1: difficulty sharpens AI scouting. sharp 1.0 (Normal, what
            // --dynasty runs at) is a strict no-op; harder reads foreign talent
            // better + lowers the upgrade bar inside score_scout_target.
            double sharp = world_difficulty();
            if (t == user_team) sharp = 1.0;   // never re-tune the user's own AI
            reach = clamp_v(reach * sharp, 0.4, 1.0);

            PlayerPtr best; double best_score = 0.0; TeamPtr best_from;
            for (auto& rp : risers) {
                auto& p = rp.second;
                if (!p || p->team_name != rp.first->name) continue;   // already poached
                if (!import_ok && p->region != t->region) continue;   // 2-import cap
                double sc = t->score_scout_target(*p, need, sign_year, sharp);
                if (p->region != t->region) sc *= reach;              // foreign-reach bias
                if (sc > best_score) { best_score = sc; best = p; best_from = rp.first; }
            }
            for (auto& p : soloq) {
                if (!p || p->team_name != "Free Agent") continue;     // already signed
                if (!import_ok && p->region != t->region) continue;
                double sc = t->score_scout_target(*p, need, sign_year, sharp);
                if (p->region != t->region) sc *= reach;              // foreign-reach bias (INC-5)
                if (sc > best_score) { best_score = sc; best = p; best_from = nullptr; }
            }
            if (!best || best_score <= 0.0) continue;
            // WS-A INC-4: respect the transfer window before committing (gate is
            // BEFORE the make-room release so a closed window frees nobody).
            if (!transfers_open()) continue;

            // Recover the ROLE this signing is meant to fill (the candidate's
            // natural role if it was the need, else the most-needed role for a
            // cross-role flex). We pin the contract to it below — otherwise the
            // 3-arg sign would lock the player to their NATURAL role and the
            // scouted hole would silently reopen (the phantom-fill bug). was_hole
            // and the swap-to-starter logic also key off the FILLED role.
            Role fill_role = best->primary_role;
            t->score_scout_target(*best, need, sign_year, sharp, &fill_role);

            int nr = static_cast<int>(fill_role);
            bool was_hole = (nr >= 0 && nr < 4 && need.count[nr] == 0);

            // Make room if the bench is full (cap 7): drop the WEAKEST player in
            // the needed role (score_scout_target already proved `best` beats it).
            if (t->roster.size() >= 7) {
                int dropr = (need.most_needed != Role::Count)
                            ? static_cast<int>(need.most_needed) : nr;
                PlayerPtr weakest; double wv = 1e9;
                for (auto& p : t->roster) {
                    if (!p || static_cast<int>(p->primary_role) != dropr) continue;
                    if (p->ovr() < wv) { wv = p->ovr(); weakest = p; }
                }
                if (!weakest) {
                    for (std::size_t i = 5; i < t->roster.size(); ++i) {
                        if (t->roster[i] && t->roster[i]->ovr() < wv) {
                            wv = t->roster[i]->ovr(); weakest = t->roster[i];
                        }
                    }
                }
                if (!weakest || best->ovr() <= wv + 1.0) continue;  // not worth the churn
                t->release_player(weakest);
            }

            // Poach (never-free): a tier-2 RISER under contract costs a TRANSFER
            // FEE paid to the selling Challengers club — that fee is their
            // develop-and-sell income (the whole point of a poor org). A solo-Q
            // FREE AGENT costs wages only (no fee, no seller). Compute the fee
            // BEFORE release (transfer_fee_for reads team_name), guard the buyer
            // can afford it, then move the money + log + sign.
            int fee_k = t->transfer_fee_for(*best, sign_year);
            // Future-finance discipline (A3): only complete the transfer if the
            // fee + the new wage fit the club's wage envelope / insolvency floor.
            // A stretched club passes on the riser; a rich win-now club (looser
            // envelope via the spend fork) buys. This is the buy side of the fork.
            if (!t->within_wage_envelope(best->contract.amount_k, fee_k)) continue;
            if (best_from) best_from->release_player(best);
            t->pay_transfer_fee(best_from, *best, sign_year, fee_k);   // buyer->seller + log
            int yrs = std::max(1, t->decide_contract_years(*best, sign_year));
            t->sign_player(best, yrs, sign_year, fill_role);   // PIN to the filled role; sign_year (=year+1)
            ++scouted;

            // If we just filled a true HOLE, make sure the signing actually
            // STARTS — swap it in for the weakest starter in an over-covered
            // role (otherwise it sits on the bench and the hole persists).
            if (was_hole && t->roster.size() > 5) {
                int rc[4] = {0, 0, 0, 0};
                for (int i = 0; i < 5 && i < static_cast<int>(t->roster.size()); ++i)
                    if (t->roster[i]) {
                        int rr = static_cast<int>(t->roster[i]->primary_role);
                        if (rr >= 0 && rr < 4) ++rc[rr];
                    }
                PlayerPtr bench_out; double bv = 1e9;
                for (int i = 0; i < 5 && i < static_cast<int>(t->roster.size()); ++i) {
                    auto& sp = t->roster[i];
                    if (!sp) continue;
                    int rr = static_cast<int>(sp->primary_role);
                    if (rr < 0 || rr >= 4 || rc[rr] < 2) continue;   // only over-covered roles
                    if (sp->ovr() < bv) { bv = sp->ovr(); bench_out = sp; }
                }
                if (bench_out) t->swap_roster_slots(bench_out, best);
            }

            char buf[300];
            if (fee_k > 0) {
                std::snprintf(buf, sizeof(buf),
                    "[SCOUT] %s signed %s (%s, %d OVR / %d POT) from %s for a $%dK fee to fill a %s need.",
                    t->name.c_str(), best->name.c_str(), role_name(best->primary_role),
                    static_cast<int>(std::lround(best->ovr())), best->potential,
                    best_from ? best_from->name.c_str() : "Solo Q", fee_k,
                    role_name(fill_role));
            } else {
                std::snprintf(buf, sizeof(buf),
                    "[SCOUT] %s signed free agent %s (%s, %d OVR / %d POT) to fill a %s need.",
                    t->name.c_str(), best->name.c_str(), role_name(best->primary_role),
                    static_cast<int>(std::lround(best->ovr())), best->potential,
                    role_name(fill_role));
            }
            log.emplace_back(buf);
        }
    }
    if (scouted > 0) {
        char buf[120];
        std::snprintf(buf, sizeof(buf),
            "[SCOUT] %d tier-2/solo-Q risers signed across the league on genuine needs.",
            scouted);
        log.emplace_back(buf);
    }
}

int GameManager::predict_sponsorship_k(const Team& t) const {
    // The annual sponsorship the market would offer THIS org: a prestige base,
    // plus recent on-field results, plus STAR POWER (a marquee player draws
    // sponsors). This is the "target" the actual figure drifts toward.
    int wins = t.wins - t.losses;   // this season's live W/L (called at the finance pass)
    double star = 0.0;
    for (auto& p : t.roster) if (p) star = std::max(star, p->ovr());
    int star_bonus = static_cast<int>(std::max(0.0, star - 72.0) * 7.0);  // a 78-OVR star ~+42
    int target = 200 + t.prestige * 8 + clamp_v(wins, -40, 60) + star_bonus;
    return clamp_v(target, kSponsorFloorK, kSponsorCeilK);
}

void GameManager::apply_dynamic_sponsorship(Team& t, int tier, int yr) {
    (void)tier; (void)yr;
    // EMA toward the market target (60% inertia) so sponsorship COMPOUNDS with
    // prestige + results rather than snapping. Clamped to the band so the
    // rich/poor gap widens over seasons without a runaway or a death spiral.
    int target = predict_sponsorship_k(t);
    int next = static_cast<int>(std::lround(0.60 * t.sponsorship_k + 0.40 * target));
    t.sponsorship_k = clamp_v(next, kSponsorFloorK, kSponsorCeilK);
}

void GameManager::run_coach_market(std::vector<std::string>& log) {
    // 1. Expire AI coaches whose contracts ended -> the FA coach pool (reused,
    //    not discarded, so a decorated name re-enters the market). The USER's
    //    coach is left alone — the user hires/fires their own staff.
    for (auto& kv : leagues) {
        for (auto& t : kv.second->teams()) {
            if (!t || t == user_team || !t->head_coach) continue;
            Coach& c = *t->head_coach;
            if (c.is_retired) { t->head_coach.reset(); continue; }
            if (c.contract_exp_year > 0 && c.contract_exp_year < year) {
                c.team_name = "Free Agent";
                free_coaches_.push_back(t->head_coach);
                t->head_coach.reset();
            }
        }
    }
    // 2. Keep the FA pool stocked with options (some up-and-comers).
    while (free_coaches_.size() < 30)
        free_coaches_.push_back(generate_coach(kRegions[rng().irange(0, 2)]));

    // 3. Hire + POACH. A RICH org lands the best available (highest-rep
    //    affordable) coach and will POACH a much better one over its current; a
    //    POOR org can only afford a cheaper up-and-comer (elite names priced out).
    int hires = 0, poaches = 0;
    for (auto& kv : leagues) {
        for (auto& t : kv.second->teams()) {
            if (!t || t == user_team) continue;
            bool rich = (t->wealth_tier == WealthTier::Rich ||
                         t->wealth_tier == WealthTier::SuperRich);
            int cur_rep = t->head_coach ? t->head_coach->reputation : -1;
            int best_i = -1, best_rep = -1;
            for (std::size_t i = 0; i < free_coaches_.size(); ++i) {
                auto& fc = free_coaches_[i];
                if (!fc || fc->is_retired) continue;
                long long cost = static_cast<long long>(fc->requested_salary_k()) * 1000LL;
                if (cost > std::max<long long>(0, t->budget)) continue;   // can't afford the wage
                if (!rich && fc->reputation > 72) continue;               // elite names go to rich orgs
                if (fc->reputation > best_rep) { best_rep = fc->reputation; best_i = static_cast<int>(i); }
            }
            bool need    = (!t->head_coach);
            bool upgrade = (t->head_coach && rich && best_rep > cur_rep + 15);
            if ((need || upgrade) && best_i >= 0) {
                bool poached = (t->head_coach != nullptr);
                if (poached) {   // old coach back into the pool
                    t->head_coach->team_name = "Free Agent";
                    free_coaches_.push_back(t->head_coach);
                    ++poaches;
                }
                CoachPtr hired = free_coaches_[best_i];
                free_coaches_.erase(free_coaches_.begin() + best_i);
                hired->team_name = t->name;
                hired->contract_years = rng().irange(2, 4);
                hired->contract_exp_year = year + hired->contract_years - 1;
                hired->salary_k = hired->requested_salary_k();
                t->head_coach = hired;
                ++hires;
                char buf[200];
                std::snprintf(buf, sizeof(buf),
                    "[COACH] %s %s %s (rep %d, $%dK/yr).",
                    t->name.c_str(), poached ? "poached" : "hired",
                    hired->name.c_str(), hired->reputation, hired->salary_k);
                log.emplace_back(buf);
            }
            // Fallback: still no coach (pool empty / all unaffordable) -> generate.
            if (!t->head_coach) {
                t->head_coach = generate_coach(t->region);
                t->head_coach->team_name = t->name;
                t->head_coach->contract_exp_year = year + t->head_coach->contract_years - 1;
            }
        }
    }
    if (free_coaches_.size() > 80) free_coaches_.resize(80);
    (void)hires; (void)poaches;
}

bool GameManager::user_hire_staff(const std::string& role, std::uint64_t staff_id) {
    if (!user_team) return false;
    Team& t = *user_team;

    // Each role mirrors run_coach_market's never-free-safe poach path: recycle the
    // displaced incumbent into the FA pool BEFORE pulling the new hire out of it
    // (appending the old one at the end leaves the hire's index valid).
    if (role == "coach") {
        int idx = -1;
        for (std::size_t i = 0; i < free_coaches_.size(); ++i) {
            const auto& c = free_coaches_[i];
            if (c && c->id == staff_id && !c->is_retired) { idx = static_cast<int>(i); break; }
        }
        if (idx < 0) return false;
        if (t.head_coach) {                       // recycle incumbent (never-free)
            t.head_coach->team_name = "Free Agent";
            free_coaches_.push_back(t.head_coach);
        }
        CoachPtr hired = free_coaches_[idx];
        free_coaches_.erase(free_coaches_.begin() + idx);
        hired->team_name        = t.name;
        hired->contract_years   = 2;
        hired->contract_exp_year = year + hired->contract_years - 1;
        hired->salary_k         = hired->requested_salary_k();
        t.head_coach = hired;
        return true;
    }
    if (role == "scout") {
        int idx = -1;
        for (std::size_t i = 0; i < free_scouts_.size(); ++i) {
            const auto& s = free_scouts_[i];
            if (s && s->id == staff_id && !s->is_retired) { idx = static_cast<int>(i); break; }
        }
        if (idx < 0) return false;
        if (t.head_scout) {
            t.head_scout->team_name = "Free Agent";
            free_scouts_.push_back(t.head_scout);
        }
        ScoutPtr hired = free_scouts_[idx];
        free_scouts_.erase(free_scouts_.begin() + idx);
        hired->team_name        = t.name;
        hired->contract_years   = 2;
        hired->contract_exp_year = year + hired->contract_years - 1;
        hired->salary_k         = hired->requested_salary_k();
        t.head_scout = hired;
        return true;
    }
    if (role == "analyst") {
        int idx = -1;
        for (std::size_t i = 0; i < free_analysts_.size(); ++i) {
            const auto& a = free_analysts_[i];
            if (a && a->id == staff_id && !a->is_retired) { idx = static_cast<int>(i); break; }
        }
        if (idx < 0) return false;
        if (t.head_analyst) {
            t.head_analyst->team_name = "Free Agent";
            free_analysts_.push_back(t.head_analyst);
        }
        AnalystPtr hired = free_analysts_[idx];
        free_analysts_.erase(free_analysts_.begin() + idx);
        hired->team_name        = t.name;
        hired->contract_years   = 2;
        hired->contract_exp_year = year + hired->contract_years - 1;
        hired->salary_k         = hired->requested_salary_k();
        t.head_analyst = hired;
        return true;
    }
    return false;
}

bool GameManager::user_fire_staff(const std::string& role) {
    if (!user_team) return false;
    Team& t = *user_team;
    if (role == "coach") {
        if (!t.head_coach) return false;
        t.head_coach->team_name = "Free Agent";
        free_coaches_.push_back(t.head_coach);   // never-free: recycle into the FA pool
        t.head_coach.reset();
        return true;
    }
    if (role == "scout") {
        if (!t.head_scout) return false;
        t.head_scout->team_name = "Free Agent";
        free_scouts_.push_back(t.head_scout);
        t.head_scout.reset();
        return true;
    }
    if (role == "analyst") {
        if (!t.head_analyst) return false;
        t.head_analyst->team_name = "Free Agent";
        free_analysts_.push_back(t.head_analyst);
        t.head_analyst.reset();
        return true;
    }
    return false;
}

PlayerPtr GameManager::find_player_by_id(std::uint64_t id) const {
    for (const auto& kv : leagues) {
        if (!kv.second) continue;
        for (const auto& t : kv.second->teams()) {
            if (!t) continue;
            for (const auto& p : t->roster) if (p && p->id == id) return p;
        }
    }
    for (const auto& kv : solo_qs) {
        if (!kv.second) continue;
        for (const auto& p : kv.second->global_ladder()) if (p && p->id == id) return p;
    }
    for (const auto& p : hall_of_fame) if (p && p->id == id) return p;
    return nullptr;
}

int GameManager::commission_scout(const PlayerPtr& target, int money_k) {
    if (!user_team || !target) return 0;
    const Scout* sc = user_team->head_scout.get();
    int coverage = sc ? scout_country_coverage(*sc, target->country_iso) : 8;   // no scout -> near-blind
    int depth    = sc ? scout_read_depth(*sc) : 25;
    if (money_k < 0) money_k = 0;
    // Re-commissioning an in-progress player REPLACES the old assignment — refund its
    // spend first so the club isn't double-charged (the net debit is the difference).
    ScoutAssignment* existing = nullptr;
    for (auto& ex : scout_assignments_)
        if (ex.player_id == target->id && !ex.done) { existing = &ex; break; }
    if (existing) user_team->budget += static_cast<long long>(existing->money_k) * 1000LL;
    // Never spend below zero: clamp the investment to the (post-refund) budget.
    long long cost = static_cast<long long>(money_k) * 1000LL;
    if (user_team->budget < cost) {
        money_k = static_cast<int>(std::max<long long>(0, user_team->budget / 1000));
        cost = static_cast<long long>(money_k) * 1000LL;
    }
    // 7..21 days: better coverage + more money -> faster; a near-blind read can't be rushed.
    int days = static_cast<int>(21.0 - coverage * 0.10 - money_k * 0.06 + 0.5);
    if (days < 7)  days = 7;
    if (days > 21) days = 21;
    if (coverage < 25) days = std::max(days, 17);   // out-of-knowledge stays slow regardless of cash
    user_team->budget -= cost;
    ScoutAssignment a;
    a.player_id = target->id; a.days_remaining = days; a.total_days = days;
    a.money_k = money_k; a.coverage = coverage; a.depth = depth;
    if (existing) *existing = a; else scout_assignments_.push_back(a);
    return days;
}

void GameManager::tick_scout_assignments() {
    if (scout_assignments_.empty()) return;   // dynasty-safe fast path (empty on the test path)
    for (auto& a : scout_assignments_) {
        if (a.done) continue;
        if (a.days_remaining > 0) --a.days_remaining;
        if (a.days_remaining <= 0) {
            a.done = true;
            int acc = static_cast<int>(a.coverage * (0.5 + 0.5 * a.depth / 100.0) + 0.5);
            if (acc > 100) acc = 100;
            scout_report_accuracy_[a.player_id] = acc;
            if (a.coverage >= 40) {           // decent coverage reveals the exact ceiling
                PlayerPtr p = find_player_by_id(a.player_id);
                if (p) p->potential_scouted = true;
            }
        }
    }
    if (scout_assignments_.size() > 40)
        scout_assignments_.erase(scout_assignments_.begin(),
                                 scout_assignments_.begin() +
                                     static_cast<std::ptrdiff_t>(scout_assignments_.size() - 40));
}

// --- FM-style scouting briefs (filter-based assignments) --------------------
void GameManager::resolve_user_name_collision() {
    if (!user_team) return;
    for (auto& kv : tier_leagues_) {
        for (auto& lg : kv.second) {
            if (!lg) continue;
            for (auto& t : lg->teams()) {
                if (!t || t == user_team || t->name != user_team->name) continue;
                std::string fresh = take_team_name();
                t->name = fresh;
                t->tag  = make_team_tag(fresh);
                for (auto& p : t->roster) if (p) p->team_name = fresh;
                if (t->head_coach)   t->head_coach->team_name   = fresh;
                if (t->head_scout)   t->head_scout->team_name   = fresh;
                if (t->head_analyst) t->head_analyst->team_name = fresh;
                seed_team_colors(*t);
            }
        }
    }
}

bool GameManager::player_on_user_team(std::uint64_t player_id) const {
    if (!user_team) return false;
    for (const auto& rp : user_team->roster)
        if (rp && rp->id == player_id) return true;
    return false;
}

bool GameManager::brief_matches(const ScoutBrief& b, const Player& p) const {
    if (p.is_retired) return false;
    if (b.role_idx >= 0 && static_cast<int>(p.primary_role) != b.role_idx) return false;
    if (p.age < b.age_min || p.age > b.age_max) return false;
    if (p.potential < b.min_pot) return false;
    if (!b.region.empty() && p.region != b.region) return false;
    return true;
}

void GameManager::add_scout_brief(const ScoutBrief& b) {
    scout_briefs_.push_back(b);
    if (scout_briefs_.size() > 8) scout_briefs_.erase(scout_briefs_.begin());   // cap the list
}

void GameManager::clear_scout_brief(int idx) {
    if (idx >= 0 && idx < static_cast<int>(scout_briefs_.size()))
        scout_briefs_.erase(scout_briefs_.begin() + idx);
}

void GameManager::tick_scout_briefs() {
    if (scout_briefs_.empty()) return;   // dynasty-safe fast path (empty on the test path)
    if (!user_team) return;
    // The head scout's read depth sets the per-tick reveal rate (1-3 / brief / day).
    int rate = 1, depth = 60;
    if (user_team->head_scout) {
        depth = scout_read_depth(*user_team->head_scout);
        rate  = 1 + depth / 40;
    }
    for (auto& b : scout_briefs_) {
        ++b.days_active;
        int total = 0, did = 0;
        for (auto& kv : solo_qs) {
            if (!kv.second) continue;
            for (auto& p : kv.second->global_ladder()) {
                if (!p || !brief_matches(b, *p)) continue;
                ++total;
                if (did >= rate || p->potential_scouted) continue;
                // Region-gate: the scout must cover this player's region well enough
                // (grounds the reveal in the Increment-12 knowledge model).
                int cov = 100;
                if (user_team->head_scout) cov = scout_region_coverage(*user_team->head_scout, p->region);
                if (cov < 25) continue;   // out of the scout's knowledge -> can't assess
                p->potential_scouted = true;               // UI-fog only; sim unaffected
                int acc = static_cast<int>(cov * (0.5 + 0.5 * depth / 100.0) + 0.5);
                scout_report_accuracy_[p->id] = acc > 100 ? 100 : acc;
                ++b.revealed; ++did;
            }
        }
        b.match_total = total;
    }
}

int GameManager::scout_report_accuracy(std::uint64_t player_id) const {
    auto it = scout_report_accuracy_.find(player_id);
    return it != scout_report_accuracy_.end() ? it->second : 0;
}

const GameManager::ScoutAssignment* GameManager::active_scout_assignment(std::uint64_t player_id) const {
    for (const auto& a : scout_assignments_)
        if (a.player_id == player_id && !a.done) return &a;
    return nullptr;
}

void GameManager::run_scout_market(std::vector<std::string>& log) {
    // Mirror of run_coach_market for the head_scout staff slot (WS-A). AI clubs
    // only; the user hires/fires their own scout. Expired/poached scouts recycle
    // through free_scouts_ (never-free), and every AI club ends with a scout.
    for (auto& kv : leagues) {
        for (auto& t : kv.second->teams()) {
            if (!t || t == user_team || !t->head_scout) continue;
            Scout& sc = *t->head_scout;
            if (sc.is_retired) { t->head_scout.reset(); continue; }
            if (sc.contract_exp_year > 0 && sc.contract_exp_year < year) {
                sc.team_name = "Free Agent";
                free_scouts_.push_back(t->head_scout);
                t->head_scout.reset();
            }
        }
    }
    while (free_scouts_.size() < 30)
        free_scouts_.push_back(generate_scout(kRegions[rng().irange(0, 2)]));

    int hires = 0, poaches = 0;
    for (auto& kv : leagues) {
        for (auto& t : kv.second->teams()) {
            if (!t || t == user_team) continue;
            bool rich = (t->wealth_tier == WealthTier::Rich ||
                         t->wealth_tier == WealthTier::SuperRich);
            int cur_rep = t->head_scout ? t->head_scout->reputation : -1;
            int best_i = -1, best_rep = -1;
            for (std::size_t i = 0; i < free_scouts_.size(); ++i) {
                auto& fs = free_scouts_[i];
                if (!fs || fs->is_retired) continue;
                long long cost = static_cast<long long>(fs->requested_salary_k()) * 1000LL;
                if (cost > std::max<long long>(0, t->budget)) continue;
                if (!rich && fs->reputation > 72) continue;
                if (fs->reputation > best_rep) { best_rep = fs->reputation; best_i = static_cast<int>(i); }
            }
            bool need    = (!t->head_scout);
            bool upgrade = (t->head_scout && rich && best_rep > cur_rep + 15);
            if ((need || upgrade) && best_i >= 0) {
                bool poached = (t->head_scout != nullptr);
                if (poached) {
                    t->head_scout->team_name = "Free Agent";
                    free_scouts_.push_back(t->head_scout);
                    ++poaches;
                }
                ScoutPtr hired = free_scouts_[best_i];
                free_scouts_.erase(free_scouts_.begin() + best_i);
                hired->team_name = t->name;
                hired->contract_years = rng().irange(2, 4);
                hired->contract_exp_year = year + hired->contract_years - 1;
                hired->salary_k = hired->requested_salary_k();
                t->head_scout = hired;
                ++hires;
                char buf[200];
                std::snprintf(buf, sizeof(buf),
                    "[SCOUT] %s %s %s (rep %d, $%dK/yr).",
                    t->name.c_str(), poached ? "poached" : "hired",
                    hired->name.c_str(), hired->reputation, hired->salary_k);
                log.emplace_back(buf);
            }
            if (!t->head_scout) {
                t->head_scout = generate_scout(t->region);
                t->head_scout->team_name = t->name;
                t->head_scout->contract_exp_year = year + t->head_scout->contract_years - 1;
            }
        }
    }
    if (free_scouts_.size() > 80) free_scouts_.resize(80);
    (void)hires; (void)poaches;
}

void GameManager::run_analyst_market(std::vector<std::string>& log) {
    // Mirror of run_scout_market for the head_analyst staff slot (Match-Prep).
    // AI clubs only; the user hires/fires their own analyst. Expired/poached
    // analysts recycle through free_analysts_ (never-free); every AI club ends
    // with an analyst. career_reports is bumped at report-generation time (the
    // opposition-report component), mirroring how career_finds is bumped at
    // discovery time — NOT here.
    for (auto& kv : leagues) {
        for (auto& t : kv.second->teams()) {
            if (!t || t == user_team || !t->head_analyst) continue;
            Analyst& an = *t->head_analyst;
            if (an.is_retired) { t->head_analyst.reset(); continue; }
            if (an.contract_exp_year > 0 && an.contract_exp_year < year) {
                an.team_name = "Free Agent";
                free_analysts_.push_back(t->head_analyst);
                t->head_analyst.reset();
            }
        }
    }
    while (free_analysts_.size() < 30)
        free_analysts_.push_back(generate_analyst(kRegions[rng().irange(0, 2)]));

    int hires = 0, poaches = 0;
    for (auto& kv : leagues) {
        for (auto& t : kv.second->teams()) {
            if (!t || t == user_team) continue;
            bool rich = (t->wealth_tier == WealthTier::Rich ||
                         t->wealth_tier == WealthTier::SuperRich);
            int cur_rep = t->head_analyst ? t->head_analyst->reputation : -1;
            int best_i = -1, best_rep = -1;
            for (std::size_t i = 0; i < free_analysts_.size(); ++i) {
                auto& fa = free_analysts_[i];
                if (!fa || fa->is_retired) continue;
                long long cost = static_cast<long long>(fa->requested_salary_k()) * 1000LL;
                if (cost > std::max<long long>(0, t->budget)) continue;
                if (!rich && fa->reputation > 72) continue;
                if (fa->reputation > best_rep) { best_rep = fa->reputation; best_i = static_cast<int>(i); }
            }
            bool need    = (!t->head_analyst);
            bool upgrade = (t->head_analyst && rich && best_rep > cur_rep + 15);
            if ((need || upgrade) && best_i >= 0) {
                bool poached = (t->head_analyst != nullptr);
                if (poached) {
                    t->head_analyst->team_name = "Free Agent";
                    free_analysts_.push_back(t->head_analyst);
                    ++poaches;
                }
                AnalystPtr hired = free_analysts_[best_i];
                free_analysts_.erase(free_analysts_.begin() + best_i);
                hired->team_name = t->name;
                hired->contract_years = rng().irange(2, 4);
                hired->contract_exp_year = year + hired->contract_years - 1;
                hired->salary_k = hired->requested_salary_k();
                t->head_analyst = hired;
                ++hires;
                char buf[200];
                std::snprintf(buf, sizeof(buf),
                    "[ANALYST] %s %s %s (rep %d, $%dK/yr).",
                    t->name.c_str(), poached ? "poached" : "hired",
                    hired->name.c_str(), hired->reputation, hired->salary_k);
                log.emplace_back(buf);
            }
            if (!t->head_analyst) {
                t->head_analyst = generate_analyst(t->region);
                t->head_analyst->team_name = t->name;
                t->head_analyst->contract_exp_year = year + t->head_analyst->contract_years - 1;
            }
        }
    }
    if (free_analysts_.size() > 80) free_analysts_.resize(80);
    (void)hires; (void)poaches;
}

void GameManager::project_team_finances(Team& t, int tier, int yr) {
    (void)yr;
    // Committed cost next season: wage bill (total_payroll_k already includes the
    // head coach's salary) + tier-scaled operating cost.
    int payroll_k = t.total_payroll_k();
    double op_mult = (tier <= 1) ? 1.0 : (tier == 2 ? 0.55 : 0.35);
    int op_k = static_cast<int>((150 + t.prestige * 4) * op_mult);
    t.committed_payroll_k = payroll_k + op_k;
    // Projected income next season: sponsorship + a stage-stipend estimate
    // (~11 matchdays * $10K) + a modest tier-scaled prize expectation.
    // Tournament purses ARE now actually paid at bracket finish (see
    // tournament_purse_k + the prize_paid blocks) — prize_k here stays a
    // flat planning ESTIMATE, deliberately not the realized payout.
    int stipend_k = 110;
    int prize_k   = (tier <= 1) ? 120 : (tier == 2 ? 40 : 15);
    t.projected_income_k = t.sponsorship_k + stipend_k + prize_k;
    // Wage envelope = how much NEW annual wage the org can responsibly ADD =
    // projected surplus, MINUS a reserve to re-sign the core (~20% of the wage
    // bill), PLUS a slice of cash reserves. Floored at 0.
    int surplus    = t.projected_income_k - t.committed_payroll_k;
    int reserve_k  = payroll_k / 5;
    int cash_slice = static_cast<int>(std::max<long long>(0LL, t.budget) / 4000LL);
    t.wage_envelope_k = std::max(0, surplus - reserve_k + cash_slice);
    t.wealth_tier = t.compute_wealth_tier();

    // Buy-vs-develop FORK (A4): a wealthy win-now club spends INTO its reserves
    // (looser envelope) to chase the title in its window; a rebuilder or a poor
    // club banks cash and builds through youth + development (tighter envelope).
    // The envelope GATE (within_wage_envelope) on the AI signing paths makes
    // these spend behaviors real instead of cosmetic.
    bool contender = (t.strategy == Team::Strategy::Contender ||
                      t.strategy == Team::Strategy::WinNow);
    bool rebuilder = (t.strategy == Team::Strategy::Rebuilding ||
                      t.strategy == Team::Strategy::DevelopmentFocus ||
                      t.strategy == Team::Strategy::TalentFarm);
    double spend_mult = 1.0;
    if (contender && (t.wealth_tier == WealthTier::Rich || t.wealth_tier == WealthTier::SuperRich))
        spend_mult = 1.40;   // rich win-now window: dip into the war chest
    else if (rebuilder)
        spend_mult = 0.55;   // rebuild: bank cash, develop in-house
    else if (t.wealth_tier == WealthTier::Poor)
        spend_mult = 0.70;   // poor non-rebuilder: cautious, but not crippled
    // The head coach's SIGNATURE also tilts the purse (C4): a win-now coach
    // (finance_lean>0, e.g. Pragmatist) loosens it; a frugal BudgetBalancer
    // tightens it. So org strategy AND coach identity both shape spending.
    double coach_fin = t.coach_lean().finance_lean;
    spend_mult *= clamp_v(1.0 + 0.30 * coach_fin, 0.60, 1.40);
    t.wage_envelope_k = std::max(0, static_cast<int>(t.wage_envelope_k * spend_mult));
}

void GameManager::announce_prospect_discovery(std::vector<std::string>& log) {
    (void)log;
    for (auto& kv : tier_leagues_) {
        for (auto& tier_lg : kv.second) {
            if (!tier_lg) continue;
            for (auto& t : tier_lg->teams()) {
                if (!t) continue;
                for (auto& p : t->roster) {
                    if (!p || p->is_retired) continue;
                    // A YOUNG, high-ceiling player who JUST signed their first pro
                    // deal this offseason (signings stamp joined_year = year + 1).
                    if (p->joined_year != year + 1) continue;
                    if (p->career_matches > 0) continue;       // already a pro -> not a first deal
                    if (p->age > 20 || p->potential < 78) continue;
                    if (p->team_name != t->name) continue;     // ghost guard

                    if (t->head_scout) t->head_scout->career_finds += 1;

                    int cur = static_cast<int>(std::lround(p->ovr()));
                    NewsItem n;
                    n.year = year; n.day_in_year = day_in_year;
                    n.category = "Prospect";
                    char hd[184];
                    std::snprintf(hd, sizeof(hd),
                        "%d-year-old %s signs first pro deal with %s",
                        p->age, p->name.c_str(), t->name.c_str());
                    n.headline = hd;
                    char bd[280];
                    std::snprintf(bd, sizeof(bd),
                        "Plucked out of ranked: %s (%s, OVR %d, ceiling %d) joins %s for "
                        "their first taste of the pro scene. One to watch.",
                        p->name.c_str(), position_name(position_of(*p)), cur,
                        p->potential, t->name.c_str());
                    n.body = bd;
                    n.team_name = t->name; n.player_name = p->name;
                    push_news(n);

                    // Mail the USER about their OWN academy signings, plus any
                    // generational (ceiling >= 85) talent emerging league-wide.
                    bool mine = (user_team && t == user_team);
                    if (user_team && (mine || p->potential >= 85)) {
                        mail_user(user_team->name,
                            "mail/phenom/" + std::to_string(p->id) + "/" + std::to_string(year),
                            MailCategory::Transfer,
                            (mine ? std::string("Your scouts land a prospect: ")
                                  : std::string("Rising star to watch: ")) + p->name,
                            (mine ? std::string("Your scouting department has signed ")
                                  : std::string("A blue-chip prospect has emerged \xE2\x80\x94 ")) +
                                p->name + " (" + std::to_string(p->age) + "yo " +
                                position_name(position_of(*p)) + ", ceiling " +
                                std::to_string(p->potential) + ") to a first pro deal at " +
                                t->name + ".",
                            MailLink::Player, p->name, 0, false, p->id);
                    }
                }
            }
        }
    }
}

void GameManager::run_lower_tier_offseason(std::vector<std::string>& log) {
    int refreshed = 0;
    for (auto* region_str : kRegions) {
        std::string r(region_str);
        auto sqit = solo_qs.find(r);
        for (int tier = 2; tier <= tier_count(r); ++tier) {
            auto lg = league_at(r, tier);
            if (!lg) continue;
            const TierConfig* tc = (tier - 1 < static_cast<int>(kTiers.size()))
                                   ? &kTiers[tier - 1] : nullptr;
            // Region-local, ovr-capped FA sub-pool (rebuilt fresh; auto_fill
            // re-filters team_name on each call so it drains correctly).
            std::vector<PlayerPtr> sub;
            if (sqit != solo_qs.end() && sqit->second) {
                for (auto& p : sqit->second->global_ladder()) {
                    if (!p || p->is_retired) continue;
                    if (p->team_name != "Free Agent") continue;
                    if (!tc || tc->ovr_cap == 0 || p->ovr() < tc->ovr_cap) sub.push_back(p);
                }
            }
            for (auto& t : lg->teams()) {
                if (!t) continue;
                // Re-sign useful / high-upside expiring players in place (so
                // developed lower-tier talent isn't auto-released), then drop
                // retired players and the ones the team passes on.
                std::vector<PlayerPtr> drops;
                for (auto& p : t->roster) {
                    if (!p) continue;
                    if (p->is_retired) { drops.push_back(p); continue; }
                    if (p->contract.exp_year <= year) {
                        bool keep = (p->avg_match_rating() >= 0.90)
                                 || (p->potential >= 70)
                                 || (tc && p->ovr() >= tc->ovr_cap - 8);
                        if (keep) {
                            int yrs = std::max(1, t->decide_contract_years(*p, year));
                            t->resign_player(p, yrs,
                                std::max(kSalaryFloorK, p->contract.amount_k), year + 1);
                        } else {
                            drops.push_back(p);
                        }
                    }
                }
                for (auto& p : drops) {
                    t->release_player(p);
                    p->team_name = p->is_retired ? "Retired" : "Free Agent";
                }
                if (t->head_coach && t->head_coach->is_retired) t->head_coach.reset();
                if (!t->head_coach) {
                    t->head_coach = generate_coach(r);
                    t->head_coach->team_name = t->name;
                    t->head_coach->contract_exp_year =
                        year + t->head_coach->contract_years - 1;
                }
                if (t->head_scout && t->head_scout->is_retired) t->head_scout.reset();
                if (t != user_team && !t->head_scout) {
                    t->head_scout = generate_scout(r);
                    t->head_scout->team_name = t->name;
                    t->head_scout->contract_exp_year =
                        year + t->head_scout->contract_years - 1;
                }
                if (t->head_analyst && t->head_analyst->is_retired) t->head_analyst.reset();
                if (t != user_team && !t->head_analyst) {
                    t->head_analyst = generate_analyst(r);
                    t->head_analyst->team_name = t->name;
                    t->head_analyst->contract_exp_year =
                        year + t->head_analyst->contract_years - 1;
                }
                // B2: archive season + reset W/L so next year's Ascension seeds
                // on the NEW season's wins, not an all-time accumulation.
                t->save_history(year);
                t->wins = 0; t->losses = 0; t->phase_wins = 0; t->phase_losses = 0;
                t->recent_results.clear();   // season-scope the FORM sparkline
                // Full AI GM pass: cut poor performers, scan targeted upgrades,
                // cascade-fill (the same depth tier-1 teams get) so lower-tier
                // teams actually develop instead of only auto-filling holes.
                t->ai_full_offseason_pass(sub, year + 1, log);
                t->strategy = classify_team_strategy(*t);
                t->identity = compute_team_identity(*t);   // WS-B
                t->enforce_one_igl();
                t->enforce_one_flex();
                ++refreshed;
            }
            lg->generate_schedule();
        }
    }
    if (refreshed > 0) {
        char buf[96];
        std::snprintf(buf, sizeof(buf),
                      "[Tier] Offseason refreshed %d lower-division teams.", refreshed);
        log.emplace_back(buf);
    }
}

}  // namespace vlr
