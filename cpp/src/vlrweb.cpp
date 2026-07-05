// vlrweb.cpp — WebView2 host + live command bridge for the Valosim web UI.
//
// The web UI (ui-poc/app.html) is a single-page app: a nav rail + a content
// area whose screens are rendered from JSON the C++ side serves on demand. This
// host owns ONE in-process GameManager for the session and acts as the backend:
//
//   web -> C++ :  window.chrome.webview.postMessage("<verb>:<arg>:<arg>")
//   C++ -> web :  PostWebMessageAsJson({type:"state", screen, data:<exporter json>})
//
// Verbs:  get:<screen> / nav:<screen>      -> push that screen's state
//         advance:<mode>:<screen>          -> advance time (day|match|phase|year),
//                                             then push <screen>'s fresh state
//
// Everything the bridge calls into the engine is READ-ONLY export or the
// existing advance_day() loop — no new simulation logic, no balance change.
// Output/host only. Requires the WebView2 runtime + the vendored SDK.

#include <windows.h>
#include <shlwapi.h>      // SHCreateStreamOnFileEx (self-capture mode)
#include <wrl.h>          // Microsoft::WRL::ComPtr, Callback
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <cstdlib>

#include "WebView2.h"

#include "Common.h"          // vlr::rng, attr_from_str
#include "GameManager.h"     // vlr::GameManager
#include "SoloQ.h"           // SoloQEngine::global_ladder (FA pool, for make-offer)
#include "Team.h"            // vlr::Team
#include "DashboardExport.h" // vlr::export_dashboard_to_json / export_roster_to_json
#include "SaveGame.h"        // vlr::SaveGame — multi-slot save/load

using namespace Microsoft::WRL;

namespace {

ComPtr<ICoreWebView2Controller> g_controller;
ComPtr<ICoreWebView2>           g_webview;
HWND         g_hwnd   = nullptr;
std::wstring g_folder;   // absolute path to the ui-poc folder
std::wstring g_shotPath; // non-empty => headless self-capture mode
std::wstring g_shotScreen = L"dashboard";
std::wstring g_advMode;   // --adv <mode>: drive an advance before capturing (verification)
std::wstring g_goScreen;  // --go <screen>: drive a nav before capturing (verification)
std::wstring g_cmd;       // --cmd <command>: drive any bridge command before capturing (verification)
std::wstring g_js;        // --js <code>: ExecuteScript arbitrary JS before capturing (verification)
std::wstring g_cmd2;      // --cmd2 <command>: a second bridge command chained after --cmd (verification)
std::string  g_profileTeam; // nav:teamprofile:<name> target — which team the Team Profile screen renders
std::string  g_compRegion;  // standings view: region ("" = user's) — persists so the screen re-opens where you left it
int          g_compTier = 0; // standings view: tier (0 = user's; 1=VCT, 2=Challengers, 3=Open Circuit)
std::uint64_t g_profilePlayerId = 0; // nav:playermodal:<id> target — which player the Player modal renders
std::uint64_t g_profileStaffId  = 0; // nav:staffmodal:<id> target — which coach/scout/analyst the Staff modal renders
vlr::RecordedMatchPtr g_lastUserMatch; // the user's most recent match recording (captured from advance_day's DayResult)
int g_lastSeriesLen = 0;               // map count of that recording's BO series
std::vector<vlr::RecordedMatchPtr> g_lastUserSeries; // EVERY map of that series (BO3/BO5) — drives the live series viewer
std::wstring g_navUrl = L"https://valosim.local/app.html";
EventRegistrationToken g_navToken = {};
EventRegistrationToken g_msgToken = {};

std::unique_ptr<vlr::GameManager> g_gm;   // the single live world for this session

// ---- string helpers ------------------------------------------------------
std::wstring to_wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}
std::string to_utf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}
std::vector<std::string> split(const std::string& s, char d) {
    std::vector<std::string> out; size_t start = 0, pos;
    while ((pos = s.find(d, start)) != std::string::npos) { out.push_back(s.substr(start, pos - start)); start = pos + 1; }
    out.push_back(s.substr(start));
    return out;
}

// ---- world + engine ------------------------------------------------------
void build_world() {
    vlr::rng().seed(0xC0FFEEULL);   // deterministic session
    g_gm = std::make_unique<vlr::GameManager>();
    g_gm->initialize_world();
    std::vector<std::string> log;
    g_gm->start_of_season_setup(log);
    // Advance past the no-match preseason into Stage 1 with a few matches played,
    // so the first render has content; the player advances from there.
    int guard = 0;
    while (guard++ < 160) {
        vlr::Team* u = g_gm->user_team.get();
        if (u && (u->phase_wins + u->phase_losses) >= 5) break;
        const std::string& ph = g_gm->current_phase();
        if (ph == "AWARDS" || ph == "OFFSEASON") break;
        g_gm->advance_day(log);
    }
}

void do_advance(const std::string& mode) {
    if (!g_gm) return;
    std::vector<std::string> log;
    vlr::Team* u = g_gm->user_team.get();
    // Each step captures the DayResult's user match recording (transient there) so
    // the post-match view can render the user's latest game after any advance.
    auto step = [&]() {
        auto dr = g_gm->advance_day(log);
        if (dr.user_match_recording) {
            g_lastUserMatch = dr.user_match_recording;
            g_lastSeriesLen = static_cast<int>(dr.user_match_series.size());
            g_lastUserSeries = dr.user_match_series;   // full BO3/BO5 for the live viewer
        }
        return dr;
    };
    if (mode == "day") {
        step();
    } else if (mode == "match") {
        int before = u ? (u->phase_wins + u->phase_losses) : 0;
        for (int i = 0; i < 45; ++i) {
            step();
            const std::string& ph = g_gm->current_phase();
            if (ph == "AWARDS" || ph == "OFFSEASON") break;
            if (u && (u->phase_wins + u->phase_losses) > before) break;
        }
    } else if (mode == "phase") {
        std::string start = g_gm->current_phase();
        for (int i = 0; i < 80; ++i) {
            step();
            if (g_gm->current_phase() != start) break;
        }
    } else if (mode == "year") {
        for (int i = 0; i < 260; ++i) {
            if (step().year_rolled) break;
        }
    }
}

std::string export_screen(const std::string& screen) {
    if (!g_gm) return "{}";
    if (screen == "roster")      return vlr::export_roster_to_json(*g_gm);
    if (screen == "competition") return vlr::export_competition_to_json(*g_gm, g_compRegion, g_compTier);
    if (screen == "finance")     return vlr::export_finance_to_json(*g_gm);
    if (screen == "mail")        return vlr::export_mail_to_json(*g_gm);
    if (screen == "strategy")    return vlr::export_strategy_to_json(*g_gm);
    if (screen == "calendar")    return vlr::export_calendar_to_json(*g_gm);
    if (screen == "market")      return vlr::export_market_to_json(*g_gm);
    if (screen == "awards")      return vlr::export_awards_to_json(*g_gm);
    if (screen == "brackets")    return vlr::export_brackets_to_json(*g_gm);
    if (screen == "teamprofile") return vlr::export_team_profile_to_json(*g_gm, g_profileTeam);
    if (screen == "playermodal") return vlr::export_player_profile_to_json(*g_gm, g_profilePlayerId);
    if (screen == "staffmodal")  return vlr::export_staff_profile_to_json(*g_gm, g_profileStaffId);
    if (screen == "favorites")   return vlr::export_favorites_to_json(*g_gm);
    if (screen == "watchlist")   return vlr::export_watchlist_to_json(*g_gm);
    if (screen == "records")     return vlr::export_records_to_json(*g_gm);
    if (screen == "match")       return vlr::export_match_to_json(*g_gm, g_lastUserSeries);
    if (screen == "newgame")     return vlr::export_newgame_options_to_json(*g_gm);
    if (screen == "scouting")    return vlr::export_scouting_to_json(*g_gm);
    if (screen == "recruitment") return vlr::export_recruitment_to_json(*g_gm);
    return vlr::export_dashboard_to_json(*g_gm);
}

