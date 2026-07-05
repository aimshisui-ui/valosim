#pragma once

// DashboardExport - JSON serializer for the user-team-centric "home dashboard"
// view consumed by the web UI proof-of-concept (ui-poc/dashboard.html).
//
// OUTPUT-ONLY. Reads GameManager / Team / Player state and produces a single
// JSON document matching the web dashboard's DATA contract. It does NOT mutate
// any engine state, consumes NO RNG, and adds NO persistent fields, so it is
// balance-neutral and safe to call at any point in a world's lifetime. Mirrors
// MatchExport.cpp's hand-rolled JsonWriter convention (C++17 stdlib only - no
// JSON library, no fmt).
//
// Schema (matches dashboard.html's `DATA`):
//   {
//     "schema_version": 1,
//     "club_color": "#RRGGBB",
//     "dashboard": {
//       "header":      { club_name, division, region, year, day_in_year, current_phase },
//       "objectives":  { total, met, goals:[{ text, mandatory, met, progress, pct }] },
//       "kpis":        [{ label, value, tone, subline?, today? }],
//       "standings":   [{ rank, team_name, wins, losses, ovr, is_user }],
//       "news":        [{ year, day, category, headline }],
//       "star_players":[{ name, primary_role, ovr, avg_rating, kd_ratio, igl }],
//       "next_match":  { opponent, opp_ovr, opp_rank, days_until } | null,
//       "last_5_form": [{ opponent, result, score }],
//       "budget_trend":[ number, ... ]
//     }
//   }

#include <cstdint>
#include <string>
#include <memory>
#include <vector>

