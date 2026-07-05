// MatchExport.cpp - JSON serializer for a finished series.
//
// Hand-rolled JSON writer (output-only, C++17 stdlib only - no JSON library,
// no boost, no fmt). Pretty-printed with 2-space indent.
//
// Schema version 1.
// Reserved for v2+: round_history (per-round events), economy_log,
// utility_impact, heatmap_kills, ability_usage, timeline_events.
//
// Defensive: nullptr-guards every team1.lock() / team2.lock() (PROJECT_GUIDE
// pitfall #23) and skips players with missing PlayerLine / PlayerMatchStats
// rather than crashing. Returns "" for an empty recordings vector.

#include "MatchExport.h"

#include "Agent.h"     // role_name
#include "Match.h"     // PlayerLine, HistoryRecord, PlayerMatchStats, RoundLog
#include "Player.h"    // Player, RecordedMatch
#include "Team.h"      // Team

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace vlr {

namespace {

// WS-C R3: RoundEndKind -> stable JSON string (schema v2 round_history).
inline const char* round_end_kind_str(RoundEndKind k) {
    switch (k) {
        case RoundEndKind::SpikeDetonation: return "spike_detonation";
        case RoundEndKind::Defuse:          return "defuse";
        case RoundEndKind::TimeExpiry:      return "time_expiry";
        case RoundEndKind::Elimination:
        default:                            return "elimination";
    }
}

// =============================================================
// Tiny output-only JSON writer.
// =============================================================
class JsonWriter {
public:
    JsonWriter() = default;

    std::string str() const { return out_; }

    void obj_open() {
        prefix_value();
        out_ += '{';
        ++indent_;
        first_ = true;
    }
    void obj_close() {
        --indent_;
        if (!first_) {
            out_ += '\n';
            indent_pad();
        }
        out_ += '}';
        first_ = false;
    }
    void arr_open() {
        prefix_value();
        out_ += '[';
        ++indent_;
        first_ = true;
    }
    void arr_close() {
        --indent_;
        if (!first_) {
            out_ += '\n';
            indent_pad();
        }
        out_ += ']';
        first_ = false;
    }

    // Key for the next value. Use before obj_open / arr_open or before any
    // raw value emitter. For top-level array entries use arr_value_*().
    void key(const std::string& k) {
        sep_for_member();
        out_ += '"';
        append_escaped(k);
        out_ += "\": ";
        pending_key_ = true;
    }

    // === Object members (key + value in one call) ===
    void kv_str(const std::string& k, const std::string& v) {
        sep_for_member();
        out_ += '"';
        append_escaped(k);
        out_ += "\": \"";
        append_escaped(v);
        out_ += '"';
    }
    void kv_int(const std::string& k, long long v) {
        sep_for_member();
        out_ += '"';
        append_escaped(k);
        out_ += "\": ";
        out_ += std::to_string(v);
    }
    void kv_bool(const std::string& k, bool v) {
        sep_for_member();
        out_ += '"';
        append_escaped(k);
        out_ += "\": ";
        out_ += (v ? "true" : "false");
    }
    void kv_double(const std::string& k, double v, int decimals = 2) {
        sep_for_member();
        out_ += '"';
        append_escaped(k);
        out_ += "\": ";
        out_ += format_double(v, decimals);
    }
    // Already-formatted JSON literal (used for nested objects/arrays via
    // recursive writers). Caller must ensure validity.
    void kv_raw(const std::string& k, const std::string& raw) {
        sep_for_member();
        out_ += '"';
        append_escaped(k);
        out_ += "\": ";
        out_ += raw;
    }

    // === Array elements (no key) ===
    void arr_str(const std::string& v) {
        sep_for_member();
        out_ += '"';
        append_escaped(v);
        out_ += '"';
    }
    void arr_int(long long v) {
        sep_for_member();
        out_ += std::to_string(v);
    }

private:
    std::string out_;
    int  indent_ = 0;
    bool first_ = true;          // first member of current container?
    bool pending_key_ = false;   // a key was just emitted, expecting its value

    void indent_pad() {
        out_.append(static_cast<std::size_t>(indent_) * 2, ' ');
    }

    // Called by container-openers (obj_open/arr_open) and value-emitters
    // when they appear as standalone array elements. If we just emitted a
    // key, the value goes inline; otherwise insert separator + indent.
    void prefix_value() {
        if (pending_key_) {
            pending_key_ = false;
            return;
        }
        sep_for_member();
    }

    // Emits comma + newline + indent before the next member of the
    // current container. No-op for the first member at root level
    // (avoids a leading '\n' before the root '{').
    void sep_for_member() {
        if (pending_key_) {
            // Direct value emitter after a key: no separator (key handled it).
            pending_key_ = false;
            return;
        }
        if (first_) {
            // Root: nothing emitted yet, so don't insert a leading newline.
            // Inside a container: emit newline + indent to start the body.
            if (!out_.empty()) {
                out_ += '\n';
                indent_pad();
            }
            first_ = false;
        } else {
            out_ += ",\n";
            indent_pad();
        }
    }

    static std::string format_double(double v, int decimals) {
        if (!std::isfinite(v)) return "0";
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
        return std::string(buf);
    }

    void append_escaped(const std::string& s) {
        for (char ch : s) {
            unsigned char c = static_cast<unsigned char>(ch);
            switch (c) {
                case '"':  out_ += "\\\""; break;
                case '\\': out_ += "\\\\"; break;
                case '\b': out_ += "\\b";  break;
                case '\f': out_ += "\\f";  break;
                case '\n': out_ += "\\n";  break;
                case '\r': out_ += "\\r";  break;
                case '\t': out_ += "\\t";  break;
                default:
                    if (c < 0x20) {
                        char hex[8];
                        std::snprintf(hex, sizeof(hex), "\\u%04x",
                                      static_cast<unsigned>(c));
                        out_ += hex;
                    } else {
                        out_ += static_cast<char>(c);
                    }
                    break;
            }
        }
    }
};

// =============================================================
// Helpers
// =============================================================
const char* series_type_name(int best_of) {
    switch (best_of) {
        case 1: return "BO1";
        case 2: return "BO2";
        case 3: return "BO3";
        case 5: return "BO5";
        default: return "BO?";
    }
}

std::string make_match_id(const std::string& event,
                          int year,
                          int day_in_year,
                          const std::string& t1_name,
                          const std::string& t2_name,
                          int best_of) {
    std::string concat;
    concat.reserve(event.size() + t1_name.size() + t2_name.size() + 32);
    concat += event;
    concat += '|';
    concat += std::to_string(year);
    concat += '|';
    concat += std::to_string(day_in_year);
    concat += '|';
    concat += t1_name;
    concat += '|';
    concat += t2_name;
    concat += '|';
    concat += std::to_string(best_of);

    std::size_t h = std::hash<std::string>{}(concat);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx",
                  static_cast<unsigned long long>(h));
    return std::string(buf);
}

// True if the sum of scores indicates the map went past regulation 24 rounds
// AND at least one side hit 13+ (so it wasn't a forfeit / weird state).
bool detect_overtime(int blue, int red) {
    int total = blue + red;
    return (total > 24) && (blue >= 13 || red >= 13);
}

double safe_div(double num, double denom) {
    return (denom > 0.0) ? (num / denom) : 0.0;
}

// Estimate Average Combat Score from VLR-style rating + ADR. Industry rule
// of thumb: ACS ~ rating * 200 + adr / 2. Honesty flag emitted alongside.
int estimate_acs(double rating, double adr) {
    double acs = rating * 200.0 + adr / 2.0;
    if (!std::isfinite(acs) || acs < 0.0) acs = 0.0;
    return static_cast<int>(std::round(acs));
}

// Per-player aggregate across the entire series (for series_totals + MVP).
struct PlayerSeriesAgg {
    Player* p = nullptr;
    int     team_idx = 0;       // 1 or 2
    int     k = 0, d = 0, a = 0;
    int     fb = 0, fd = 0;
    int     damage = 0;
    int     rounds = 0;
    double  rating_sum = 0.0;   // sum across maps played (per spec)
    int     maps_played = 0;
};

// Per-team series totals.
struct TeamSeriesTotals {
    int k = 0, d = 0, a = 0;
    int fb = 0, fd = 0;
    int damage = 0;
    int rounds = 0;
};

// Resolve a name for a (possibly-expired) Team weak_ptr, falling back to the
// recorded blue/red name if the weak_ptr is gone (PROJECT_GUIDE pitfall #23).
std::string team_display_name(const std::weak_ptr<Team>& wp,
                              const std::string& fallback) {
    if (auto t = wp.lock()) return t->name;
    return fallback.empty() ? std::string("?") : fallback;
}

// Emit a "team meta" object: name, tag, region, country.
void write_team_meta(JsonWriter& jw,
                     const std::weak_ptr<Team>& wp,
                     const std::string& fallback_name) {
    jw.obj_open();
    if (auto t = wp.lock()) {
        jw.kv_str("name",    t->name);
        jw.kv_str("tag",     t->tag);
        jw.kv_str("region",  t->region);
        jw.kv_str("country", t->home_country);
    } else {
        jw.kv_str("name",    fallback_name);
        jw.kv_str("tag",     "");
        jw.kv_str("region",  "");
        jw.kv_str("country", "");
    }
    jw.obj_close();
}

// Emit a single per-player object. Returns true if emitted, false if the
// player had no PlayerLine / PlayerMatchStats and we skipped them.
bool write_player_object(JsonWriter& jw,
                         Player* p,
                         const HistoryRecord& hr,
                         const std::unordered_map<Player*, PlayerMatchStats>& stats,
                         int total_rounds) {
    if (!p) return false;
    auto line_it = hr.stats.find(p);
    auto stat_it = stats.find(p);
    if (line_it == hr.stats.end() || stat_it == stats.end()) return false;

    const PlayerLine& line = line_it->second;
    const PlayerMatchStats& s = stat_it->second;

    int rounds = std::max(1, total_rounds);
    double adr = safe_div(static_cast<double>(s.damage), rounds);
    double kast_pct = safe_div(static_cast<double>(s.rounds_with_kast), rounds) * 100.0;
    int acs = estimate_acs(line.rating, adr);
    int fk_fd_diff = line.fb - line.fd;

    jw.obj_open();
    jw.kv_str ("name",       p->name);
    jw.kv_str ("agent",      line.agent);
    // Role of the AGENT they piloted this map, not the player's career
    // default. A Controller main flexed onto Killjoy must serialize as
    // "Sentinel". Falls back to primary_role only if the agent name
    // didn't resolve (shouldn't happen but defensive).
    {
        Role agent_role = p->primary_role;
        if (const auto* ag = find_agent_by_name(line.agent)) {
            agent_role = ag->role;
        }
        jw.kv_str("role", role_name(agent_role));
    }
    jw.kv_bool("is_igl",     p->is_igl);
    jw.kv_int ("k",          line.k);
    jw.kv_int ("d",          line.d);
    jw.kv_int ("a",          line.a);
    jw.kv_double("rating",   line.rating, 2);
    jw.kv_int ("acs",        acs);
    jw.kv_bool("acs_estimated", true);
    jw.kv_int ("adr",        static_cast<int>(std::round(adr)));
    jw.kv_int ("hs_pct",     line.hs);
    jw.kv_int ("fb",         line.fb);
    jw.kv_int ("fd",         line.fd);
    jw.kv_int ("fk_fd_diff", fk_fd_diff);
    jw.kv_int ("fb_pct",     line.fb_pct);
    jw.kv_int ("kast_pct",   static_cast<int>(std::round(kast_pct)));
    jw.kv_int ("clutches",   line.clutch);
    jw.kv_int ("damage",     s.damage);
    jw.kv_int ("max_round_dmg", line.max_dmg);

    jw.key("multi_kills");
    jw.obj_open();
    jw.kv_int("2k", s.mk2);
    jw.kv_int("3k", s.mk3);
    jw.kv_int("4k", s.mk4);
    jw.kv_int("5k", s.mk5);
    jw.obj_close();

    jw.kv_int("rounds_played", total_rounds);
    jw.obj_close();
    return true;
}

// Aggregate one player's per-map line into the running series-wide bucket.
void accumulate_player(PlayerSeriesAgg& agg,
                       const PlayerLine& line,
                       const PlayerMatchStats& s,
                       int total_rounds) {
    agg.k       += line.k;
    agg.d       += line.d;
    agg.a       += line.a;
    agg.fb      += line.fb;
    agg.fd      += line.fd;
    agg.damage  += s.damage;
    agg.rounds  += total_rounds;
    agg.rating_sum += line.rating;
    agg.maps_played += 1;
}

}  // namespace