void post_state(const std::string& screen) {
    if (!g_webview) return;
    std::string data = export_screen(screen);
    std::string wrapper = "{\"type\":\"state\",\"screen\":\"" + screen + "\",\"data\":" + data + "}";
    g_webview->PostWebMessageAsJson(to_wide(wrapper).c_str());
}

// Normalize to a known screen so the echoed name in the wrapper JSON is always
// a safe bare token (defensive: commands come from our own app.html).
std::string safe_screen(const std::string& s) {
    if (s == "roster" || s == "competition" || s == "finance" ||
        s == "mail" || s == "strategy" || s == "calendar" || s == "market" ||
        s == "awards" || s == "brackets" || s == "teamprofile" ||
        s == "playermodal" || s == "staffmodal" || s == "favorites" ||
        s == "watchlist" ||
        s == "records" || s == "match" || s == "newgame" || s == "scouting" ||
        s == "recruitment") return s;
    return "dashboard";
}

// JSON-escape a short message for a toast (map/agent names are clean, but be safe).
std::string json_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else if (static_cast<unsigned char>(c) >= 0x20) out += c;
    }
    return out;
}

void post_toast(const std::string& level, const std::string& msg) {
    if (!g_webview) return;
    std::string w = "{\"type\":\"toast\",\"level\":\"" + json_escape(level) +
                    "\",\"message\":\"" + json_escape(msg) + "\"}";
    g_webview->PostWebMessageAsJson(to_wide(w).c_str());
}