namespace vlr {

class GameManager;
struct RecordedMatch;
using RecordedMatchPtr = std::shared_ptr<RecordedMatch>;   // mirrors Player.h

// Serialize the dashboard view for gm.user_team. Returns "{}" if there is no
// user team (pre-game / unloaded world). star_limit caps the star_players
// list (default 8 = the dashboard squad card capacity); news_limit caps the
// news feed slice (default 8).
std::string export_dashboard_to_json(const GameManager& gm,
                                     int star_limit = 8,
                                     int news_limit = 8);

// Serialize the deep Roster / Squad view for gm.user_team (the second web
// screen). Same OUTPUT-ONLY contract as the dashboard export. Returns "{}" if
// there is no user team. Emits:
//   { "club_color", "roster": {
//       header, kpis[6], team,
//       players:[{ slot, role, contract_role, age, country/iso, ovr, potential,
//                  growth, trajectory, ratings, season + career stat lines,
//                  igl/flex/transfer_requested, mood, contract, signature_agent,
//                  agent_pool, personality, attributes[], solo, accolades }],
//       chemistry:[{a,b,value}], staff:{coach|null, scout|null, analyst|null} } }
std::string export_roster_to_json(const GameManager& gm);

// Competition view: full league standings (with recent form) + upcoming schedule.
// view_region ""=user's region; view_tier 0=user's tier (1=VCT, 2=Challengers,
// 3=Open Circuit) — lets the UI browse ANY region/division. Same OUTPUT-ONLY contract.
std::string export_competition_to_json(const GameManager& gm,
                                       const std::string& view_region = std::string(),
                                       int view_tier = 0);

// Finance view: org-finance summary + KPIs, the salary breakdown (roster wages),
// staff costs, and the transfer log. Same OUTPUT-ONLY contract.
std::string export_finance_to_json(const GameManager& gm);

// Mail/Inbox view: the user club's inbox — unread count + recent items
// (subject/body/category/flags). Same OUTPUT-ONLY contract.
std::string export_mail_to_json(const GameManager& gm);

// Strategy / match-prep view: the next-opponent scouting report + per-map prep
// levels, agent overrides, and signature agents. Same OUTPUT-ONLY contract.
// (The set-prep / set-agents WRITES happen via the bridge, not here.)
std::string export_strategy_to_json(const GameManager& gm);

// Calendar view: the 11-phase season ring (current/past markers + current-phase
// progress) + the upcoming schedule for the user's region. OUTPUT-ONLY.
std::string export_calendar_to_json(const GameManager& gm);

// Market view: the free-agent pool (top by OVR) with asking prices + the user's
// budget/wage room. OUTPUT-ONLY. (The make-offer/sign WRITE is via the bridge.)
std::string export_market_to_json(const GameManager& gm);

// Awards view: the live MVP/role races + last season's awards recap + all-time
// history. OUTPUT-ONLY. (Races/history are empty until ~mid-season / a season ends.)
std::string export_awards_to_json(const GameManager& gm);

// Brackets view: every active tournament's full bracket (UB/LB/GF history +
// current matchups + champion). OUTPUT-ONLY. (Empty outside playoff phases.)
std::string export_brackets_to_json(const GameManager& gm);

// Team Profile view of ANY team (resolved by name): identity + record + roster +
// finances + staff + season history. OUTPUT-ONLY. Returns a {found:false} shell
// if the name doesn't resolve.
std::string export_team_profile_to_json(const GameManager& gm, const std::string& team_name);

// Player Profile modal for ANY player (resolved by stable id across the whole
// world): identity + OVR/POT (fog-of-war band for unscouted non-user players) +
// mood (active-team players) + ranked/career stats + attributes + agent pool +
// badges + accolades + season history + favorite/watch flags. OUTPUT-ONLY.
// Returns a {found:false} shell if the id doesn't resolve.
std::string export_player_profile_to_json(const GameManager& gm, std::uint64_t player_id);

// Staff Profile modal: ANY coach/scout/analyst by stable id — employed (any tier),
// free-agent, or retired. Identity + personality + reputation + contract + career +
// role-specific attribute bars (a generic `attrs` array so one modal serves all three
// roles). OUTPUT-ONLY. Returns a {found:false} shell if the id doesn't resolve.
std::string export_staff_profile_to_json(const GameManager& gm, std::uint64_t staff_id);

// Favorites list — the user's bookmarked players (GOAT/HoF curation). OUTPUT-ONLY.
std::string export_favorites_to_json(const GameManager& gm);

// Watchlist — the user's transfer/scouting targets with per-entry status + note.
// OUTPUT-ONLY (the status/note WRITES happen via the bridge).
std::string export_watchlist_to_json(const GameManager& gm);

// Re-sign negotiation PREVIEW for a user player (resolved by id): given proposed
// terms (years/salary/bonus/starter-promise/offered-role) it returns the player's
// asking price + the full acceptance breakdown (will_accept, verdict, factor
// labels, counter-offer). OUTPUT-ONLY — evaluate_resign_offer is const; nothing
// commits until the act:resign verb. Returns {found:false} if the id isn't a
// user-roster player. role_idx 0..3 = Duelist/Initiator/Controller/Sentinel; any
// other value means "offer the player's natural role" (no role-fit penalty).
std::string export_resign_preview_to_json(const GameManager& gm, std::uint64_t player_id,
                                          int years, int amount_k, int bonus_k,
                                          bool promise_starter, int role_idx);

// Records hub — all-time GOAT (career + season), Hall of Fame, and league-leader
// boards, computed over every player in the world (SoloQ ladders + hall of fame).
// OUTPUT-ONLY. Heavy (walks the whole player population) but read-only + rng-free.
std::string export_records_to_json(const GameManager& gm);

// Scout REPORT for a player (resolved by id): status (unscouted / in-progress with
// its 1-3 week countdown / scouted), and — once scouted — an FM-style categorized
// Strengths/Weaknesses breakdown, a potential assessment (exact or a band widened by
// a poor read), per-role fit, and a scout's verdict. Everything scales with the
// completed report's ACCURACY (scout coverage x read-depth): a vague out-of-region
// read shows fewer, hedged points + a wide band. OUTPUT-ONLY.
std::string export_scout_report_to_json(const GameManager& gm, std::uint64_t player_id);

// Scouting SCREEN — the user's head scout, their per-region + per-country KNOWLEDGE
// map (Scout.h coverage functions), the active 1-3 week assignments (with progress),
// and recently-completed reports. OUTPUT-ONLY.
std::string export_scouting_to_json(const GameManager& gm);

// New-Game wizard options — the region list + org-tier choices (with blurbs) +
// difficulty range for the start-career form. OUTPUT-ONLY / static-ish.
std::string export_newgame_options_to_json(const GameManager& gm);

// Recruitment hub: the full world player pool (all region ladders, active only)
// for a client-side filterable/sortable/infinite-scroll search table.
std::string export_recruitment_to_json(const GameManager& gm);

// Transfer-INTEREST analysis for a player (resolved by id): which rival clubs
// would genuinely PURSUE them — Team::score_scout_target > 0, the SAME scorer the
// AI uses to sign — and which ADMIRE but are priced out, each with reasons DERIVED
// FROM REAL STATE (positional hole, upgrade over the incumbent, youth+potential for
// rebuilders, form for contenders, an expiring incumbent's succession) plus the
// concrete blocker (funds / wage room / the seller's fee). Nothing is fabricated:
// every interested club actually clears the engine's own signing gate. OUTPUT-ONLY.
std::string export_player_interest_to_json(const GameManager& gm, std::uint64_t player_id);

// Post-match results — SERIES-AWARE. `series` is every map of the user's latest BO3/BO5
// (captured by the host from advance_day's DayResult since recordings are transient),
// in play order. Emits a series summary (best-of, map wins, winner, series MVP) plus a
// per-map array — each map carries header + scoreline, both teams' scoreboards
// (agent/K-D-A/rating/HS/ADR/MVP), the round-by-round timeline and a derived storyline.
// OUTPUT-ONLY. Returns {found:false} when empty. Drives the live match viewer.
struct RecordedMatch;
std::string export_match_to_json(const GameManager& gm,
                                 const std::vector<RecordedMatchPtr>& series);

}  // namespace vlr
