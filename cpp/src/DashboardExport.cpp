// DashboardExport.cpp - JSON serializer for the user-team-centric "home
// dashboard" view consumed by the web UI POC (ui-poc/dashboard.html).
//
// Hand-rolled JSON writer (output-only, C++17 stdlib only - no JSON library).
// OUTPUT-ONLY: reads engine state, mutates nothing, consumes no RNG. The
// JsonWriter below is the same convention as MatchExport.cpp (each lives in its
// own anonymous namespace, internal linkage, so they do not collide at link).
//
// Every field maps to a real GameManager / Team / Player signal. Two honest
// limitations are documented inline: (1) last_5_form carries real W/L from
// Team::recent_results but no opponent/score (the engine keeps no team-level
// match log), and (2) budget_trend is a single current point (no persisted
// budget history). Both are flagged as the obvious productionize follow-ups.

#include "DashboardExport.h"

#include "Agent.h"        // role_name, Agent
#include "Analyst.h"      // Analyst, analyst_personality_name
#include "Goat.h"         // GoatWeights, compute_goat_career/season (records hub)
#include "Coach.h"        // Coach, coach_personality_name
#include "Common.h"       // attr_name, kAttrCount, Attr
#include "Country.h"      // countries(), region_name (scout knowledge map)
#include "GameManager.h"  // GameManager, BoardObjective, ObjectiveStatus, NewsItem, UpcomingFixture
#include "League.h"       // League::teams()
#include "Match.h"        // HistoryRecord, RoundLog, PlayerLine, RoundEndKind (match view)
#include "Player.h"       // Player, desire_name, TrajClass
#include "Scout.h"        // Scout, scout_personality_name
#include "SoloQ.h"        // SoloQEngine::global_ladder (free-agent pool)
#include "Tournament.h"   // Tournament, BracketMatch (brackets view)
#include "Team.h"         // Team, TeamWindow, team_window_name, brand_tag_name, wealth_tier_name

#include <algorithm>
#include <cctype>
#include <climits>
#include <functional>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <tuple>
#include <unordered_set>
#include <vector>

namespace vlr {

namespace {

// =============================================================
// Tiny output-only JSON writer (mirrors MatchExport.cpp).
// =============================================================
class JsonWriter {
public:
    JsonWriter() = default;
    std::string str() const { return out_; }

    void obj_open() { prefix_value(); out_ += '{'; ++indent_; first_ = true; }
    void obj_close() {
        --indent_;
        if (!first_) { out_ += '\n'; indent_pad(); }
        out_ += '}';
        first_ = false;
    }
    void arr_open() { prefix_value(); out_ += '['; ++indent_; first_ = true; }
    void arr_close() {
        --indent_;
        if (!first_) { out_ += '\n'; indent_pad(); }
        out_ += ']';
        first_ = false;
    }

    void key(const std::string& k) {
        sep_for_member();
        out_ += '"'; append_escaped(k); out_ += "\": ";
        pending_key_ = true;
    }

    void kv_str(const std::string& k, const std::string& v) {
        sep_for_member();
        out_ += '"'; append_escaped(k); out_ += "\": \"";
        append_escaped(v); out_ += '"';
    }
    void kv_int(const std::string& k, long long v) {
        sep_for_member();
        out_ += '"'; append_escaped(k); out_ += "\": ";
        out_ += std::to_string(v);
    }
    void kv_bool(const std::string& k, bool v) {
        sep_for_member();
        out_ += '"'; append_escaped(k); out_ += "\": ";
        out_ += (v ? "true" : "false");
    }
    void kv_double(const std::string& k, double v, int decimals = 2) {
        sep_for_member();
        out_ += '"'; append_escaped(k); out_ += "\": ";
        out_ += format_double(v, decimals);
    }
    void kv_raw(const std::string& k, const std::string& raw) {
        sep_for_member();
        out_ += '"'; append_escaped(k); out_ += "\": ";
        out_ += raw;
    }

    void arr_int(long long v) { sep_for_member(); out_ += std::to_string(v); }
    void arr_str(const std::string& v) {
        sep_for_member();
        out_ += '"'; append_escaped(v); out_ += '"';
    }

private:
    std::string out_;
    int  indent_ = 0;
    bool first_ = true;
    bool pending_key_ = false;

    void indent_pad() { out_.append(static_cast<std::size_t>(indent_) * 2, ' '); }