// --- ACTIONS (mutate the live world; reuse the exact engine paths the GUI uses).
// All are USER-team config writes: never-free safe, no rng(), bounded balance.
void do_set_prep(const std::string& map_name, int level) {
    if (!g_gm || !g_gm->user_team) return;
    if (level < 0) level = 0; if (level > 2) level = 2;
    g_gm->user_team->map_prep[map_name].level = level;   // pure config; prep tilt clamps [1.0,1.03]
}
void do_clear_agents(const std::string& map_name) {
    if (!g_gm || !g_gm->user_team) return;
    g_gm->user_team->agent_override.erase(map_name);     // reset that map to Auto
}
// Author a manual per-map comp: exactly 5 comma-separated agent names, each a real
// agent + all distinct (matches the engine's 5-slot expectation; build_round_selection
// best-fits them to the roster). User-only config; no rng → dynasty path untouched.
bool do_agent_set(const std::string& map_name, const std::string& csv) {
    if (!g_gm || !g_gm->user_team) return false;
    std::vector<std::string> names = split(csv, ',');
    if (names.size() != 5) return false;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (names[i].empty() || !vlr::find_agent_by_name(names[i])) return false;   // real agent
        for (std::size_t j = 0; j < i; ++j) if (names[j] == names[i]) return false; // distinct
    }
    g_gm->user_team->agent_override[map_name] = names;
    return true;
}
// FM-style scouting BRIEF: create a filter-based assignment the head scout works
// over time (GameManager::add_scout_brief). User-only config; no rng.
bool do_add_brief(const std::string& name, int role, int age_min, int age_max,
                  int min_pot, const std::string& region) {
    if (!g_gm || !g_gm->user_team) return false;
    vlr::GameManager::ScoutBrief b;
    b.name     = name.empty() ? std::string("Scouting focus") : name;
    b.role_idx = (role >= 0 && role < 4) ? role : -1;
    b.age_min  = std::max(15, std::min(40, age_min));
    b.age_max  = std::max(b.age_min, std::min(40, age_max));
    b.min_pot  = std::max(0, std::min(99, min_pot));
    b.region   = (region == "Americas" || region == "EMEA" || region == "Pacific") ? region : std::string();
    g_gm->add_scout_brief(b);
    return true;
}
bool do_clear_brief(int idx) {
    if (!g_gm) return false;
    g_gm->clear_scout_brief(idx);
    return true;
}
// Bench/start: swap two of the user's roster players by their stable ids. Reuses
// Team::swap_roster_slots (no rng, never-free safe, auto re-runs enforce_one_igl/flex).
bool do_bench(std::uint64_t idA, std::uint64_t idB) {
    if (!g_gm || !g_gm->user_team || idA == idB) return false;
    vlr::PlayerPtr a, b;
    for (const auto& p : g_gm->user_team->roster) {
        if (!p) continue;
        if (p->id == idA) a = p;
        if (p->id == idB) b = p;
    }
    if (!a || !b) return false;
    return g_gm->user_team->swap_roster_slots(a, b);
}
// GOD MODE — direct edits to a user player by id. Sandbox tool: intentionally
// changes the player (and thus OVR, recomputed from attributes). Never-free
// safe, no rng. Only touches the user's session world, never the dynasty path.
bool do_setattr(std::uint64_t id, const std::string& attr_name, int value) {
    if (!g_gm || !g_gm->user_team) return false;
    if (value < 1) value = 1; if (value > 99) value = 99;
    std::size_t idx = static_cast<std::size_t>(vlr::attr_from_str(attr_name));
    if (idx >= vlr::kAttrCount) return false;
    for (const auto& p : g_gm->user_team->roster) {
        if (p && p->id == id) { p->attributes[idx] = value; return true; }
    }
    return false;
}
bool do_setpot(std::uint64_t id, int value) {
    if (!g_gm || !g_gm->user_team) return false;
    if (value < 1) value = 1; if (value > 99) value = 99;
    for (const auto& p : g_gm->user_team->roster) {
        if (p && p->id == id) { p->potential = value; return true; }
    }
    return false;
}
// GOD MODE — rename a user player (gamertag/display name). User roster only, no rng.
// Trims, caps length, rejects empty. The UI strips ':' so the colon-delimited command
// stays well-formed.
bool do_setname(std::uint64_t id, const std::string& name) {
    if (!g_gm || !g_gm->user_team) return false;
    std::string nm = name;
    while (!nm.empty() && nm.front() == ' ') nm.erase(nm.begin());
    while (!nm.empty() && nm.back()  == ' ') nm.pop_back();
    if (nm.empty()) return false;
    if (nm.size() > 24) nm = nm.substr(0, 24);
    for (const auto& p : g_gm->user_team->roster) {
        if (p && p->id == id) { p->name = nm; return true; }
    }
    return false;
}
// GOD MODE — edit the USER team's staff (coach/scout/analyst by id): role attributes,
// reputation, salary, contract years, name. User staff only, no rng.
bool do_staffset(std::uint64_t id, const std::string& field, const std::string& val) {
    if (!g_gm || !g_gm->user_team) return false;
    auto& t = *g_gm->user_team;
    int iv = std::atoi(val.c_str());
    auto attr = [&](int v){ return v < 1 ? 1 : v > 99 ? 99 : v; };
    auto name_of = [&](const std::string& raw) {
        std::string nm = raw;
        while (!nm.empty() && nm.front() == ' ') nm.erase(nm.begin());
        while (!nm.empty() && nm.back()  == ' ') nm.pop_back();
        if (nm.size() > 26) nm = nm.substr(0, 26);
        return nm;
    };
    // Shared fields work on whichever staffer matches the id.
    auto shared = [&](auto& s) -> bool {
        if (field == "reputation") { s.reputation = attr(iv); return true; }
        if (field == "salary")     { s.salary_k = iv < 10 ? 10 : iv > 500 ? 500 : iv; return true; }
        if (field == "years")      { int y = iv < 0 ? 0 : iv > 5 ? 5 : iv;
                                     s.contract_years = y;
                                     s.contract_exp_year = g_gm->year + y - 1; return true; }
        if (field == "name")       { std::string nm = name_of(val); if (nm.empty()) return false;
                                     s.name = nm; return true; }
        return false;
    };
    if (t.head_coach && t.head_coach->id == id) {
        auto& c = *t.head_coach;
        if      (field == "tactical")    c.tactical    = attr(iv);
        else if (field == "development") c.development = attr(iv);
        else if (field == "leadership")  c.leadership  = attr(iv);
        else if (field == "experience")  c.experience  = attr(iv);
        else return shared(c);
        return true;
    }
    if (t.head_scout && t.head_scout->id == id) {
        auto& s = *t.head_scout;
        if      (field == "judgement")  s.judgement  = attr(iv);
        else if (field == "network")    s.network    = attr(iv);
        else if (field == "projection") s.projection = attr(iv);
        else if (field == "experience") s.experience = attr(iv);
        else return shared(s);
        return true;
    }
    if (t.head_analyst && t.head_analyst->id == id) {
        auto& a = *t.head_analyst;
        if      (field == "tactical_read")    a.tactical_read    = attr(iv);
        else if (field == "opponent_insight") a.opponent_insight = attr(iv);
        else if (field == "prep")             a.prep             = attr(iv);
        else if (field == "experience")       a.experience       = attr(iv);
        else return shared(a);
        return true;
    }
    return false;
}
// GOD MODE — edit the USER team itself: name (mirrors the wizard rename: roster +
// staff team_name stamps, tag, reseeded colors/logo), budget ($K) and prestige.
bool do_teamset(const std::string& field, const std::string& val) {
    if (!g_gm || !g_gm->user_team) return false;
    auto& t = *g_gm->user_team;
    if (field == "name") {
        std::string nm = val;
        while (!nm.empty() && nm.front() == ' ') nm.erase(nm.begin());
        while (!nm.empty() && nm.back()  == ' ') nm.pop_back();
        if (nm.empty()) return false;
        if (nm.size() > 26) nm = nm.substr(0, 26);
        t.name = nm;
        t.tag  = vlr::make_team_tag(nm);
        for (auto& p : t.roster) if (p) p->team_name = t.name;
        if (t.head_coach)   t.head_coach->team_name   = t.name;
        if (t.head_scout)   t.head_scout->team_name   = t.name;
        if (t.head_analyst) t.head_analyst->team_name = t.name;
        vlr::seed_team_colors(t);
        constexpr int kLogoCount = static_cast<int>(vlr::LogoShape::Count);
        std::size_t h = std::hash<std::string>{}(t.name);
        t.logo_shape = static_cast<vlr::LogoShape>(h % static_cast<std::size_t>(kLogoCount));
        g_gm->resolve_user_name_collision();   // typed name is exclusive league-wide
        return true;
    }
    if (field == "budget") {
        long long bk = std::atoll(val.c_str());
        if (bk < 0) bk = 0; if (bk > 100000) bk = 100000;   // $0..$100M in $K
        t.budget = bk * 1000LL;
        return true;
    }
    if (field == "prestige") {
        int v = std::atoi(val.c_str());
        t.prestige = v < 0 ? 0 : v > 99 ? 99 : v;
        return true;
    }
    return false;
}
// GOD MODE — add/remove a named badge on a user player. Reuses Player::god_set_badge
// (applies/reverses the badge's attribute mods; OVR recomputes). Never-free, no rng.
bool do_set_badge(std::uint64_t id, const std::string& badge, bool on) {
    if (!g_gm || !g_gm->user_team) return false;
    for (const auto& p : g_gm->user_team->roster) {
        if (p && p->id == id) return p->god_set_badge(badge, on);
    }
    return false;
}
// COMMISSION the head scout to assess a player over 1-3 weeks (wraps the proven
// GameManager::commission_scout: deducts money, snapshots coverage/depth, queues the
// assignment which advance_day ticks down). Returns the planned duration in days.
int do_commission(std::uint64_t id, int money_k) {
    if (!g_gm) return 0;
    vlr::PlayerPtr p = g_gm->find_player_by_id(id);
    if (!p) return 0;
    return g_gm->commission_scout(p, money_k);
}
// BUY a CONTRACTED player from a rival club. Wraps the proven, never-free-safe
// GameManager::user_transfer_bid: it validates availability/roster-room/seller-min/
// affordability/will_sell/transfer-window, then release-seller -> pay_fee -> sign-user
// (the player MOVES, nothing is destroyed) and mails the user. years=0 -> engine picks.
vlr::GameManager::TransferBidOutcome do_buy_player(const std::string& seller_name,
                                                   std::uint64_t target_id, int fee_k) {
    vlr::GameManager::TransferBidOutcome o;
    if (!g_gm || !g_gm->user_team) { o.message = "No active game."; return o; }
    vlr::TeamPtr   seller;
    vlr::PlayerPtr target;
    for (auto& kv : g_gm->leagues) {
        if (!kv.second) continue;
        for (auto& t : kv.second->teams()) {
            if (!t) continue;
            if (t->name == seller_name) seller = t;
            for (const auto& p : t->roster) if (p && p->id == target_id) target = p;
        }
    }
    if (!seller || !target) { o.message = "Could not find that player or club."; return o; }
    return g_gm->user_transfer_bid(seller, target, fee_k, 0);
}
// PLAYER-MODAL actions — operate on ANY player by id (resolved world-wide via
// find_player_by_id). favorite/watch are read-only list toggles; scout reveals a
// potential (user's scout credits); queuematch plays one SoloQ match (consumes rng,
// never-free: records a match, the player persists). All only touch the user session.
bool do_set_favorite(std::uint64_t id, bool on) {
    if (!g_gm) return false;
    vlr::PlayerPtr p = g_gm->find_player_by_id(id);
    if (!p) return false;
    if (on) g_gm->favorite_player(p); else g_gm->unfavorite_player(p);
    return true;
}
bool do_set_watch(std::uint64_t id, bool on) {
    if (!g_gm) return false;
    vlr::PlayerPtr p = g_gm->find_player_by_id(id);
    if (!p) return false;
    if (on) g_gm->watch_player(p); else g_gm->unwatch_player(p);
    return true;
}
bool do_scout(std::uint64_t id) {
    if (!g_gm) return false;
    vlr::PlayerPtr p = g_gm->find_player_by_id(id);
    if (!p) return false;
    return g_gm->scout_player(p);
}
bool do_queue_match(std::uint64_t id) {
    if (!g_gm) return false;
    vlr::PlayerPtr p = g_gm->find_player_by_id(id);
    if (!p) return false;
    auto it = g_gm->solo_qs.find(p->region);
    if (it == g_gm->solo_qs.end() || !it->second) return false;
    return it->second->force_solo_match(p) != nullptr;
}
// RELEASE a USER player to free agency (Team::release_player — never-free: the
// player persists in the SoloQ ladders as a free agent). User roster only.
bool do_release(std::uint64_t id) {
    if (!g_gm || !g_gm->user_team) return false;
    vlr::PlayerPtr target;
    for (const auto& p : g_gm->user_team->roster) if (p && p->id == id) { target = p; break; }
    if (!target) return false;
    g_gm->user_team->release_player(target);
    return true;
}
// WATCHLIST status/note edits (the player must already be watched; resolve world-wide).
bool do_set_watch_status(std::uint64_t id, int status) {
    if (!g_gm) return false;
    vlr::PlayerPtr p = g_gm->find_player_by_id(id);
    if (!p) return false;
    vlr::GameManager::WatchEntry* e = g_gm->watch_entry(p);
    if (!e) return false;
    if (status < 0) status = 0; if (status > 2) status = 2;
    e->status = static_cast<vlr::GameManager::WatchStatus>(status);
    return true;
}
bool do_set_watch_note(std::uint64_t id, const std::string& note) {
    if (!g_gm) return false;
    vlr::PlayerPtr p = g_gm->find_player_by_id(id);
    if (!p) return false;
    vlr::GameManager::WatchEntry* e = g_gm->watch_entry(p);
    if (!e) return false;
    e->note = note;
    return true;
}
// RE-SIGN a user player (extension). Gates on evaluate_resign_offer().will_accept
// (a lowball is rejected AND registers the reject cost, killing slider-spam), then
// commits exactly like the GUI's negotiation modal: Team::resign_player (5-arg with
// the offered role) + set signing_bonus_k/promised_starter/promise_active + deduct
// the one-time bonus from budget. rng: the resign path may consume rng (user session
// only). never-free: none (player already rostered; only the contract mutates).
// Returns 1 accepted, 0 rejected, -1 invalid/not-a-user-player.
int do_resign(std::uint64_t id, int years, int amount, int bonus, bool starter, int role_idx) {
    if (!g_gm || !g_gm->user_team) return -1;
    vlr::PlayerPtr p;
    for (const auto& rp : g_gm->user_team->roster) if (rp && rp->id == id) { p = rp; break; }
    if (!p) return -1;
    vlr::TeamPtr team = g_gm->user_team;
    vlr::Role role = (role_idx >= 0 && role_idx < 4) ? static_cast<vlr::Role>(role_idx) : p->primary_role;
    vlr::Player::ResignBreakdown bd = p->evaluate_resign_offer(amount, years, *team, role, bonus, starter);
    if (!bd.will_accept) {
        p->register_rejected_offer(amount, years, *team);
        return 0;
    }
    // The club must be able to fund the one-time signing bonus (the player accepts,
    // but we won't commit a deal the budget can't cover).
    if (bonus > 0 && team->budget < static_cast<long long>(bonus) * 1000LL) return 0;
    int base_yr = (g_gm->current_phase() == "OFFSEASON") ? g_gm->year : g_gm->year + 1;
    bool committed = team->resign_player(p, years, amount, base_yr, role);
    if (committed) {
        p->contract.signing_bonus_k  = bonus;
        p->contract.promised_starter = starter;
        p->contract.promise_active   = starter;
        if (bonus > 0) team->budget -= static_cast<long long>(bonus) * 1000LL;
        return 1;
    }
    return 0;
}
// NEW GAME — reset the session world and re-boot it from the wizard config. The
// default launch keeps the fixed deterministic 0xC0FFEE world (for --shot + tests);
// this is an OPT-IN reset seeded reproducibly from the config, so the dynasty test
// path (in vlrtest, a separate exe) is never touched. Returns false on init failure.
bool do_newgame(const std::string& name, const std::string& region, int tier,
                int diff_x100, const std::string& color_hex) {
    if (!g_gm) return false;
    vlr::NewGameConfig cfg;
    cfg.user_team_name = name.empty() ? std::string("VLR Manager") : name;
    cfg.user_region    = (region == "EMEA" || region == "Pacific") ? region : std::string("Americas");
    int t = tier; if (t < 0) t = 0; if (t > 4) t = 4;
    cfg.user_tier = static_cast<vlr::NewGameConfig::OrgTier>(t);
    double d = diff_x100 / 100.0; if (d < 0.7) d = 0.7; if (d > 1.3) d = 1.3;
    cfg.difficulty = d;
    if (color_hex.size() == 6)
        cfg.user_color_primary = static_cast<std::uint32_t>(std::strtoul(color_hex.c_str(), nullptr, 16));
    cfg.starting_year = 2026;

    std::uint64_t seed = 0xC0FFEEULL;
    for (char c : cfg.user_team_name) seed = seed * 131u + static_cast<unsigned char>(c);
    seed ^= (static_cast<std::uint64_t>(t) << 33) ^ (static_cast<std::uint64_t>(diff_x100) << 41);
    vlr::rng().seed(seed);

    g_gm->reset_world();
    std::vector<std::string> log;
    if (!g_gm->initialize_world_with_config(cfg, log)) return false;
    g_gm->start_of_season_setup(log);

    // Clear transient view state tied to the old world.
    g_lastUserMatch.reset();
    g_lastSeriesLen  = 0;
    g_lastUserSeries.clear();
    g_profileTeam.clear();
    g_compRegion.clear();
    g_compTier = 0;
    g_profileStaffId = 0;
    g_profilePlayerId = 0;
    return true;
}
// MAKE-OFFER (free agent): resolve a FA by id from the per-region SoloQ ladders,
// then sign to the user team via the proven Team::sign_player path (never-free
// safe: the player persists in the ladders; sign_player auto-enforces IGL/flex).
// sync_player_ranked_region moves them to the user's region ladder post-sign.
// years/role_idx/bonus default to the legacy 2-year natural-role no-bonus deal.
// role_idx 0..3 pins Player::contract_role (role-aware sign_player overload);
// a signing bonus is a one-time budget debit (same shape as the GUI FA modal).
// FA signing is a NEGOTIATION (2026-06-28): the player evaluates the offer with
// the same engine math as re-signing (wage vs ask, years vs length preference,
// prestige/contender/personality/role fit) and can REFUSE. amount_k < 0 = "meet
// their ask" (the market quick-sign path). g_signfaReason carries the refusal
// text for the toast.
std::string g_signfaReason;
bool do_signfa(std::uint64_t id, int years = 2, int role_idx = -1, int bonus = 0,
               int amount_k = -1) {
    g_signfaReason.clear();
    if (!g_gm || !g_gm->user_team) return false;
    vlr::PlayerPtr fa;
    for (auto& kv : g_gm->solo_qs) {
        if (!kv.second) continue;
        for (const auto& p : kv.second->global_ladder()) {
            if (p && p->id == id && p->team_name == "Free Agent") { fa = p; break; }
        }
        if (fa) break;
    }
    if (!fa) return false;
    if (years < 1) years = 1; if (years > 5) years = 5;
    vlr::Role role = (role_idx >= 0 && role_idx < 4) ? static_cast<vlr::Role>(role_idx) : vlr::Role::Count;
    vlr::Role eval_role = (role == vlr::Role::Count) ? fa->primary_role : role;
    // "Meet their ask" resolves the deterministic ask first (probe at 0 salary
    // just to read ask_k), then evaluates the real offer.
    vlr::Player::ResignBreakdown probe =
        fa->evaluate_resign_offer(0, years, *g_gm->user_team, eval_role, bonus, false);
    int amount = (amount_k > 0) ? amount_k : probe.ask_k;
    vlr::Player::ResignBreakdown bd =
        fa->evaluate_resign_offer(amount, years, *g_gm->user_team, eval_role, bonus, false);
    if (!bd.will_accept) {
        g_signfaReason = !bd.reject_reason.empty() ? bd.reject_reason
                       : (fa->name + " turned the offer down \xE2\x80\x94 sweeten the terms.");
        fa->register_rejected_offer(amount, years, *g_gm->user_team);
        return false;
    }
    // Don't leave a half-signed deal: bail before signing if the bonus is unaffordable.
    if (bonus > 0 && g_gm->user_team->budget < static_cast<long long>(bonus) * 1000LL) {
        g_signfaReason = "You can't fund that signing bonus.";
        return false;
    }
    g_gm->user_team->sign_player(fa, years, g_gm->year, role);   // Role::Count = natural role
    fa->contract.amount_k = amount;   // the NEGOTIATED wage, not the auto-generated one
    if (bonus > 0) {
        fa->contract.signing_bonus_k = bonus;
        g_gm->user_team->budget -= static_cast<long long>(bonus) * 1000LL;
    }
    g_gm->sync_player_ranked_region(fa, g_gm->user_team->region);
    return true;
}
// ---- SAVE / LOAD (SaveGame.cpp) -------------------------------------------
// Slots live in <exe_dir>\saves\slot_<slot>.vlrs ("1","2","3" + "auto").
std::wstring saves_dir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring dir(buf);
    std::size_t cut = dir.find_last_of(L"\\/");
    if (cut != std::wstring::npos) dir.resize(cut);
    dir += L"\\saves";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}