// =============================================================
// Public entry point.
// =============================================================
std::string export_series_to_json(
    const std::vector<RecordedMatchPtr>& recordings,
    const std::string& event_name,
    int best_of,
    int year,
    int day_in_year)
{
    if (recordings.empty()) return "";

    // Resolve "team1" / "team2" identity from the FIRST map's recording.
    // We keep these as the canonical labels for the entire series export
    // (subsequent maps may swap blue/red sides, but team1/team2 identity
    // is derived from the recorded weak_ptrs, falling back to first map's
    // blue_name/red_name).
    const RecordedMatchPtr& first_rec = recordings.front();
    std::weak_ptr<Team> team1_wp = first_rec ? first_rec->team1 : std::weak_ptr<Team>{};
    std::weak_ptr<Team> team2_wp = first_rec ? first_rec->team2 : std::weak_ptr<Team>{};
    std::string team1_name = team_display_name(team1_wp,
                                               first_rec ? first_rec->blue_name : "?");
    std::string team2_name = team_display_name(team2_wp,
                                               first_rec ? first_rec->red_name  : "?");

    // ---- Series totals + per-player aggregation ----
    int team1_maps_won = 0;
    int team2_maps_won = 0;
    TeamSeriesTotals team1_totals;
    TeamSeriesTotals team2_totals;
    std::unordered_map<Player*, PlayerSeriesAgg> player_agg;

    // First pass: aggregate. Done up front so we can emit series_totals
    // after the maps array without a second scan.
    for (const auto& rec : recordings) {
        if (!rec) continue;

        // Determine whether this map's "blue" side corresponds to team1 of
        // the series. Map sides can swap between maps; we identify by the
        // recorded weak_ptr identity (preferred) or by name fallback.
        bool blue_is_team1 = false;
        if (auto t1 = team1_wp.lock(); t1) {
            blue_is_team1 = (rec->team1.lock().get() == t1.get());
        } else {
            blue_is_team1 = (rec->blue_name == team1_name);
        }

        int t1_score = blue_is_team1 ? rec->blue_score : rec->red_score;
        int t2_score = blue_is_team1 ? rec->red_score  : rec->blue_score;
        if (t1_score > t2_score) ++team1_maps_won;
        else if (t2_score > t1_score) ++team2_maps_won;

        int total_rounds = std::max(1, rec->blue_score + rec->red_score);

        if (!rec->history_record || !rec->match_stats) continue;
        const HistoryRecord& hr = *rec->history_record;
        const auto& stats = *rec->match_stats;

        auto process_side = [&](const std::vector<Player*>& side, bool is_team1) {
            TeamSeriesTotals& tot = is_team1 ? team1_totals : team2_totals;
            for (Player* p : side) {
                if (!p) continue;
                auto line_it = hr.stats.find(p);
                auto stat_it = stats.find(p);
                if (line_it == hr.stats.end() || stat_it == stats.end()) continue;
                const PlayerLine& line = line_it->second;
                const PlayerMatchStats& s = stat_it->second;

                tot.k      += line.k;
                tot.d      += line.d;
                tot.a      += line.a;
                tot.fb     += line.fb;
                tot.fd     += line.fd;
                tot.damage += s.damage;
                tot.rounds += total_rounds;

                auto& agg = player_agg[p];
                if (!agg.p) {
                    agg.p = p;
                    agg.team_idx = is_team1 ? 1 : 2;
                }
                accumulate_player(agg, line, s, total_rounds);
            }
        };

        // hr.blue_team / hr.red_team correspond to this map's blue/red,
        // which we map back onto team1/team2 of the series.
        process_side(hr.blue_team, blue_is_team1);
        process_side(hr.red_team,  !blue_is_team1);
    }

    // ---- Series MVP: highest summed rating across maps where they played ----
    Player* mvp_player = nullptr;
    const PlayerSeriesAgg* mvp_agg = nullptr;
    {
        double best = -1.0;
        for (const auto& kv : player_agg) {
            if (kv.second.rating_sum > best) {
                best = kv.second.rating_sum;
                mvp_player = kv.first;
                mvp_agg = &kv.second;
            }
        }
    }

    // ---- Determine series winner ----
    std::string winner;
    if (team1_maps_won > team2_maps_won) winner = "team1";
    else if (team2_maps_won > team1_maps_won) winner = "team2";
    else winner = "tied";  // valid for BO2

    // ---- Infer the true best_of from the result ----
    // The caller can only pass the PLAYED map count (the configured Series
    // best_of is not retained in the live viewer / replay path), so a BO5 won
    // 3-0 arrives here as best_of==3 and would mislabel as "BO3". For a
    // completed best-of-odd series the winner's map count IS the clinch
    // number, so best_of = clinch*2 - 1 is exact: 3-0/3-1/3-2 -> 5, 2-0/2-1
    // -> 3, 1-0 -> 1. Only apply when a side actually clinched; otherwise
    // (BO2 / tie / partial data) fall back to the value the caller supplied.
    int effective_best_of = best_of;
    {
        int winner_maps = std::max(team1_maps_won, team2_maps_won);
        int maps_played = team1_maps_won + team2_maps_won;
        int inferred    = winner_maps * 2 - 1;
        // Never up-convert a 2-map series: a real BO2 (group stage) that ends
        // 2-0 would otherwise infer to BO3 (winner_maps*2-1 = 3 >= 2). Keep
        // the caller-supplied best_of for 2-map series.
        if (winner_maps > 0 && inferred >= maps_played && maps_played != 2) {
            effective_best_of = inferred;
        }
    }

    // ---- Build the JSON ----
    JsonWriter jw;
    jw.obj_open();
    jw.kv_int ("schema_version", 2);
    jw.kv_str ("exporter",       "VLR Manager Valosim");
    jw.kv_str ("exporter_notes",
               "rating is VLR-style; ACS estimated from rating+ADR; "
               "utility_impact not tracked. v2 adds per-map round_history "
               "(round-by-round winner/score/economy + spike/defuse end_kind, "
               "a deterministic narrative layer).");
    jw.kv_str ("match_id", make_match_id(event_name, year, day_in_year,
                                         team1_name, team2_name, effective_best_of));
    jw.kv_str ("event",       event_name);
    jw.kv_str ("series_type", series_type_name(effective_best_of));
    jw.kv_int ("best_of",     effective_best_of);

    jw.key("exported");
    jw.obj_open();
    jw.kv_int("year", year);
    jw.kv_int("day_in_year", day_in_year);
    jw.obj_close();

    jw.key("teams");
    jw.obj_open();
    jw.key("team1"); write_team_meta(jw, team1_wp, team1_name);
    jw.key("team2"); write_team_meta(jw, team2_wp, team2_name);
    jw.obj_close();

    jw.key("final_score");
    jw.obj_open();
    jw.kv_int("team1", team1_maps_won);
    jw.kv_int("team2", team2_maps_won);
    jw.obj_close();

    jw.kv_str("winner", winner);

    // ---- Per-map array ----
    jw.key("maps");
    jw.arr_open();
    int map_number = 0;
    for (const auto& rec : recordings) {
        ++map_number;
        if (!rec) {
            jw.obj_open();
            jw.kv_int("map_number", map_number);
            jw.kv_str("map_name", "?");
            jw.obj_close();
            continue;
        }

        bool blue_is_team1 = false;
        if (auto t1 = team1_wp.lock(); t1) {
            blue_is_team1 = (rec->team1.lock().get() == t1.get());
        } else {
            blue_is_team1 = (rec->blue_name == team1_name);
        }
        int t1_score = blue_is_team1 ? rec->blue_score : rec->red_score;
        int t2_score = blue_is_team1 ? rec->red_score  : rec->blue_score;
        int total_rounds = rec->blue_score + rec->red_score;
        bool overtime = detect_overtime(rec->blue_score, rec->red_score);

        std::string map_winner;
        if (t1_score > t2_score) map_winner = "team1";
        else if (t2_score > t1_score) map_winner = "team2";
        else map_winner = "tied";

        jw.obj_open();
        jw.kv_int("map_number", map_number);
        jw.kv_str("map_name",   rec->map_name);

        jw.key("score");
        jw.obj_open();
        jw.kv_int("team1", t1_score);
        jw.kv_int("team2", t2_score);
        jw.obj_close();

        jw.kv_str ("winner",        map_winner);
        jw.kv_int ("rounds_played", total_rounds);
        jw.kv_bool("overtime",      overtime);
        jw.kv_str ("mvp",           rec->mvp_name);

        // Per-player stats grouped by team1/team2.
        jw.key("players");
        jw.obj_open();
        if (rec->history_record && rec->match_stats) {
            const HistoryRecord& hr = *rec->history_record;
            const auto& stats = *rec->match_stats;
            int eff_rounds = std::max(1, total_rounds);

            const std::vector<Player*>& team1_side =
                blue_is_team1 ? hr.blue_team : hr.red_team;
            const std::vector<Player*>& team2_side =
                blue_is_team1 ? hr.red_team  : hr.blue_team;

            jw.key("team1");
            jw.arr_open();
            for (Player* p : team1_side) {
                write_player_object(jw, p, hr, stats, eff_rounds);
            }
            jw.arr_close();

            jw.key("team2");
            jw.arr_open();
            for (Player* p : team2_side) {
                write_player_object(jw, p, hr, stats, eff_rounds);
            }
            jw.arr_close();
        } else {
            jw.key("team1"); jw.arr_open(); jw.arr_close();
            jw.key("team2"); jw.arr_open(); jw.arr_close();
        }
        jw.obj_close();   // players

        // ---- WS-C R3: per-round history (schema v2) ----
        // Read-only serialization of the deterministic R1 round metadata; runs
        // post-match and never feeds the sim. Normalized to the EXPORT's
        // team1/team2 (blue_is_team1), independent of the match's internal sides.
        if (rec->round_history && !rec->round_history->empty()) {
            jw.key("round_history");
            jw.arr_open();
            for (const RoundLog& rl : *rec->round_history) {
                bool exp_t1_atk = blue_is_team1 ? rl.t1_attacking : !rl.t1_attacking;
                int  r_t1  = blue_is_team1 ? rl.t1_score  : rl.t2_score;
                int  r_t2  = blue_is_team1 ? rl.t2_score  : rl.t1_score;
                int  inv1  = blue_is_team1 ? rl.t1_invest : rl.t2_invest;
                int  inv2  = blue_is_team1 ? rl.t2_invest : rl.t1_invest;
                jw.obj_open();
                jw.kv_int ("round",          rl.round);
                jw.kv_str ("winner",         rl.winner_name == team1_name ? "team1" : "team2");
                jw.kv_str ("end_kind",       round_end_kind_str(rl.end_kind));
                jw.kv_bool("spike_planted",  rl.spike_planted);
                jw.kv_bool("was_retake",     rl.was_retake);
                jw.kv_str ("attacking_side", exp_t1_atk ? "team1" : "team2");
                jw.key("score");
                jw.obj_open();
                jw.kv_int("team1", r_t1);
                jw.kv_int("team2", r_t2);
                jw.obj_close();
                jw.key("economy");
                jw.obj_open();
                jw.kv_int("team1_invest", inv1);
                jw.kv_int("team2_invest", inv2);
                jw.obj_close();
                jw.obj_close();
            }
            jw.arr_close();
        }

        jw.obj_close();   // map entry
    }
    jw.arr_close();       // maps

    // ---- Series totals ----
    jw.key("series_totals");
    jw.obj_open();

    auto write_team_totals = [&](const std::string& key,
                                 const TeamSeriesTotals& t) {
        jw.key(key);
        jw.obj_open();
        jw.kv_int("k",      t.k);
        jw.kv_int("d",      t.d);
        jw.kv_int("a",      t.a);
        jw.kv_int("fb",     t.fb);
        jw.kv_int("fd",     t.fd);
        jw.kv_int("damage", t.damage);
        jw.kv_int("rounds", t.rounds);
        jw.obj_close();
    };
    write_team_totals("team1", team1_totals);
    write_team_totals("team2", team2_totals);

    jw.key("mvp");
    if (mvp_player && mvp_agg) {
        jw.obj_open();
        jw.kv_str("name",   mvp_player->name);
        jw.kv_str("team",   mvp_agg->team_idx == 1 ? "team1" : "team2");
        jw.kv_int("k",      mvp_agg->k);
        jw.kv_int("d",      mvp_agg->d);
        jw.kv_int("a",      mvp_agg->a);
        // Aggregate rating: average across maps played (more intuitive in
        // the MVP block than the raw sum we used for ranking).
        double avg_rating = (mvp_agg->maps_played > 0)
            ? mvp_agg->rating_sum / static_cast<double>(mvp_agg->maps_played)
            : 0.0;
        jw.kv_double("rating", avg_rating, 2);
        jw.kv_int("maps_played", mvp_agg->maps_played);
        jw.obj_close();
    } else {
        jw.obj_open();
        jw.obj_close();
    }
    jw.obj_close();   // series_totals

    jw.obj_close();   // root
    return jw.str();
}

}  // namespace vlr