    void prefix_value() {
        if (pending_key_) { pending_key_ = false; return; }
        sep_for_member();
    }
    void sep_for_member() {
        if (pending_key_) { pending_key_ = false; return; }
        if (first_) {
            if (!out_.empty()) { out_ += '\n'; indent_pad(); }
            first_ = false;
        } else {
            out_ += ",\n"; indent_pad();
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
                        std::snprintf(hex, sizeof(hex), "\\u%04x", static_cast<unsigned>(c));
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

// Replica of the GUI's fmt_money (gui_main.cpp) - that helper lives in the GUI
// translation unit and is NOT linked into the core, so the exporter formats its
// own budget string with the SAME $ / $K / $M convention for visual parity.
// Magnitude is computed via unsigned to avoid signed-overflow UB on LLONG_MIN.
std::string format_money(long long dollars) {
    char buf[48];
    const char* sign = dollars < 0 ? "-" : "";
    unsigned long long a = dollars < 0
        ? (0ULL - static_cast<unsigned long long>(dollars))
        : static_cast<unsigned long long>(dollars);
    if (a >= 1000000)   std::snprintf(buf, sizeof(buf), "%s$%.2fM", sign, a / 1000000.0);
    else if (a >= 1000) std::snprintf(buf, sizeof(buf), "%s$%lluK", sign, a / 1000);
    else                std::snprintf(buf, sizeof(buf), "%s$%llu",  sign, a);
    return std::string(buf);
}

std::string hex_color(int r, int g, int b) {
    auto cl = [](int x) { return x < 0 ? 0 : (x > 255 ? 255 : x); };
    r = cl(r); g = cl(g); b = cl(b);
    // A near-black primary would make every --club accent (header wash, crest,
    // user row, hero chip) invisible against the dark bg; fall back to brand red.
    if (r + g + b < 36) { r = 0xFF; g = 0x46; b = 0x55; }
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", r, g, b);
    return std::string(buf);
}

std::string to_upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// "STAGE 1" -> "Stage 1", "REGIONALS 1" -> "Regionals 1", etc. so the header
// plate matches the title-case day-rail labels instead of shouting all-caps.
std::string title_case(std::string s) {
    bool word_start = true;
    for (char& c : s) {
        unsigned char u = static_cast<unsigned char>(c);
        if (std::isspace(u)) { word_start = true; continue; }
        c = word_start ? static_cast<char>(std::toupper(u))
                       : static_cast<char>(std::tolower(u));
        word_start = false;
    }
    return s;
}

// Format a value already in thousands-of-dollars (the *_k fields) through the
// same $ / $K / $M convention as format_money.
std::string format_money_k(long long thousands) {
    return format_money(thousands * 1000LL);
}

// Player::TrajClass -> a readable development label for the roster card.
const char* trajectory_label(Player::TrajClass tc) {
    switch (tc) {
        case Player::TrajClass::Established: return "Established";
        case Player::TrajClass::FutureStar:  return "Future Star";
        case Player::TrajClass::Rising:      return "Rising";
        case Player::TrajClass::Developing:  return "Developing";
        case Player::TrajClass::Slump:       return "Slump";
        case Player::TrajClass::Declining:   return "Declining";
        case Player::TrajClass::Twilight:    return "Twilight";
    }
    return "Established";
}

// Standings sort: phase wins desc, then phase losses asc, then roster OVR desc,
// then name asc for a stable, deterministic order (matches the Standings view's
// default ordering). Null-tolerant (nulls sort last) so it can never UB even if
// a future feature ever lets a null TeamPtr into a league roster.
bool team_standings_less(const TeamPtr& a, const TeamPtr& b) {
    if (!a || !b) return a && !b;           // non-null before null; nulls equal
    if (a->phase_wins != b->phase_wins)     return a->phase_wins  > b->phase_wins;
    if (a->phase_losses != b->phase_losses) return a->phase_losses < b->phase_losses;
    double oa = a->ovr(), ob = b->ovr();
    if (oa != ob)                            return oa > ob;
    return a->name < b->name;
}

// The user/opponent team's league, sorted into standings order. Empty if the
// team has no resolvable league (pre-season / unranked).
std::vector<TeamPtr> sorted_league(const GameManager& gm, const TeamPtr& t) {
    std::vector<TeamPtr> v;
    if (!t) return v;
    auto lg = gm.league_for(t);
    if (!lg) return v;
    v = lg->teams();
    v.erase(std::remove(v.begin(), v.end(), nullptr), v.end());  // defensive
    std::sort(v.begin(), v.end(), team_standings_less);
    return v;
}

// 1-based position of t within its own league standings; 0 if unresolved.
int rank_in_league(const GameManager& gm, const TeamPtr& t) {
    auto v = sorted_league(gm, t);
    for (std::size_t i = 0; i < v.size(); ++i)
        if (v[i].get() == t.get()) return static_cast<int>(i) + 1;
    return 0;
}

// Tier index -> human division label for the header plate.
std::string division_label(const GameManager& gm, const TeamPtr& t) {
    // Prefer the league's own division name when it carries one; fall back to a
    // tier-derived label so relegated teams still read correctly.
    if (t) {
        if (auto lg = gm.league_for(t)) {
            const std::string& dn = lg->division_name();
            if (!dn.empty()) return dn;
        }
    }
    switch (gm.tier_of(t)) {
        case 1:  return "VCT";
        case 2:  return "Challengers";
        case 3:  return "Open Circuit";
        default: return "VCT";
    }
}

// Reputation (hidden 1..10000) -> a coarse FM-style STATURE band for display. The raw
// number stays hidden; the label is what the UI shows (transfer/wage effects reveal the rest).
const char* rep_tier_label(int rep) {
    if (rep >= 8500) return "Global Icon";
    if (rep >= 7000) return "Superstar";
    if (rep >= 5500) return "Star";
    if (rep >= 4000) return "Established";
    if (rep >= 2500) return "Rising";
    if (rep >= 1200) return "Prospect";
    return "Unknown";
}

}  // namespace

// =============================================================
// Public entry point.
// =============================================================
std::string export_dashboard_to_json(const GameManager& gm,
                                     int star_limit,
                                     int news_limit) {
    const TeamPtr& user = gm.user_team;
    if (!user) return "{}";

    if (star_limit < 0) star_limit = 0;
    if (news_limit < 0) news_limit = 0;

    // --- Pre-compute cross-cutting values (standings, ranks, next match). ----
    std::vector<TeamPtr> standings = sorted_league(gm, user);
    int regional_rank = 0;
    for (std::size_t i = 0; i < standings.size(); ++i)
        if (standings[i].get() == user.get()) { regional_rank = static_cast<int>(i) + 1; break; }

    // Next fixture involving the user team (smallest day >= today).
    bool    has_next = false;
    int     next_day = INT_MAX;
    TeamPtr next_opp;
    for (const auto& f : gm.upcoming_fixtures(14)) {
        if (f.a == user || f.b == user) {
            if (f.day_in_year < next_day) {
                next_day = f.day_in_year;
                next_opp = (f.a == user) ? f.b : f.a;
                has_next = true;
            }
        }
    }
    int next_days_until = has_next ? (next_day - gm.day_in_year) : 0;
    if (next_days_until < 0) next_days_until = 0;

    JsonWriter jw;
    jw.obj_open();
    jw.kv_int("schema_version", 1);
    jw.kv_str("exporter", "VLR Manager Valosim");
    jw.kv_str("club_color", hex_color(user->color_primary_r,
                                      user->color_primary_g,
                                      user->color_primary_b));

    jw.key("dashboard");
    jw.obj_open();

    // ---- header --------------------------------------------------------------
    jw.key("header");
    jw.obj_open();
    jw.kv_str("club_name",     user->name);
    jw.kv_str("division",      division_label(gm, user));
    jw.kv_str("region",        user->region);
    jw.kv_int("year",          gm.year);
    jw.kv_int("day_in_year",   gm.day_in_year + 1);   // 1-based for display
    jw.kv_str("current_phase", title_case(gm.current_phase()));
    jw.kv_bool("sponsor_pending", gm.sponsor_choice_pending_);   // dashboard nudge → Finance
    jw.kv_bool("in_preseason",   gm.in_preseason_buffer_);       // no-match onboarding window
    jw.kv_int("preseason_days",  gm.preseason_days_left_);       // days left until Stage 1
    jw.kv_int("team_talk",       gm.user_team_talk);             // pre-match talk (0=none 1=calm 2=fire 3=focus)
    jw.kv_int("unread_mail",     gm.unread_mail_count());        // inbox badge (mailbox items with !read)
    jw.obj_close();

    // ---- objectives ----------------------------------------------------------
    jw.key("objectives");
    jw.obj_open();
    const auto& objectives = gm.board_objectives();
    jw.kv_int("total", static_cast<long long>(objectives.size()));
    jw.kv_int("met",   gm.board_objectives_met());
    jw.key("goals");
    jw.arr_open();
    for (const BoardObjective& o : objectives) {
        ObjectiveStatus st = gm.evaluate_objective(o);
        jw.obj_open();
        jw.kv_str("text",      o.text);
        jw.kv_bool("mandatory", o.mandatory);
        jw.kv_bool("met",       st.met);
        jw.kv_str("progress",   st.progress);
        jw.kv_double("pct",     std::max(0.0, std::min(1.0, st.pct)), 2);
        jw.obj_close();
    }
    jw.arr_close();
    jw.obj_close();

    // ---- kpis ----------------------------------------------------------------
    auto emit_kpi = [&](const std::string& label, const std::string& value,
                        const char* tone, const std::string& subline, bool today) {
        jw.obj_open();
        jw.kv_str("label", label);
        jw.kv_str("value", value);
        jw.kv_str("tone",  tone);
        if (!subline.empty()) jw.kv_str("subline", subline);
        if (today)            jw.kv_bool("today", true);
        jw.obj_close();
    };

    jw.key("kpis");
    jw.arr_open();
    {
        // BUDGET (tone by cash magnitude).
        long long ab = user->budget < 0 ? -user->budget : user->budget;
        const char* btone = ab >= 2000000 ? "pos" : (ab >= 1000000 ? "info"
                          : (ab >= 400000 ? "warn" : "crit"));
        emit_kpi("BUDGET", format_money(user->budget), btone, "", false);

        // PHASE RECORD.
        std::string prec = std::to_string(user->phase_wins) + "-" +
                           std::to_string(user->phase_losses);
        const char* ptone = user->phase_wins > user->phase_losses ? "pos"
                          : (user->phase_losses > user->phase_wins ? "warn" : "info");
        emit_kpi("PHASE RECORD", prec, ptone, "", false);

        // REGIONAL RANK.
        std::string rval = regional_rank > 0 ? ("#" + std::to_string(regional_rank)) : "—";
        const char* rtone = regional_rank <= 0 ? "neutral"
                          : (regional_rank <= 2 ? "pos"
                          : (regional_rank <= 4 ? "info"
                          : (regional_rank <= 8 ? "warn" : "crit")));
        emit_kpi("REGIONAL RANK", rval, rtone, "", false);

        // ROSTER OVR.
        int rovr = static_cast<int>(std::lround(user->ovr()));
        const char* otone = rovr >= 80 ? "pos" : (rovr >= 74 ? "info"
                          : (rovr >= 68 ? "warn" : "crit"));
        emit_kpi("ROSTER OVR", std::to_string(rovr), otone, "", false);

        // NEXT MATCH — during the preseason buffer no match plays, so show the
        // countdown to Stage 1 rather than a misleading "TODAY".
        if (gm.in_preseason_buffer_) {
            emit_kpi("NEXT MATCH", std::to_string(gm.preseason_days_left_) + "d",
                     "neutral", "Preseason", false);
        } else if (has_next && next_opp) {
            std::string nmval = next_days_until <= 0 ? "TODAY"
                              : (std::to_string(next_days_until) + "d");
            emit_kpi("NEXT MATCH", nmval, "neutral", "vs " + next_opp->name,
                     next_days_until <= 0);
        } else {
            emit_kpi("NEXT MATCH", "—", "neutral", "No fixture", false);
        }

        // STAR DEPENDENCY (balanced core = healthy; very high = fragile).
        int sd = user->memory.star_dependency;
        std::string sdval = (sd >= 0 ? "+" : "") + std::to_string(sd);
        const char* sdtone = sd >= 30 ? "warn" : (sd <= 0 ? "pos" : "info");
        emit_kpi("STAR DEPENDENCY", sdval, sdtone, "", false);

        // WINDOW.
        std::string wval = to_upper(team_window_name(user->window));
        const char* wsub = "";
        const char* wtone = "info";
        switch (user->window) {
            case TeamWindow::Opening: wsub = "Building";   wtone = "info"; break;
            case TeamWindow::Open:    wsub = "Contending"; wtone = "pos";  break;
            case TeamWindow::Closing: wsub = "Urgency";    wtone = "warn"; break;
            case TeamWindow::Closed:  wsub = "Rebuild";    wtone = "crit"; break;
        }
        emit_kpi("WINDOW", wval, wtone, wsub, false);
    }
    jw.arr_close();

    // ---- standings -----------------------------------------------------------
    jw.key("standings");
    jw.arr_open();
    {
        int rank = 0;
        for (const TeamPtr& t : standings) {
            ++rank;
            jw.obj_open();
            jw.kv_int("rank",      rank);
            jw.kv_str("team_name", t->name);
            jw.kv_int("wins",      t->phase_wins);
            jw.kv_int("losses",    t->phase_losses);
            jw.kv_double("ovr",    t->ovr(), 1);
            jw.kv_bool("is_user",  t.get() == user.get());
            jw.obj_close();
        }
    }
    jw.arr_close();

    // ---- news ----------------------------------------------------------------
    jw.key("news");
    jw.arr_open();
    {
        int n = static_cast<int>(gm.news_feed.size());
        int cap = std::min(news_limit, n);
        for (int i = 0; i < cap; ++i) {
            const auto& item = gm.news_feed[static_cast<std::size_t>(i)];
            jw.obj_open();
            jw.kv_int("year",     item.year);
            jw.kv_int("day",      item.day_in_year + 1);   // 1-based, matches mail
            jw.kv_str("category", item.category);
            jw.kv_str("headline", item.headline);
            jw.obj_close();
        }
    }
    jw.arr_close();

    // ---- star_players (full roster, sorted by OVR desc, capped) --------------
    jw.key("star_players");
    jw.arr_open();
    {
        std::vector<Player*> roster;
        roster.reserve(user->roster.size());
        for (const auto& p : user->roster) if (p) roster.push_back(p.get());
        std::sort(roster.begin(), roster.end(),
                  [](const Player* a, const Player* b) { return a->ovr() > b->ovr(); });
        int cap = std::min(star_limit, static_cast<int>(roster.size()));
        for (int i = 0; i < cap; ++i) {
            const Player* p = roster[static_cast<std::size_t>(i)];
            jw.obj_open();
            jw.kv_int("id",           static_cast<long long>(p->id));   // player-modal handle
            jw.kv_str("name",         p->name);
            jw.kv_str("primary_role", role_name(p->primary_role));
            jw.kv_int("ovr",          static_cast<long long>(std::lround(p->ovr())));
            jw.kv_double("avg_rating", p->avg_match_rating(), 2);
            jw.kv_double("kd_ratio",   p->kd_ratio(), 2);
            jw.kv_bool("igl",          p->is_igl);
            jw.obj_close();
        }
    }
    jw.arr_close();

    // ---- next_match ----------------------------------------------------------
    if (has_next && next_opp) {
        jw.key("next_match");
        jw.obj_open();
        jw.kv_str("opponent",    next_opp->name);
        jw.kv_double("opp_ovr",  next_opp->ovr(), 1);
        jw.kv_int("opp_rank",    rank_in_league(gm, next_opp));
        jw.kv_int("days_until",  next_days_until);
        jw.obj_close();
    } else {
        jw.kv_raw("next_match", "null");
    }

    // ---- last_5_form ---------------------------------------------------------
    // Real W/L from Team::recent_results (the engine's canonical recent-form
    // signal). Opponent/score are intentionally empty: the engine keeps no
    // team-level match log, so they would be fabricated. A team match-log is the
    // productionize follow-up that fills these in.
    jw.key("last_5_form");
    jw.arr_open();
    {
        const auto& rr = user->recent_results;
        int n = static_cast<int>(rr.size());
        for (int i = std::max(0, n - 5); i < n; ++i) {
            jw.obj_open();
            jw.kv_str("opponent", "");
            jw.kv_str("result",   rr[static_cast<std::size_t>(i)] ? "W" : "L");
            jw.kv_str("score",    "");
            jw.obj_close();
        }
    }
    jw.arr_close();

    // ---- budget_trend --------------------------------------------------------
    // Single honest current point ($K). No budget history is persisted, so a
    // multi-point sparkline would be synthetic; a budget-history ring buffer on
    // Team is the productionize follow-up. The dashboard's single-point guard
    // renders this as a centred marker.
    jw.key("budget_trend");
    jw.arr_open();
    jw.arr_int(static_cast<long long>(std::llround(user->budget / 1000.0)));
    jw.arr_close();

    // ---- phases (day-rail) -----------------------------------------------------
    // Season phase ring with real start days for the dashboard day-rail. Starts
    // are 1-based (same display convention as day_in_year above). A phase spans
    // exactly days_per_round * total_rounds days, so the pacing table below
    // mirrors GameManager::current_phase_pacing() (the source of truth), keyed
    // on phase name and accumulated across the ring. One honest caveat: the
    // engine resets day_in_year at the OFFSEASON boundary, so during OFFSEASON
    // the live day_in_year restarts from 1 rather than continuing past AWARDS.
    jw.key("phases");
    jw.arr_open();
    {
        auto pacing_for = [](const std::string& p) -> std::pair<int,int> {
            if (p.find("STAGE")     != std::string::npos) return {3, 11};
            if (p.find("REGIONALS") != std::string::npos) return {1, 9};
            if (p.find("MASTERS")   != std::string::npos) return {1, 14};
            if (p.find("CHAMPIONS") != std::string::npos) return {1, 16};
            if (p == "AWARDS")                            return {1, 1};
            if (p == "OFFSEASON")                         return {1, 14};
            return {1, 1};
        };
        int start = 0;   // day-in-year at phase start (0-based accumulator)
        for (const std::string& p : GameManager::phases()) {
            jw.obj_open();
            jw.kv_str("name",  title_case(p));
            jw.kv_int("start", start + 1);   // 1-based for display
            jw.obj_close();
            std::pair<int,int> pacing = pacing_for(p);
            start += pacing.first * pacing.second;
        }
    }
    jw.arr_close();
    jw.kv_int("season_days", GameManager::kDaysPerYear);

    jw.obj_close();   // dashboard
    jw.obj_close();   // root
    return jw.str();
}

// =============================================================
// Roster / Squad view (the second web screen).
// =============================================================
std::string export_roster_to_json(const GameManager& gm) {
    const TeamPtr& user = gm.user_team;
    if (!user) return "{}";
    Team& t = *user;

    // --- roster aggregates for the KPI ribbon ---
    int    counted = 0;
    double age_sum = 0.0;
    for (const auto& p : t.roster) if (p) { ++counted; age_sum += p->age; }
    double avg_age = counted ? age_sum / counted : 0.0;

    // Team chemistry index: mean pairwise chemistry mapped onto 0..99.
    double chem_sum = 0.0; int chem_pairs = 0;
    for (std::size_t i = 0; i < t.roster.size(); ++i)
        for (std::size_t j = i + 1; j < t.roster.size(); ++j)
            if (t.roster[i] && t.roster[j]) {
                chem_sum += t.chemistry_between(*t.roster[i], *t.roster[j]);
                ++chem_pairs;
            }
    double chem_mean  = chem_pairs ? chem_sum / chem_pairs : 0.0;   // [-2, +2]
    int    chem_index = static_cast<int>(std::lround(
        std::max(0.0, std::min(99.0, 50.0 + 24.0 * chem_mean))));

    JsonWriter jw;
    jw.obj_open();
    jw.kv_int("schema_version", 1);
    jw.kv_str("exporter", "VLR Manager Valosim");
    jw.kv_str("club_color", hex_color(t.color_primary_r,
                                      t.color_primary_g,
                                      t.color_primary_b));

    jw.key("roster");
    jw.obj_open();

    // ---- header --------------------------------------------------------------
    jw.key("header");
    jw.obj_open();
    jw.kv_str("club_name",     t.name);
    jw.kv_str("tag",           t.tag);
    jw.kv_str("division",      division_label(gm, user));
    jw.kv_str("region",        t.region);
    jw.kv_int("year",          gm.year);
    jw.kv_int("day_in_year",   gm.day_in_year + 1);
    jw.kv_str("current_phase", title_case(gm.current_phase()));
    jw.obj_close();

    // ---- kpis (6) ------------------------------------------------------------
    auto emit_kpi = [&](const std::string& label, const std::string& value,
                        const char* tone, const std::string& subline) {
        jw.obj_open();
        jw.kv_str("label", label);
        jw.kv_str("value", value);
        jw.kv_str("tone",  tone);
        if (!subline.empty()) jw.kv_str("subline", subline);
        jw.obj_close();
    };
    jw.key("kpis");
    jw.arr_open();
    {
        int rovr = static_cast<int>(std::lround(t.ovr()));
        const char* otone = rovr >= 80 ? "pos" : (rovr >= 74 ? "info"
                          : (rovr >= 68 ? "warn" : "crit"));
        emit_kpi("ROSTER OVR", std::to_string(rovr), otone, "");

        emit_kpi("PAYROLL", format_money_k(t.total_payroll_k()), "info", "per year");

        long long wage = t.wage_envelope_k;
        const char* wtone = wage > 0 ? "pos" : "warn";
        emit_kpi("WAGE BUDGET", format_money_k(wage), wtone, "to spend");

        char agebuf[16];
        std::snprintf(agebuf, sizeof(agebuf), "%.1f", avg_age);
        const char* atone = avg_age <= 23.0 ? "pos" : (avg_age <= 27.0 ? "info" : "warn");
        emit_kpi("AVG AGE", std::string(agebuf), atone, "");

        const char* ctone = chem_index >= 62 ? "pos" : (chem_index >= 45 ? "info" : "warn");
        emit_kpi("CHEMISTRY", std::to_string(chem_index), ctone, "");

        std::string wval = to_upper(team_window_name(t.window));
        const char* wsub = "";
        const char* wintone = "info";
        switch (t.window) {
            case TeamWindow::Opening: wsub = "Building";   wintone = "info"; break;
            case TeamWindow::Open:    wsub = "Contending"; wintone = "pos";  break;
            case TeamWindow::Closing: wsub = "Urgency";    wintone = "warn"; break;
            case TeamWindow::Closed:  wsub = "Rebuild";    wintone = "crit"; break;
        }
        emit_kpi("WINDOW", wval, wintone, wsub);
    }
    jw.arr_close();

    // ---- team meta -----------------------------------------------------------
    jw.key("team");
    jw.obj_open();
    jw.kv_double("ovr",            t.ovr(), 1);
    jw.kv_int("prestige",          t.prestige);
    jw.kv_str("brand",             brand_tag_name(t.identity.brand));
    jw.kv_str("wealth_tier",       wealth_tier_name(t.wealth_tier));
    jw.kv_str("window",            to_upper(team_window_name(t.window)));
    jw.kv_int("payroll_k",         t.total_payroll_k());
    jw.kv_int("wage_envelope_k",   t.wage_envelope_k);
    jw.kv_int("committed_payroll_k", t.committed_payroll_k);
    jw.kv_int("sponsorship_k",     t.sponsorship_k);
    jw.kv_int("net_transfer_k",    t.net_transfer_k);
    jw.kv_int("size",              static_cast<long long>(t.roster.size()));
    jw.kv_double("avg_age",        avg_age, 1);
    jw.kv_int("chemistry_index",   chem_index);
    jw.obj_close();

    // ---- players (roster order: 0..4 starters, 5+ bench) ---------------------
    jw.key("players");
    jw.arr_open();
    int filled = 0;   // non-null players emitted so far; the first 5 are starters
    for (std::size_t idx = 0; idx < t.roster.size(); ++idx) {
        const auto& pp = t.roster[idx];
        if (!pp) continue;
        Player& p = *pp;
        int ovr = static_cast<int>(std::lround(p.ovr()));

        jw.obj_open();
        // Starter/bench keys off the count of REAL players (not the raw index)
        // so a vacated starter slot can't mislabel the next player as bench.
        jw.kv_int("id",            static_cast<long long>(p.id));   // stable key for web->engine actions
        jw.kv_str("slot",          filled < 5 ? "starter" : "bench");
        jw.kv_int("idx",           static_cast<long long>(idx));
        jw.kv_str("name",          p.name);
        jw.kv_str("role",          role_name(p.primary_role));
        jw.kv_str("contract_role", p.contract_role != Role::Count
                                   ? role_name(p.contract_role)
                                   : role_name(p.primary_role));
        jw.kv_int("age",           p.age);
        jw.kv_str("country",       p.country);
        jw.kv_str("country_iso",   p.country_iso);
        jw.kv_str("region",        p.region);
        jw.kv_int("ovr",           ovr);
        jw.kv_int("potential",     p.potential);
        jw.kv_int("growth",        std::max(0, p.potential - ovr));
        jw.kv_str("trajectory",    trajectory_label(p.trajectory_class));
        jw.kv_double("avg_rating", p.avg_match_rating(), 2);
        jw.kv_double("kd",         p.kd_ratio(), 2);
        jw.kv_int("season_matches", p.season_matches);
        double s_rat = p.season_matches > 0
            ? p.season_rating_total / p.season_matches : 0.0;
        jw.kv_double("season_avg_rating", s_rat, 2);
        double s_kd = p.season_deaths > 0
            ? static_cast<double>(p.season_kills) / p.season_deaths
            : static_cast<double>(p.season_kills);
        jw.kv_double("season_kd",  s_kd, 2);
        jw.kv_bool("igl",          p.is_igl);
        jw.kv_bool("flex",         p.is_flex);
        jw.kv_bool("transfer_requested", p.transfer_requested);
        jw.kv_int("mood",          p.overall_mood(t, gm.year));
        jw.kv_double("discontent", p.discontent, 2);

        jw.key("contract");
        jw.obj_open();
        jw.kv_int("years_left",      p.years_left(gm.year));
        jw.kv_int("salary_k",        p.contract.amount_k);
        jw.kv_int("signing_bonus_k", p.contract.signing_bonus_k);
        jw.kv_bool("promised_starter", p.contract.promised_starter);
        jw.obj_close();

        jw.kv_str("signature_agent", p.signature_agent());
        jw.kv_int("agent_pool_size", p.agent_pool_size);
        jw.key("agent_pool");
        jw.arr_open();
        for (const Agent* ag : p.agent_pool) {
            if (!ag) continue;
            jw.obj_open();
            jw.kv_str("name", ag->name);
            jw.kv_str("role", role_name(ag->role));
            jw.obj_close();
        }
        jw.arr_close();

        jw.key("personality");
        jw.obj_open();
        jw.kv_int("ambition", p.ambition);
        jw.kv_int("loyalty",  p.loyalty);
        jw.kv_int("greed",    p.greed);
        jw.kv_int("ego",      p.ego);
        jw.kv_str("desire",   desire_name(p.desire));
        jw.obj_close();

        jw.key("attributes");
        jw.arr_open();
        for (std::size_t i = 0; i < kAttrCount; ++i) {
            const char* an = attr_name(static_cast<Attr>(i));
            if (!an || !an[0] || an[0] == '?') continue;
            jw.obj_open();
            jw.kv_str("name",  an);
            jw.kv_int("value", p.attributes[i]);
            jw.obj_close();
        }
        jw.arr_close();

        jw.key("career");
        jw.obj_open();
        jw.kv_int("matches",   p.career_matches);
        jw.kv_int("kills",     p.career_kills);
        jw.kv_int("deaths",    p.career_deaths);
        jw.kv_int("assists",   p.career_assists);
        jw.kv_double("kd",     p.kd_ratio(), 2);
        jw.kv_double("adr",    p.career_adr(), 0);
        jw.kv_double("hs_pct", p.career_hs_pct(), 0);
        jw.kv_double("kast_pct", p.career_kast_pct(), 0);
        jw.kv_double("entry_pct", p.career_entry_pct(), 0);
        jw.kv_double("avg_rating", p.avg_match_rating(), 2);
        jw.obj_close();

        jw.key("solo");
        jw.obj_open();
        jw.kv_int("mmr",      p.solo_mmr);
        jw.kv_int("peak_mmr", p.peak_mmr);
        jw.kv_double("kd",    p.solo_kd(), 2);
        jw.kv_int("wins",     p.solo_wins);
        jw.kv_int("losses",   p.solo_losses);
        jw.obj_close();

        Player::TrophySummary tsum = p.trophy_summary();
        jw.key("accolades");
        jw.obj_open();
        jw.kv_int("regional",    tsum.regional);
        jw.kv_int("masters",     tsum.masters);
        jw.kv_int("worlds",      tsum.worlds);
        jw.kv_int("mvps",        tsum.mvps);
        jw.kv_int("role_awards", tsum.role_awards);
        jw.kv_int("igl_oty",     tsum.igl_oty);
        jw.kv_int("total",       tsum.total_trophies());
        jw.key("titles");
        jw.arr_open();
        for (const std::string& title : tsum.all_titles) jw.arr_str(title);
        jw.arr_close();
        jw.obj_close();

        jw.kv_int("joined_year",     p.joined_year);
        jw.kv_int("titles_with_org", p.titles_with_org);

        jw.key("badges");
        jw.arr_open();
        for (const std::string& bdg : p.badges) jw.arr_str(bdg);
        jw.arr_close();

        jw.obj_close();   // player
        ++filled;
    }
    jw.arr_close();

    // ---- chemistry (top pairs) ----------------------------------------------
    // a/b are raw Player* from the chemistry map and MAY be off-roster mid-season
    // (the map is pruned only at year-end). Dereferencing them is safe ONLY
    // because of the never-free invariant: released/retired Players are never
    // destroyed, so the pointer always targets a live object. Do NOT assume these
    // are current-roster, and do NOT add pruning that frees Players without
    // revisiting this read.
    jw.key("chemistry");
    jw.arr_open();
    for (const auto& tup : t.top_chemistry_pairs(6)) {
        const Player* a = std::get<0>(tup);
        const Player* b = std::get<1>(tup);
        double v = std::get<2>(tup);
        if (!a || !b) continue;
        jw.obj_open();
        jw.kv_str("a", a->name);
        jw.kv_str("b", b->name);
        jw.kv_double("value", v, 2);
        jw.obj_close();
    }
    jw.arr_close();

    // ---- staff (coach / scout / analyst; null if unstaffed) ------------------
    jw.key("staff");
    jw.obj_open();

    // Coach intentionally has no "quality" field (the engine exposes no
    // Coach::quality01, unlike Scout/Analyst), so the UI falls back to reputation.
    if (t.head_coach) {
        Coach& c = *t.head_coach;
        jw.key("coach");
        jw.obj_open();
        jw.kv_int("id",          static_cast<long long>(c.id));   // staff-profile modal handle
        jw.kv_str("name",        c.name);
        jw.kv_str("personality", coach_personality_name(c.personality));
        jw.kv_int("tactical",    c.tactical);
        jw.kv_int("development", c.development);
        jw.kv_int("leadership",  c.leadership);
        jw.kv_int("experience",  c.experience);
        jw.kv_int("reputation",  c.reputation);
        jw.kv_int("career_titles",  c.career_titles);
        jw.kv_int("career_seasons", c.career_seasons);
        jw.kv_int("salary_k",    c.requested_salary_k());
        jw.obj_close();
    } else {
        jw.kv_raw("coach", "null");
    }

    if (t.head_scout) {
        Scout& s = *t.head_scout;
        jw.key("scout");
        jw.obj_open();
        jw.kv_int("id",          static_cast<long long>(s.id));   // staff-profile modal handle
        jw.kv_str("name",        s.name);
        jw.kv_str("personality", scout_personality_name(s.personality));
        jw.kv_int("judgement",   s.judgement);
        jw.kv_int("network",     s.network);
        jw.kv_int("projection",  s.projection);
        jw.kv_int("experience",  s.experience);
        jw.kv_int("reputation",  s.reputation);
        jw.kv_int("quality",     static_cast<int>(std::lround(s.quality01() * 99.0)));
        jw.kv_int("salary_k",    s.requested_salary_k());
        jw.obj_close();
    } else {
        jw.kv_raw("scout", "null");
    }

    if (t.head_analyst) {
        Analyst& a = *t.head_analyst;
        jw.key("analyst");
        jw.obj_open();
        jw.kv_int("id",              static_cast<long long>(a.id));   // staff-profile modal handle
        jw.kv_str("name",            a.name);
        jw.kv_str("personality",     analyst_personality_name(a.personality));
        jw.kv_int("tactical_read",   a.tactical_read);
        jw.kv_int("opponent_insight", a.opponent_insight);
        jw.kv_int("prep",            a.prep);
        jw.kv_int("experience",      a.experience);
        jw.kv_int("reputation",      a.reputation);
        jw.kv_int("quality",         static_cast<int>(std::lround(a.quality01() * 99.0)));
        jw.kv_int("report_depth",    a.report_depth_sections());
        jw.kv_int("salary_k",        a.requested_salary_k());
        jw.obj_close();
    } else {
        jw.kv_raw("analyst", "null");
    }

    jw.obj_close();   // staff

    // Canonical badge catalogue (names only) — powers the god-mode "add badge"
    // picker on the web roster screen. OUTPUT-ONLY (reads the static badge table).
    // Emitted INSIDE the roster object: the web renderRoster receives data.roster
    // and reads R.all_badges.
    jw.key("all_badges");
    jw.arr_open();
    for (const Badge& b : badges()) jw.arr_str(b.name);
    jw.arr_close();

    jw.obj_close();   // roster
    jw.obj_close();   // root
    return jw.str();
}

// =============================================================
// Competition view — full standings + upcoming schedule.
// =============================================================
std::string export_competition_to_json(const GameManager& gm,
                                        const std::string& view_region, int view_tier) {
    const TeamPtr& user = gm.user_team;
    if (!user) return "{}";
    Team& t = *user;

    // Resolve the VIEWED league: any region + any tier (Challengers / Open Circuit),
    // defaulting to the user's own division. Falls back to the user's league when the
    // requested combination doesn't exist so the screen never blanks.
    std::string region = view_region.empty() ? t.region : view_region;
    int tier = (view_tier >= 1) ? view_tier : gm.tier_of(user);
    if (tier < 1) tier = 1;
    std::shared_ptr<League> lg = gm.league_at(region, tier);
    if (!lg) { region = t.region; tier = gm.tier_of(user); if (tier < 1) tier = 1; lg = gm.league_at(region, tier); }
    std::vector<TeamPtr> standings;
    if (lg) {
        standings = lg->teams();
        standings.erase(std::remove(standings.begin(), standings.end(), nullptr), standings.end());
        std::sort(standings.begin(), standings.end(), team_standings_less);
    } else {
        standings = sorted_league(gm, user);   // defensive: pre-tier worlds
    }
    std::string div_label = (lg && !lg->division_name().empty()) ? lg->division_name()
                          : (tier == 2 ? "Challengers" : tier >= 3 ? "Open Circuit" : "VCT");

    JsonWriter jw;
    jw.obj_open();
    jw.kv_int("schema_version", 1);
    jw.kv_str("exporter", "VLR Manager Valosim");
    jw.kv_str("club_color", hex_color(t.color_primary_r, t.color_primary_g, t.color_primary_b));

    jw.key("competition");
    jw.obj_open();

    jw.key("header");
    jw.obj_open();
    jw.kv_str("club_name",     t.name);
    jw.kv_str("division",      div_label);
    jw.kv_str("region",        region);
    jw.kv_int("year",          gm.year);
    jw.kv_int("day_in_year",   gm.day_in_year + 1);
    jw.kv_str("current_phase", title_case(gm.current_phase()));
    jw.kv_str("league_name",   region + " " + div_label);
    jw.obj_close();

    // Switcher state for the UI: which league is on screen, which exist, and where
    // the user's own division lives (so its pill can carry a "home" marker).
    jw.key("view");
    jw.obj_open();
    jw.kv_str("region", region);
    jw.kv_int("tier",   tier);
    jw.kv_str("user_region", t.region);
    jw.kv_int("user_tier",   std::max(1, gm.tier_of(user)));
    jw.key("regions");
    jw.arr_open();
    jw.arr_str("Americas"); jw.arr_str("EMEA"); jw.arr_str("Pacific");
    jw.arr_close();
    jw.key("tiers");
    jw.arr_open();
    {
        int tc = gm.tier_count(region);
        if (tc < 1) tc = 1;
        for (int ti = 1; ti <= tc; ++ti) {
            auto tl = gm.league_at(region, ti);
            jw.obj_open();
            jw.kv_int("tier", ti);
            jw.kv_str("label", (tl && !tl->division_name().empty()) ? tl->division_name()
                              : (ti == 2 ? "Challengers" : ti >= 3 ? "Open Circuit" : "VCT"));
            jw.obj_close();
        }
    }
    jw.arr_close();
    jw.obj_close();

    jw.key("standings");
    jw.arr_open();
    int rank = 0;
    for (const TeamPtr& tm : standings) {
        ++rank;
        jw.obj_open();
        jw.kv_int("rank",      rank);
        jw.kv_str("team_name", tm->name);
        jw.kv_str("tag",       tm->tag);
        jw.kv_int("wins",      tm->phase_wins);
        jw.kv_int("losses",    tm->phase_losses);
        jw.kv_double("ovr",    tm->ovr(), 1);
        jw.kv_bool("is_user",  tm.get() == user.get());
        jw.key("form");
        jw.arr_open();
        const auto& rr = tm->recent_results;
        int n = static_cast<int>(rr.size());
        for (int i = std::max(0, n - 5); i < n; ++i)
            jw.arr_str(rr[static_cast<std::size_t>(i)] ? "W" : "L");
        jw.arr_close();
        jw.obj_close();
    }
    jw.arr_close();

    jw.key("schedule");
    jw.arr_open();
    int emitted = 0;
    // Fixtures come from the tier-1 weekly matchups; lower divisions replay their
    // round-robin internally, so a tier-2/3 view shows standings without a slate.
    for (const auto& f : (tier == 1 ? gm.upcoming_fixtures(28)
                                    : std::vector<UpcomingFixture>{})) {
        if (f.region != region || !f.a || !f.b) continue;
        if (emitted++ >= 16) break;
        jw.obj_open();
        jw.kv_int("day_in_year", f.day_in_year + 1);
        jw.kv_int("days_until",  std::max(0, f.day_in_year - gm.day_in_year));
        jw.kv_str("a",       f.a->name);
        jw.kv_str("b",       f.b->name);
        jw.kv_str("a_tag",   f.a->tag);
        jw.kv_str("b_tag",   f.b->tag);
        jw.kv_str("label",   f.label);
        jw.kv_bool("is_user", f.a.get() == user.get() || f.b.get() == user.get());
        jw.obj_close();
    }
    jw.arr_close();

    jw.obj_close();   // competition
    jw.obj_close();   // root
    return jw.str();
}

// =============================================================
// Finance view — summary + KPIs + salary breakdown + transfers.
// =============================================================
std::string export_finance_to_json(const GameManager& gm) {
    const TeamPtr& user = gm.user_team;
    if (!user) return "{}";
    Team& t = *user;

    long long payroll      = t.total_payroll_k();
    long long proj_balance = static_cast<long long>(t.projected_income_k)
                           - t.committed_payroll_k + t.net_transfer_k;

    JsonWriter jw;
    jw.obj_open();
    jw.kv_int("schema_version", 1);
    jw.kv_str("exporter", "VLR Manager Valosim");
    jw.kv_str("club_color", hex_color(t.color_primary_r, t.color_primary_g, t.color_primary_b));

    jw.key("finance");
    jw.obj_open();

    jw.key("header");
    jw.obj_open();
    jw.kv_str("club_name",     t.name);
    jw.kv_str("division",      division_label(gm, user));
    jw.kv_str("region",        t.region);
    jw.kv_int("year",          gm.year);
    jw.kv_int("day_in_year",   gm.day_in_year + 1);
    jw.kv_str("current_phase", title_case(gm.current_phase()));
    jw.obj_close();

    jw.key("summary");
    jw.obj_open();
    jw.kv_int("budget",              t.budget);
    jw.kv_int("sponsorship_k",       t.sponsorship_k);
    jw.kv_int("last_revenue_k",      t.last_revenue_k);
    jw.kv_int("projected_income_k",  t.projected_income_k);
    jw.kv_int("committed_payroll_k", t.committed_payroll_k);
    jw.kv_int("wage_envelope_k",     t.wage_envelope_k);
    jw.kv_int("net_transfer_k",      t.net_transfer_k);
    jw.kv_int("payroll_k",           payroll);
    jw.kv_int("projected_balance_k", proj_balance);
    jw.kv_str("wealth_tier",         wealth_tier_name(t.wealth_tier));
    jw.kv_int("prestige",            t.prestige);
    jw.obj_close();

    // Preseason sponsor system (read-only) — the pending 3-offer choice while
    // sponsor_choice_pending_, plus the active deal + live progress toward its
    // requirement. Mirrors the ImGui Finance panel + the choose-sponsor modal.
    {
        auto rat = [](int v){ int a = v / 100, b = v % 100; return std::to_string(a) + "." + (b < 10 ? "0" : "") + std::to_string(b); };
        jw.key("sponsor");
        jw.obj_open();
        jw.kv_bool("pending", gm.sponsor_choice_pending_);
        jw.key("offers");
        jw.arr_open();
        if (gm.sponsor_choice_pending_) {
            int oi = 0;
            for (const auto& off : gm.pending_sponsor_offers_) {
                jw.obj_open();
                jw.kv_int("idx",         oi++);
                jw.kv_str("name",        off.name);
                jw.kv_str("requirement", off.requirement_label);
                jw.kv_int("reward_k",    off.reward_k);
                jw.obj_close();
            }
        }
        jw.arr_close();
        jw.kv_bool("active", t.sponsor_active);
        if (t.sponsor_active) {
            jw.kv_str("name",      t.sponsor_name);
            jw.kv_int("reward_k",  t.sponsor_reward_k);
            jw.kv_bool("credited", t.sponsor_credited);
            int tgt = t.sponsor_req_value, cur = 0;
            std::string req, prog;
            bool on_track = false;
            switch (t.sponsor_req_type) {
                case SponsorReqType::Placement:
                case SponsorReqType::TitleBerth: {
                    auto pl = gm.user_league_placement();
                    cur = pl.first;
                    req = (t.sponsor_req_type == SponsorReqType::Placement)
                          ? ("Finish top " + std::to_string(tgt))
                          : ("Reach a playoff berth (top " + std::to_string(tgt) + ")");
                    prog = (cur > 0) ? ("currently #" + std::to_string(cur)) : "unranked";
                    on_track = (cur > 0 && cur <= tgt);
                    break;
                }
                case SponsorReqType::WinCount:
                    cur = t.wins;
                    req = std::to_string(tgt) + "+ regular-season wins";
                    prog = std::to_string(cur) + " so far";
                    on_track = (cur >= tgt);
                    break;
                case SponsorReqType::IndividualMilestone: {
                    double best = 0.0;
                    for (auto& p : t.roster) {
                        if (!p || p->season_matches <= 0) continue;
                        double r = p->season_rating_total / p->season_matches;
                        if (r > best) best = r;
                    }
                    cur = static_cast<int>(best * 100.0 + 0.5);
                    req = "A player ends the season rated " + rat(tgt) + "+";
                    prog = "best so far " + rat(cur);
                    on_track = (cur >= tgt);
                    break;
                }
            }
            jw.kv_str("requirement", req);
            jw.kv_str("progress",    prog);
            jw.kv_bool("on_track",   on_track);
        }
        jw.obj_close();
    }

    auto emit_kpi = [&](const std::string& label, const std::string& value,
                        const char* tone, const std::string& subline) {
        jw.obj_open();
        jw.kv_str("label", label);
        jw.kv_str("value", value);
        jw.kv_str("tone",  tone);
        if (!subline.empty()) jw.kv_str("subline", subline);
        jw.obj_close();
    };
    jw.key("kpis");
    jw.arr_open();
    {
        long long ab = t.budget < 0 ? -t.budget : t.budget;
        const char* btone = ab >= 2000000 ? "pos" : (ab >= 1000000 ? "info" : (ab >= 400000 ? "warn" : "crit"));
        emit_kpi("BUDGET",       format_money(t.budget),               btone, "cash on hand");
        emit_kpi("SPONSORSHIP",  format_money_k(t.sponsorship_k),      "info", "per year");
        emit_kpi("PAYROLL",      format_money_k(payroll),              "warn", "per year");
        emit_kpi("WAGE BUDGET",  format_money_k(t.wage_envelope_k),    t.wage_envelope_k > 0 ? "pos" : "warn", "to spend");
        emit_kpi("NET TRANSFERS", format_money_k(t.net_transfer_k),    t.net_transfer_k >= 0 ? "pos" : "crit", "this season");
        emit_kpi("PROJ. BALANCE", format_money_k(proj_balance),        proj_balance >= 0 ? "pos" : "crit", "next season");
    }
    jw.arr_close();

    // Salary breakdown (roster, highest wage first).
    jw.key("wages");
    jw.arr_open();
    {
        std::vector<Player*> roster;
        roster.reserve(t.roster.size());
        for (const auto& p : t.roster) if (p) roster.push_back(p.get());
        std::sort(roster.begin(), roster.end(),
                  [](const Player* a, const Player* b) { return a->contract.amount_k > b->contract.amount_k; });
        for (const Player* p : roster) {
            jw.obj_open();
            jw.kv_str("name",       p->name);
            jw.kv_str("role",       role_name(p->primary_role));
            jw.kv_int("salary_k",   p->contract.amount_k);
            jw.kv_int("years_left", p->years_left(gm.year));
            jw.obj_close();
        }
    }
    jw.arr_close();

    // Staff costs.
    jw.key("staff");
    jw.arr_open();
    if (t.head_coach) {
        jw.obj_open(); jw.kv_str("role", "Head Coach");   jw.kv_str("name", t.head_coach->name);
        jw.kv_int("salary_k", t.head_coach->requested_salary_k());   jw.obj_close();
    }
    if (t.head_scout) {
        jw.obj_open(); jw.kv_str("role", "Head Scout");   jw.kv_str("name", t.head_scout->name);
        jw.kv_int("salary_k", t.head_scout->requested_salary_k());   jw.obj_close();
    }
    if (t.head_analyst) {
        jw.obj_open(); jw.kv_str("role", "Head Analyst"); jw.kv_str("name", t.head_analyst->name);
        jw.kv_int("salary_k", t.head_analyst->requested_salary_k()); jw.obj_close();
    }
    jw.arr_close();

    // Transfer log (most-recent-first, capped).
    jw.key("transfers");
    jw.arr_open();
    {
        int cap = 0;
        for (const auto& r : t.transfer_log_) {
            if (cap++ >= 20) break;
            jw.obj_open();
            jw.kv_int("year",   r.year);
            jw.kv_str("player", r.player);
            jw.kv_str("from",   r.from_team);
            jw.kv_str("to",     r.to_team);
            jw.kv_int("fee_k",  r.fee_k);
            jw.kv_int("wage_k", r.wage_k);
            jw.obj_close();
        }
    }
    jw.arr_close();

    jw.obj_close();   // finance
    jw.obj_close();   // root
    return jw.str();
}

// =============================================================
// Mail / Inbox view.
// =============================================================
std::string export_mail_to_json(const GameManager& gm) {
    const TeamPtr& user = gm.user_team;
    if (!user) return "{}";
    Team& t = *user;

    auto cat_name = [](GameManager::MailCategory c) -> const char* {
        switch (c) {
            case GameManager::MailCategory::Board:    return "Board";
            case GameManager::MailCategory::Transfer: return "Transfer";
            case GameManager::MailCategory::Contract: return "Contract";
            case GameManager::MailCategory::Award:    return "Award";
            case GameManager::MailCategory::Squad:    return "Squad";
            case GameManager::MailCategory::Result:   return "Result";
            case GameManager::MailCategory::Media:
            default:                                  return "Media";
        }
    };

    JsonWriter jw;
    jw.obj_open();
    jw.kv_int("schema_version", 1);
    jw.kv_str("exporter", "VLR Manager Valosim");
    jw.kv_str("club_color", hex_color(t.color_primary_r, t.color_primary_g, t.color_primary_b));

    jw.key("mail");
    jw.obj_open();

    jw.key("header");
    jw.obj_open();
    jw.kv_str("club_name",     t.name);
    jw.kv_str("division",      division_label(gm, user));
    jw.kv_str("region",        t.region);
    jw.kv_int("year",          gm.year);
    jw.kv_int("day_in_year",   gm.day_in_year + 1);
    jw.kv_str("current_phase", title_case(gm.current_phase()));
    jw.obj_close();

    jw.kv_int("unread", gm.unread_mail_count());
    jw.kv_int("total",  static_cast<long long>(gm.mailbox.size()));

    jw.key("items");
    jw.arr_open();
    int cap = 0;
    for (const auto& m : gm.mailbox) {
        if (cap++ >= 150) break;           // most-recent-first; full engine mailbox (capped 150)
        jw.obj_open();
        jw.kv_int("id",          m.id);
        jw.kv_int("year",        m.year);
        jw.kv_int("day",         m.day + 1);
        jw.kv_str("category",    cat_name(m.category));
        jw.kv_str("subject",     m.subject);
        jw.kv_str("body",        m.body);
        jw.kv_bool("read",       m.read);
        jw.kv_bool("important",  m.important);
        jw.kv_str("player_name", m.player_name);
        jw.kv_int("player_id",   static_cast<long long>(m.player_id));   // deep-link → player modal
        jw.kv_str("team_name",   m.team_name);
        jw.kv_int("amount_k",    m.amount_k);
        jw.obj_close();
    }
    jw.arr_close();

    jw.obj_close();   // mail
    jw.obj_close();   // root
    return jw.str();
}

// =============================================================
// Strategy / match-prep view.
// =============================================================
std::string export_strategy_to_json(const GameManager& gm) {
    const TeamPtr& user = gm.user_team;
    if (!user) return "{}";
    Team& t = *user;

    // Next fixture involving the user team (for the opposition report).
    bool has_next = false; int next_day = INT_MAX; TeamPtr opp;
    for (const auto& f : gm.upcoming_fixtures(14)) {
        if (f.a == user || f.b == user) {
            if (f.day_in_year < next_day) { next_day = f.day_in_year; opp = (f.a == user) ? f.b : f.a; has_next = true; }
        }
    }

    JsonWriter jw;
    jw.obj_open();
    jw.kv_int("schema_version", 1);
    jw.kv_str("exporter", "VLR Manager Valosim");
    jw.kv_str("club_color", hex_color(t.color_primary_r, t.color_primary_g, t.color_primary_b));

    jw.key("strategy");
    jw.obj_open();

    jw.key("header");
    jw.obj_open();
    jw.kv_str("club_name",     t.name);
    jw.kv_str("division",      division_label(gm, user));
    jw.kv_str("region",        t.region);
    jw.kv_int("year",          gm.year);
    jw.kv_int("day_in_year",   gm.day_in_year + 1);
    jw.kv_str("current_phase", title_case(gm.current_phase()));
    jw.kv_int("team_talk",     gm.user_team_talk);   // strategy hosts talk chips too (matchday-prep hub)
    jw.obj_close();

    // AI General Manager delegation — when on, the AI GM manages the user roster
    // (mid-season cuts/replacements + the full offseason pass) like any AI club.
    jw.kv_bool("auto_manage", gm.user_auto_manage);

    if (has_next && opp) {
        GameManager::OppositionReport rep = gm.scout_opposition(*opp);
        jw.key("next_opponent");
        jw.obj_open();
        jw.kv_str("name",          opp->name);
        jw.kv_str("region",        opp->region);
        jw.kv_int("days_until",    std::max(0, next_day - gm.day_in_year));
        jw.kv_bool("valid",        rep.valid);
        jw.kv_bool("has_analyst",  rep.has_analyst);
        jw.kv_int("detail_level",  rep.detail_level);
        jw.kv_double("accuracy",   rep.accuracy, 2);
        jw.kv_int("ovr_est",       rep.opp_ovr_est);
        jw.kv_int("ovr_band",      rep.ovr_band);
        jw.kv_str("form",          rep.form);
        jw.kv_str("comp_tendency", rep.comp_tendency);
        jw.kv_str("role_read",     rep.role_read);
        jw.kv_str("recommendation", rep.recommendation);
        jw.key("key_players");
        jw.arr_open();
        for (const auto& kp : rep.key_players) jw.arr_str(kp);
        jw.arr_close();
        jw.obj_close();
    } else {
        jw.kv_raw("next_opponent", "null");
    }

    jw.key("maps");
    jw.arr_open();
    for (const GameMap& m : maps()) {
        jw.obj_open();
        jw.kv_str("name", m.name);
        int lvl = 0;
        auto pit = t.map_prep.find(m.name);
        if (pit != t.map_prep.end()) lvl = pit->second.level;
        jw.kv_int("prep_level", lvl);
        auto ait = t.agent_override.find(m.name);
        if (ait != t.agent_override.end() && !ait->second.empty()) {
            jw.key("agents");
            jw.arr_open();
            for (const auto& an : ait->second) jw.arr_str(an);
            jw.arr_close();
        } else {
            jw.kv_raw("agents", "null");
        }
        jw.key("signature_agents");
        jw.arr_open();
        for (const auto& sa : map_signature_agents(m.name)) jw.arr_str(sa);
        jw.arr_close();
        jw.obj_close();
    }
    jw.arr_close();

    // Full agent roster (name + role idx) so the comp editor can populate its
    // role-grouped pickers. Static list — same for every map. Read-only.
    jw.key("all_agents");
    jw.arr_open();
    for (const auto& a : agents()) {
        jw.obj_open();
        jw.kv_str("name", a.name);
        jw.kv_int("role", static_cast<int>(a.role));
        jw.obj_close();
    }
    jw.arr_close();

    jw.obj_close();   // strategy
    jw.obj_close();   // root
    return jw.str();
}

// =============================================================
// Calendar view — the 11-phase season ring + upcoming schedule.
// =============================================================
std::string export_calendar_to_json(const GameManager& gm) {
    const TeamPtr& user = gm.user_team;
    if (!user) return "{}";
    Team& t = *user;

    const std::vector<std::string>& ph = GameManager::phases();
    int n = static_cast<int>(ph.size());
    int cur = n > 0 ? (gm.phase_idx % n) : 0;
    std::pair<int,int> pacing = gm.current_phase_pacing();
    int matchdays = pacing.first * pacing.second;

    JsonWriter jw;
    jw.obj_open();
    jw.kv_int("schema_version", 1);
    jw.kv_str("exporter", "VLR Manager Valosim");
    jw.kv_str("club_color", hex_color(t.color_primary_r, t.color_primary_g, t.color_primary_b));

    jw.key("calendar");
    jw.obj_open();

    jw.key("header");
    jw.obj_open();
    jw.kv_str("club_name",     t.name);
    jw.kv_str("division",      division_label(gm, user));
    jw.kv_str("region",        t.region);
    jw.kv_int("year",          gm.year);
    jw.kv_int("day_in_year",   gm.day_in_year + 1);
    jw.kv_str("current_phase", title_case(gm.current_phase()));
    jw.obj_close();

    jw.kv_int("day_in_year",         gm.day_in_year + 1);
    jw.kv_int("day_total",           GameManager::kDaysPerYear);
    jw.kv_int("phase_idx",           cur);
    jw.kv_int("day_in_phase",        gm.day_in_phase);
    jw.kv_int("phase_matchdays",     matchdays);
    jw.kv_int("phase_days_per_round", pacing.first);
    jw.kv_int("phase_total_rounds",  pacing.second);

    // The 11-phase season ring with current/past markers (within this cycle).
    jw.key("phases");
    jw.arr_open();
    for (int i = 0; i < n; ++i) {
        jw.obj_open();
        jw.kv_str("name",       title_case(ph[static_cast<std::size_t>(i)]));
        jw.kv_bool("is_current", i == cur);
        jw.kv_bool("is_past",    i < cur);
        jw.obj_close();
    }
    jw.arr_close();

    // Upcoming fixtures in the user's region.
    jw.key("schedule");
    jw.arr_open();
    int emitted = 0;
    for (const auto& f : gm.upcoming_fixtures(28)) {
        if (f.region != t.region || !f.a || !f.b) continue;
        if (emitted++ >= 18) break;
        jw.obj_open();
        jw.kv_int("day_in_year", f.day_in_year + 1);
        jw.kv_int("days_until",  std::max(0, f.day_in_year - gm.day_in_year));
        jw.kv_str("a",       f.a->name);
        jw.kv_str("b",       f.b->name);
        jw.kv_str("a_tag",   f.a->tag);
        jw.kv_str("b_tag",   f.b->tag);
        jw.kv_str("label",   f.label);
        jw.kv_bool("is_user", f.a.get() == user.get() || f.b.get() == user.get());
        jw.obj_close();
    }
    jw.arr_close();

    jw.obj_close();   // calendar
    jw.obj_close();   // root
    return jw.str();
}

// =============================================================
// Market view — free-agent pool + the user's spending room.
// =============================================================
std::string export_market_to_json(const GameManager& gm) {
    const TeamPtr& user = gm.user_team;
    if (!user) return "{}";
    Team& t = *user;

    // Collect free agents from the per-region SoloQ ladders (never-free pools).
    std::vector<Player*> fas;
    for (const auto& kv : gm.solo_qs) {
        if (!kv.second) continue;
        for (const auto& p : kv.second->global_ladder()) {
            if (p && p->team_name == "Free Agent") fas.push_back(p.get());
        }
    }
    std::sort(fas.begin(), fas.end(),
              [](const Player* a, const Player* b) { return a->ovr() > b->ovr(); });

    JsonWriter jw;
    jw.obj_open();
    jw.kv_int("schema_version", 1);
    jw.kv_str("exporter", "VLR Manager Valosim");
    jw.kv_str("club_color", hex_color(t.color_primary_r, t.color_primary_g, t.color_primary_b));

    jw.key("market");
    jw.obj_open();

    jw.key("header");
    jw.obj_open();
    jw.kv_str("club_name",     t.name);
    jw.kv_str("division",      division_label(gm, user));
    jw.kv_str("region",        t.region);
    jw.kv_int("year",          gm.year);
    jw.kv_int("day_in_year",   gm.day_in_year + 1);
    jw.kv_str("current_phase", title_case(gm.current_phase()));
    jw.obj_close();

    jw.kv_int("budget",          t.budget);
    jw.kv_int("wage_envelope_k", t.wage_envelope_k);
    jw.kv_int("roster_size",     static_cast<long long>(t.roster.size()));
    jw.kv_int("fa_count",        static_cast<long long>(fas.size()));

    jw.key("free_agents");
    jw.arr_open();
    int cap = std::min(static_cast<int>(fas.size()), 40);
    for (int i = 0; i < cap; ++i) {
        const Player* p = fas[static_cast<std::size_t>(i)];
        jw.obj_open();
        jw.kv_int("id",              static_cast<long long>(p->id));
        jw.kv_str("name",            p->name);
        jw.kv_str("role",            role_name(p->primary_role));
        jw.kv_int("ovr",             static_cast<long long>(std::lround(p->ovr())));
        // Potential fog — same rule as the recruitment exporter: the TRUE hidden
        // potential only leaves the engine once scouted. Free agents are never on
        // the user team, so no on_user term here.
        bool pot_known = p->potential_scouted;
        jw.kv_bool("pot_known",      pot_known);
        if (pot_known) { jw.kv_int("potential", p->potential); }
        else { int lo = 0, hi = 0; gm.scouted_band(*p, lo, hi); jw.kv_int("pot_lo", lo); jw.kv_int("pot_hi", hi); }
        jw.kv_int("age",             p->age);
        jw.kv_str("region",          p->region);
        jw.kv_str("country_iso",     p->country_iso);
        jw.kv_int("ask_k",           gm.fa_current_demand_k(*p));
        jw.kv_bool("igl",            p->is_igl);
        jw.kv_str("signature_agent", p->signature_agent());
        jw.obj_close();
    }
    jw.arr_close();

    // ---- Free-staff market: the user's current staff + the FA pools (top by
    // reputation). Powers the web hire-staff action (act:hirestaff:<role>:<id>).
    jw.key("free_staff");
    jw.obj_open();

    jw.key("current");
    jw.obj_open();
    if (t.head_coach) {
        jw.key("coach"); jw.obj_open();
        jw.kv_str("name", t.head_coach->name);
        jw.kv_str("personality", coach_personality_name(t.head_coach->personality));
        jw.kv_int("reputation", t.head_coach->reputation);
        jw.kv_int("salary_k", t.head_coach->salary_k);
        jw.obj_close();
    } else jw.kv_raw("coach", "null");
    if (t.head_scout) {
        jw.key("scout"); jw.obj_open();
        jw.kv_str("name", t.head_scout->name);
        jw.kv_str("personality", scout_personality_name(t.head_scout->personality));
        jw.kv_int("reputation", t.head_scout->reputation);
        jw.kv_int("salary_k", t.head_scout->salary_k);
        jw.obj_close();
    } else jw.kv_raw("scout", "null");
    if (t.head_analyst) {
        jw.key("analyst"); jw.obj_open();
        jw.kv_str("name", t.head_analyst->name);
        jw.kv_str("personality", analyst_personality_name(t.head_analyst->personality));
        jw.kv_int("reputation", t.head_analyst->reputation);
        jw.kv_int("salary_k", t.head_analyst->salary_k);
        jw.obj_close();
    } else jw.kv_raw("analyst", "null");
    jw.obj_close();   // current

    {  // Coaches
        std::vector<Coach*> pool;
        for (const auto& c : gm.free_coaches_)
            if (c && !c->is_retired) pool.push_back(c.get());
        std::sort(pool.begin(), pool.end(),
                  [](const Coach* a, const Coach* b) { return a->reputation > b->reputation; });
        jw.key("coaches");
        jw.arr_open();
        int cap = std::min(static_cast<int>(pool.size()), 12);
        for (int i = 0; i < cap; ++i) {
            const Coach* c = pool[static_cast<std::size_t>(i)];
            jw.obj_open();
            jw.kv_int("id",          static_cast<long long>(c->id));
            jw.kv_str("name",        c->name);
            jw.kv_str("personality", coach_personality_name(c->personality));
            jw.kv_int("reputation",  c->reputation);
            jw.kv_int("ask_k",       c->requested_salary_k());
            jw.obj_close();
        }
        jw.arr_close();
    }
    {  // Scouts
        std::vector<Scout*> pool;
        for (const auto& s : gm.free_scouts_)
            if (s && !s->is_retired) pool.push_back(s.get());
        std::sort(pool.begin(), pool.end(),
                  [](const Scout* a, const Scout* b) { return a->reputation > b->reputation; });
        jw.key("scouts");
        jw.arr_open();
        int cap = std::min(static_cast<int>(pool.size()), 12);
        for (int i = 0; i < cap; ++i) {
            const Scout* s = pool[static_cast<std::size_t>(i)];
            jw.obj_open();
            jw.kv_int("id",          static_cast<long long>(s->id));
            jw.kv_str("name",        s->name);
            jw.kv_str("personality", scout_personality_name(s->personality));
            jw.kv_int("reputation",  s->reputation);
            jw.kv_int("ask_k",       s->requested_salary_k());
            jw.obj_close();
        }
        jw.arr_close();
    }
    {  // Analysts
        std::vector<Analyst*> pool;
        for (const auto& a : gm.free_analysts_)
            if (a && !a->is_retired) pool.push_back(a.get());
        std::sort(pool.begin(), pool.end(),
                  [](const Analyst* a, const Analyst* b) { return a->reputation > b->reputation; });
        jw.key("analysts");
        jw.arr_open();
        int cap = std::min(static_cast<int>(pool.size()), 12);
        for (int i = 0; i < cap; ++i) {
            const Analyst* a = pool[static_cast<std::size_t>(i)];
            jw.obj_open();
            jw.kv_int("id",          static_cast<long long>(a->id));
            jw.kv_str("name",        a->name);
            jw.kv_str("personality", analyst_personality_name(a->personality));
            jw.kv_int("reputation",  a->reputation);
            jw.kv_int("ask_k",       a->requested_salary_k());
            jw.obj_close();
        }
        jw.arr_close();
    }

    jw.obj_close();   // free_staff

    jw.obj_close();   // market
    jw.obj_close();   // root
    return jw.str();
}

// =============================================================
// Awards view — live MVP/role races + season recap + history.
// =============================================================
std::string export_awards_to_json(const GameManager& gm) {
    const TeamPtr& user = gm.user_team;
    if (!user) return "{}";
    Team& t = *user;

    JsonWriter jw;
    jw.obj_open();
    jw.kv_int("schema_version", 1);
    jw.kv_str("exporter", "VLR Manager Valosim");
    jw.kv_str("club_color", hex_color(t.color_primary_r, t.color_primary_g, t.color_primary_b));

    jw.key("awards");
    jw.obj_open();

    jw.key("header");
    jw.obj_open();
    jw.kv_str("club_name",     t.name);
    jw.kv_str("division",      division_label(gm, user));
    jw.kv_str("region",        t.region);
    jw.kv_int("year",          gm.year);
    jw.kv_int("day_in_year",   gm.day_in_year + 1);
    jw.kv_str("current_phase", title_case(gm.current_phase()));
    jw.obj_close();

    // Live races (empty until ~mid-season). Top 8 per category, already ranked.
    jw.key("mvp_race");
    jw.arr_open();
    for (const auto& board : gm.mvp_race) {
        jw.obj_open();
        jw.kv_str("category", board.category);
        jw.key("candidates");
        jw.arr_open();
        int n = 0;
        for (const auto& c : board.candidates) {
            if (n >= 8) break;
            if (!c.player) continue;
            ++n;
            const Player& p = *c.player;
            jw.obj_open();
            jw.kv_str("name",        p.name);
            jw.kv_str("role",        role_name(p.primary_role));
            jw.kv_str("team",        p.team_name);
            jw.kv_str("country_iso", p.country_iso);
            jw.kv_double("score",    c.score, 2);
            jw.kv_str("blurb",       c.blurb);
            jw.kv_int("delta",       c.delta);
            jw.kv_bool("igl",        p.is_igl);
            jw.obj_close();
        }
        jw.arr_close();
        jw.obj_close();
    }
    jw.arr_close();

    // SeasonAward emitter (player or coach award).
    auto emit_award = [&](const GameManager::SeasonAward& aw) {
        jw.obj_open();
        jw.kv_int("year",      aw.year);
        jw.kv_str("category",  aw.category);
        bool is_coach = (aw.coach_winner != nullptr);
        jw.kv_bool("is_coach", is_coach);
        if (is_coach) {
            jw.kv_str("winner_name", aw.coach_winner->name);
            jw.kv_str("winner_team", aw.coach_winner->team_name);
        } else if (aw.winner) {
            jw.kv_str("winner_name", aw.winner->name);
            jw.kv_str("winner_team", aw.winner->team_name);
        } else {
            jw.kv_str("winner_name", "");
            jw.kv_str("winner_team", "");
        }
        jw.kv_str("explanation", aw.explanation);
        // Runner-up finalists (skip index 0 = the winner).
        jw.key("finalists");
        jw.arr_open();
        if (is_coach) {
            for (std::size_t i = 1; i < aw.coach_finalists.size(); ++i) {
                if (!aw.coach_finalists[i]) continue;
                jw.obj_open();
                jw.kv_str("name", aw.coach_finalists[i]->name);
                jw.kv_double("score", i < aw.scores.size() ? aw.scores[i] : 0.0, 2);
                jw.obj_close();
            }
        } else {
            for (std::size_t i = 1; i < aw.finalists.size(); ++i) {
                if (!aw.finalists[i]) continue;
                jw.obj_open();
                jw.kv_str("name", aw.finalists[i]->name);
                jw.kv_double("score", i < aw.scores.size() ? aw.scores[i] : 0.0, 2);
                jw.obj_close();
            }
        }
        jw.arr_close();
        jw.obj_close();
    };

    jw.key("last_season");
    jw.arr_open();
    for (const auto& aw : gm.last_season_awards) emit_award(aw);
    jw.arr_close();

    // All-time history, most-recent first (history is append-only oldest-first).
    jw.key("history");
    jw.arr_open();
    {
        int cap = 0;
        for (auto it = gm.awards_history.rbegin(); it != gm.awards_history.rend(); ++it) {
            if (cap++ >= 40) break;
            const GameManager::SeasonAward& aw = *it;
            jw.obj_open();
            jw.kv_int("year",     aw.year);
            jw.kv_str("category", aw.category);
            bool is_coach = (aw.coach_winner != nullptr);
            const char* wn = is_coach ? (aw.coach_winner ? aw.coach_winner->name.c_str() : "")
                                      : (aw.winner ? aw.winner->name.c_str() : "");
            jw.kv_str("winner_name", wn);
            jw.kv_bool("is_coach", is_coach);
            jw.obj_close();
        }
    }
    jw.arr_close();

    jw.obj_close();   // awards
    jw.obj_close();   // root
    return jw.str();
}

// =============================================================
// Brackets view — every active tournament's full bracket.
// =============================================================
namespace {
// Emit one round's matches (a vector<BracketMatch>) as a JSON array.
void write_bracket_round(JsonWriter& jw, const std::vector<BracketMatch>& round) {
    jw.arr_open();
    for (const BracketMatch& m : round) {
        jw.obj_open();
        jw.kv_str("a",       m.a ? m.a->name : std::string("TBD"));
        jw.kv_str("b",       m.b ? m.b->name : std::string("TBD"));
        jw.kv_str("a_tag",   m.a ? m.a->tag  : std::string(""));
        jw.kv_str("b_tag",   m.b ? m.b->tag  : std::string(""));
        jw.kv_int("a_score", m.a_score);
        jw.kv_int("b_score", m.b_score);
        jw.kv_str("winner",  m.winner ? m.winner->name : std::string(""));
        jw.kv_bool("played", m.played);
        jw.kv_int("best_of", m.best_of);
        jw.kv_str("label",   m.label);
        jw.obj_close();
    }
    jw.arr_close();
}
}  // namespace

std::string export_brackets_to_json(const GameManager& gm) {
    const TeamPtr& user = gm.user_team;
    if (!user) return "{}";
    Team& t = *user;

    JsonWriter jw;
    jw.obj_open();
    jw.kv_int("schema_version", 1);
    jw.kv_str("exporter", "VLR Manager Valosim");
    jw.kv_str("club_color", hex_color(t.color_primary_r, t.color_primary_g, t.color_primary_b));

    jw.key("brackets");
    jw.obj_open();

    jw.key("header");
    jw.obj_open();
    jw.kv_str("club_name",     t.name);
    jw.kv_str("division",      division_label(gm, user));
    jw.kv_str("region",        t.region);
    jw.kv_int("year",          gm.year);
    jw.kv_int("day_in_year",   gm.day_in_year + 1);
    jw.kv_str("current_phase", title_case(gm.current_phase()));
    jw.obj_close();

    jw.key("tournaments");
    jw.arr_open();
    for (const auto& tp : gm.active_tournaments) {
        if (!tp) continue;
        const Tournament& tour = *tp;
        jw.obj_open();
        jw.kv_str("name",        tour.name());
        jw.kv_bool("finished",   tour.finished());
        jw.kv_str("round_label", tour.current_round_label());
        jw.kv_str("champion",    tour.champion() ? tour.champion()->name : std::string(""));
        jw.kv_str("runner_up",   tour.runner_up() ? tour.runner_up()->name : std::string(""));
        jw.kv_bool("user_in",    tour.user_team_in_round(user));

        // Current scheduled matchups (upper + lower) for the live round.
        jw.key("current");
        jw.arr_open();
        for (const auto& mm : tour.current_matchups()) {
            jw.obj_open();
            jw.kv_str("a", mm.first ? mm.first->name : std::string("TBD"));
            jw.kv_str("b", mm.second ? mm.second->name : std::string("TBD"));
            jw.kv_bool("lower", false);
            jw.obj_close();
        }
        for (const auto& mm : tour.lower_matchups()) {
            jw.obj_open();
            jw.kv_str("a", mm.first ? mm.first->name : std::string("TBD"));
            jw.kv_str("b", mm.second ? mm.second->name : std::string("TBD"));
            jw.kv_bool("lower", true);
            jw.obj_close();
        }
        jw.arr_close();

        // Full played history: upper rounds, lower rounds, grand final.
        jw.key("upper");
        jw.arr_open();
        for (const auto& round : tour.ub_history()) write_bracket_round(jw, round);
        jw.arr_close();
        jw.key("lower");
        jw.arr_open();
        for (const auto& round : tour.lb_history()) write_bracket_round(jw, round);
        jw.arr_close();
        jw.key("grand_final");
        write_bracket_round(jw, tour.gf_history());

        jw.obj_close();
    }
    jw.arr_close();

    jw.obj_close();   // brackets
    jw.obj_close();   // root
    return jw.str();
}

// =============================================================
// Team Profile view of ANY team (resolved by name).
// =============================================================
std::string export_team_profile_to_json(const GameManager& gm, const std::string& team_name) {
    // Resolve the team by name across all tier-1 leagues (find_team_by_name pattern).
    // Prefer the USER's club when the name matches: clicking your own team must always
    // resolve to YOUR org (correct region, no transfer fees), even if a rival in an
    // alphabetically-earlier region shares the name.
    TeamPtr team;
    if (gm.user_team && gm.user_team->name == team_name) {
        team = gm.user_team;
    } else {
        for (const auto& kv : gm.leagues) {
            if (!kv.second) continue;
            for (const auto& tm : kv.second->teams()) {
                if (tm && tm->name == team_name) { team = tm; break; }
            }
            if (team) break;
        }
    }

    JsonWriter jw;
    jw.obj_open();
    jw.kv_int("schema_version", 1);
    jw.kv_str("exporter", "VLR Manager Valosim");
    if (!team) {
        jw.kv_bool("found", false);
        jw.kv_str("requested", team_name);
        jw.obj_close();
        return jw.str();
    }
    Team& t = *team;
    jw.kv_bool("found", true);
    jw.kv_str("club_color", hex_color(t.color_primary_r, t.color_primary_g, t.color_primary_b));

    jw.key("team_profile");
    jw.obj_open();

    jw.key("identity");
    jw.obj_open();
    jw.kv_str("name",        t.name);
    jw.kv_str("tag",         t.tag);
    jw.kv_str("region",      t.region);
    jw.kv_str("division",    division_label(gm, team));
    jw.kv_int("prestige",    t.prestige);
    jw.kv_str("brand",       brand_tag_name(t.identity.brand));
    jw.kv_str("wealth_tier", wealth_tier_name(t.wealth_tier));
    jw.kv_str("window",      to_upper(team_window_name(t.window)));
    jw.kv_double("ovr",      t.ovr(), 1);
    jw.kv_int("rank",        rank_in_league(gm, team));
    jw.kv_int("phase_wins",  t.phase_wins);
    jw.kv_int("phase_losses", t.phase_losses);
    jw.kv_bool("is_user",    gm.user_team && team.get() == gm.user_team.get());
    jw.obj_close();

    jw.key("finances");
    jw.obj_open();
    jw.kv_int("budget",        t.budget);
    jw.kv_int("payroll_k",     t.total_payroll_k());
    jw.kv_int("sponsorship_k", t.sponsorship_k);
    jw.obj_close();

    bool is_user_team = (gm.user_team && team.get() == gm.user_team.get());
    jw.key("roster");
    jw.arr_open();
    for (std::size_t i = 0; i < t.roster.size(); ++i) {
        const auto& p = t.roster[i];
        if (!p) continue;
        jw.obj_open();
        jw.kv_int("id",         static_cast<long long>(p->id));   // stable key for make-offer
        jw.kv_str("slot",       i < 5 ? "starter" : "bench");
        jw.kv_str("name",       p->name);
        jw.kv_str("role",       role_name(p->primary_role));
        jw.kv_int("ovr",        static_cast<long long>(std::lround(p->ovr())));
        jw.kv_int("potential",  p->potential);
        jw.kv_int("age",        p->age);
        jw.kv_bool("igl",       p->is_igl);
        jw.kv_double("avg_rating", p->avg_match_rating(), 2);
        jw.kv_int("wage_k",     p->contract.amount_k);
        // Suggested transfer fee — the UI's default make-offer bid. Only meaningful
        // for a RIVAL club's contracted player (you buy FROM another team).
        if (!is_user_team) jw.kv_int("fee_k", t.transfer_fee_for(*p, gm.year));
        jw.obj_close();
    }
    jw.arr_close();

    jw.key("staff");
    jw.obj_open();
    if (t.head_coach) {
        jw.key("coach"); jw.obj_open();
        jw.kv_str("name", t.head_coach->name);
        jw.kv_str("personality", coach_personality_name(t.head_coach->personality));
        jw.obj_close();
    } else jw.kv_raw("coach", "null");
    if (t.head_scout) {
        jw.key("scout"); jw.obj_open();
        jw.kv_str("name", t.head_scout->name);
        jw.kv_str("personality", scout_personality_name(t.head_scout->personality));
        jw.obj_close();
    } else jw.kv_raw("scout", "null");
    if (t.head_analyst) {
        jw.key("analyst"); jw.obj_open();
        jw.kv_str("name", t.head_analyst->name);
        jw.kv_str("personality", analyst_personality_name(t.head_analyst->personality));
        jw.obj_close();
    } else jw.kv_raw("analyst", "null");
    jw.obj_close();

    jw.key("history");
    jw.arr_open();
    for (const auto& h : t.history) {
        jw.obj_open();
        jw.kv_int("year",   h.year);
        jw.kv_int("wins",   h.wins);
        jw.kv_int("losses", h.losses);
        jw.obj_close();
    }
    jw.arr_close();

    jw.obj_close();   // team_profile
    jw.obj_close();   // root
    return jw.str();
}

// =============================================================
// Player Profile modal — ANY player by id (rival/FA/retired/HoF).
// =============================================================
std::string export_player_profile_to_json(const GameManager& gm, std::uint64_t player_id) {
    PlayerPtr pp = gm.find_player_by_id(player_id);

    JsonWriter jw;
    jw.obj_open();
    jw.kv_int("schema_version", 1);
    jw.kv_str("exporter", "VLR Manager Valosim");
    if (!pp) {
        jw.kv_bool("found", false);
        jw.kv_int("requested_id", static_cast<long long>(player_id));
        jw.obj_close();
        return jw.str();
    }
    const Player& p = *pp;
    jw.kv_bool("found", true);

    // Ownership is decided by ROSTER MEMBERSHIP (collision-proof), NOT by the by-name
    // club lookup below: if the user's club name coincides with a rival's, a by-name
    // match points at the alphabetically-first org and reads the user's OWN players as
    // scoutable/biddable rivals with fogged stats. Matching player id is unambiguous.
    bool on_user_team = gm.player_on_user_team(p.id);
    // Resolve the player's club for color/chemistry. Force the user's team when the
    // player is ours so a name collision can't point `club` at the wrong org.
    TeamPtr club;
    if (on_user_team) {
        club = gm.user_team;
    } else {
        for (const auto& kv : gm.leagues) {
            if (!kv.second) continue;
            for (const auto& t : kv.second->teams()) {
                if (t && t->name == p.team_name) { club = t; break; }
            }
            if (club) break;
        }
    }
    if (club)
        jw.kv_str("club_color", hex_color(club->color_primary_r, club->color_primary_g, club->color_primary_b));
    else
        jw.kv_str("club_color", "#5a6cff");
    // User bankroll (read-only) — lets the sign form cap its signing-bonus slider
    // to what the club can actually afford, matching do_signfa's affordability gate.
    jw.kv_int("user_budget_k", gm.user_team ? static_cast<long long>(gm.user_team->budget / 1000) : 0);

    jw.key("player");
    jw.obj_open();

    // ---- identity
    jw.kv_int("id",              static_cast<long long>(p.id));
    jw.kv_str("name",            p.name);
    jw.kv_str("first",           p.first);
    jw.kv_str("last",            p.last);
    jw.kv_str("country",         p.country);
    jw.kv_str("country_iso",     p.country_iso);
    jw.kv_str("birth_city",      p.birth_city);
    jw.kv_int("birth_year",      p.birth_year);
    jw.kv_int("age",             p.age);
    jw.kv_str("role",            role_name(p.primary_role));
    jw.kv_bool("flex",           p.is_flex);
    jw.kv_bool("igl",            p.is_igl);
    jw.kv_bool("retired",        p.is_retired);
    jw.kv_str("archetype",       archetype_name(p.archetype));
    jw.kv_str("team_name",       p.team_name);
    jw.kv_bool("on_user_team",   on_user_team);
    jw.kv_bool("editable",       on_user_team);   // god-mode verbs only touch the user roster
    jw.kv_str("signature_agent", p.signature_agent());
    jw.kv_int("work_ethic",      p.work_ethic);
    jw.kv_int("consistency",     p.consistency);
    jw.kv_str("desire",          desire_name(p.desire));
    jw.kv_str("desire_blurb",    desire_blurb(p.desire));
    jw.kv_str("rep_tier",        rep_tier_label(p.reputation));   // FM-style stature band (raw rep hidden)

    jw.key("contract");
    jw.obj_open();
    jw.kv_int("salary_k",   p.contract.amount_k);
    jw.kv_int("exp_year",   p.contract.exp_year);
    jw.kv_int("years_left", p.years_left(gm.year));
    jw.obj_close();

    // ---- OVR / potential (fog-of-war band for unscouted non-user players)
    jw.kv_int("ovr", static_cast<long long>(std::lround(p.ovr())));
    bool pot_known = p.potential_scouted || on_user_team;
    jw.kv_bool("potential_known",   pot_known);
    jw.kv_bool("potential_scouted", p.potential_scouted);
    if (pot_known) {
        jw.kv_int("potential", p.potential);
    } else {
        int lo = 0, hi = 0;
        gm.scouted_band(p, lo, hi);
        jw.kv_int("potential_lo", lo);
        jw.kv_int("potential_hi", hi);
    }

    // ---- header action flags
    jw.kv_bool("favorited",     gm.is_favorited(pp));
    jw.kv_bool("watched",       gm.is_watched(pp));
    bool scoutable = (!on_user_team && !p.potential_scouted &&
                      p.team_name != "Retired" && gm.scout_credits() > 0);
    jw.kv_bool("scoutable",     scoutable);
    jw.kv_int("scout_credits",  gm.scout_credits());
    jw.kv_bool("can_queue",     p.team_name != "Retired");

    // ---- per-role fit — grounds the sign / re-sign role picker: how well this
    // player projects in each of the four roles. role_fit_score / role_fit_verdict
    // are const (no rng / no mutation), so this stays read-only + dynasty-neutral.
    jw.key("role_fits");
    jw.arr_open();
    for (int ri = 0; ri < 4; ++ri) {
        Role r = static_cast<Role>(ri);
        jw.obj_open();
        jw.kv_int("idx",      ri);
        jw.kv_str("name",     role_name(r));
        jw.kv_double("fit",   p.role_fit_score(r), 2);
        jw.kv_str("verdict",  p.role_fit_verdict(r));
        jw.kv_bool("natural", r == p.primary_role);
        jw.obj_close();
    }
    jw.arr_close();

    // ---- transfer target context (read-only) — a rival's contracted player can
    // be bid on; surface the market value so the bid slider has a grounded anchor.
    bool biddable = (club && !on_user_team && !p.is_retired && p.team_name != "Free Agent");
    jw.kv_bool("biddable", biddable);
    if (biddable) {
        jw.kv_int("transfer_value_k", static_cast<long long>(p.get_transfer_value() / 1000));
        jw.kv_str("seller_team", p.team_name);
    }

    // ---- mood (active-team players only)
    if (club && !p.is_retired) {
        Player::MoodBreakdown mb = p.mood_breakdown(*club, gm.year);
        jw.key("mood");
        jw.obj_open();
        jw.kv_int("total",   mb.total);
        jw.kv_str("verdict", mb.verdict);
        jw.kv_bool("transfer_requested", p.transfer_requested);
        jw.key("labels");
        jw.arr_open();
        for (const auto& lab : mb.labels) {
            jw.obj_open();
            jw.kv_str("label", lab.first);
            jw.kv_int("delta", lab.second);
            jw.obj_close();
        }
        jw.arr_close();
        const Player* best = nullptr;
        double best_chem = -1e9;
        for (const auto& tm : club->roster) {
            if (!tm || tm.get() == &p) continue;
            double c = club->chemistry_between(p, *tm);
            if (c > best_chem) { best_chem = c; best = tm.get(); }
        }
        if (best) {
            jw.key("best_duo");
            jw.obj_open();
            jw.kv_str("name", best->name);
            jw.kv_double("value", best_chem, 2);
            jw.obj_close();
        } else {
            jw.kv_raw("best_duo", "null");
        }
        jw.obj_close();
    } else {
        jw.kv_raw("mood", "null");
    }

    // ---- attributes (same shape as the roster export so the web shares its renderer)
    jw.key("attributes");
    jw.arr_open();
    for (std::size_t i = 0; i < kAttrCount; ++i) {
        const char* an = attr_name(static_cast<Attr>(i));
        if (!an || !an[0] || an[0] == '?') continue;
        jw.obj_open();
        jw.kv_str("name",  an);
        jw.kv_int("value", p.attributes[i]);
        jw.obj_close();
    }
    jw.arr_close();

    // ---- ranked + career
    jw.key("solo");
    jw.obj_open();
    jw.kv_int("mmr",      p.solo_mmr);
    jw.kv_int("peak_mmr", p.peak_mmr);
    jw.kv_str("rank",     p.rank_name());
    jw.kv_double("kd",    p.solo_kd(), 2);
    jw.kv_int("wins",     p.solo_wins);
    jw.kv_int("losses",   p.solo_losses);
    jw.obj_close();

    jw.key("career");
    jw.obj_open();
    jw.kv_int("matches",    p.career_matches);
    jw.kv_int("kills",      p.career_kills);
    jw.kv_int("deaths",     p.career_deaths);
    jw.kv_int("assists",    p.career_assists);
    jw.kv_double("kd",         p.kd_ratio(), 2);
    jw.kv_double("avg_rating", p.avg_match_rating(), 2);
    jw.kv_double("kast_pct",   p.career_kast_pct(), 0);
    jw.kv_double("adr",        p.career_adr(), 0);
    jw.kv_double("hs_pct",     p.career_hs_pct(), 0);
    jw.kv_int("mvps",       p.career_mvps);
    jw.obj_close();

    // ---- per-tournament stat lines (Player::tourn_stats — already accumulated by
    // Match.cpp under collapsed identities: a split's STAGE league + REGIONALS share
    // one "<region> Split N" bucket; each Masters and Champions is its own). Emitted
    // newest-year first so the UI's tournament view leads with the current season.
    {
        std::vector<std::pair<std::string, const Player::TournStatLine*>> tlines;
        tlines.reserve(p.tourn_stats.size());
        for (const auto& kv : p.tourn_stats)
            if (kv.second.maps > 0) tlines.emplace_back(kv.first, &kv.second);
        auto key_year = [](const std::string& k) {
            std::size_t bar = k.rfind('|');
            return (bar == std::string::npos) ? 0 : std::atoi(k.c_str() + bar + 1);
        };
        auto key_label = [](const std::string& k) {
            std::size_t bar = k.rfind('|');
            return (bar == std::string::npos) ? k : k.substr(0, bar);
        };
        std::sort(tlines.begin(), tlines.end(),
                  [&](const auto& a, const auto& b) {
                      int ya = key_year(a.first), yb = key_year(b.first);
                      if (ya != yb) return ya > yb;
                      return key_label(a.first) < key_label(b.first);
                  });
        jw.key("tournament_stats");
        jw.arr_open();
        for (const auto& [key, tsp] : tlines) {
            const Player::TournStatLine& ts = *tsp;
            jw.obj_open();
            jw.kv_str("label",   key_label(key));
            jw.kv_int("year",    key_year(key));
            jw.kv_int("maps",    ts.maps);
            jw.kv_int("rounds",  ts.rounds);
            jw.kv_int("kills",   ts.kills);
            jw.kv_int("deaths",  ts.deaths);
            jw.kv_int("assists", ts.assists);
            jw.kv_double("kd",     ts.deaths > 0 ? static_cast<double>(ts.kills) / ts.deaths
                                                 : static_cast<double>(ts.kills), 2);
            jw.kv_double("rating", ts.maps > 0 ? ts.rating_total / ts.maps : 0.0, 2);
            jw.kv_double("adr",    ts.rounds > 0 ? static_cast<double>(ts.damage) / ts.rounds : 0.0, 0);
            jw.kv_double("kast_pct", ts.rounds > 0 ? 100.0 * ts.rounds_with_kast / ts.rounds : 0.0, 0);
            jw.kv_double("hs_pct",   ts.kills > 0 ? 100.0 * ts.hs_hits / ts.kills : 0.0, 0);
            jw.kv_int("fb", ts.fb);
            jw.kv_int("fd", ts.fd);
            jw.obj_close();
        }
        jw.arr_close();
    }

    // ---- agent pool (enriched with per-agent mastery + signature flag)
    jw.kv_str("signature_agent_name", p.signature_agent());
    jw.key("agent_pool");
    jw.arr_open();
    {
        const std::string sig = p.signature_agent();
        for (const Agent* a : p.agent_pool) {
            if (!a) continue;
            jw.obj_open();
            jw.kv_str("name", a->name);
            jw.kv_str("role", role_name(a->role));
            jw.kv_bool("signature", a->name == sig);
            jw.kv_double("mastery_bonus", p.mastery_bonus_for(a->name), 1);
            auto mit = p.agent_mastery.find(a->name);
            if (mit != p.agent_mastery.end()) {
                jw.kv_int("matches",       mit->second.matches);
                jw.kv_double("avg_rating", mit->second.avg_rating, 2);
                jw.kv_double("peak_rating", mit->second.peak_rating, 2);
            } else {
                jw.kv_int("matches", 0);
            }
            jw.obj_close();
        }
    }
    jw.arr_close();

    // ---- map mastery
    jw.kv_str("comfort_map", p.comfort_map());
    jw.key("maps");
    jw.arr_open();
    for (const auto& mm : p.map_mastery) {
        jw.obj_open();
        jw.kv_str("map",         mm.first);
        jw.kv_int("matches",     mm.second.matches);
        jw.kv_double("avg_rating",  mm.second.avg_rating, 2);
        jw.kv_double("peak_rating", mm.second.peak_rating, 2);
        jw.kv_double("bonus",    p.map_mastery_bonus(mm.first), 1);
        jw.obj_close();
    }
    jw.arr_close();

    // ---- IGL profile (only meaningful for an IGL)
    if (p.is_igl) {
        jw.key("igl_profile");
        jw.obj_open();
        jw.kv_int("tend_aggressive", p.tend_play_aggressive);
        jw.kv_int("tend_lurk",       p.tend_lurk_vs_execute);
        jw.kv_int("tend_vocal",      p.tend_vocal);
        jw.kv_int("tend_adaptive",   p.tend_adaptive);
        jw.kv_double("impact_season",      p.igl_impact_season, 1);
        jw.kv_double("impact_season_peak", p.igl_impact_season_peak, 1);
        jw.kv_double("impact_total",       p.igl_impact_total, 1);
        jw.kv_int("match_count",     p.igl_match_count);
        jw.key("attrs");
        jw.obj_open();
        jw.kv_int("leadership",    p.attributes[static_cast<std::size_t>(Attr::Leadership)]);
        jw.kv_int("communication", p.attributes[static_cast<std::size_t>(Attr::Communication)]);
        jw.kv_int("midround",      p.attributes[static_cast<std::size_t>(Attr::MidRoundCalling)]);
        jw.kv_int("economy",       p.attributes[static_cast<std::size_t>(Attr::EconomyMgmt)]);
        jw.kv_int("antistrat",     p.attributes[static_cast<std::size_t>(Attr::AntiStrat)]);
        jw.kv_int("gamesense",     p.attributes[static_cast<std::size_t>(Attr::GameSense)]);
        jw.kv_int("decision",      p.attributes[static_cast<std::size_t>(Attr::DecisionMaking)]);
        jw.kv_int("intelligence",  p.attributes[static_cast<std::size_t>(Attr::Intelligence)]);
        jw.kv_int("adaptability",  p.attributes[static_cast<std::size_t>(Attr::Adaptability)]);
        jw.obj_close();
        jw.obj_close();
    } else {
        jw.kv_raw("igl_profile", "null");
    }

    // ---- badges (+ catalogue for the god-mode picker; edits are user-only)
    jw.key("badges");
    jw.arr_open();
    for (const std::string& b : p.badges) jw.arr_str(b);
    jw.arr_close();
    jw.key("all_badges");
    jw.arr_open();
    for (const Badge& b : badges()) jw.arr_str(b.name);
    jw.arr_close();

    // ---- accolades
    Player::TrophySummary ts = p.trophy_summary();
    jw.key("accolades");
    jw.obj_open();
    jw.kv_int("regional",    ts.regional);
    jw.kv_int("masters",     ts.masters);
    jw.kv_int("worlds",      ts.worlds);
    jw.kv_int("role_awards", ts.role_awards);
    jw.kv_int("mvps",        ts.mvps);
    jw.kv_int("igl_oty",     ts.igl_oty);
    jw.kv_int("total",       ts.total_trophies());
    jw.key("titles");
    jw.arr_open();
    for (const std::string& t : ts.all_titles) jw.arr_str(t);
    jw.arr_close();
    jw.obj_close();

    // ---- season history (career arc)
    jw.key("history");
    jw.arr_open();
    for (const MatchHistoryEntry& h : p.history) {
        jw.obj_open();
        jw.kv_int("year",      h.year);
        jw.kv_int("age",       h.age);
        jw.kv_str("team",      h.team);
        jw.kv_double("rating", h.rating, 2);
        jw.kv_double("kd",     h.kd, 2);
        jw.kv_double("kast",   h.kast, 0);
        jw.kv_str("placement", h.placement);
        jw.kv_int("salary_k",  h.salary_k);
        jw.kv_int("matches",   h.matches);
        jw.obj_close();
    }
    jw.arr_close();

    jw.obj_close();   // player
    jw.obj_close();   // root
    return jw.str();
}

// =============================================================
// Favorites list — the user's bookmarked players.
// =============================================================
namespace {
void emit_player_card(JsonWriter& jw, const Player& p) {
    jw.kv_int("id",          static_cast<long long>(p.id));
    jw.kv_str("name",        p.name);
    jw.kv_str("country_iso", p.country_iso);
    jw.kv_str("role",        role_name(p.primary_role));
    jw.kv_int("age",         p.age);
    jw.kv_int("ovr",         static_cast<long long>(std::lround(p.ovr())));
    jw.kv_str("team_name",   p.team_name);
    jw.kv_int("career_matches", p.career_matches);
    jw.kv_double("avg_rating",  p.avg_match_rating(), 2);
    jw.kv_int("mmr",         p.solo_mmr);
    jw.kv_bool("igl",        p.is_igl);
}
}  // namespace

std::string export_favorites_to_json(const GameManager& gm) {
    JsonWriter jw;
    jw.obj_open();
    jw.kv_int("schema_version", 1);
    jw.kv_str("exporter", "VLR Manager Valosim");
    if (gm.user_team)
        jw.kv_str("club_color", hex_color(gm.user_team->color_primary_r,
                                          gm.user_team->color_primary_g,
                                          gm.user_team->color_primary_b));
    else
        jw.kv_str("club_color", "#5a6cff");

    jw.key("favorites");
    jw.obj_open();
    jw.kv_int("count", static_cast<long long>(gm.favorite_players.size()));
    jw.key("players");
    jw.arr_open();
    for (const auto& pp : gm.favorite_players) {
        if (!pp) continue;
        jw.obj_open();
        emit_player_card(jw, *pp);
        jw.obj_close();
    }
    jw.arr_close();
    jw.obj_close();   // favorites
    jw.obj_close();   // root
    return jw.str();
}

// =============================================================
// Watchlist — the user's transfer/scouting targets (status + note).
// =============================================================
std::string export_watchlist_to_json(const GameManager& gm) {
    JsonWriter jw;
    jw.obj_open();
    jw.kv_int("schema_version", 1);
    jw.kv_str("exporter", "VLR Manager Valosim");
    if (gm.user_team)
        jw.kv_str("club_color", hex_color(gm.user_team->color_primary_r,
                                          gm.user_team->color_primary_g,
                                          gm.user_team->color_primary_b));
    else
        jw.kv_str("club_color", "#5a6cff");

    jw.key("watchlist");
    jw.obj_open();
    const auto& wl = gm.watchlist();
    jw.kv_int("count", static_cast<long long>(wl.size()));
    jw.key("entries");
    jw.arr_open();
    for (const auto& e : wl) {
        if (!e.player) continue;
        jw.obj_open();
        emit_player_card(jw, *e.player);
        jw.kv_int("status",      static_cast<long long>(e.status));   // 0 Watching / 1 Scouting / 2 Bidding
        jw.kv_str("status_name", e.status == GameManager::WatchStatus::Scouting ? "Scouting"
                               : e.status == GameManager::WatchStatus::Bidding  ? "Bidding" : "Watching");
        jw.kv_str("note",        e.note);
        jw.obj_close();
    }
    jw.arr_close();
    jw.obj_close();   // watchlist
    jw.obj_close();   // root
    return jw.str();
}

// =============================================================
// Re-sign negotiation PREVIEW (read-only; nothing commits here).
// =============================================================
std::string export_resign_preview_to_json(const GameManager& gm, std::uint64_t player_id,
                                          int years, int amount_k, int bonus_k,
                                          bool promise_starter, int role_idx) {
    JsonWriter jw;
    jw.obj_open();
    jw.kv_int("schema_version", 1);
    jw.kv_str("exporter", "VLR Manager Valosim");

    // User roster (re-sign) OR a free agent (the FA sign form uses the same
    // negotiation math — evaluate_resign_offer handles the not-my-team path).
    PlayerPtr pp;
    if (gm.user_team) {
        for (const auto& p : gm.user_team->roster) if (p && p->id == player_id) { pp = p; break; }
        if (!pp) {
            for (const auto& kv : gm.solo_qs) {
                if (!kv.second) continue;
                for (const auto& p : kv.second->global_ladder())
                    if (p && p->id == player_id && p->team_name == "Free Agent") { pp = p; break; }
                if (pp) break;
            }
        }
    }
    if (!pp || !gm.user_team) {
        jw.kv_bool("found", false);
        jw.kv_int("requested_id", static_cast<long long>(player_id));
        jw.obj_close();
        return jw.str();
    }
    const Player& p = *pp;
    Team& t = *gm.user_team;
    Role offered = (role_idx >= 0 && role_idx < 4) ? static_cast<Role>(role_idx) : p.primary_role;

    Player::ResignOffer     ask = p.propose_resign_offer(t, gm.year);
    Player::ResignBreakdown bd  = p.evaluate_resign_offer(amount_k, years, t, offered,
                                                          bonus_k, promise_starter);

    jw.kv_bool("found", true);
    jw.kv_str("club_color", hex_color(t.color_primary_r, t.color_primary_g, t.color_primary_b));

    jw.key("player");
    jw.obj_open();
    jw.kv_int("id",         static_cast<long long>(p.id));
    jw.kv_str("name",       p.name);
    jw.kv_str("role",       role_name(p.primary_role));
    jw.kv_int("ovr",        static_cast<long long>(std::lround(p.ovr())));
    jw.kv_int("age",        p.age);
    jw.kv_int("cur_salary_k", p.contract.amount_k);
    jw.kv_int("cur_exp_year", p.contract.exp_year);
    jw.kv_int("years_left", p.years_left(gm.year));
    jw.kv_str("role_fit",   p.role_fit_verdict(offered));
    jw.obj_close();

    jw.key("ask");
    jw.obj_open();
    jw.kv_int("amount_k",             ask.amount_k);
    jw.kv_int("years",               ask.years);
    jw.kv_int("min_acceptable_k",     ask.min_acceptable_k);
    jw.kv_int("max_acceptable_years", ask.max_acceptable_years);
    jw.kv_double("willingness",       ask.willingness, 2);
    jw.kv_str("explainer",            ask.explainer);
    jw.obj_close();

    jw.key("offer");
    jw.obj_open();
    jw.kv_int("years",     years);
    jw.kv_int("amount_k",  amount_k);
    jw.kv_int("bonus_k",   bonus_k);
    jw.kv_bool("promise_starter", promise_starter);
    jw.kv_int("role_idx",  role_idx);
    jw.kv_str("offered_role", role_name(offered));
    jw.obj_close();

    jw.key("breakdown");
    jw.obj_open();
    jw.kv_int("total",         bd.total);
    jw.kv_bool("will_accept",  bd.will_accept);
    jw.kv_str("verdict",       bd.verdict);
    jw.kv_str("reject_reason", bd.reject_reason);
    jw.kv_int("ask_k",         bd.ask_k);
    jw.kv_bool("has_counter",  bd.has_counter);
    jw.kv_int("counter_amount_k", bd.counter_amount_k);
    jw.kv_int("counter_years",    bd.counter_years);
    jw.key("labels");
    jw.arr_open();
    for (const auto& lab : bd.labels) {
        jw.obj_open();
        jw.kv_str("label", lab.first);
        jw.kv_int("delta", lab.second);
        jw.obj_close();
    }
    jw.arr_close();
    jw.obj_close();

    jw.obj_close();   // root
    return jw.str();
}

// =============================================================
// Records hub — GOAT (career + season) + Hall of Fame + leaders.
// =============================================================
namespace {
void emit_record_player(JsonWriter& jw, const Player& p) {
    jw.kv_int("id",          static_cast<long long>(p.id));
    jw.kv_str("name",        p.name);
    jw.kv_str("country_iso", p.country_iso);
    jw.kv_str("role",        role_name(p.primary_role));
    jw.kv_str("team_name",   p.team_name);
    jw.kv_bool("igl",        p.is_igl);
}
}  // namespace

std::string export_records_to_json(const GameManager& gm) {
    // Every distinct player in the world (all region ladders + the HoF).
    std::vector<PlayerPtr> all;
    {
        std::unordered_set<std::uint64_t> seen;
        for (const auto& kv : gm.solo_qs) {
            if (!kv.second) continue;
            for (const auto& p : kv.second->global_ladder())
                if (p && seen.insert(p->id).second) all.push_back(p);
        }
        for (const auto& p : gm.hall_of_fame)
            if (p && seen.insert(p->id).second) all.push_back(p);
    }

    JsonWriter jw;
    jw.obj_open();
    jw.kv_int("schema_version", 1);
    jw.kv_str("exporter", "VLR Manager Valosim");
    if (gm.user_team)
        jw.kv_str("club_color", hex_color(gm.user_team->color_primary_r,
                                          gm.user_team->color_primary_g,
                                          gm.user_team->color_primary_b));
    else
        jw.kv_str("club_color", "#5a6cff");

    jw.key("records");
    jw.obj_open();
    jw.kv_int("player_pool", static_cast<long long>(all.size()));

    GoatWeights w;   // canonical default GOAT weights

    jw.key("goat_career");
    jw.arr_open();
    {
        std::vector<GoatRow> rows = compute_goat_career(all, w, 50);
        int rank = 0;
        for (const auto& r : rows) {
            if (!r.player) continue;
            jw.obj_open();
            jw.kv_int("rank", ++rank);
            emit_record_player(jw, *r.player);
            jw.kv_double("score", r.score, 1);
            jw.obj_close();
        }
    }
    jw.arr_close();

    jw.key("goat_season");
    jw.arr_open();
    {
        std::vector<GoatRow> rows = compute_goat_season(all, w, 50);
        int rank = 0;
        for (const auto& r : rows) {
            if (!r.player) continue;
            jw.obj_open();
            jw.kv_int("rank", ++rank);
            emit_record_player(jw, *r.player);
            jw.kv_int("season",        r.season);
            jw.kv_str("season_team",   r.season_team);
            jw.kv_double("season_rating", r.season_rating, 2);
            jw.kv_double("season_kd",     r.season_kd, 2);
            jw.kv_double("score",      r.score, 1);
            jw.obj_close();
        }
    }
    jw.arr_close();

    jw.key("hall_of_fame");
    jw.arr_open();
    for (const auto& pp : gm.hall_of_fame) {
        if (!pp) continue;
        const Player& p = *pp;
        jw.obj_open();
        emit_record_player(jw, p);
        jw.kv_int("career_matches", p.career_matches);
        jw.kv_double("avg_rating",  p.avg_match_rating(), 2);
        jw.kv_int("trophies",       p.trophy_summary().total_trophies());
        jw.kv_int("mvps",           p.career_mvps);
        jw.kv_bool("won_mvp",        p.ever_won_mvp());
        jw.kv_bool("won_role_award", p.ever_won_role_award());
        jw.kv_int("gf_clutches",    p.career_grand_final_clutches);
        jw.obj_close();
    }
    jw.arr_close();

    // League-leader boards (top 15 each; min-match gate where rate stats).
    auto leader_board = [&](const char* key, const char* label,
                            const std::function<double(const Player&)>& metric,
                            int min_matches, bool as_int) {
        std::vector<const Player*> v;
        for (const auto& pp : all)
            if (pp && pp->career_matches >= min_matches) v.push_back(pp.get());
        std::sort(v.begin(), v.end(),
                  [&](const Player* a, const Player* b) { return metric(*a) > metric(*b); });
        jw.key(key);
        jw.obj_open();
        jw.kv_str("label", label);
        jw.key("entries");
        jw.arr_open();
        int n = 0;
        for (const Player* p : v) {
            if (n >= 15) break;
            ++n;
            jw.obj_open();
            emit_record_player(jw, *p);
            if (as_int) jw.kv_int("value", static_cast<long long>(std::llround(metric(*p))));
            else        jw.kv_double("value", metric(*p), 2);
            jw.obj_close();
        }
        jw.arr_close();
        jw.obj_close();
    };

    jw.key("leaders");
    jw.obj_open();
    leader_board("rating",   "Career Rating (min 20)", [](const Player& p) { return p.avg_match_rating(); }, 20, false);
    leader_board("kills",    "Career Kills",           [](const Player& p) { return static_cast<double>(p.career_kills); }, 1, true);
    leader_board("mvps",     "Career MVPs",            [](const Player& p) { return static_cast<double>(p.career_mvps); }, 1, true);
    leader_board("trophies", "Trophies",               [](const Player& p) { return static_cast<double>(p.trophy_summary().total_trophies()); }, 1, true);
    leader_board("adr",      "Career ADR (min 20)",    [](const Player& p) { return p.career_adr(); }, 20, false);
    leader_board("kast",     "Career KAST% (min 20)",  [](const Player& p) { return p.career_kast_pct(); }, 20, false);
    jw.obj_close();   // leaders

    jw.obj_close();   // records
    jw.obj_close();   // root
    return jw.str();
}

// =============================================================
// Staff Profile modal — ANY coach/scout/analyst by stable id (employed on any
// team in any tier, in a free-agent pool, or retired). OUTPUT-ONLY.
// =============================================================
std::string export_staff_profile_to_json(const GameManager& gm, std::uint64_t staff_id) {
    // Resolve the staffer across every place staff live. Employed staff are found
    // via league_at over ALL tiers (a Challengers coach is clickable too).
    CoachPtr coach; ScoutPtr scout; AnalystPtr analyst;
    static const char* kAllRegions[] = {"Americas", "EMEA", "Pacific"};
    for (const char* rg : kAllRegions) {
        int tc = gm.tier_count(rg);
        for (int ti = 1; ti <= tc && !coach && !scout && !analyst; ++ti) {
            auto lg = gm.league_at(rg, ti);
            if (!lg) continue;
            for (const auto& t : lg->teams()) {
                if (!t) continue;
                if (t->head_coach   && t->head_coach->id   == staff_id) { coach   = t->head_coach;   break; }
                if (t->head_scout   && t->head_scout->id   == staff_id) { scout   = t->head_scout;   break; }
                if (t->head_analyst && t->head_analyst->id == staff_id) { analyst = t->head_analyst; break; }
            }
        }
        if (coach || scout || analyst) break;
    }
    if (!coach && !scout && !analyst) {
        for (const auto& c : gm.free_coaches_)    if (c && c->id == staff_id) { coach = c; break; }
        for (const auto& s : gm.free_scouts_)     if (s && s->id == staff_id) { scout = s; break; }
        for (const auto& a : gm.free_analysts_)   if (a && a->id == staff_id) { analyst = a; break; }
        for (const auto& c : gm.retired_coaches_) if (c && c->id == staff_id) { coach = c; break; }
        for (const auto& s : gm.retired_scouts_)  if (s && s->id == staff_id) { scout = s; break; }
        for (const auto& a : gm.retired_analysts_) if (a && a->id == staff_id) { analyst = a; break; }
    }

    JsonWriter jw;
    jw.obj_open();
    jw.kv_int("schema_version", 1);
    jw.kv_str("exporter", "VLR Manager Valosim");
    if (!coach && !scout && !analyst) {
        jw.kv_bool("found", false);
        jw.kv_int("requested_id", static_cast<long long>(staff_id));
        jw.obj_close();
        return jw.str();
    }
    jw.kv_bool("found", true);
    if (gm.user_team)
        jw.kv_str("club_color", hex_color(gm.user_team->color_primary_r,
                                          gm.user_team->color_primary_g,
                                          gm.user_team->color_primary_b));
    else jw.kv_str("club_color", "#5a6cff");

    jw.key("staff");
    jw.obj_open();
    // Shared identity + contract shape, then role-specific attribute bars. The UI
    // renders `attrs` generically so all three roles share one modal.
    auto emit_common = [&](const char* role, std::uint64_t id, const std::string& name,
                           const std::string& country, const std::string& iso,
                           const std::string& region, int age, bool retired,
                           const std::string& team, const std::string& personality,
                           int reputation, int salary_k, int ask_k,
                           int contract_exp_year, int career_seasons) {
        jw.kv_str("role",        role);
        jw.kv_int("id",          static_cast<long long>(id));
        jw.kv_str("name",        name);
        jw.kv_str("country",     country);
        jw.kv_str("country_iso", iso);
        jw.kv_str("region",      region);
        jw.kv_int("age",         age);
        jw.kv_bool("retired",    retired);
        jw.kv_str("team_name",   team);
        bool is_fa = (team == "Free Agent") && !retired;
        jw.kv_bool("is_free_agent", is_fa);
        // Ownership by IDENTITY, not team-name string — a stale/renamed team_name
        // (e.g. staff kept their procedural club name through the wizard rename)
        // must not block god-mode editing of your own staff.
        bool on_user = gm.user_team &&
            ((gm.user_team->head_coach   && gm.user_team->head_coach->id   == id) ||
             (gm.user_team->head_scout   && gm.user_team->head_scout->id   == id) ||
             (gm.user_team->head_analyst && gm.user_team->head_analyst->id == id));
        jw.kv_bool("on_user_team", on_user);
        jw.kv_str("personality", personality);
        jw.kv_int("reputation",  reputation);
        jw.kv_int("salary_k",    salary_k);
        jw.kv_int("ask_k",       ask_k);
        jw.kv_int("contract_exp_year", contract_exp_year);
        if (contract_exp_year > 0 && !is_fa)
            jw.kv_int("years_left", std::max(0, contract_exp_year - gm.year + 1));
        jw.kv_int("career_seasons", career_seasons);
    };
    auto emit_attr = [&](const char* nm, const char* key, int v) {
        jw.obj_open(); jw.kv_str("name", nm); jw.kv_str("key", key); jw.kv_int("value", v); jw.obj_close();
    };

    if (coach) {
        const Coach& c = *coach;
        emit_common("coach", c.id, c.name, c.country, c.country_iso, c.region, c.age,
                    c.is_retired, c.team_name, coach_personality_name(c.personality),
                    c.reputation, c.salary_k, c.requested_salary_k(),
                    c.contract_exp_year, c.career_seasons);
        jw.kv_int("career_titles", c.career_titles);
        jw.kv_int("career_coty",   c.career_coty);
        jw.key("attrs"); jw.arr_open();
        emit_attr("Tactical",    "tactical",    c.tactical);
        emit_attr("Development", "development", c.development);
        emit_attr("Leadership",  "leadership",  c.leadership);
        emit_attr("Experience",  "experience",  c.experience);
        jw.arr_close();
    } else if (scout) {
        const Scout& s = *scout;
        emit_common("scout", s.id, s.name, s.country, s.country_iso, s.region, s.age,
                    s.is_retired, s.team_name, scout_personality_name(s.personality),
                    s.reputation, s.salary_k, s.requested_salary_k(),
                    s.contract_exp_year, s.career_seasons);
        jw.kv_int("career_finds", s.career_finds);
        jw.kv_int("quality",      static_cast<int>(std::lround(s.quality01() * 99.0)));
        jw.key("attrs"); jw.arr_open();
        emit_attr("Judgement",  "judgement",  s.judgement);
        emit_attr("Network",    "network",    s.network);
        emit_attr("Projection", "projection", s.projection);
        emit_attr("Experience", "experience", s.experience);
        jw.arr_close();
    } else {
        const Analyst& a = *analyst;
        emit_common("analyst", a.id, a.name, a.country, a.country_iso, a.region, a.age,
                    a.is_retired, a.team_name, analyst_personality_name(a.personality),
                    a.reputation, a.salary_k, a.requested_salary_k(),
                    a.contract_exp_year, a.career_seasons);
        jw.kv_int("career_reports", a.career_reports);
        jw.kv_int("quality",        static_cast<int>(std::lround(a.quality01() * 99.0)));
        jw.kv_int("report_depth",   a.report_depth_sections());
        jw.key("attrs"); jw.arr_open();
        emit_attr("Tactical Read", "tactical_read",    a.tactical_read);
        emit_attr("Opp. Insight",  "opponent_insight", a.opponent_insight);
        emit_attr("Prep",          "prep",             a.prep);
        emit_attr("Experience",    "experience",       a.experience);
        jw.arr_close();
    }

    jw.obj_close();   // staff
    jw.obj_close();   // root
    return jw.str();
}

// =============================================================
// Scout REPORT (FM-style strengths/weaknesses, accuracy-scaled) + Scouting screen.
// =============================================================
namespace {
const char* attr_category(Attr a) {
    switch (a) {
        case Attr::Aim: case Attr::Headshot: case Attr::Reaction:
        case Attr::CrosshairPlacement: case Attr::Awping: case Attr::SpikeHandle:
        case Attr::Movement:            return "Firepower";
        case Attr::GameSense: case Attr::DecisionMaking: case Attr::Intelligence:
        case Attr::Positioning: case Attr::MidRoundCalling: case Attr::AntiStrat:
                                        return "Game Sense";
        case Attr::Utility: case Attr::EconomyMgmt: case Attr::Communication:
                                        return "Utility & Support";
        case Attr::Clutch: case Attr::Anchor: case Attr::Stamina: case Attr::Adaptability:
                                        return "Mentality";
        case Attr::Entry: case Attr::Aggressiveness: case Attr::Lurking:
                                        return "Aggression";
        case Attr::Leadership:          return "Leadership";
        default:                        return "General";
    }
}
}  // namespace

std::string export_scout_report_to_json(const GameManager& gm, std::uint64_t player_id) {
    JsonWriter jw;
    jw.obj_open();
    jw.kv_int("schema_version", 1);
    jw.kv_str("exporter", "VLR Manager Valosim");
    PlayerPtr pp = gm.find_player_by_id(player_id);
    if (!pp) { jw.kv_bool("found", false); jw.obj_close(); return jw.str(); }
    const Player& p = *pp;
    jw.kv_bool("found", true);
    if (gm.user_team)
        jw.kv_str("club_color", hex_color(gm.user_team->color_primary_r, gm.user_team->color_primary_g, gm.user_team->color_primary_b));
    else jw.kv_str("club_color", "#5a6cff");

    const GameManager::ScoutAssignment* active = gm.active_scout_assignment(player_id);
    int accuracy = gm.scout_report_accuracy(player_id);
    // Your own players need no scouting — you know them completely. Show a full,
    // 100%-accuracy report for FREE and instantly (no commission, no waiting).
    bool own_player = gm.player_on_user_team(player_id);
    if (own_player) accuracy = 100;

    jw.key("scout");
    jw.obj_open();
    jw.kv_bool("own_player", own_player);
    jw.key("player");
    jw.obj_open();
    jw.kv_int("id",   static_cast<long long>(p.id));
    jw.kv_str("name", p.name);
    jw.kv_str("role", role_name(p.primary_role));
    jw.kv_int("ovr",  static_cast<long long>(std::lround(p.ovr())));
    jw.kv_int("age",  p.age);
    jw.kv_str("country_iso", p.country_iso);
    jw.kv_str("team_name",   p.team_name);
    jw.kv_bool("is_free_agent", p.team_name == "Free Agent");
    jw.obj_close();

    jw.kv_str("status", active ? "in_progress" : (accuracy > 0 ? "scouted" : "unscouted"));

    // What the user's own scout could do on THIS player — coverage of their country
    // + the duration they'd take (mirrors GameManager::commission_scout). Grounds
    // the commission form: you SEE why a Korean prospect is a slow, vague read for
    // an Americas specialist. Always present.
    {
        const Scout* usc = (gm.user_team && gm.user_team->head_scout) ? gm.user_team->head_scout.get() : nullptr;
        int cov = usc ? scout_country_coverage(*usc, p.country_iso) : 8;
        int dep = usc ? scout_read_depth(*usc) : 25;
        auto est = [&](int money) {
            int d = static_cast<int>(21.0 - cov * 0.10 - money * 0.06 + 0.5);
            if (d < 7) d = 7; if (d > 21) d = 21; if (cov < 25) d = std::max(d, 17);
            return d;
        };
        jw.key("scout_preview");
        jw.obj_open();
        jw.kv_bool("has_scout", usc != nullptr);
        jw.kv_int("coverage",   cov);
        jw.kv_int("read_depth", dep);
        jw.kv_int("est_days_min_money", est(0));
        jw.kv_int("est_days_max_money", est(200));
        jw.kv_str("region_note", cov >= 70 ? "Home turf \xE2\x80\x94 a thorough read"
                               : cov >= 40 ? "Familiar ground"
                               : cov >= 15 ? "Limited local knowledge"
                               : "Out of their network \xE2\x80\x94 slow, costly, vague");
        jw.obj_close();
    }

    if (active) {
        jw.key("assignment");
        jw.obj_open();
        jw.kv_int("days_remaining", active->days_remaining);
        jw.kv_int("total_days",     active->total_days);
        int pct = active->total_days > 0
                    ? static_cast<int>((active->total_days - active->days_remaining) * 100.0 / active->total_days)
                    : 0;
        jw.kv_int("pct",      pct);
        jw.kv_int("coverage", active->coverage);
        jw.kv_int("money_k",  active->money_k);
        jw.obj_close();
    }

    if (accuracy > 0) {
        jw.kv_int("accuracy", accuracy);
        jw.kv_str("confidence", accuracy >= 80 ? "Thorough" : accuracy >= 55 ? "Solid"
                              : accuracy >= 30 ? "Partial"  : "Vague");
        bool show_values = accuracy >= 55;
        int n = 2 + accuracy / 25;   // 2..6 items each

        struct AV { int idx; int val; };
        std::vector<AV> avs;
        for (std::size_t i = 0; i < kAttrCount; ++i) {
            const char* an = attr_name(static_cast<Attr>(i));
            if (!an || !an[0] || an[0] == '?') continue;
            avs.push_back({static_cast<int>(i), p.attributes[i]});
        }
        std::sort(avs.begin(), avs.end(), [](const AV& a, const AV& b) { return a.val > b.val; });

        // Strengths/weaknesses are RELATIVE to the player's own level, not just
        // absolute bands (2026-06-28): a 70-OVR star with every attr 55+ used to
        // read "no glaring weaknesses", which is never true — scout a world-class
        // player and a good scout still names his softest spots. An item makes the
        // list if it clears the absolute band OR deviates from the player's own
        // mean; the top/bottom TWO always surface (every profile has a best and a
        // worst) — the deviation gate only prunes the tail beyond that.
        double attr_mean = 0.0;
        for (const auto& av : avs) attr_mean += av.val;
        if (!avs.empty()) attr_mean /= static_cast<double>(avs.size());

        jw.key("strengths");
        jw.arr_open();
        { int c = 0; for (const auto& av : avs) {
            if (c >= n) break;
            if (c >= 2 && av.val < 68 && av.val < attr_mean + 9.0) break;
            jw.obj_open(); jw.kv_str("attr", attr_name(static_cast<Attr>(av.idx)));
            jw.kv_str("category", attr_category(static_cast<Attr>(av.idx)));
            jw.kv_bool("relative", av.val < 68);   // "for his level" (not elite in absolute terms)
            if (show_values) jw.kv_int("value", av.val); jw.obj_close(); ++c; } }
        jw.arr_close();

        jw.key("weaknesses");
        jw.arr_open();
        { int c = 0; for (auto it = avs.rbegin(); it != avs.rend(); ++it) {
            if (c >= n) break;
            if (c >= 2 && it->val > 52 && it->val > attr_mean - 9.0) break;
            jw.obj_open(); jw.kv_str("attr", attr_name(static_cast<Attr>(it->idx)));
            jw.kv_str("category", attr_category(static_cast<Attr>(it->idx)));
            jw.kv_bool("relative", it->val > 52);  // soft FOR HIM, not bad outright
            if (show_values) jw.kv_int("value", it->val); jw.obj_close(); ++c; } }
        jw.arr_close();

        jw.key("potential");
        // own_player: you know your player's ceiling exactly — no fog band.
        jw.obj_open();
        if (p.potential_scouted || own_player) { jw.kv_bool("known", true); jw.kv_int("value", p.potential); }
        else {
            int lo = 0, hi = 0; gm.scouted_band(p, lo, hi);
            int widen = (100 - accuracy) / 12;
            lo = std::max(1, lo - widen); hi = std::min(99, hi + widen);
            jw.kv_bool("known", false); jw.kv_int("lo", lo); jw.kv_int("hi", hi);
        }
        jw.obj_close();

        jw.key("role_fit");
        jw.arr_open();
        for (int r = 0; r < 4; ++r) {
            jw.obj_open();
            jw.kv_str("role", role_name(static_cast<Role>(r)));
            jw.kv_double("fit", p.role_fit_score(static_cast<Role>(r)), 2);
            jw.obj_close();
        }
        jw.arr_close();

        // Attribute-COMBINATION flavor — richer scouting "read" sentences derived
        // from specific stat combos (grounded in real attributes, gated by accuracy
        // so a weak scout surfaces fewer/vaguer notes). e.g. smart + decisive but
        // low AntiStrat => "makes the right reads but is prone to being counter-played".
        {
            auto A = [&](Attr a){ return p.attributes[static_cast<std::size_t>(a)]; };
            // Deterministic per-player PHRASING: hash the player id with a per-rule
            // salt to choose among several wordings of the same observation. Two
            // players with the same trait read differently; the same player's report
            // is stable across opens. No rng consumed — dynasty-safe.
            auto pick = [&](std::uint64_t salt,
                            std::initializer_list<const char*> v) -> std::string {
                std::uint64_t h = (p.id + 0x9E3779B97F4A7C15ULL) * 1099511628211ULL ^ (salt * 0xC2B2AE3D27D4EB4FULL);
                h ^= h >> 29; h *= 0xBF58476D1CE4E5B9ULL; h ^= h >> 32;
                auto it = v.begin();
                std::advance(it, static_cast<std::size_t>(h % v.size()));
                return std::string(*it);
            };
            std::vector<std::string> traits;
            const std::string rn  = role_name(p.primary_role);
            const std::string arch = archetype_name(p.archetype);
            const std::string sig  = p.signature_agent();
            char nb[320];

            // ---- PLAYER-TYPE identity read (always first, never truncated). Dominant
            // attribute cluster + tempo + the player's own archetype label — this is
            // "what kind of player he is" in one line, phrased per player.
            {
                int firepower = A(Attr::Aim) + A(Attr::Headshot) + A(Attr::Reaction) + A(Attr::CrosshairPlacement);
                int brain     = A(Attr::GameSense) + A(Attr::Intelligence) + A(Attr::DecisionMaking) + A(Attr::MidRoundCalling);
                int support   = A(Attr::Utility) + A(Attr::Communication) + A(Attr::Positioning) + A(Attr::Anchor);
                bool hot      = A(Attr::Aggressiveness) >= 66 || A(Attr::Entry) >= 70;
                std::string core;
                if (firepower >= brain && firepower >= support) {
                    core = hot
                        ? pick(11, {"a firepower-first %R who hunts the opening duel — the gun does the talking",
                                    "an aim-heavy %R who wants first contact and usually wins it",
                                    "a duel-seeking %R — point him at the fight and get out of the way"})
                        : pick(12, {"a mechanically-led %R who wins clean gunfights rather than forcing tempo",
                                    "a rifle-first %R — patient, precise, takes the fights on his terms",
                                    "a controlled shooter at %R whose value is the raw duel win-rate"});
                } else if (brain >= support) {
                    core = hot
                        ? pick(13, {"a thinking aggressor at %R — picks his moments, then commits hard",
                                    "a calculated risk-taker at %R who turns good reads into fast plays",
                                    "an instinct-plus-brains %R — the aggression is informed, not reckless"})
                        : pick(14, {"a cerebral, structured %R who beats you with reads and round-craft",
                                    "a chess-player at %R — wins with information and timing, not aim-duels",
                                    "a systems %R whose impact shows on the scoreboard a round late"});
                } else {
                    core = hot
                        ? pick(15, {"a team-first %R who creates space and feeds his stars",
                                    "an engine-room %R — utility, tempo and touches that never make the highlight reel",
                                    "a selfless %R who does the ugly work that wins rounds"})
                        : pick(16, {"a low-ego glue player at %R — positioning, utility and comms over highlights",
                                    "a stabilizer at %R — the calm structural piece every young core needs",
                                    "a quiet professional %R whose value is everything that doesn't get clipped"});
                }
                std::size_t rp = core.find("%R");
                if (rp != std::string::npos) core.replace(rp, 2, rn);
                std::string ident = "Profile: " + core + ". ";
                ident += pick(17, {"The scene shorthand fits: a classic \"", "Tape says \"", "Around the circuit he's the \""});
                ident += arch + pick(18, {"\".", "\" type.", "\" through and through."});
                traits.push_back(ident);
            }

            // ---- Signature-agent read (agent name makes it inherently player-specific)
            if (!sig.empty() && sig != "None") {
                std::snprintf(nb, sizeof(nb), "%s",
                    pick(21, {"Builds his game around %A — take that comfort pick away and you've solved half of him.",
                              "The %A is the identity: whole setups run through that agent's kit.",
                              "%A specialist — bans and comp pressure that force him off it pay off."}).c_str());
                std::string s(nb);
                std::size_t ap = s.find("%A");
                if (ap != std::string::npos) s.replace(ap, 2, sig);
                traits.push_back(s);
            }

            // ---- Combination reads (two/three-attribute interplay), each phrased per player
            if (A(Attr::Intelligence) >= 70 && A(Attr::DecisionMaking) >= 70) {
                if (A(Attr::AntiStrat) <= 48)
                    traits.push_back(pick(22, {"An intelligent player who makes the right reads in the moment — but weak anti-stratting leaves him prone to being counter-played.",
                                               "Sharp in-round brain, lazy between rounds: he solves what's in front of him but never studies the opponent, so prepared teams bait him.",
                                               "Live reads are elite, preparation isn't — a coach who scripts anti-strats around him fixes his one leak."}));
                else
                    traits.push_back(pick(23, {"A cerebral operator who makes the right read and comes prepared for what the opponent throws at him.",
                                               "Thinks the game at a caller's level — pre-round prep and mid-round adjustments both check out.",
                                               "The full mental package: studies tape, then out-decides you live."}));
            }
            if (A(Attr::Aim) >= 76 && A(Attr::Headshot) >= 72)
                traits.push_back(pick(24, {"Mechanically gifted with a clinical, headshot-heavy crosshair.",
                                           "One-taps for days — the first bullet lands on heads at an unusual rate.",
                                           "The mechanics are the calling card: crisp flicks, high headshot share, rarely loses a clean 50/50."}));
            else if (A(Attr::Aim) <= 46 || A(Attr::Headshot) <= 46)
                traits.push_back(pick(25, {"Raw aim is a soft spot that sharper duelists will exploit.",
                                           "Loses straight-up duels too often — needs the team to manufacture his fights.",
                                           "The gun lets him down under pressure; don't sign him to out-shoot anyone."}));
            if (A(Attr::CrosshairPlacement) >= 74 && A(Attr::Reaction) >= 72)
                traits.push_back(pick(26, {"Wins the first millisecond of a duel — crosshair is already there when the fight starts.",
                                           "Pre-aims like he's read the lobby's minds; fights are over before they start.",
                                           "Head-level discipline plus twitch reactions — the duel math tilts his way before either player shoots."}));
            if (A(Attr::Clutch) >= 76)
                traits.push_back(pick(27, {"Ice-cold with the round on the line — a genuine clutch threat.",
                                           "Save your timeouts: in the 1vX he's the calmest player in the building.",
                                           "Late-round killer — isolates, baits utility, and wins the ones that decide maps."}));
            else if (A(Attr::Clutch) <= 44)
                traits.push_back(pick(28, {"Tends to tighten up when the round comes down to him.",
                                           "The 1vX numbers are ugly — don't design late-round packages around him.",
                                           "Great in the 5v5 chaos, shaky when the server goes quiet and it's all on him."}));
            if (A(Attr::Aggressiveness) >= 72 && A(Attr::Positioning) <= 48)
                traits.push_back(pick(29, {"Aggressive to a fault — great for opening picks but prone to overextending.",
                                           "Throttle stuck open: buys openings, but gives them right back over-peeking.",
                                           "First-contact monster who re-peeks one too many times — a disciplined IGL reins it in."}));
            else if (A(Attr::Aggressiveness) >= 72 && A(Attr::Positioning) >= 68)
                traits.push_back(pick(30, {"Aggressive but disciplined holding his angles.",
                                           "Plays fast without playing loose — the aggression is positionally sound.",
                                           "Pushes tempo from smart spots; rarely dies for free despite the forward style."}));
            if (A(Attr::GameSense) >= 72 && A(Attr::Communication) >= 70)
                traits.push_back(pick(31, {"A vocal presence who reads the game and keeps the side organized.",
                                           "The comms hub — turns what he sees into calls the team can play off.",
                                           "Sees rotations early and says it early; teammates play smarter around him."}));
            if (A(Attr::Adaptability) <= 45 && A(Attr::AntiStrat) <= 50)
                traits.push_back(pick(32, {"One-dimensional — good teams will have him figured out by the second half.",
                                           "Plan A is strong; there is no plan B. Book teams beat him on the rematch.",
                                           "Predictable patterns — his heatmap barely changes map to map, and scouts notice."}));
            else if (A(Attr::Adaptability) >= 74)
                traits.push_back(pick(33, {"Adjusts fast mid-series — losing map one rarely means losing map two the same way.",
                                           "A different player by the second half — counters the counter almost immediately.",
                                           "You can't scout him twice: the look he shows on Tuesday is gone by Friday."}));
            if (A(Attr::MidRoundCalling) >= 72 && A(Attr::Communication) >= 66)
                traits.push_back(pick(34, {"Sees the mid-round before it happens — a second caller wherever he plays.",
                                           "De-facto second brain: when the primary call dies, he picks up the round.",
                                           "Mid-round IQ that turns broken rounds into won ones — caller insurance in the buy."}));
            if (A(Attr::EconomyMgmt) <= 45)
                traits.push_back(pick(35, {"Loose with the credits — buys into rounds his team shouldn't take.",
                                           "Economy liability: force-buys on tilt and drags the team bank with him.",
                                           "Needs the IGL managing his wallet — left alone he'll rifle on a save round."}));
            if (A(Attr::SpikeHandle) >= 74)
                traits.push_back(pick(36, {"Trustworthy on the spike when the round turns to chaos.",
                                           "Give him the bomb: plants under pressure and never panic-drops it.",
                                           "Objective-first — spike discipline that quietly banks extra rounds all season."}));
            if (A(Attr::AntiStrat) >= 78)
                traits.push_back(pick(37, {"Does his homework — reads opponent tendencies and rarely gets surprised.",
                                           "A tape junkie: walks in knowing the opponent's favorite timings and defaults.",
                                           "Preparation is a weapon — his anti-strat reads set traps the whole team eats off."}));
            if (A(Attr::Leadership) >= 76)
                traits.push_back(pick(38, {"Carries natural leadership weight in the server.",
                                           "Players follow him — the locker-room gravity is real even without the IGL tag.",
                                           "A tone-setter: when it goes 3-9 at the half, he's the one who steadies it."}));
            if (A(Attr::Utility) >= 78)
                traits.push_back(pick(39, {"Squeezes maximum value out of his utility.",
                                           "Every piece of util does a job — his flash assists don't show in K/D but win rounds.",
                                           "Utility artist: lineups, timings and one-way setups most players never learn."}));
            if (A(Attr::Lurking) >= 76)
                traits.push_back(pick(40, {"A patient lurker who controls space and denies info.",
                                           "Lives in the dark corners — his silence on the map is itself information control.",
                                           "A flank presence that changes how opponents rotate — they check for him even when he's not there."}));
            if (A(Attr::Awping) >= 78)
                traits.push_back(pick(41, {"A dedicated AWPer who anchors the eco on the big buys.",
                                           "The operator is the win condition — give him the $4,700 and a sightline.",
                                           "Big-window sniper: one opening pick from him and the round math collapses."}));
            if (A(Attr::Anchor) >= 78)
                traits.push_back(pick(42, {"An immovable anchor on the defensive side.",
                                           "Holds sites 1v2 long enough for rotates to matter — retake teams love him.",
                                           "Defensive rock — trading him out of a site costs attackers two players, minimum."}));
            if (A(Attr::Movement) >= 74 && A(Attr::Reaction) >= 74)
                traits.push_back(pick(43, {"Quick and slippery in duels — hard to pin down.",
                                           "Strafes like the servers favor him — half the deaths on his tape are whiffs at air.",
                                           "Movement demon: jiggle-peeks, wide-swings and re-peeks that break crosshair discipline."}));
            if (A(Attr::Entry) >= 78)
                traits.push_back(pick(44, {"A natural entry fragger who trades space for the team.",
                                           "First through the door every round — and more often than not, the door loses.",
                                           "Entry specialist: the site hits start working the day he arrives."}));
            if (A(Attr::Stamina) >= 80)
                traits.push_back(pick(45, {"Holds his level deep into long series.",
                                           "Map five looks like map one — conditioning that shows in elimination matches.",
                                           "No late-series fade: his worst map in a Bo5 is usually everyone else's average."}));
            if (p.is_flex)
                traits.push_back(pick(46, {"Genuine multi-role flex — can be contracted off his natural role without falling apart.",
                                           "Roster glue: solves two lineup problems at once because the role hardly matters.",
                                           "Comp-proof — meta shifts that break other rosters just slide him to a new seat."}));
            if (p.is_igl && A(Attr::Leadership) >= 60)
                traits.push_back(pick(47, {"The in-game leader — buying him is buying a system, not just a player.",
                                           "An IGL first: the value is the structure he installs, not his own scoreboard.",
                                           "Comes with a playbook — expect the whole team's shape to change around him."}));

            // ---- Deep reads a WEAK scout can't surface: production-vs-tools with the
            // player's ACTUAL numbers, plus temperament/character. accuracy >= 65 only —
            // this is the insider knowledge a top scout brings back.
            if (accuracy >= 65) {
                double amr = p.avg_match_rating();
                if (amr >= 1.15) {
                    std::snprintf(nb, sizeof(nb),
                        pick(51, {"Produces above his tools — a %.2f career rating that the practice-range numbers don't fully explain.",
                                  "The output outruns the inputs: %.2f rating on tape while the measurables say less. Winner's intangibles.",
                                  "Whatever the drills say, the matches say %.2f — he finds production the models miss."}).c_str(), amr);
                    traits.push_back(nb);
                } else if (amr > 0.0 && amr <= 0.90) {
                    std::snprintf(nb, sizeof(nb),
                        pick(52, {"The tools haven't translated yet — %.2f rating trails what he shows in scrims.",
                                  "Scrim hero, matchday mystery: rating sitting at %.2f despite the toolkit.",
                                  "There's a %.2f rating attached to top-shelf tools — someone has to unlock the gap."}).c_str(), amr);
                    traits.push_back(nb);
                }
                if (p.consistency <= 40)
                    traits.push_back(pick(53, {"Boom-or-bust: carries one map, disappears the next. Needs a stable structure around him.",
                                               "The variance is the risk — his ceiling wins you playoffs, his floor loses you groups.",
                                               "Two players in one jersey: scout the good one, plan for the other showing up."}));
                else if (p.consistency >= 75)
                    traits.push_back(pick(54, {"A high floor you can plan around — rarely posts a bad series.",
                                               "Metronome reliability: books the same game every week, which is rarer than a highlight reel.",
                                               "No dips in the sample — the kind of stability that lets a coach build around others' variance."}));
                if (p.work_ethic >= 75)
                    traits.push_back(pick(55, {"First into the practice server, last out — the trajectory tends to keep pointing up.",
                                               "Practice habits are elite; whatever he lacks today he tends to fix by next split.",
                                               "A gym-rat profile — development staff will get their money's worth."}));
                else if (p.work_ethic <= 38)
                    traits.push_back(pick(56, {"Questions about the work rate — talent may plateau without a demanding coach.",
                                               "Cruises on talent; the tape shows the same flaws month after month.",
                                               "Motivation is the project: pair him with a disciplinarian or watch the ceiling stay theoretical."}));
                if (p.ego >= 75)
                    traits.push_back(pick(57, {"A big personality that needs managing — expects star treatment and the star role.",
                                               "Wants the graphic, the desk segment and the last word in comp talks — plan the locker room accordingly.",
                                               "Star-sized ego: brilliant when featured, corrosive when benched."}));
                if (p.greed >= 75)
                    traits.push_back(pick(58, {"Follows the money — expect hardball at every contract renewal.",
                                               "His agent is the real opponent: every renewal will go to the deadline.",
                                               "Pay-cheque loyalty — match the market every year or start scouting the replacement."}));
                else if (p.loyalty >= 75)
                    traits.push_back(pick(59, {"Clubhouse glue — the type who repays faith with tenure.",
                                               "A one-club heart: treat him right and he'll turn down bigger offers to stay.",
                                               "Loyalty runs deep — the discount he takes to stay is real budget room."}));
                if (p.age <= 21 && p.potential_scouted && p.potential - static_cast<int>(std::lround(p.ovr())) >= 12) {
                    std::snprintf(nb, sizeof(nb),
                        pick(60, {"The gap between today and the ceiling is the story — at %d, sign him for the player he'll be in two seasons.",
                                  "%d years old with most of the growth curve still ahead — the price never gets better than now.",
                                  "A development bet: at %d the frame is raw, but the ceiling reads like a franchise player."}).c_str(), p.age);
                    traits.push_back(nb);
                } else if (p.age >= 31) {
                    std::snprintf(nb, sizeof(nb),
                        pick(61, {"On the back nine at %d — a win-now piece, not a project.",
                                  "At %d the legs have a shelf life; buy the experience, budget for the decline.",
                                  "%d and still effective — but this is a two-season window, price it that way."}).c_str(), p.age);
                    traits.push_back(nb);
                }
            }

            // A sharper scout surfaces more (and deeper) reads. When the pool exceeds
            // the cap, keep the identity line, then ROTATE which of the rest survive
            // (id-keyed) — so even similar players lead with different aspects.
            std::size_t cap = (accuracy >= 85) ? 10u
                            : (accuracy >= 70) ? 8u
                            : (accuracy >= 55) ? 5u
                            : (accuracy >= 35) ? 3u : 2u;
            if (traits.size() > cap) {
                std::vector<std::string> kept;
                kept.push_back(traits.front());
                std::size_t rest = traits.size() - 1;
                std::size_t start = static_cast<std::size_t>(p.id % rest);
                for (std::size_t k = 0; k < rest && kept.size() < cap; ++k)
                    kept.push_back(traits[1 + (start + k) % rest]);
                traits.swap(kept);
            }
            jw.key("traits");
            jw.arr_open();
            for (const auto& t : traits) jw.arr_str(t);
            jw.arr_close();
        }

        std::string verdict;
        if (!avs.empty()) {
            const char* topS = attr_name(static_cast<Attr>(avs.front().idx));
            const char* topW = attr_name(static_cast<Attr>(avs.back().idx));
            char vb[288];
            if (p.potential_scouted && p.potential - static_cast<int>(std::lround(p.ovr())) >= 8)
                std::snprintf(vb, sizeof(vb), "A %s with standout %s and real headroom (%d ceiling) — one to develop.",
                              role_name(p.primary_role), topS, p.potential);
            else
                std::snprintf(vb, sizeof(vb), "A %s whose %s stands out; %s is the clear soft spot.",
                              role_name(p.primary_role), topS, topW);
            verdict = vb;
            if (accuracy < 55) verdict += " Read is limited — a closer look would firm this up.";
        }
        jw.kv_str("verdict", verdict);
    }
    jw.obj_close();   // scout
    jw.obj_close();   // root
    return jw.str();
}

std::string export_scouting_to_json(const GameManager& gm) {
    JsonWriter jw;
    jw.obj_open();
    jw.kv_int("schema_version", 1);
    jw.kv_str("exporter", "VLR Manager Valosim");
    if (gm.user_team)
        jw.kv_str("club_color", hex_color(gm.user_team->color_primary_r, gm.user_team->color_primary_g, gm.user_team->color_primary_b));
    else jw.kv_str("club_color", "#5a6cff");

    jw.key("scouting");
    jw.obj_open();
    const Scout* sc = (gm.user_team && gm.user_team->head_scout) ? gm.user_team->head_scout.get() : nullptr;
    jw.kv_bool("has_scout", sc != nullptr);
    jw.kv_int("scout_credits", gm.scout_credits());
    jw.kv_int("budget", gm.user_team ? gm.user_team->budget : 0);

    if (sc) {
        jw.key("scout");
        jw.obj_open();
        jw.kv_str("name", sc->name);
        jw.kv_str("personality", scout_personality_name(sc->personality));
        jw.kv_str("region", sc->region);
        jw.kv_str("country_iso", sc->country_iso);
        jw.kv_int("judgement", sc->judgement);
        jw.kv_int("network", sc->network);
        jw.kv_int("projection", sc->projection);
        jw.kv_int("reputation", sc->reputation);
        jw.kv_int("salary_k", sc->salary_k);
        jw.kv_int("read_depth", scout_read_depth(*sc));
        jw.obj_close();

        jw.key("coverage");
        jw.obj_open();
        jw.key("regions");
        jw.arr_open();
        for (const char* r : {"Americas", "EMEA", "Pacific"}) {
            jw.obj_open(); jw.kv_str("region", r); jw.kv_int("coverage", scout_region_coverage(*sc, r)); jw.obj_close();
        }
        jw.arr_close();
        jw.key("countries");
        jw.arr_open();
        for (const Country& c : countries()) {
            jw.obj_open();
            jw.kv_str("iso", c.iso);
            jw.kv_str("name", c.name);
            jw.kv_str("region", region_name(c.region));
            jw.kv_int("coverage", scout_country_coverage(*sc, c.iso));
            jw.obj_close();
        }
        jw.arr_close();
        jw.obj_close();   // coverage
    }

    jw.key("assignments");
    jw.arr_open();
    for (const auto& a : gm.scout_assignments_) {
        if (a.done) continue;
        PlayerPtr p = gm.find_player_by_id(a.player_id);
        jw.obj_open();
        jw.kv_int("player_id", static_cast<long long>(a.player_id));
        jw.kv_str("name", p ? p->name : std::string("?"));
        jw.kv_str("role", p ? role_name(p->primary_role) : "?");
        jw.kv_str("team", p ? p->team_name : std::string("?"));
        jw.kv_str("country_iso", p ? p->country_iso : std::string(""));
        jw.kv_int("days_remaining", a.days_remaining);
        jw.kv_int("total_days", a.total_days);
        int pct = a.total_days > 0 ? static_cast<int>((a.total_days - a.days_remaining) * 100.0 / a.total_days) : 0;
        jw.kv_int("pct", pct);
        jw.kv_int("money_k", a.money_k);
        jw.kv_int("coverage", a.coverage);
        jw.obj_close();
    }
    jw.arr_close();

    jw.key("completed");
    jw.arr_open();
    for (const auto& kv : gm.scout_report_accuracy_) {
        PlayerPtr p = gm.find_player_by_id(kv.first);
        if (!p) continue;
        jw.obj_open();
        jw.kv_int("player_id", static_cast<long long>(kv.first));
        jw.kv_str("name", p->name);
        jw.kv_str("role", role_name(p->primary_role));
        jw.kv_str("team", p->team_name);
        jw.kv_str("country_iso", p->country_iso);
        jw.kv_int("accuracy", kv.second);
        jw.obj_close();
    }
    jw.arr_close();

    jw.obj_close();   // scouting
    jw.obj_close();   // root
    return jw.str();
}

// =============================================================
// New-Game wizard options (region list + org tiers + difficulty range).
// =============================================================
std::string export_newgame_options_to_json(const GameManager& gm) {
    (void)gm;
    JsonWriter jw;
    jw.obj_open();
    jw.kv_int("schema_version", 1);
    jw.kv_str("exporter", "VLR Manager Valosim");
    jw.key("newgame");
    jw.obj_open();
    jw.key("regions");
    jw.arr_open();
    jw.arr_str("Americas"); jw.arr_str("EMEA"); jw.arr_str("Pacific");
    jw.arr_close();
    // Order MUST match NewGameConfig::OrgTier {Rich,Contender,Mid,Budget,Expansion}.
    struct Tier { const char* name; const char* blurb; };
    const Tier tiers[] = {
        {"Rich",      "Deep pockets, elite prestige — win-now expectations"},
        {"Contender", "Strong budget and real ambitions"},
        {"Mid",       "A balanced org with room to grow"},
        {"Budget",    "Tight funds — scout smart and develop talent"},
        {"Expansion", "A brand-new org with everything to build"},
    };
    jw.key("tiers");
    jw.arr_open();
    for (int i = 0; i < 5; ++i) {
        jw.obj_open();
        jw.kv_int("idx",  i);
        jw.kv_str("name", tiers[i].name);
        jw.kv_str("blurb", tiers[i].blurb);
        jw.obj_close();
    }
    jw.arr_close();
    jw.kv_double("diff_min",     0.7, 1);
    jw.kv_double("diff_max",     1.3, 1);
    jw.kv_double("diff_default", 1.0, 1);
    jw.obj_close();   // newgame
    jw.obj_close();   // root
    return jw.str();
}

// =============================================================
// Transfer-interest analysis — which clubs pursue/admire a player + WHY.
// Every "interested" club clears Team::score_scout_target (>0), the SAME gate the
// AI uses to sign; reasons are read off the real factors, never invented.
// =============================================================
namespace {
const char* strategy_label(Team::Strategy s) {
    switch (s) {
        case Team::Strategy::Contender:        return "Contender";
        case Team::Strategy::Rebuilding:       return "Rebuilding";
        case Team::Strategy::Bridge:           return "Retooling";
        case Team::Strategy::BudgetRoster:     return "Budget";
        case Team::Strategy::DevelopmentFocus: return "Development";
        case Team::Strategy::WinNow:           return "Win-now";
        case Team::Strategy::TalentFarm:       return "Talent farm";
    }
    return "?";
}
bool is_rebuild_strategy(Team::Strategy s) {
    return s == Team::Strategy::Rebuilding || s == Team::Strategy::DevelopmentFocus ||
           s == Team::Strategy::TalentFarm;
}
}  // namespace

std::string export_player_interest_to_json(const GameManager& gm, std::uint64_t player_id) {
    JsonWriter jw;
    jw.obj_open();
    jw.kv_int("schema_version", 1);
    jw.kv_str("exporter", "VLR Manager Valosim");

    PlayerPtr pp = gm.find_player_by_id(player_id);
    if (!pp) { jw.kv_bool("found", false); jw.obj_close(); return jw.str(); }
    const Player& Y = *pp;
    jw.kv_bool("found", true);
    if (gm.user_team)
        jw.kv_str("club_color", hex_color(gm.user_team->color_primary_r,
                                          gm.user_team->color_primary_g,
                                          gm.user_team->color_primary_b));
    else jw.kv_str("club_color", "#5a6cff");

    TeamPtr seller;
    for (const auto& kv : gm.leagues) {
        if (!kv.second) continue;
        for (const auto& t : kv.second->teams()) if (t && t->name == Y.team_name) { seller = t; break; }
        if (seller) break;
    }
    bool is_fa = (Y.team_name == "Free Agent");
    int  nat_r = static_cast<int>(Y.primary_role);
    int  asking_fee = (seller && !is_fa) ? seller->transfer_fee_for(Y, gm.year) : 0;

    jw.key("interest");
    jw.obj_open();

    jw.key("player");
    jw.obj_open();
    jw.kv_int("id",         static_cast<long long>(Y.id));
    jw.kv_str("name",       Y.name);
    jw.kv_str("role",       role_name(Y.primary_role));
    jw.kv_int("ovr",        static_cast<long long>(std::lround(Y.ovr())));
    jw.kv_int("potential",  Y.potential);
    jw.kv_int("age",        Y.age);
    jw.kv_str("team_name",  Y.team_name);
    jw.kv_str("rep_tier",   rep_tier_label(Y.reputation));   // FM-style stature band
    jw.kv_bool("is_free_agent", is_fa);
    jw.kv_int("salary_k",   Y.contract.amount_k);
    jw.kv_int("years_left", Y.years_left(gm.year));
    jw.kv_int("value_k",    Y.get_transfer_value() / 1000);
    if (!is_fa) jw.kv_int("asking_fee_k", asking_fee);
    jw.obj_close();

    struct Cand {
        const Team* t; double score;
        std::vector<std::string> reasons;
        std::string blocker, blocker_detail;
    };
    std::vector<Cand> pursuing, admirers;

    for (const auto& kv : gm.leagues) {
        if (!kv.second) continue;
        for (const auto& tp : kv.second->teams()) {
            if (!tp || tp.get() == seller.get()) continue;
            const Team& X = *tp;
            RoleNeed need = X.compute_role_need();
            Role fill = Role::Count;
            double score = X.score_scout_target(Y, need, gm.year, gm.world_difficulty(), &fill);

            int r = nat_r;
            if (fill != Role::Count) r = static_cast<int>(fill);
            else if (need.need[nat_r] < 0.15 && need.most_needed != Role::Count) r = static_cast<int>(need.most_needed);

            double rf = Y.role_fit_score(static_cast<Role>(r));
            bool fills_hole = (need.need[r] >= 0.15) ||
                              (need.most_needed != Role::Count &&
                               need.need[static_cast<int>(need.most_needed)] >= 0.15 &&
                               Y.role_fit_score(need.most_needed) >= 0.80);
            double best_here = need.best_ovr[r];
            bool beats_incumbent = (need.count[r] == 0) || (Y.ovr() > best_here + 1.0);
            bool genuine_fit = fills_hole && rf >= 0.62 && beats_incumbent;

            if (score <= 0.0 && !genuine_fit) continue;   // no honest interest at all

            bool afford_value = (Y.get_transfer_value() <= std::max<long long>(0, static_cast<long long>(X.budget)));
            bool wage_room    = X.within_wage_envelope(Y.contract.amount_k, is_fa ? 0 : asking_fee);
            bool seller_sells = is_fa || (seller && seller->will_sell(Y, asking_fee, gm.year));

            std::vector<std::string> reasons;
            const char* rn = role_name(static_cast<Role>(r));
            char buf[144];
            if (need.count[r] == 0)
                reasons.push_back(std::string("No established ") + rn + " in their starting five");
            else if (need.need[r] >= 0.35)
                reasons.push_back(std::string("A clear weakness at ") + rn);
            else if (need.need[r] >= 0.15)
                reasons.push_back(std::string("Looking to strengthen ") + rn);
            if (need.count[r] > 0 && Y.ovr() > best_here + 1.0) {
                std::snprintf(buf, sizeof(buf), "An upgrade on their best %s (%d OVR)", rn, static_cast<int>(std::lround(best_here)));
                reasons.push_back(buf);
            }
            if (r != nat_r)
                reasons.push_back(std::string("Would flex from ") + role_name(Y.primary_role) + " to plug the hole");
            if (is_rebuild_strategy(X.strategy) && Y.potential - static_cast<int>(std::lround(Y.ovr())) >= 6 && Y.age <= 23) {
                std::snprintf(buf, sizeof(buf), "High-ceiling fit for their rebuild (%d potential at age %d)", Y.potential, Y.age);
                reasons.push_back(buf);
            }
            if ((X.strategy == Team::Strategy::Contender || X.strategy == Team::Strategy::WinNow) && Y.avg_match_rating() >= 1.10) {
                std::snprintf(buf, sizeof(buf), "Chasing proven form (%.2f avg rating)", Y.avg_match_rating());
                reasons.push_back(buf);
            }
            if (Y.age <= 20 && Y.potential >= 80) {
                std::snprintf(buf, sizeof(buf), "Elite young talent (%d potential, age %d)", Y.potential, Y.age);
                reasons.push_back(buf);
            }
            for (std::size_t i = 0; i < std::min<std::size_t>(5, X.roster.size()); ++i) {
                const auto& inc = X.roster[i];
                if (inc && inc.get() != &Y && static_cast<int>(inc->primary_role) == r &&
                    inc->years_left(gm.year) <= 1 && inc->team_name != "Free Agent") {
                    reasons.push_back(std::string("Their ") + rn + " " + inc->name + "'s contract is up — a succession target");
                    break;
                }
            }
            if (reasons.empty()) reasons.push_back(std::string("Squad-building fit at ") + rn);

            Cand c; c.t = &X; c.score = score; c.reasons = std::move(reasons);
            if (score > 0.0) {
                pursuing.push_back(std::move(c));
            } else {
                if (!afford_value) {
                    c.blocker = "funds";
                    std::snprintf(buf, sizeof(buf), "Can't afford the ~$%dK value on a $%dK budget",
                                  Y.get_transfer_value() / 1000, static_cast<int>(static_cast<long long>(X.budget) / 1000));
                    c.blocker_detail = buf;
                } else if (!is_fa && !seller_sells) {
                    c.blocker = "fee";
                    std::snprintf(buf, sizeof(buf), "%s's asking fee (~$%dK) is out of reach", Y.team_name.c_str(), asking_fee);
                    c.blocker_detail = buf;
                } else if (!wage_room) {
                    c.blocker = "wage";
                    std::snprintf(buf, sizeof(buf), "No wage room for his $%dK/yr salary", Y.contract.amount_k);
                    c.blocker_detail = buf;
                } else {
                    c.blocker = "marginal";
                    c.blocker_detail = "Keen, but not enough of an upgrade to move yet";
                }
                admirers.push_back(std::move(c));
            }
        }
    }

    std::sort(pursuing.begin(), pursuing.end(),
              [](const Cand& a, const Cand& b) { return a.score > b.score; });

    auto emit_cand = [&](const Cand& c, bool with_blocker) {
        jw.obj_open();
        jw.kv_str("team",       c.t->name);
        jw.kv_str("tag",        c.t->tag);
        jw.kv_str("strategy",   strategy_label(c.t->strategy));
        jw.kv_str("club_color", hex_color(c.t->color_primary_r, c.t->color_primary_g, c.t->color_primary_b));
        jw.key("reasons");
        jw.arr_open();
        for (const auto& rr : c.reasons) jw.arr_str(rr);
        jw.arr_close();
        if (with_blocker) { jw.kv_str("blocker", c.blocker); jw.kv_str("blocker_detail", c.blocker_detail); }
        jw.obj_close();
    };

    jw.key("pursuing");
    jw.arr_open();
    for (const auto& c : pursuing) emit_cand(c, false);
    jw.arr_close();

    jw.key("admirers");
    jw.arr_open();
    { int n = 0; for (const auto& c : admirers) { if (n++ >= 8) break; emit_cand(c, true); } }
    jw.arr_close();

    jw.kv_int("pursuing_count", static_cast<long long>(pursuing.size()));
    jw.kv_int("admirer_count",  static_cast<long long>(admirers.size()));
    jw.obj_close();   // interest
    jw.obj_close();   // root
    return jw.str();
}

// =============================================================
// Post-match results — the user's most recent recorded match.
// =============================================================
namespace {
const char* round_end_name(RoundEndKind k) {
    switch (k) {
        case RoundEndKind::SpikeDetonation: return "Detonation";
        case RoundEndKind::Defuse:          return "Defuse";
        case RoundEndKind::TimeExpiry:      return "Time";
        default:                            return "Elimination";
    }
}
}  // namespace

// Emit ONE map of a series as a JSON object (header scores + storyline + both
// scoreboards + round-by-round timeline). Shared by every map in the series.
static void emit_match_map(JsonWriter& jw, const RecordedMatch& rec) {
    jw.obj_open();
    jw.kv_str("map",        rec.map_name);
    jw.kv_str("blue_name",  rec.blue_name);
    jw.kv_str("red_name",   rec.red_name);
    jw.kv_int("blue_score", rec.blue_score);
    jw.kv_int("red_score",  rec.red_score);
    jw.kv_str("mvp",        rec.mvp_name);

    int diff  = std::abs(rec.blue_score - rec.red_score);
    int loser = std::min(rec.blue_score, rec.red_score);
    const char* story = "Solid win";
    if (loser == 0)                                       story = "Flawless";
    else if (rec.blue_score >= 13 && rec.red_score >= 13) story = "Overtime thriller";
    else if (diff <= 2)                                   story = "Nailbiter";
    else if (diff >= 9)                                   story = "Domination";
    jw.kv_str("storyline", story);

    const HistoryRecord* hr = rec.history_record.get();
    const std::unordered_map<Player*, PlayerMatchStats>* ms = rec.match_stats.get();
    int total_rounds = rec.blue_score + rec.red_score;
    auto emit_team = [&](const char* key, const std::vector<Player*>& roster) {
        jw.key(key);
        jw.arr_open();
        for (Player* pl : roster) {
            if (!pl) continue;
            jw.obj_open();
            jw.kv_int("id",   static_cast<long long>(pl->id));
            jw.kv_str("name", pl->name);
            jw.kv_str("role", role_name(pl->primary_role));
            jw.kv_bool("mvp", hr && pl == hr->mvp);
            if (hr) {
                auto it = hr->stats.find(pl);
                if (it != hr->stats.end()) {
                    const PlayerLine& s = it->second;
                    jw.kv_str("agent",   s.agent);
                    jw.kv_int("k", s.k); jw.kv_int("d", s.d); jw.kv_int("a", s.a);
                    jw.kv_double("rating", s.rating, 2);
                    jw.kv_int("hs",  s.hs);
                    jw.kv_int("fb",  s.fb);
                    jw.kv_int("fd",  s.fd);
                    jw.kv_str("mk",  s.mk);
                    jw.kv_int("clutch", s.clutch);
                }
            }
            if (ms && total_rounds > 0) {
                auto mit = ms->find(pl);
                if (mit != ms->end())
                    jw.kv_int("adr", mit->second.damage / total_rounds);
            }
            jw.obj_close();
        }
        jw.arr_close();
    };
    if (hr) {
        emit_team("blue_team", hr->blue_team);
        emit_team("red_team",  hr->red_team);
    } else {
        jw.kv_raw("blue_team", "[]");
        jw.kv_raw("red_team",  "[]");
    }

    jw.key("rounds");
    jw.arr_open();
    if (rec.round_history) {
        for (const RoundLog& r : *rec.round_history) {
            jw.obj_open();
            jw.kv_int("round",        r.round);
            jw.kv_str("winner",       r.winner_name);
            jw.kv_int("blue_score",   r.t1_score);
            jw.kv_int("red_score",    r.t2_score);
            jw.kv_str("end_kind",     round_end_name(r.end_kind));
            jw.kv_bool("spike_planted", r.spike_planted);
            jw.kv_bool("retake",      r.was_retake);
            // Replay enrichment (read-only projection of RoundLog fields — no rng/
            // mutation): which side attacked + per-team economy investment so the UI
            // can tell the full round story (side switch at R12/24, eco vs full-buy).
            jw.kv_bool("blue_attacking", r.t1_attacking);
            jw.kv_int("blue_invest",     r.t1_invest);
            jw.kv_int("red_invest",      r.t2_invest);
            // Per-kill feed (RoundEvent stream): killer/victim by id+name so the
            // live viewer can tick each player's K/D up round by round. Ids join
            // against the scoreboard rows' ids (name collisions can't confuse it).
            jw.key("kills");
            jw.arr_open();
            for (const RoundEvent& ev : r.events) {
                if (!ev.killer || !ev.victim) continue;   // defensive; always set
                jw.obj_open();
                jw.kv_int("k_id",   static_cast<long long>(ev.killer->id));
                jw.kv_str("k",      ev.killer->name);
                jw.kv_int("v_id",   static_cast<long long>(ev.victim->id));
                jw.kv_str("v",      ev.victim->name);
                jw.kv_int("side",   ev.team_won);          // 1 = blue's kill, 2 = red's
                jw.kv_int("t1_alive", ev.t1_alive);
                jw.kv_int("t2_alive", ev.t2_alive);
                jw.obj_close();
            }
            jw.arr_close();
            jw.obj_close();
        }
    }
    jw.arr_close();
    jw.obj_close();
}

std::string export_match_to_json(const GameManager& gm,
                                 const std::vector<RecordedMatchPtr>& series) {
    JsonWriter jw;
    jw.obj_open();
    jw.kv_int("schema_version", 1);
    jw.kv_str("exporter", "VLR Manager Valosim");

    // Filter to valid recordings, front..back = play order (map 1, 2, ...).
    std::vector<const RecordedMatch*> maps;
    for (const auto& rp : series) if (rp) maps.push_back(rp.get());
    if (maps.empty()) {
        jw.kv_bool("found", false);
        jw.obj_close();
        return jw.str();
    }
    jw.kv_bool("found", true);
    if (gm.user_team)
        jw.kv_str("club_color", hex_color(gm.user_team->color_primary_r,
                                          gm.user_team->color_primary_g,
                                          gm.user_team->color_primary_b));
    else
        jw.kv_str("club_color", "#5a6cff");

    // Blue/red are consistent across the series (team a is always blue/team1).
    const RecordedMatch& first = *maps.front();
    const std::string& blue_name = first.blue_name;
    const std::string& red_name  = first.red_name;

    int blue_maps = 0, red_maps = 0;
    for (const RecordedMatch* m : maps) {
        if (m->blue_score > m->red_score) ++blue_maps;
        else if (m->red_score > m->blue_score) ++red_maps;
    }
    int winner_maps = std::max(blue_maps, red_maps);
    // best-of: a single map is BO1; a BO3 winner needs 2 map wins, a BO5 needs 3.
    int best_of = (maps.size() == 1) ? 1 : (winner_maps >= 3 ? 5 : 3);

    // Series MVP: highest aggregate per-map rating across every map played.
    std::unordered_map<Player*, double> rating_sum;
    for (const RecordedMatch* m : maps) {
        const HistoryRecord* hr = m->history_record.get();
        if (!hr) continue;
        for (const auto& kv : hr->stats) rating_sum[kv.first] += kv.second.rating;
    }
    std::string series_mvp;
    { double best = -1.0;
      for (const auto& kv : rating_sum)
          if (kv.first && kv.second > best) { best = kv.second; series_mvp = kv.first->name; } }

    bool user_is_blue = (gm.user_team && blue_name == gm.user_team->name);
    bool user_is_red  = (gm.user_team && red_name  == gm.user_team->name);

    jw.key("match");
    jw.obj_open();
    jw.key("series");
    jw.obj_open();
    jw.kv_str("event",       first.event);
    jw.kv_int("best_of",     best_of);
    jw.kv_str("blue_name",   blue_name);
    jw.kv_str("red_name",    red_name);
    jw.kv_int("blue_maps",   blue_maps);
    jw.kv_int("red_maps",    red_maps);
    jw.kv_str("winner_name", blue_maps > red_maps ? blue_name
                            : red_maps > blue_maps ? red_name : "");
    jw.kv_bool("is_solo_q",  first.is_solo_q);
    jw.kv_str("user_side",   user_is_blue ? "blue" : user_is_red ? "red" : "none");
    if (user_is_blue || user_is_red)
        jw.kv_bool("user_won", user_is_blue ? (blue_maps > red_maps) : (red_maps > blue_maps));
    jw.kv_str("series_mvp",  series_mvp);
    jw.key("maps");
    jw.arr_open();
    for (const RecordedMatch* m : maps) emit_match_map(jw, *m);
    jw.arr_close();
    jw.obj_close();   // series
    jw.obj_close();   // match
    jw.obj_close();   // root
    return jw.str();
}

// =============================================================
// Recruitment hub — the full world player pool for a filterable/sortable/
// infinite-scroll search table (FM-style). Read-only: no rng/mutation, so the
// --dynasty path is untouched. The client does all filtering/sorting/paging.
// =============================================================
std::string export_recruitment_to_json(const GameManager& gm) {
    JsonWriter jw;
    jw.obj_open();
    jw.kv_int("schema_version", 1);
    jw.kv_str("exporter", "VLR Manager Valosim");
    const TeamPtr& user = gm.user_team;
    if (user)
        jw.kv_str("club_color", hex_color(user->color_primary_r, user->color_primary_g, user->color_primary_b));
    else jw.kv_str("club_color", "#5a6cff");

    jw.key("recruitment");
    jw.obj_open();

    jw.key("header");
    jw.obj_open();
    jw.kv_str("club_name",   user ? user->name : std::string("\xE2\x80\x94"));
    jw.kv_int("budget",      user ? user->budget : 0);
    jw.kv_int("budget_k",    user ? static_cast<long long>(user->budget / 1000) : 0);
    jw.kv_int("wage_room_k", user ? user->wage_envelope_k : 0);
    jw.kv_int("year",        gm.year);
    jw.obj_close();

    jw.key("regions"); jw.arr_open(); jw.arr_str("Americas"); jw.arr_str("EMEA"); jw.arr_str("Pacific"); jw.arr_close();

    // Every distinct active player across all region ladders (rostered + FA).
    std::vector<PlayerPtr> all;
    {
        std::unordered_set<std::uint64_t> seen;
        for (const auto& kv : gm.solo_qs) {
            if (!kv.second) continue;
            for (const auto& p : kv.second->global_ladder())
                if (p && !p->is_retired && seen.insert(p->id).second) all.push_back(p);
        }
    }

    int fa_count = 0, shortlist_count = 0;
    jw.key("players");
    jw.arr_open();
    for (const auto& pp : all) {
        const Player& p = *pp;
        bool on_user = gm.player_on_user_team(p.id);   // roster id, not team-name (collision-proof)
        bool is_fa   = (p.team_name == "Free Agent");
        bool fav     = gm.is_favorited(pp);
        bool watch   = gm.is_watched(pp);
        if (is_fa) ++fa_count;
        if (fav || watch) ++shortlist_count;
        bool pot_known = p.potential_scouted || on_user;

        jw.obj_open();
        jw.kv_int("id",          static_cast<long long>(p.id));
        jw.kv_str("name",        p.name);
        jw.kv_str("team",        p.team_name);
        jw.kv_str("region",      p.region);
        jw.kv_str("country_iso", p.country_iso);
        jw.kv_str("role",        role_name(p.primary_role));
        jw.kv_int("age",         p.age);
        jw.kv_int("ovr",         static_cast<long long>(std::lround(p.ovr())));
        jw.kv_bool("pot_known",  pot_known);
        if (pot_known) { jw.kv_int("pot", p.potential); }
        else { int lo = 0, hi = 0; gm.scouted_band(p, lo, hi); jw.kv_int("pot_lo", lo); jw.kv_int("pot_hi", hi); }
        jw.kv_int("value_k",     is_fa ? 0 : static_cast<long long>(p.get_transfer_value() / 1000));
        jw.kv_int("wage_k",      p.contract.amount_k);
        jw.kv_int("years_left",  p.years_left(gm.year));
        jw.kv_str("archetype",   archetype_name(p.archetype));
        jw.kv_str("desire",      desire_name(p.desire));
        jw.kv_bool("igl",        p.is_igl);
        jw.kv_bool("wants_out",  p.transfer_requested);
        jw.kv_bool("is_fa",      is_fa);
        jw.kv_bool("on_user_team", on_user);
        jw.kv_bool("favorited",  fav);
        jw.kv_bool("watched",    watch);
        jw.kv_str("knowledge",   on_user ? "Complete" : (p.potential_scouted ? "Extensive" : "Basic"));
        jw.obj_close();
    }
    jw.arr_close();

    jw.key("counts");
    jw.obj_open();
    jw.kv_int("total",       static_cast<long long>(all.size()));
    jw.kv_int("free_agents", fa_count);
    jw.kv_int("shortlist",   shortlist_count);
    jw.obj_close();

    // Head scout (drives brief reveal rate) + the FM-style scouting briefs, with a
    // LIVE match count over the pool so a fresh brief shows its target size at once.
    jw.key("scout");
    jw.obj_open();
    if (user && user->head_scout) {
        jw.kv_str("name",       user->head_scout->name);
        jw.kv_int("read_depth", scout_read_depth(*user->head_scout));
    } else { jw.kv_str("name", ""); jw.kv_int("read_depth", 0); }
    jw.obj_close();

    jw.key("briefs");
    jw.arr_open();
    {
        int bi = 0;
        for (const auto& b : gm.scout_briefs_) {
            int matches = 0;
            for (const auto& pp2 : all) if (gm.brief_matches(b, *pp2)) ++matches;
            jw.obj_open();
            jw.kv_int("idx",      bi++);
            jw.kv_str("name",     b.name);
            jw.kv_int("role_idx", b.role_idx);
            jw.kv_str("role",     b.role_idx >= 0 ? role_name(static_cast<Role>(b.role_idx)) : std::string("Any role"));
            jw.kv_int("age_min",  b.age_min);
            jw.kv_int("age_max",  b.age_max);
            jw.kv_int("min_pot",  b.min_pot);
            jw.kv_str("region",   b.region.empty() ? std::string("Any region") : b.region);
            jw.kv_int("days",     b.days_active);
            jw.kv_int("revealed", b.revealed);
            jw.kv_int("matches",  matches);
            jw.obj_close();
        }
    }
    jw.arr_close();

    // Staff market — the user's current staff + the FA pools (coaches/scouts/
    // analysts) for the hub's Staff sub-tab. Read-only; mirrors export_market.
    jw.key("staff");
    jw.obj_open();
    jw.key("current");
    jw.obj_open();
    if (user && user->head_coach) {
        jw.key("coach"); jw.obj_open();
        jw.kv_str("name", user->head_coach->name);
        jw.kv_str("personality", coach_personality_name(user->head_coach->personality));
        jw.kv_int("reputation", user->head_coach->reputation);
        jw.kv_int("salary_k", user->head_coach->salary_k);
        jw.obj_close();
    } else jw.kv_raw("coach", "null");
    if (user && user->head_scout) {
        jw.key("scout"); jw.obj_open();
        jw.kv_str("name", user->head_scout->name);
        jw.kv_str("personality", scout_personality_name(user->head_scout->personality));
        jw.kv_int("reputation", user->head_scout->reputation);
        jw.kv_int("salary_k", user->head_scout->salary_k);
        jw.obj_close();
    } else jw.kv_raw("scout", "null");
    if (user && user->head_analyst) {
        jw.key("analyst"); jw.obj_open();
        jw.kv_str("name", user->head_analyst->name);
        jw.kv_str("personality", analyst_personality_name(user->head_analyst->personality));
        jw.kv_int("reputation", user->head_analyst->reputation);
        jw.kv_int("salary_k", user->head_analyst->salary_k);
        jw.obj_close();
    } else jw.kv_raw("analyst", "null");
    jw.obj_close();   // current

    jw.key("coaches");
    jw.arr_open();
    for (const auto& c : gm.free_coaches_) {
        if (!c || c->is_retired) continue;
        jw.obj_open();
        jw.kv_int("id",          static_cast<long long>(c->id));
        jw.kv_str("name",        c->name);
        jw.kv_str("personality", coach_personality_name(c->personality));
        jw.kv_int("reputation",  c->reputation);
        jw.kv_int("ask_k",       c->requested_salary_k());
        jw.obj_close();
    }
    jw.arr_close();
    jw.key("scouts");
    jw.arr_open();
    for (const auto& s : gm.free_scouts_) {
        if (!s || s->is_retired) continue;
        jw.obj_open();
        jw.kv_int("id",          static_cast<long long>(s->id));
        jw.kv_str("name",        s->name);
        jw.kv_str("personality", scout_personality_name(s->personality));
        jw.kv_int("reputation",  s->reputation);
        jw.kv_int("ask_k",       s->requested_salary_k());
        jw.obj_close();
    }
    jw.arr_close();
    jw.key("analysts");
    jw.arr_open();
    for (const auto& a : gm.free_analysts_) {
        if (!a || a->is_retired) continue;
        jw.obj_open();
        jw.kv_int("id",          static_cast<long long>(a->id));
        jw.kv_str("name",        a->name);
        jw.kv_str("personality", analyst_personality_name(a->personality));
        jw.kv_int("reputation",  a->reputation);
        jw.kv_int("ask_k",       a->requested_salary_k());
        jw.obj_close();
    }
    jw.arr_close();
    jw.obj_close();   // staff

    jw.obj_close();   // recruitment
    jw.obj_close();   // root
    return jw.str();
}

}  // namespace vlr