std::string slot_path(const std::string& slot) {
    std::string safe;
    for (char c : slot) if (std::isalnum(static_cast<unsigned char>(c))) safe += c;
    if (safe.empty()) safe = "1";
    return to_utf8(saves_dir()) + "\\slot_" + safe + ".vlrs";
}
// Push the slot list as a transient {screen:"saves"} state (main-menu panel).
void post_saves_list() {
    if (!g_webview) return;
    static const char* kSlots[] = {"1", "2", "3", "auto"};
    std::string json = "{\"type\":\"state\",\"screen\":\"saves\",\"data\":{\"slots\":[";
    bool first = true;
    for (const char* s : kSlots) {
        vlr::SaveGame::SlotInfo si = vlr::SaveGame::peek(slot_path(s));
        if (!first) json += ",";
        first = false;
        json += "{\"slot\":\"" + std::string(s) + "\",\"exists\":" + (si.exists ? "true" : "false");
        if (si.exists) {
            json += ",\"club\":\"" + json_escape(si.club) + "\"";
            json += ",\"year\":" + std::to_string(si.year);
            json += ",\"day\":" + std::to_string(si.day);
            json += ",\"saved_at\":\"" + json_escape(si.saved_at) + "\"";
        }
        json += "}";
    }
    json += "]}}";
    g_webview->PostWebMessageAsJson(to_wide(json).c_str());
}

// Reputation gate for USER signings (FM "won't negotiate"): a low-stature club can't
// attract a player whose FAME far exceeds it. Returns "" if OK, else the refusal note.
// Club stature mirrors Team::score_scout_target's (prestige + wealth tier); the user's
// reach is a touch more generous than the AI's so the protagonist isn't over-boxed-in.
std::string user_reputation_block(std::uint64_t id) {
    if (!g_gm || !g_gm->user_team) return "";
    vlr::PlayerPtr pl;
    for (auto& kv : g_gm->solo_qs) {
        if (!kv.second) continue;
        for (const auto& p : kv.second->global_ladder()) if (p && p->id == id) { pl = p; break; }
        if (pl) break;
    }
    if (!pl) return "";
    const auto& ut = *g_gm->user_team;
    double club_rep = ut.prestige * 85.0 + static_cast<int>(ut.wealth_tier) * 500.0 + 600.0;
    if (club_rep > 10000.0) club_rep = 10000.0;
    double reach = club_rep + 900.0;   // a touch more generous than the AI gate
    if (static_cast<double>(pl->reputation) > reach)
        return pl->name + " won't negotiate \xE2\x80\x94 your club's reputation is too low for a player of his stature.";
    return "";
}
// HIRE-STAFF (coach/scout/analyst): resolve a free-agent staffer by id and install
// them on the user team via GameManager::user_hire_staff — which recycles any
// displaced incumbent into the FA pool (never-free safe, no rng, user-only world).
bool do_hire_staff(const std::string& role, std::uint64_t id) {
    if (!g_gm || !g_gm->user_team) return false;
    return g_gm->user_hire_staff(role, id);
}
// CHOOSE a preseason sponsor by index into the pending 3-offer list (idx -1 =
// decline / no sponsor). Only valid while sponsor_choice_pending_. choose_sponsor
// stamps the deal on user_team + clears the pending flag. User-only, no rng —
// never touches the dynasty path.
bool do_choose_sponsor(int idx) {
    if (!g_gm || !g_gm->user_team) return false;
    if (!g_gm->sponsor_choice_pending_) return false;
    if (idx < 0) { g_gm->choose_sponsor(vlr::SponsorOffer{}); return true; }   // decline
    if (idx >= static_cast<int>(g_gm->pending_sponsor_offers_.size())) return false;
    g_gm->choose_sponsor(g_gm->pending_sponsor_offers_[idx]);
    return true;
}
// MAIL actions — mutate the user inbox (read/star/delete) by mail id. User-only,
// no rng, informational-only (mail never feeds the sim) → dynasty-path untouched.
bool do_mail_set_read(int id, bool read) {
    if (!g_gm) return false;
    for (auto& m : g_gm->mailbox) if (m.id == id) { m.read = read; return true; }
    return false;
}
bool do_mail_set_star(int id, bool star) {
    if (!g_gm) return false;
    for (auto& m : g_gm->mailbox) if (m.id == id) { m.important = star; return true; }
    return false;
}
bool do_mail_delete(int id) {
    if (!g_gm) return false;
    auto& mb = g_gm->mailbox;
    for (auto it = mb.begin(); it != mb.end(); ++it) if (it->id == id) { mb.erase(it); return true; }
    return false;
}

void handle_command(const std::string& cmd) {
    std::vector<std::string> p = split(cmd, ':');
    if (p.empty()) return;
    if (p[0] == "get" || p[0] == "nav") {
        // Parameterized: get/nav:teamprofile:<team name> — capture the target team,
        // then push the Team Profile view. Team names are colon-free, but rejoin the
        // tail defensively so a stray colon in a name still resolves.
        if (p.size() == 2 && p[1] == "saves") {
            // get:saves — slot metadata for the main-menu save/load panels.
            post_saves_list();
        } else if (p.size() >= 4 && p[1] == "competition") {
            // get/nav:competition:<region>:<tier> — standings view switcher.
            // region "" = user's; tier 0 = user's. Persists across re-pushes.
            g_compRegion = p[2];
            g_compTier   = std::atoi(p[3].c_str());
            post_state("competition");
        } else if (p.size() > 2 && p[1] == "teamprofile") {
            std::string name = p[2];
            for (std::size_t i = 3; i < p.size(); ++i) name += ":" + p[i];
            g_profileTeam = name;
            post_state("teamprofile");
        } else if (p.size() > 2 && p[1] == "playermodal") {
            g_profilePlayerId = std::strtoull(p[2].c_str(), nullptr, 10);
            post_state("playermodal");
        } else if (p.size() > 2 && p[1] == "staffmodal") {
            g_profileStaffId = std::strtoull(p[2].c_str(), nullptr, 10);
            post_state("staffmodal");
        } else if (p.size() >= 8 && p[1] == "resignpreview") {
            // get:resignpreview:<id>:<years>:<amount>:<bonus>:<starter01>:<role> — live
            // acceptance preview (no commit). Posts a transient resignpreview state.
            if (g_webview && g_gm) {
                std::string data = vlr::export_resign_preview_to_json(*g_gm,
                    std::strtoull(p[2].c_str(), nullptr, 10), std::atoi(p[3].c_str()),
                    std::atoi(p[4].c_str()), std::atoi(p[5].c_str()),
                    std::atoi(p[6].c_str()) != 0, std::atoi(p[7].c_str()));
                std::string wrapper =
                    "{\"type\":\"state\",\"screen\":\"resignpreview\",\"data\":" + data + "}";
                g_webview->PostWebMessageAsJson(to_wide(wrapper).c_str());
            }
        } else if (p.size() >= 3 && p[1] == "playerinterest") {
            // get:playerinterest:<id> — which clubs pursue/admire this player + why.
            if (g_webview && g_gm) {
                std::string data = vlr::export_player_interest_to_json(*g_gm,
                    std::strtoull(p[2].c_str(), nullptr, 10));
                std::string wrapper =
                    "{\"type\":\"state\",\"screen\":\"playerinterest\",\"data\":" + data + "}";
                g_webview->PostWebMessageAsJson(to_wide(wrapper).c_str());
            }
        } else if (p.size() >= 3 && p[1] == "scoutreport") {
            // get:scoutreport:<id> — the scout's report/status for this player.
            if (g_webview && g_gm) {
                std::string data = vlr::export_scout_report_to_json(*g_gm,
                    std::strtoull(p[2].c_str(), nullptr, 10));
                std::string wrapper =
                    "{\"type\":\"state\",\"screen\":\"scoutreport\",\"data\":" + data + "}";
                g_webview->PostWebMessageAsJson(to_wide(wrapper).c_str());
            }
        } else {
            post_state(safe_screen(p.size() > 1 ? p[1] : "dashboard"));
        }
    } else if (p[0] == "advance") {
        do_advance(p.size() > 1 ? p[1] : "day");
        post_state(safe_screen(p.size() > 2 ? p[2] : "dashboard"));
    } else if (p[0] == "act") {
        // act:<verb>:<args...>:<screen>  — mutate the world, re-push, then toast.
        const std::string verb = (p.size() > 1) ? p[1] : "";
        const std::string screen = p.back();
        if (verb == "prep" && p.size() == 5) {            // act:prep:<map>:<level>:<screen>
            do_set_prep(p[2], std::atoi(p[3].c_str()));
            post_state(safe_screen(screen));
            const char* lv = (p[3] == "2") ? "Heavy" : (p[3] == "1") ? "Standard" : "None";
            post_toast("ok", std::string("Match prep for ") + p[2] + " set to " + lv);
        } else if (verb == "agentauto" && p.size() == 4) { // act:agentauto:<map>:<screen>
            do_clear_agents(p[2]);
            post_state(safe_screen(screen));
            post_toast("ok", p[2] + " agents reset to Auto");
        } else if (verb == "agentset" && p.size() == 5) {  // act:agentset:<map>:<a1,..,a5>:<screen>
            bool ok = do_agent_set(p[2], p[3]);
            post_state(safe_screen(screen));
            post_toast(ok ? "ok" : "warn", ok ? (p[2] + " comp set") : "Pick 5 distinct valid agents");
        } else if (verb == "addbrief" && p.size() == 9) {
            // act:addbrief:<name>:<role>:<ageMin>:<ageMax>:<minPot>:<region>:<screen>
            bool ok = do_add_brief(p[2], std::atoi(p[3].c_str()), std::atoi(p[4].c_str()),
                                   std::atoi(p[5].c_str()), std::atoi(p[6].c_str()), p[7]);
            post_state(safe_screen(screen));
            post_toast(ok ? "ok" : "warn", ok ? "Scouting brief created" : "Could not create the brief");
        } else if (verb == "clearbrief" && p.size() == 4) {   // act:clearbrief:<idx>:<screen>
            do_clear_brief(std::atoi(p[2].c_str()));
            post_state(safe_screen(screen));
            post_toast("ok", "Brief removed");
        } else if (verb == "bench" && p.size() == 5) {     // act:bench:<idA>:<idB>:<screen>
            std::uint64_t a = std::strtoull(p[2].c_str(), nullptr, 10);
            std::uint64_t b = std::strtoull(p[3].c_str(), nullptr, 10);
            bool ok = do_bench(a, b);
            post_state(safe_screen(screen));
            post_toast(ok ? "ok" : "warn", ok ? "Lineup updated" : "Could not swap those players");
        } else if (verb == "setattr" && p.size() == 6) {   // act:setattr:<id>:<attr>:<value>:<screen>
            bool ok = do_setattr(std::strtoull(p[2].c_str(), nullptr, 10), p[3], std::atoi(p[4].c_str()));
            post_state(safe_screen(screen));
            post_toast(ok ? "ok" : "warn", ok ? "Attribute updated (god mode)" : "Edit failed");
        } else if (verb == "setpot" && p.size() == 5) {    // act:setpot:<id>:<value>:<screen>
            bool ok = do_setpot(std::strtoull(p[2].c_str(), nullptr, 10), std::atoi(p[3].c_str()));
            post_state(safe_screen(screen));
            post_toast(ok ? "ok" : "warn", ok ? "Potential updated (god mode)" : "Edit failed");
        } else if (verb == "setname" && p.size() == 5) {   // act:setname:<id>:<name>:<screen>
            bool ok = do_setname(std::strtoull(p[2].c_str(), nullptr, 10), p[3]);
            post_state(safe_screen(screen));
            post_toast(ok ? "ok" : "warn", ok ? "Name updated (god mode)" : "Edit failed");
        } else if (verb == "staffset" && p.size() == 6) {  // act:staffset:<id>:<field>:<value>:<screen>
            bool ok = do_staffset(std::strtoull(p[2].c_str(), nullptr, 10), p[3], p[4]);
            post_state(safe_screen(screen));
            post_toast(ok ? "ok" : "warn", ok ? "Staff updated (god mode)" : "Edit failed");
        } else if (verb == "teamset" && p.size() == 5) {   // act:teamset:<field>:<value>:<screen>
            bool ok = do_teamset(p[2], p[3]);
            post_state(safe_screen(screen));
            post_toast(ok ? "ok" : "warn", ok ? "Club updated (god mode)" : "Edit failed");
        } else if (verb == "signfa" && p.size() == 4) {    // act:signfa:<faId>:<screen>  (quick-sign at their ask)
            std::uint64_t fid = std::strtoull(p[2].c_str(), nullptr, 10);
            std::string block = user_reputation_block(fid);
            bool ok = block.empty() && do_signfa(fid);
            post_state(safe_screen(screen));
            post_toast(ok ? "ok" : "warn", ok ? "Free agent signed"
                       : (!block.empty() ? block
                          : !g_signfaReason.empty() ? g_signfaReason
                          : "Could not sign that player"));
        } else if (verb == "signfa" && (p.size() == 7 || p.size() == 8)) {
            // act:signfa:<id>:<years>:<role>:<bonus>[:<amount>]:<screen> — negotiated
            // offer; the amount-less legacy shape means "meet their ask".
            std::uint64_t fid = std::strtoull(p[2].c_str(), nullptr, 10);
            int amount = (p.size() == 8) ? std::atoi(p[6].c_str()) : -1;
            std::string block = user_reputation_block(fid);
            bool ok = block.empty() && do_signfa(fid, std::atoi(p[3].c_str()),
                                std::atoi(p[4].c_str()), std::atoi(p[5].c_str()), amount);
            post_state(safe_screen(screen));
            post_toast(ok ? "ok" : "warn", ok ? "Deal agreed \xE2\x80\x94 free agent signed"
                       : (!block.empty() ? block
                          : !g_signfaReason.empty() ? g_signfaReason
                          : "Could not sign that player"));
        } else if (verb == "hirestaff" && p.size() == 5) { // act:hirestaff:<role>:<id>:<screen>
            bool ok = do_hire_staff(p[2], std::strtoull(p[3].c_str(), nullptr, 10));
            post_state(safe_screen(screen));
            const std::string r = (p[2] == "coach") ? "Coach" : (p[2] == "scout") ? "Scout"
                                : (p[2] == "analyst") ? "Analyst" : "Staff";
            post_toast(ok ? "ok" : "warn", ok ? (r + " hired") : "Could not hire that staffer");
        } else if ((verb == "addbadge" || verb == "rmbadge") && p.size() == 5) {
            // act:addbadge:<id>:<badge name>:<screen> / act:rmbadge:<id>:<badge name>:<screen>
            bool on = (verb == "addbadge");
            bool ok = do_set_badge(std::strtoull(p[2].c_str(), nullptr, 10), p[3], on);
            post_state(safe_screen(screen));
            post_toast(ok ? "ok" : "warn",
                       ok ? (std::string("Badge ") + (on ? "added (god mode)" : "removed (god mode)"))
                          : "Badge edit failed");
        } else if (verb == "buyplayer" && p.size() == 6) {
            // act:buyplayer:<seller team>:<targetId>:<feeK>:<screen> — bid for a rival's player.
            auto o = do_buy_player(p[2], std::strtoull(p[3].c_str(), nullptr, 10),
                                   std::atoi(p[4].c_str()));
            post_state(safe_screen(screen));
            bool ok = (o.code == vlr::GameManager::TransferBidOutcome::Accepted);
            post_toast(ok ? "ok" : "warn",
                       o.message.empty() ? (ok ? "Transfer complete" : "Offer rejected") : o.message);
        } else if ((verb == "favorite" || verb == "unfavorite") && p.size() == 4) {
            bool on = (verb == "favorite");
            bool ok = do_set_favorite(std::strtoull(p[2].c_str(), nullptr, 10), on);
            post_state(safe_screen(screen));
            post_toast(ok ? "ok" : "warn",
                       ok ? (on ? "Added to favorites" : "Removed from favorites") : "Action failed");
        } else if ((verb == "watch" || verb == "unwatch") && p.size() == 4) {
            bool on = (verb == "watch");
            bool ok = do_set_watch(std::strtoull(p[2].c_str(), nullptr, 10), on);
            post_state(safe_screen(screen));
            post_toast(ok ? "ok" : "warn",
                       ok ? (on ? "Added to watchlist" : "Removed from watchlist") : "Action failed");
        } else if (verb == "scout" && p.size() == 4) {
            bool ok = do_scout(std::strtoull(p[2].c_str(), nullptr, 10));
            post_state(safe_screen(screen));
            post_toast(ok ? "ok" : "warn", ok ? "Potential revealed" : "No scout credits, or already known");
        } else if (verb == "queuematch" && p.size() == 4) {
            bool ok = do_queue_match(std::strtoull(p[2].c_str(), nullptr, 10));
            post_state(safe_screen(screen));
            post_toast(ok ? "ok" : "warn", ok ? "Ranked match played" : "Could not queue a match");
        } else if (verb == "release" && p.size() == 4) {       // act:release:<id>:<screen>
            bool ok = do_release(std::strtoull(p[2].c_str(), nullptr, 10));
            post_state(safe_screen(screen));
            post_toast(ok ? "ok" : "warn", ok ? "Player released to free agency" : "Could not release that player");
        } else if (verb == "firestaff" && p.size() == 4) {     // act:firestaff:<role>:<screen>
            bool ok = g_gm && g_gm->user_fire_staff(p[2]);
            post_state(safe_screen(screen));
            const std::string r = (p[2] == "coach") ? "Coach" : (p[2] == "scout") ? "Scout"
                                : (p[2] == "analyst") ? "Analyst" : "Staff";
            post_toast(ok ? "ok" : "warn", ok ? (r + " released") : "No staffer in that slot");
        } else if (verb == "watchstatus" && p.size() == 5) {   // act:watchstatus:<id>:<0|1|2>:<screen>
            bool ok = do_set_watch_status(std::strtoull(p[2].c_str(), nullptr, 10), std::atoi(p[3].c_str()));
            post_state(safe_screen(screen));
            post_toast(ok ? "ok" : "warn", ok ? "Watchlist status updated" : "Not on your watchlist");
        } else if (verb == "watchnote" && p.size() >= 5) {     // act:watchnote:<id>:<note...>:<screen>
            std::string note = p[3];
            for (std::size_t i = 4; i + 1 < p.size(); ++i) note += ":" + p[i];   // rejoin a note that contains ':'
            bool ok = do_set_watch_note(std::strtoull(p[2].c_str(), nullptr, 10), note);
            post_state(safe_screen(screen));
            post_toast(ok ? "ok" : "warn", ok ? "Note saved" : "Not on your watchlist");
        } else if (verb == "commission" && p.size() == 5) {
            // act:commission:<playerId>:<moneyK>:<screen> — task the scout over 1-3 weeks.
            int days = do_commission(std::strtoull(p[2].c_str(), nullptr, 10), std::atoi(p[3].c_str()));
            post_state(safe_screen(screen));
            if (days > 0) {
                int wk = (days + 6) / 7;
                post_toast("ok", std::string("Scout commissioned — report in ~") + std::to_string(wk) +
                                 (wk > 1 ? " weeks" : " week"));
            } else {
                post_toast("warn", "Could not commission that scout");
            }
        } else if (verb == "choosesponsor" && p.size() == 4) {
            // act:choosesponsor:<idx>:<screen> — pick a preseason sponsor (idx -1 = decline).
            bool ok = do_choose_sponsor(std::atoi(p[2].c_str()));
            post_state(safe_screen(screen));
            post_toast(ok ? "ok" : "warn", ok ? "Sponsor confirmed" : "No sponsor choice is pending");
        } else if (verb == "save" && p.size() == 4) {
            // act:save:<slot>:<screen> — screen "saves" refreshes the slot panel,
            // "quiet" is the autosave path (no toast, no re-push).
            std::string err;
            bool ok = g_gm && vlr::SaveGame::save(*g_gm, slot_path(p[2]), &err);
            if (screen == "quiet") { /* autosave: silent either way */ }
            else if (screen == "saves") {
                post_saves_list();
                post_toast(ok ? "ok" : "warn", ok ? "Game saved" : (err.empty() ? "Save failed" : err));
            } else {
                post_state(safe_screen(screen));
                post_toast(ok ? "ok" : "warn", ok ? "Game saved" : (err.empty() ? "Save failed" : err));
            }
        } else if (verb == "load" && p.size() == 4) {
            // act:load:<slot>:<screen> — restore a world, clear view transients, land
            // on the requested screen (dashboard from the menu).
            std::string err;
            bool ok = g_gm && vlr::SaveGame::load(*g_gm, slot_path(p[2]), &err);
            if (ok) {
                g_lastUserMatch.reset();
                g_lastSeriesLen = 0;
                g_lastUserSeries.clear();
                g_profileTeam.clear();
                g_profilePlayerId = 0;
                g_profileStaffId  = 0;
                g_compRegion.clear();
                g_compTier = 0;
                post_state(safe_screen(screen == "saves" ? "dashboard" : screen));
                post_toast("ok", "Save loaded \xE2\x80\x94 welcome back");
            } else {
                post_toast("warn", err.empty() ? "Could not load that save" : err);
            }
        } else if (verb == "teamtalk" && p.size() == 4) {
            // act:teamtalk:<0-3>:<screen> — one-shot pre-match talk (consumed by the
            // next user series). 1=Calm 2=Fire-up 3=Focus, 0=clear.
            if (g_gm) {
                int t = std::atoi(p[2].c_str());
                g_gm->user_team_talk = (t < 0 || t > 3) ? 0 : t;
            }
            post_state(safe_screen(screen));
            const int tt = g_gm ? g_gm->user_team_talk : 0;
            post_toast("ok", tt == 1 ? "Team talk set: stay calm and play your game"
                          : tt == 2 ? "Team talk set: all gas \xE2\x80\x94 fire them up"
                          : tt == 3 ? "Team talk set: lock in and focus"
                                     : "Team talk cleared");
        } else if (verb == "automanage" && p.size() == 4) {
            // act:automanage:<0|1>:<screen> — delegate roster management to the AI GM
            // (mid-season cuts/replacements + the offseason pass run like an AI club).
            if (g_gm) g_gm->user_auto_manage = (std::atoi(p[2].c_str()) != 0);
            post_state(safe_screen(screen));
            post_toast("ok", (g_gm && g_gm->user_auto_manage)
                ? "AI GM enabled — the AI now manages your roster"
                : "AI GM disabled — you're back in charge of the roster");
        } else if (verb == "mailallread" && p.size() == 3) {
            // act:mailallread:<screen> — mark every mailbox message read.
            if (g_gm) for (auto& m : g_gm->mailbox) m.read = true;
            post_state(safe_screen(screen));
            post_toast("ok", "All mail marked read");
        } else if (verb == "mailread" && p.size() == 5) {
            // act:mailread:<id>:<0|1>:<screen> — mark a message read/unread (silent re-push).
            do_mail_set_read(std::atoi(p[2].c_str()), std::atoi(p[3].c_str()) != 0);
            post_state(safe_screen(screen));
        } else if (verb == "mailstar" && p.size() == 5) {
            // act:mailstar:<id>:<0|1>:<screen> — star/unstar (marks it protected from cap-drop).
            bool ok = do_mail_set_star(std::atoi(p[2].c_str()), std::atoi(p[3].c_str()) != 0);
            post_state(safe_screen(screen));
            post_toast(ok ? "ok" : "warn", std::atoi(p[3].c_str()) != 0 ? "Message starred" : "Star removed");
        } else if (verb == "maildelete" && p.size() == 4) {
            // act:maildelete:<id>:<screen> — remove a message from the inbox.
            bool ok = do_mail_delete(std::atoi(p[2].c_str()));
            post_state(safe_screen(screen));
            post_toast(ok ? "ok" : "warn", ok ? "Message deleted" : "Could not delete that message");
        } else if (verb == "newgame" && p.size() == 8) {
            // act:newgame:<name>:<region>:<tierIdx>:<diffx100>:<colorHex>:<screen>
            bool ok = do_newgame(p[2], p[3], std::atoi(p[4].c_str()), std::atoi(p[5].c_str()), p[6]);
            post_state(ok ? "dashboard" : safe_screen(screen));
            post_toast(ok ? "ok" : "warn",
                       ok ? (std::string("New career started: ") + p[2]) : "Could not start a new game");
        } else if (verb == "resign" && p.size() == 9) {
            // act:resign:<id>:<years>:<amount>:<bonus>:<starter01>:<role>:<screen>
            int r = do_resign(std::strtoull(p[2].c_str(), nullptr, 10), std::atoi(p[3].c_str()),
                              std::atoi(p[4].c_str()), std::atoi(p[5].c_str()),
                              std::atoi(p[6].c_str()) != 0, std::atoi(p[7].c_str()));
            post_state(safe_screen(screen));
            post_toast(r == 1 ? "ok" : "warn",
                       r == 1 ? "Contract extension signed"
                     : r == 0 ? "Player rejected those terms" : "Could not re-sign that player");
        } else {
            post_state(safe_screen(screen));               // unknown action: just refresh
        }
    }
}

// ---- window + webview ----------------------------------------------------
std::wstring resolve_uipoc_folder() {
    wchar_t exePath[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring exe(exePath);
    size_t slash = exe.find_last_of(L"\\/");
    std::wstring dir = (slash == std::wstring::npos) ? exe : exe.substr(0, slash);
    std::wstring rel = dir + L"\\..\\..\\ui-poc";
    wchar_t full[MAX_PATH] = {0};
    if (GetFullPathNameW(rel.c_str(), MAX_PATH, full, nullptr)) return std::wstring(full);
    return rel;
}
std::wstring user_data_folder() {
    wchar_t* base = nullptr; size_t len = 0; std::wstring folder;
    if (_wdupenv_s(&base, &len, L"LOCALAPPDATA") == 0 && base) {
        folder = std::wstring(base) + L"\\Valosim\\WebView2"; free(base);
    } else { folder = L".\\Valosim_WebView2"; }
    return folder;
}
void resize_to_client() {
    if (!g_controller) return;
    RECT rc; GetClientRect(g_hwnd, &rc);
    g_controller->put_Bounds(rc);
}

// Self-capture (--shot): write the rendered surface to PNG, then quit. The
// shell pulls its data over the bridge, so a non-empty shot proves the bridge
// works end-to-end. Doubles as the per-screen screenshot tool for verification.
void do_capture() {
    if (!g_webview || g_shotPath.empty()) { PostQuitMessage(2); return; }
    ComPtr<IStream> stream;
    HRESULT hr = SHCreateStreamOnFileEx(
        g_shotPath.c_str(), STGM_CREATE | STGM_WRITE | STGM_SHARE_DENY_WRITE,
        FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, &stream);
    if (FAILED(hr) || !stream) { PostQuitMessage(3); return; }
    g_webview->CapturePreview(
        COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, stream.Get(),
        Callback<ICoreWebView2CapturePreviewCompletedHandler>(
            [stream](HRESULT result) -> HRESULT {
                PostQuitMessage(SUCCEEDED(result) ? 0 : 4); return S_OK;
            }).Get());
}

// Drive the shell's advance()/go() from the host (verification only).
void trigger_advance() {
    if (!g_webview || g_advMode.empty()) return;
    std::wstring js = L"window.__advance && window.__advance('" + g_advMode + L"')";
    g_webview->ExecuteScript(js.c_str(),
        Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
            [](HRESULT, PCWSTR) -> HRESULT { return S_OK; }).Get());
}
void trigger_go() {
    if (!g_webview || g_goScreen.empty()) return;
    std::wstring js = L"window.__go && window.__go('" + to_wide(safe_screen(to_utf8(g_goScreen))) + L"')";
    g_webview->ExecuteScript(js.c_str(),
        Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
            [](HRESULT, PCWSTR) -> HRESULT { return S_OK; }).Get());
}
void trigger_js() {
    if (!g_webview || g_js.empty()) return;
    g_webview->ExecuteScript(g_js.c_str(),
        Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
            [](HRESULT, PCWSTR) -> HRESULT { return S_OK; }).Get());
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE:  resize_to_client(); return 0;
        case WM_TIMER:
            if (wp == 1) {
                KillTimer(hwnd, 1);
                if (!g_advMode.empty())      { trigger_advance(); SetTimer(hwnd, 2, 3000, nullptr); }
                else if (!g_goScreen.empty()) { trigger_go();     SetTimer(hwnd, 2, 3000, nullptr); }
                else if (!g_cmd.empty())     { handle_command(to_utf8(g_cmd)); if (!g_cmd2.empty()) handle_command(to_utf8(g_cmd2)); SetTimer(hwnd, 2, 3000, nullptr); }
                else if (!g_js.empty())      { trigger_js(); SetTimer(hwnd, 2, 3000, nullptr); }
                else do_capture();
            } else if (wp == 2) {
                KillTimer(hwnd, 2);
                do_capture();   // capture after the advance/nav round-trip + re-render
            }
            return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HRESULT on_controller_created(HRESULT result, ICoreWebView2Controller* controller) {
    if (FAILED(result) || !controller) {
        MessageBoxW(g_hwnd, L"Failed to create the WebView2 controller.", L"Valosim", MB_ICONERROR);
        return result;
    }
    g_controller = controller;
    g_controller->get_CoreWebView2(&g_webview);
    resize_to_client();

    ComPtr<ICoreWebView2_3> wv3;
    if (SUCCEEDED(g_webview.As(&wv3)) && wv3) {
        wv3->SetVirtualHostNameToFolderMapping(
            L"valosim.local", g_folder.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
    }

    // web -> C++ command bridge.
    g_webview->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                LPWSTR raw = nullptr;
                if (SUCCEEDED(args->TryGetWebMessageAsString(&raw)) && raw) {
                    std::wstring w(raw); CoTaskMemFree(raw);
                    handle_command(to_utf8(w));
                }
                return S_OK;
            }).Get(), &g_msgToken);

    // Self-capture: once the chosen screen has loaded + settled, shoot it.
    if (!g_shotPath.empty()) {
        g_webview->add_NavigationCompleted(
            Callback<ICoreWebView2NavigationCompletedEventHandler>(
                [](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                    SetTimer(g_hwnd, 1, 3000, nullptr);   // bridge round-trip + entrance anims
                    return S_OK;
                }).Get(), &g_navToken);
    }

    // The shell can be deep-linked to a screen via the URL hash (used by --shot).
    std::wstring url = g_navUrl;
    if (!g_shotPath.empty()) url += L"#" + g_shotScreen;
    g_webview->Navigate(url.c_str());
    return S_OK;
}

HRESULT on_env_created(HRESULT result, ICoreWebView2Environment* env) {
    if (FAILED(result) || !env) {
        MessageBoxW(g_hwnd,
            L"Failed to create the WebView2 environment.\nIs the WebView2 runtime installed?",
            L"Valosim", MB_ICONERROR);
        return result;
    }
    env->CreateCoreWebView2Controller(
        g_hwnd,
        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [](HRESULT hr, ICoreWebView2Controller* c) -> HRESULT {
                return on_controller_created(hr, c);
            }).Get());
    return S_OK;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    HRESULT comhr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // --shot <path> [screen]  => headless verification: render <screen>, capture, exit.
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv) {
            for (int i = 1; i < argc; ++i) {
                if (wcscmp(argv[i], L"--shot") == 0 && i + 1 < argc) {
                    g_shotPath = argv[i + 1]; ++i;
                    if (i + 1 < argc && argv[i + 1][0] != L'-') { g_shotScreen = argv[i + 1]; ++i; }
                } else if (wcscmp(argv[i], L"--adv") == 0 && i + 1 < argc) {
                    g_advMode = argv[i + 1]; ++i;
                } else if (wcscmp(argv[i], L"--go") == 0 && i + 1 < argc) {
                    g_goScreen = argv[i + 1]; ++i;
                } else if (wcscmp(argv[i], L"--cmd") == 0 && i + 1 < argc) {
                    g_cmd = argv[i + 1]; ++i;
                } else if (wcscmp(argv[i], L"--js") == 0 && i + 1 < argc) {
                    g_js = argv[i + 1]; ++i;
                } else if (wcscmp(argv[i], L"--cmd2") == 0 && i + 1 < argc) {
                    g_cmd2 = argv[i + 1]; ++i;
                }
            }
            LocalFree(argv);
        }
    }

    build_world();              // one live world for the session
    g_folder = resolve_uipoc_folder();

    WNDCLASSEXW wc = {0};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = L"ValosimWebHost";
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"Valosim — VLR Manager (Web)",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1480, 940,
        nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) return 1;
    // Normal play starts MAXIMIZED (user request). Headless verification (--shot)
    // keeps the fixed 1480x940 surface so captures stay byte-comparable.
    ShowWindow(g_hwnd, g_shotPath.empty() ? SW_SHOWMAXIMIZED
                                          : (nCmdShow ? nCmdShow : SW_SHOW));
    UpdateWindow(g_hwnd);

    std::wstring udf = user_data_folder();
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, udf.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT r, ICoreWebView2Environment* e) -> HRESULT { return on_env_created(r, e); }).Get());
    if (FAILED(hr)) {
        MessageBoxW(g_hwnd,
            L"Could not start WebView2. Install the Microsoft Edge WebView2 Runtime.",
            L"Valosim", MB_ICONERROR);
        return 1;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }

    g_webview.Reset();
    g_controller.Reset();
    g_gm.reset();
    if (SUCCEEDED(comhr)) CoUninitialize();
    return static_cast<int>(msg.wParam);
}
