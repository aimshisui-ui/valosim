// gui_match.inc - unity-build fragment #included by gui_main.cpp (inside its
// anonymous namespace). Match viewer: Open* helpers + score banner, round
// timeline, team scoreboard, live match modal. NOT a standalone TU.


// Spin up a fresh simulated match from the Watch screen / brackets and
// hand it to the Live Match viewer.
// Run a full BO3/BO5 series (friendly — no career pollution) and open the
// live viewer with every map navigable. Used for "Watch" buttons on the
// tournament bracket so users actually see the series, not just one map.
void OpenLiveMatchSeries(AppState& s, vlr::TeamPtr a, vlr::TeamPtr b,
                          const std::string& event, int best_of) {
    if (!a || !b) return;
    auto ita = s.gm.solo_qs.find(a->region);
    auto itb = s.gm.solo_qs.find(b->region);
    if (ita == s.gm.solo_qs.end() || !ita->second) return;
    if (itb == s.gm.solo_qs.end() || !itb->second) return;
    std::vector<vlr::PlayerPtr> fa1 = ita->second->global_ladder();
    std::vector<vlr::PlayerPtr> fa2 = itb->second->global_ladder();
    a->auto_fill_roster(fa1);
    b->auto_fill_roster(fa2);
    if (a->roster.size() < 5 || b->roster.size() < 5) return;

    auto series = std::make_shared<vlr::Series>(a, b, best_of, event);
    std::vector<vlr::RecordedMatchPtr> recordings;
    while (!series->is_over()) {
        const auto& M = vlr::maps()[static_cast<std::size_t>(
            vlr::rng().irange(0, static_cast<int>(vlr::maps().size()) - 1))];
        vlr::Match m(a, b, M, /*is_solo_q=*/false, event, /*friendly=*/true);
        m.play();
        series->add_match_data(m);
        if (auto rec = series->last_recording()) recordings.push_back(rec);
    }
    if (recordings.empty()) return;
    s.live.match.reset();
    s.live.series = recordings;
    s.live.current_map_idx = 0;
    s.live.recording = recordings.front();
    s.live.round_cursor = 0;
    s.live.event_cursor = 0;
    s.live.auto_play = false;
    s.live.auto_timer = 0.0f;
    s.live.open = true;
}

void OpenLiveMatch(AppState& s, vlr::TeamPtr a, vlr::TeamPtr b, const std::string& event) {
    if (!a || !b) return;
    auto ita = s.gm.solo_qs.find(a->region);
    auto itb = s.gm.solo_qs.find(b->region);
    if (ita == s.gm.solo_qs.end() || !ita->second) return;
    if (itb == s.gm.solo_qs.end() || !itb->second) return;
    std::vector<vlr::PlayerPtr> fa1 = ita->second->global_ladder();
    std::vector<vlr::PlayerPtr> fa2 = itb->second->global_ladder();
    a->auto_fill_roster(fa1);
    b->auto_fill_roster(fa2);
    if (a->roster.size() < 5 || b->roster.size() < 5) return;

    const auto& M = vlr::maps()[static_cast<std::size_t>(
        vlr::rng().irange(0, static_cast<int>(vlr::maps().size()) - 1))];
    // friendly=true: this is a demo/Watch match; stats won't pollute careers.
    auto m = std::make_shared<vlr::Match>(a, b, M, false, event, /*friendly=*/true);
    m->play();
    s.live.match     = m;
    s.live.recording = vlr::make_recorded_match(*m);
    // CRITICAL: clear any leftover series state from a previous viewer
    // open. Without this, OpenLiveMatch (single map) inherits the series
    // vector + current_map_idx from a prior BO3, which causes "Map 2/2"
    // to display and crashes when the user switches maps to a stale ptr.
    s.live.series.clear();
    s.live.current_map_idx = 0;
    // Start at round 0 — viewers want to watch the match unfold from the
    // beginning. They can hit Auto Play or Skip to End if they prefer the
    // result first.
    s.live.round_cursor = 0;
    s.live.event_cursor = 0;
    s.live.auto_play = false;
    s.live.auto_timer = 0.0f;
    s.live.open = true;
}

// Replay an already-recorded match (e.g. from match history). No new
// simulation; we walk the saved round_history.
void OpenReplay(AppState& s, vlr::RecordedMatchPtr rec) {
    if (!rec) return;
    s.live.match.reset();
    s.live.recording = rec;
    s.live.series.clear();
    s.live.current_map_idx = 0;
    // Start at round 0 so the user can watch the whole match play out.
    s.live.round_cursor = 0;
    s.live.event_cursor = 0;
    s.live.auto_play = false;
    s.live.auto_timer = 0.0f;
    s.live.open = true;
}

// Open a multi-map series (BO3/BO5). Starts on Map 1. The Live Match
// modal exposes Next/Prev Map buttons to navigate.
void OpenReplaySeries(AppState& s, const std::vector<vlr::RecordedMatchPtr>& maps) {
    if (maps.empty()) return;
    s.live.match.reset();
    s.live.series = maps;
    s.live.current_map_idx = 0;
    s.live.recording = maps.front();
    s.live.round_cursor = 0;
    s.live.event_cursor = 0;
    s.live.auto_play = false;
    s.live.auto_timer = 0.0f;
    s.live.open = true;
}

// Score banner: a clean pro-broadcast header. Big team tags with team-color
// accent strips, a large centered scoreline, the current map + map-in-series
// (NEVER the series total), and a LIVE/FINAL status pill. Leading team is
// emphasised, trailing dimmed. Every element is clamped to the modal width
// so nothing can bleed off-screen.
void DrawScoreBanner(const vlr::RecordedMatch& rec, int t1_score_now, int t2_score_now,
                     int round_idx, std::size_t total_rounds) {
    ImGui::BeginChild("##banner", ImVec2(0, 122), ImGuiChildFlags_Border,
                      ImGuiWindowFlags_NoScrollbar |
                      ImGuiWindowFlags_NoScrollWithMouse);
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 sz = ImGui::GetContentRegionAvail();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Use team-specific colors when available; fall back to blue/red.
    auto team_col = [](const vlr::TeamPtr& t, ImU32 fallback) {
        if (!t) return fallback;
        return IM_COL32(t->color_primary_r, t->color_primary_g, t->color_primary_b, 0xFF);
    };
    vlr::TeamPtr t1 = rec.team1.lock();
    vlr::TeamPtr t2 = rec.team2.lock();
    ImU32 c1 = team_col(t1, kVlrBlue);
    ImU32 c2 = team_col(t2, kVlrRed);
    bool t1_leading = (t1_score_now > t2_score_now);
    bool t2_leading = (t2_score_now > t1_score_now);

    auto dim = [](ImU32 c, float a) {
        ImColor col(c);
        col.Value.x *= a; col.Value.y *= a; col.Value.z *= a;
        return (ImU32)col;
    };

    float x0 = origin.x, x1 = origin.x + sz.x;
    float y0 = origin.y, y1 = origin.y + sz.y;
    float half = sz.x * 0.5f;
    float center_w = std::min(150.0f, sz.x * 0.22f);
    float left_end  = origin.x + half - center_w * 0.5f;
    float right_beg = origin.x + half + center_w * 0.5f;

    // Deep base + side washes. Leading side gets a stronger team tint.
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), kBgDeep, 8.0f);
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(left_end, y1),
                      dim(c1, t1_leading ? 0.34f : 0.14f), 8.0f);
    dl->AddRectFilled(ImVec2(right_beg, y0), ImVec2(x1, y1),
                      dim(c2, t2_leading ? 0.34f : 0.14f), 8.0f);
    // Team-color accent strips along the outer top+bottom edges.
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(left_end, y0 + 4), c1);
    dl->AddRectFilled(ImVec2(x0, y1 - 4), ImVec2(left_end, y1), c1);
    dl->AddRectFilled(ImVec2(right_beg, y0), ImVec2(x1, y0 + 4), c2);
    dl->AddRectFilled(ImVec2(right_beg, y1 - 4), ImVec2(x1, y1), c2);

    // Center scoreline panel.
    dl->AddRectFilled(ImVec2(left_end, y0), ImVec2(right_beg, y1),
                      IM_COL32(0x06, 0x09, 0x10, 0xFF), 6.0f);
    dl->AddRect(ImVec2(left_end, y0), ImVec2(right_beg, y1),
                kBorderStrong, 6.0f, 0, 1.0f);

    // Big centered score.
    char sb[16];
    ImGui::PushFont(g_font_kpi);
    std::snprintf(sb, sizeof(sb), "%d", t1_score_now);
    ImVec2 ssz1 = ImGui::CalcTextSize(sb);
    dl->AddText(ImVec2(origin.x + half - 22.0f - ssz1.x, y0 + 34),
                t1_leading ? c1 : kVlrText, sb);
    std::snprintf(sb, sizeof(sb), "%d", t2_score_now);
    dl->AddText(ImVec2(origin.x + half + 22.0f, y0 + 34),
                t2_leading ? c2 : kVlrText, sb);
    ImGui::PopFont();
    ImGui::PushFont(g_font_h2);
    ImVec2 dsz = ImGui::CalcTextSize("-");
    dl->AddText(ImVec2(origin.x + half - dsz.x * 0.5f, y0 + 48), kVlrSub, "-");
    ImGui::PopFont();

    // Helper: draw a string clamped to a max width with ellipsis.
    auto clamped = [&](ImFont* f, float fs, ImVec2 at, ImU32 col,
                       const std::string& txt, float max_w, bool right_align) {
        ImGui::PushFont(f);
        std::string t = txt;
        ImVec2 m = ImGui::CalcTextSize(t.c_str());
        if (m.x > max_w && t.size() > 1) {
            while (t.size() > 1 &&
                   ImGui::CalcTextSize((t + "\xE2\x80\xA6").c_str()).x > max_w)
                t.pop_back();
            t += "\xE2\x80\xA6";
            m = ImGui::CalcTextSize(t.c_str());
        }
        float px = right_align ? (at.x - m.x) : at.x;
        dl->AddText(f, fs, ImVec2(px, at.y), col, t.c_str());
        ImGui::PopFont();
        return m.x;
    };
    float h1fs = g_font_h1 ? g_font_h1->FontSize : 30.0f;
    float h2fs = g_font_h2 ? g_font_h2->FontSize : 22.0f;
    float smfs = g_font_small ? g_font_small->FontSize : 13.0f;
    float side_max = (left_end - x0) - 36.0f;
    if (side_max < 60.0f) side_max = 60.0f;

    // === Team 1 (left) ===
    {
        std::string tag = (t1 && !t1->tag.empty()) ? t1->tag : rec.blue_name;
        clamped(g_font_h1, h1fs, ImVec2(x0 + 18, y0 + 16),
                t1_leading ? c1 : kVlrText, tag, side_max, false);
        clamped(g_font_h2, h2fs, ImVec2(x0 + 18, y0 + 16 + h1fs + 2),
                t1_leading ? kVlrText : kVlrSub, rec.blue_name, side_max, false);
        std::string sub = (t1 ? t1->region : std::string()) + "  \xE2\x80\xA2  "
                          + rec.map_name;
        clamped(g_font_small, smfs, ImVec2(x0 + 18, y1 - smfs - 10),
                kVlrSub, sub, side_max, false);
    }

    // === Team 2 (right, right-aligned) ===
    {
        std::string tag = (t2 && !t2->tag.empty()) ? t2->tag : rec.red_name;
        clamped(g_font_h1, h1fs, ImVec2(x1 - 18, y0 + 16),
                t2_leading ? c2 : kVlrText, tag, side_max, true);
        clamped(g_font_h2, h2fs, ImVec2(x1 - 18, y0 + 16 + h1fs + 2),
                t2_leading ? kVlrText : kVlrSub, rec.red_name, side_max, true);
        // Spoiler-free: show "Round N" only, never the total.
        char rb[48];
        std::snprintf(rb, sizeof(rb), "%s  \xE2\x80\xA2  Round %d",
                      t2 ? t2->region.c_str() : "", round_idx);
        clamped(g_font_small, smfs, ImVec2(x1 - 18, y1 - smfs - 10),
                kVlrSub, rb, side_max, true);
    }

    // Event name centered under the scoreline (clamped to the center panel).
    clamped(g_font_small, smfs,
            ImVec2(origin.x + half, y1 - smfs - 10), kTextFaint,
            rec.event, sz.x - side_max * 2.0f - 24.0f, false);

    // LIVE / FINAL status pill in the center, just above the scoreline base.
    bool is_final = (round_idx >= (int)total_rounds);
    const char* status = is_final ? "FINAL" : "LIVE";
    ImU32 status_col = is_final ? kVlrSub : kVlrGreen;
    ImGui::PushFont(g_font_small);
    ImVec2 stsz = ImGui::CalcTextSize(status);
    float pw = stsz.x + 18.0f;
    ImVec2 ptl(origin.x + half - pw * 0.5f, y1 - 24);
    ImVec2 pbr(origin.x + half + pw * 0.5f, y1 - 6);
    dl->AddRectFilled(ptl, pbr,
                      is_final ? kSurface : IM_COL32(0x12, 0x2A, 0x18, 0xFF), 9.0f);
    dl->AddRect(ptl, pbr, status_col, 9.0f, 0, 1.0f);
    dl->AddText(ImVec2(origin.x + half - stsz.x * 0.5f, y1 - 22),
                status_col, status);
    ImGui::PopFont();

    ImGui::EndChild();
}

// Round timeline strip: split-half presentation with momentum streaks,
// half-divider, OT shading, and click-to-jump. Each round square is colored
// by the team that won; consecutive wins for the same side show a top
// stripe of growing intensity to indicate momentum.
void DrawRoundTimeline(LiveMatchState& live,
                       const std::vector<vlr::RoundLog>& rounds,
                       const vlr::RecordedMatch& rec) {
    if (rounds.empty()) {
        ImGui::Dummy(ImVec2(0, 30));
        return;
    }
    // Outer frame is FIXED to the modal width; the round strip itself gets
    // its own narrow horizontally-scrolling child so a long match (OT) can
    // never push the rest of the modal off-screen — only this strip scrolls.
    ImGui::BeginChild("##timeline", ImVec2(0, 60), ImGuiChildFlags_Border,
                      ImGuiWindowFlags_HorizontalScrollbar |
                      ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Use team colors for visual identity rather than hard-coded blue/red.
    vlr::TeamPtr t1 = rec.team1.lock();
    vlr::TeamPtr t2 = rec.team2.lock();
    ImU32 c1 = t1
        ? IM_COL32(t1->color_primary_r, t1->color_primary_g, t1->color_primary_b, 0xFF)
        : kVlrBlue;
    ImU32 c2 = t2
        ? IM_COL32(t2->color_primary_r, t2->color_primary_g, t2->color_primary_b, 0xFF)
        : kVlrRed;

    int n = (int)rounds.size();
    // SPOILER GUARD: render only rounds that have been "played" up to
    // the cursor. Pre-rendering all `n` slots (even dimmed) leaks the
    // total round count — the user can count squares to figure out
    // whether the match went to OT before they've watched it. Keep
    // slot width FIXED so adding squares can't be inferred from layout
    // shifts either.
    int n_visible = std::clamp(live.round_cursor, 0, n);
    // Allow a single un-revealed slot for "current round" while in the
    // middle of play-by-play (so the gold ring has somewhere to land).
    // This is the same round_cursor that already drives the score banner,
    // so we're not leaking new info.
    if (n_visible < n) ++n_visible;
    if (n_visible == 0) {
        // No rounds played yet — just leave an empty strip.
        ImGui::EndChild();
        return;
    }
    float pad = 8.0f;
    float gap = 4.0f;
    // Fixed slot width — independent of total round count. Keep it FIXED;
    // scaling by (avail / n) would re-leak total round count via square
    // geometry. When the strip is wider than the visible region the child's
    // own horizontal scrollbar handles overflow (the rest of the modal is
    // unaffected — only this narrow strip scrolls).
    float w = 24.0f;
    float h = 30.0f;
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float strip_w = pad * 2.0f + n_visible * (w + gap);
    // Reserve the strip's full content width so the child can scroll to it.
    ImGui::Dummy(ImVec2(strip_w, 44.0f));
    float y = origin.y + (44.0f - h) * 0.5f;

    auto winner_is_t1 = [&](int i) -> bool {
        if (i == 0) return rounds[i].t1_score > 0;
        return rounds[i].t1_score > rounds[i - 1].t1_score;
    };

    // Half labels at the top — only show DEF / OT once we've reached
    // those rounds. Showing them ahead of time spoils that the match has
    // 12+ / 24+ rounds.
    ImGui::PushFont(g_font_small);
    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
    dl->AddText(ImVec2(origin.x + pad, origin.y + 3), kTextFaint, "ATK");
    if (n_visible > 12) {
        float x12 = origin.x + pad + 12 * (w + gap);
        dl->AddText(ImVec2(x12 + 4, origin.y + 3), kTextFaint, "DEF");
    }
    if (n_visible > 24) {
        float x24 = origin.x + pad + 24 * (w + gap);
        dl->AddText(ImVec2(x24 + 4, origin.y + 3), kTextFaint, "OT");
    }
    ImGui::PopStyleColor();
    ImGui::PopFont();

    int streak = 0;
    bool last_was_t1 = false;
    for (int i = 0; i < n_visible; ++i) {
        float x = origin.x + pad + i * (w + gap);
        bool t1_wins = winner_is_t1(i);
        ImU32 col = t1_wins ? c1 : c2;
        bool revealed = (i < live.round_cursor);

        // Track momentum streak (only across revealed rounds)
        if (revealed) {
            if (i == 0) { streak = 1; last_was_t1 = t1_wins; }
            else if (t1_wins == last_was_t1) ++streak;
            else { streak = 1; last_was_t1 = t1_wins; }
        }

        if (!revealed) {
            // Empty slot — hairline-framed placeholder, no color leak.
            dl->AddRectFilled(ImVec2(x, y + 6), ImVec2(x + w, y + h),
                              kBgDeep, 4.0f);
            dl->AddRect(ImVec2(x, y + 6), ImVec2(x + w, y + h),
                        kBorder, 4.0f, 0, 1.0f);
        } else {
            // Won round: deep base then the winning side's team color.
            dl->AddRectFilled(ImVec2(x, y + 6), ImVec2(x + w, y + h),
                              kBgDeep, 4.0f);
            dl->AddRectFilled(ImVec2(x, y + 6), ImVec2(x + w, y + h),
                              col, 4.0f);
            // Momentum streak indicator: brighter top stripe with streak.
            if (streak >= 2) {
                float intensity = std::min(1.0f, 0.45f + 0.16f * streak);
                ImColor streak_col = ImColor(col);
                streak_col.Value.x = std::min(1.0f, streak_col.Value.x * 1.35f);
                streak_col.Value.y = std::min(1.0f, streak_col.Value.y * 1.35f);
                streak_col.Value.z = std::min(1.0f, streak_col.Value.z * 1.35f);
                streak_col.Value.w = intensity;
                dl->AddRectFilled(ImVec2(x, y + 6), ImVec2(x + w, y + 10),
                                  (ImU32)streak_col, 2.0f);
            }
        }

        // Half / OT divider lines — only draw the divider when the
        // boundary round has been reached (otherwise we'd telegraph
        // "OT is coming" before round 24 is even played).
        if ((i == 12 || i == 24) && revealed) {
            dl->AddLine(ImVec2(x - gap * 0.5f, y - 2),
                        ImVec2(x - gap * 0.5f, y + h + 4),
                        kVlrGold, 1.5f);
        }

        // Pistol round indicator (rounds 0 and 12) — gold hairline frame.
        if (revealed && (i == 0 || i == 12)) {
            dl->AddRect(ImVec2(x, y + 6), ImVec2(x + w, y + h),
                        kVlrGold, 4.0f, 0, 1.4f);
        }

        // Current round highlight — electric accent ring.
        if (i == live.round_cursor - 1) {
            dl->AddRect(ImVec2(x - 2, y + 4), ImVec2(x + w + 2, y + h + 2),
                        kAccent, 5.0f, 0, 2.5f);
        }

        // Click hit-area to jump
        ImGui::SetCursorScreenPos(ImVec2(x, y + 6));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##rj", ImVec2(w, h - 6))) {
            live.round_cursor = i + 1;
            live.auto_play = false;
        }
        // Tooltip only on REVEALED slots — never spoil future round
        // outcomes by hovering an un-played slot. (We still let users
        // click the un-played slot to skip ahead, matching the existing
        // Skip-to-End / per-round-step semantics.)
        if (revealed && ImGui::IsItemHovered()) {
            vlr::TeamPtr tt1 = rec.team1.lock();
            vlr::TeamPtr tt2 = rec.team2.lock();
            const char* w1 = tt1 ? tt1->tag.c_str() : "T1";
            const char* w2 = tt2 ? tt2->tag.c_str() : "T2";
            ImGui::SetTooltip("Round %d  |  %s %d - %d %s\nWinner: %s",
                              rounds[i].round,
                              w1, rounds[i].t1_score, rounds[i].t2_score, w2,
                              t1_wins ? (tt1 ? tt1->name.c_str() : "T1")
                                      : (tt2 ? tt2->name.c_str() : "T2"));
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
}

// Compute a partial scoreboard from the round-by-round event log up to the
// given cursor (exclusive). Used by the Live Match viewer so the on-screen
// scoreboard never reveals stats from rounds that haven't been "played" yet.
//
// Accurate fields (derived directly from the event log):
//     k, d, fb, fd, survivals, rounds_with_kast
// Approximated (event log doesn't store per-event values):
//     a, damage, hs_hits         — scaled as final * (cursor / total_rounds)
//     rating                      — rough K/D-based live estimate while
//                                   running, true VLR rating once complete
std::unordered_map<vlr::Player*, vlr::PlayerMatchStats>
compute_partial_stats(const vlr::RecordedMatch& rec, int cursor) {
    std::unordered_map<vlr::Player*, vlr::PlayerMatchStats> out;
    if (!rec.round_history) return out;

    auto init_from_team = [&](const vlr::TeamPtr& t) {
        if (!t) return;
        std::size_t n = std::min<std::size_t>(5, t->roster.size());
        for (std::size_t i = 0; i < n; ++i) out[t->roster[i].get()] = vlr::PlayerMatchStats{};
    };
    auto init_from_history = [&](const std::vector<vlr::Player*>& roster) {
        std::size_t n = std::min<std::size_t>(5, roster.size());
        for (std::size_t i = 0; i < n; ++i) {
            if (roster[i]) out[roster[i]] = vlr::PlayerMatchStats{};
        }
    };
    // Try the live TeamPtr first (pro matches); fall back to the
    // recording's history_record team rosters when the TeamPtr has died
    // (solo Q matches — synthetic Blue/Red Teams don't outlive
    // force_solo_match). Without the fallback the partial-stats map is
    // empty for solo Q replays and the live scoreboard reads zeros even
    // when stats exist in match_stats.
    if (auto t1 = rec.team1.lock()) init_from_team(t1);
    else if (rec.history_record) init_from_history(rec.history_record->blue_team);
    if (auto t2 = rec.team2.lock()) init_from_team(t2);
    else if (rec.history_record) init_from_history(rec.history_record->red_team);

    int total = static_cast<int>(rec.round_history->size());
    int n     = std::min(cursor, total);

    for (int r = 0; r < n; ++r) {
        const auto& round = (*rec.round_history)[r];
        std::set<vlr::Player*> died_this_round;
        std::set<vlr::Player*> kast_this_round;
        for (std::size_t j = 0; j < round.events.size(); ++j) {
            const auto& ev = round.events[j];
            if (!ev.killer || !ev.victim) continue;
            out[ev.killer].k++;
            out[ev.victim].d++;
            if (j == 0) {
                out[ev.killer].fb++;
                out[ev.victim].fd++;
            }
            died_this_round.insert(ev.victim);
            kast_this_round.insert(ev.killer);  // got a kill -> KAST
            // Trade: previous kill went the other way + trader gets KAST,
            // and the now-traded victim also keeps their KAST credit.
            if (j > 0) {
                const auto& prev = round.events[j - 1];
                if (prev.team_won != ev.team_won) {
                    kast_this_round.insert(prev.victim);  // traded -> KAST
                }
            }
        }
        // Survivors: any participant who didn't die this round.
        for (auto& kv : out) {
            if (!died_this_round.count(kv.first)) {
                kast_this_round.insert(kv.first);
                kv.second.survivals++;
            }
        }
        for (auto* p : kast_this_round) out[p].rounds_with_kast++;
    }

    // Damage / assists / HS aren't per-event in the log — scale the final
    // values by the fraction of rounds revealed. When the cursor reaches
    // the end of the match we expose the actual final figures.
    if (rec.match_stats && total > 0) {
        double frac = (n <= 0) ? 0.0 : static_cast<double>(n) / total;
        bool live = (n < total);
        for (auto& kv : out) {
            auto fit = rec.match_stats->find(kv.first);
            if (fit == rec.match_stats->end()) continue;
            kv.second.damage  = static_cast<int>(fit->second.damage  * frac);
            kv.second.a       = static_cast<int>(fit->second.a       * frac);
            kv.second.hs_hits = static_cast<int>(fit->second.hs_hits * frac);
            if (!live) {
                // Match's actually finished — expose the true VLR rating.
                kv.second.rating = fit->second.rating;
            } else if (n > 0) {
                // Live mode estimate: simple K/D-driven rating that updates
                // every round without spoiling the final figure.
                double kpr  = kv.second.k / static_cast<double>(n);
                double dpr  = kv.second.d / static_cast<double>(n);
                double rough = 0.70 + (kpr - dpr) * 0.40 + (kv.second.fb * 0.05);
                kv.second.rating = vlr::clamp_v(rough, 0.10, 2.50);
            } else {
                kv.second.rating = 1.00;
            }
        }
    }
    return out;
}

// One side of the scoreboard. Header strip uses the team's primary color
// for instant visual identification, then a clean stats table with
// right-aligned numeric columns and team-color-coded rating cells.
//
// Refactored to accept the roster as a raw `vector<Player*>` so solo Q
// matches (whose synthetic Blue/Red TeamPtrs die when force_solo_match
// returns) can still render their scoreboard from the recording's
// history_record. For pro/league matches the caller passes
// team->roster's player pointers + the team's display data; for solo Q
// matches the caller passes history_record.blue_team / red_team + the
// recording's blue_name / red_name as fallbacks.
//
// roster_players: ordered list of starters (any size; iterated as-is).
// display_name / display_tag: header strip text. Tag may be empty.
// brand_col: primary color for the strip + accent (already resolved).
void DrawTeamScoreboard(const std::vector<vlr::Player*>& roster_players,
                        const std::string& display_name,
                        const std::string& display_tag,
                        ImU32 brand,
                        const std::unordered_map<vlr::Player*, vlr::PlayerMatchStats>& stats,
                        const vlr::HistoryRecord& history,
                        const char* table_id) {
    if (roster_players.empty()) {
        // Defensive: empty roster shouldn't happen post-fix, but if it
        // does (a recording with no history_record AND no team1.lock())
        // we want the user to see something rather than a silent gap.
        ImGui::TextDisabled("(no scoreboard data for %s)", display_name.c_str());
        return;
    }

    int total_k = 0, total_fb = 0, total_dmg = 0;
    int total_hs = 0;
    for (auto* p : roster_players) {
        if (!p) continue;
        auto it = stats.find(p);
        if (it == stats.end()) continue;
        total_k  += it->second.k;
        total_fb += it->second.fb;
        total_dmg += it->second.damage;
        total_hs += it->second.hs_hits;
    }
    int hs_pct = total_k > 0 ? (int)(100.0 * total_hs / total_k) : 0;

    // === Team header strip (colored bar with team name + tag + KPIs) ===
    ImVec2 hdr_pos = ImGui::GetCursorScreenPos();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float hdr_h = 40.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Deep base + a faint brand wash so the strip reads as a broadcast
    // lower-third rather than a flat block.
    dl->AddRectFilled(hdr_pos, ImVec2(hdr_pos.x + avail.x, hdr_pos.y + hdr_h),
                      kBgDeep, 4.0f);
    ImColor dim((ImU32)brand);
    dim.Value.x *= 0.30f; dim.Value.y *= 0.30f; dim.Value.z *= 0.30f; dim.Value.w = 1.0f;
    dl->AddRectFilled(hdr_pos, ImVec2(hdr_pos.x + avail.x, hdr_pos.y + hdr_h),
                      (ImU32)dim, 4.0f);
    dl->AddRect(hdr_pos, ImVec2(hdr_pos.x + avail.x, hdr_pos.y + hdr_h),
                kBorder, 4.0f, 0, 1.0f);
    // Left brand stripe (5px, full team color).
    dl->AddRectFilled(hdr_pos, ImVec2(hdr_pos.x + 5, hdr_pos.y + hdr_h), brand);
    ImGui::SetCursorScreenPos(ImVec2(hdr_pos.x + 14, hdr_pos.y + 6));
    ImGui::PushFont(g_font_h2);
    if (!display_tag.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(brand));
        ImGui::Text("%s", display_tag.c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::Dummy(ImVec2(8, 0)); ImGui::SameLine();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrText));
    ImGui::TextUnformatted(display_name.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();

    // KPI row right-aligned in the header strip
    char kpi[160];
    int adr = roster_players.empty() ? 0
            : (int)((double)total_dmg / std::max<int>(1, (int)roster_players.size() * 12));
    std::snprintf(kpi, sizeof(kpi), "K %d   FB %d   HS %d%%   ADR %d",
                  total_k, total_fb, hs_pct, adr);
    ImGui::PushFont(g_font_small);
    ImVec2 ksz = ImGui::CalcTextSize(kpi);
    ImGui::SetCursorScreenPos(ImVec2(hdr_pos.x + avail.x - 14 - ksz.x, hdr_pos.y + 12));
    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
    ImGui::TextUnformatted(kpi);
    ImGui::PopStyleColor();
    ImGui::PopFont();

    // Reserve the strip's vertical space for the layout
    ImGui::SetCursorScreenPos(ImVec2(hdr_pos.x, hdr_pos.y + hdr_h + 6));

    // Identify the top-rated player so their row gets a faint accent
    // left-edge (the broadcast "team MVP" highlight).
    vlr::Player* top_rated = nullptr;
    {
        double best = -1.0;
        for (auto* p : roster_players) {
            if (!p) continue;
            auto it = stats.find(p);
            if (it == stats.end()) continue;
            if (it->second.rating > best) { best = it->second.rating; top_rated = p; }
        }
    }

    // === Stats table (sortable) ===
    // Columns map to a sort key; clicking a numeric header sorts that side's
    // players by it (DESC default, toggle), name ASC. Role-name tinting,
    // MVP edge and mono numerics preserved. We sort a local copy of the
    // roster pointers so the caller's vector is untouched.
    enum SbCol { SB_FLAG, SB_PLAYER, SB_AGENT, SB_K, SB_D, SB_A,
                 SB_FB, SB_HS, SB_RAT, SB_NCOL };
    if (!ImGui::BeginTable(table_id, SB_NCOL,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
            ImGuiTableFlags_Sortable | ImGuiTableFlags_SortMulti |
            ImGuiTableFlags_SizingFixedFit, ImVec2(-FLT_MIN, 0))) return;
    ImGui::TableSetupColumn(" ", ImGuiTableColumnFlags_WidthFixed |
                            ImGuiTableColumnFlags_NoSort, 24.0f);
    ImGui::TableSetupColumn("PLAYER", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("AGENT", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableSetupColumn("K", ImGuiTableColumnFlags_WidthFixed |
        ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending, 36.0f);
    ImGui::TableSetupColumn("D", ImGuiTableColumnFlags_WidthFixed |
        ImGuiTableColumnFlags_PreferSortDescending, 36.0f);
    ImGui::TableSetupColumn("A", ImGuiTableColumnFlags_WidthFixed |
        ImGuiTableColumnFlags_PreferSortDescending, 36.0f);
    ImGui::TableSetupColumn("FB", ImGuiTableColumnFlags_WidthFixed |
        ImGuiTableColumnFlags_PreferSortDescending, 36.0f);
    ImGui::TableSetupColumn("HS%", ImGuiTableColumnFlags_WidthFixed |
        ImGuiTableColumnFlags_PreferSortDescending, 44.0f);
    ImGui::TableSetupColumn("RAT", ImGuiTableColumnFlags_WidthFixed |
        ImGuiTableColumnFlags_PreferSortDescending, 56.0f);
    ImGui::TableHeadersRow();

    std::vector<vlr::Player*> ordered;
    ordered.reserve(roster_players.size());
    for (auto* p : roster_players) if (p && stats.count(p)) ordered.push_back(p);
    if (ImGuiTableSortSpecs* sp = ImGui::TableGetSortSpecs()) {
        if (sp->SpecsCount > 0) {
            auto stat_of = [&](vlr::Player* p) -> const vlr::PlayerMatchStats& {
                return stats.find(p)->second;
            };
            std::stable_sort(ordered.begin(), ordered.end(),
                [&](vlr::Player* a, vlr::Player* b) {
                    for (int si = 0; si < sp->SpecsCount; ++si) {
                        const auto& s2 = sp->Specs[si];
                        bool asc = (s2.SortDirection == ImGuiSortDirection_Ascending);
                        const auto& sa = stat_of(a);
                        const auto& sb = stat_of(b);
                        double va = 0, vb = 0; int cmp = 0;
                        switch (s2.ColumnIndex) {
                        case SB_PLAYER:
                            cmp = player_label(*a).compare(player_label(*b)); break;
                        case SB_AGENT: {
                            auto ia = history.stats.find(a);
                            auto ib = history.stats.find(b);
                            std::string aa = ia != history.stats.end() ? ia->second.agent : "";
                            std::string ab = ib != history.stats.end() ? ib->second.agent : "";
                            cmp = aa.compare(ab); break; }
                        case SB_K:   va = sa.k; vb = sb.k; break;
                        case SB_D:   va = sa.d; vb = sb.d; break;
                        case SB_A:   va = sa.a; vb = sb.a; break;
                        case SB_FB:  va = sa.fb; vb = sb.fb; break;
                        case SB_HS:  // sort by HS% (hs_hits/kills), matching the displayed value, not raw count
                            va = sa.k > 0 ? 100.0 * sa.hs_hits / sa.k : 0.0;
                            vb = sb.k > 0 ? 100.0 * sb.hs_hits / sb.k : 0.0; break;
                        case SB_RAT: va = sa.rating; vb = sb.rating; break;
                        default: break;
                        }
                        if (cmp == 0 && va != vb) cmp = (va < vb) ? -1 : 1;
                        if (cmp != 0) return asc ? (cmp < 0) : (cmp > 0);
                    }
                    return player_label(*a) < player_label(*b);
                });
        }
    }
    for (auto* p : ordered) {
        if (!p) continue;
        auto it = stats.find(p);
        if (it == stats.end()) continue;
        auto& ss = it->second;
        bool is_top = (p == top_rated);
        ImGui::TableNextRow();
        // Faint accent left-edge on the team's top-rated row.
        if (is_top) {
            ImVec2 rmin = ImGui::GetCursorScreenPos();
            float rh = ImGui::GetTextLineHeightWithSpacing() + 4.0f;
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2(rmin.x, rmin.y),
                ImVec2(rmin.x + 3.0f, rmin.y + rh), kAccent);
        }
        ImGui::TableNextColumn(); CountryFlag(p->country_iso);
        ImGui::TableNextColumn();
        ImGui::PushStyleColor(ImGuiCol_Text,
                              toV4(is_top ? kVlrText : kVlrSub));
        ImGui::Text("%s", player_label(*p).c_str());
        ImGui::PopStyleColor();
        // Phase 1 (Agent C) — subtle signature agent chip beside the
        // gamertag. Dimmer, smaller font; skipped if empty.
        {
            std::string sig_chip = p->signature_agent();
            if (!sig_chip.empty()) {
                ImGui::SameLine();
                ImGui::PushFont(g_font_small);
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
                ImGui::Text("[%s]", sig_chip.c_str());
                ImGui::PopStyleColor();
                ImGui::PopFont();
            }
        }
        ImGui::TableNextColumn();
        auto agentit = history.stats.find(p);
        std::string agname = agentit != history.stats.end() ? agentit->second.agent : "?";
        // Color the agent name by the AGENT's role (the role of the agent
        // they're piloting this map), NOT by the player's primary_role
        // (career default). A Controller main flexed onto Killjoy must
        // render in the Sentinel color, not the Controller color.
        vlr::Role agent_role = p->primary_role;  // safe fallback
        if (const auto* ag = vlr::find_agent_by_name(agname)) {
            agent_role = ag->role;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(role_color(agent_role)));
        ImGui::Text("%s", agname.c_str());
        ImGui::PopStyleColor();
        // Monospaced numeric columns for tight pro-scoreboard alignment.
        ImGui::PushFont(g_font_mono);
        ImGui::TableNextColumn(); ImGui::Text("%d", ss.k);
        ImGui::TableNextColumn();
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
        ImGui::Text("%d", ss.d);
        ImGui::PopStyleColor();
        ImGui::TableNextColumn(); ImGui::Text("%d", ss.a);
        ImGui::TableNextColumn();
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(ss.fb > 0 ? kVlrGold : kVlrSub));
        ImGui::Text("%d", ss.fb);
        ImGui::PopStyleColor();
        ImGui::TableNextColumn();
        // Per-match HS% — read from PlayerLine (history_record.stats[p].hs)
        // which is the per-map computed value. Falls back to '—' if the
        // player has no kills yet (HS% over zero kills is undefined).
        if (agentit != history.stats.end() && ss.k > 0) {
            ImGui::Text("%d%%", agentit->second.hs);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kTextFaint));
            ImGui::TextUnformatted("\xE2\x80\x94");
            ImGui::PopStyleColor();
        }
        ImGui::TableNextColumn();
        ImU32 rcol = ss.rating >= 1.10 ? kVlrGreen : (ss.rating >= 0.95 ? kVlrText : kVlrRed);
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(rcol));
        ImGui::Text("%.2f", ss.rating);
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }
    ImGui::EndTable();
}

void DrawLiveMatchModal(AppState& s, float dt) {
    if (!s.live.open) return;
    ImGui::OpenPopup("Live Match##modal");
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 c = vp->GetCenter();
    ImGui::SetNextWindowPos(c, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    // Large broadcast modal, but clamped to the viewport so it never
    // exceeds the screen. The modal window itself owns the ONE vertical
    // scroll for the whole live view — every inner panel is non-scrolling.
    ImVec2 lm_size(std::min(1180.0f, vp->WorkSize.x - 40.0f),
                   std::min(820.0f,  vp->WorkSize.y - 40.0f));
    ImGui::SetNextWindowSize(lm_size, ImGuiCond_Appearing);

    if (ImGui::BeginPopupModal("Live Match##modal", &s.live.open,
                               ImGuiWindowFlags_NoSavedSettings)) {
        if (!s.live.recording || !s.live.recording->round_history) {
            ImGui::TextDisabled("No live match.");
            ImGui::EndPopup();
            return;
        }
        const auto& rec = *s.live.recording;
        const auto& rounds = *rec.round_history;
        const auto& match_stats = *rec.match_stats;
        const auto& history_record = *rec.history_record;

        auto& live = s.live;

        // === Export-to-JSON helpers ===
        // Sanitize a filename component: replace anything outside
        // [A-Za-z0-9_-] with '_', strip leading/trailing whitespace,
        // collapse the result to <= cap chars.
        auto sanitize_filename_part = [](std::string in, std::size_t cap = 40) {
            // Strip leading/trailing whitespace.
            std::size_t l = 0, r = in.size();
            while (l < r && std::isspace((unsigned char)in[l])) ++l;
            while (r > l && std::isspace((unsigned char)in[r - 1])) --r;
            std::string s2 = in.substr(l, r - l);
            for (auto& c : s2) {
                unsigned char uc = (unsigned char)c;
                bool ok = std::isalnum(uc) || c == '_' || c == '-';
                if (!ok) c = '_';
            }
            if (s2.size() > cap) s2.resize(cap);
            if (s2.empty()) s2 = "match";
            return s2;
        };

        // Native Win32 Save File Dialog. Returns "" if user cancels.
        auto prompt_save_path = [](const std::string& default_filename) -> std::string {
            char buf[MAX_PATH];
            std::strncpy(buf, default_filename.c_str(), sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';

            OPENFILENAMEA ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner   = nullptr;
            ofn.lpstrFilter = "JSON files (*.json)\0*.json\0All files (*.*)\0*.*\0";
            ofn.lpstrFile   = buf;
            ofn.nMaxFile    = sizeof(buf);
            ofn.lpstrDefExt = "json";
            ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
            if (GetSaveFileNameA(&ofn)) return std::string(buf);
            return std::string();
        };

        // Build a default filename "{event}_{team1}_vs_{team2}_{year}-{day}.json"
        // from the current series. Capped at ~80 chars total.
        auto build_default_filename = [&]() {
            std::string ev   = sanitize_filename_part(rec.event,      24);
            std::string t1n  = sanitize_filename_part(rec.blue_name,  16);
            std::string t2n  = sanitize_filename_part(rec.red_name,   16);
            char tail[32];
            std::snprintf(tail, sizeof(tail), "_%d-%d.json",
                          s.gm.year, s.gm.day_in_year);
            std::string fn = ev + "_" + t1n + "_vs_" + t2n + tail;
            if (fn.size() > 80) {
                // Trim event part first, keep matchup + tail intact.
                std::size_t excess = fn.size() - 80;
                if (excess < ev.size()) {
                    ev.resize(ev.size() - excess);
                    fn = ev + "_" + t1n + "_vs_" + t2n + tail;
                }
            }
            return fn;
        };

        // Run the export. Pulls live.series, derives event/best_of/year/day,
        // calls export_series_to_json, prompts for save path, writes file,
        // pushes status to s.log.
        auto run_export = [&]() {
            if (live.series.empty()) {
                s.log.emplace_back("Export failed: no series data.");
                return;
            }
            // Derive event_name from the first recording's event field
            // (works for both fresh and replayed series).
            std::string event_name = live.series.front()
                ? live.series.front()->event
                : rec.event;
            // best_of: we only have the played map count here (the configured
            // Series best_of is not retained in the live viewer). This is a
            // floor, not the true bound (a BO5 won 3-0 has size()==3); the
            // exporter re-derives the true best_of from the per-team map wins.
            int bo = (int)live.series.size();
            std::string json = vlr::export_series_to_json(
                live.series, event_name, bo, s.gm.year, s.gm.day_in_year);
            if (json.empty()) {
                s.log.emplace_back("Export failed: serializer returned empty.");
                return;
            }
            std::string path = prompt_save_path(build_default_filename());
            if (path.empty()) return;  // user cancelled — no log noise
            std::ofstream out(path, std::ios::binary);
            if (!out.is_open()) {
                s.log.emplace_back("Export failed: could not open " + path);
                return;
            }
            out << json;
            out.close();
            if (!out) {
                s.log.emplace_back("Export failed: write error on " + path);
                return;
            }
            s.log.emplace_back("Exported series to: " + path);
        };

        // Gate: at least one map in series + currently viewing the LAST map.
        // Recordings are by definition finished, so no further match-state
        // check is needed.
        bool can_export_series =
            !live.series.empty()
            && live.current_map_idx == (int)live.series.size() - 1;

        // === Auto-play: per-kill pacing inside a round ===
        // We advance event_cursor through the current round's events one
        // at a time (showing kills sequentially in the feed). When we run
        // out of events for the current round we tick round_cursor and
        // reset event_cursor to start the next round. Speed selector:
        //   0=Slow (1.2s/kill), 1=Normal (0.6s), 2=Fast (0.25s),
        //   3=Round-skip (legacy: round_cursor++ per tick).
        const float kSpeedDelays[4] = { 1.20f, 0.60f, 0.25f, 0.30f };
        float tick_delay = kSpeedDelays[std::clamp(live.playback_speed, 0, 3)];
        if (live.auto_play && live.round_cursor < (int)rounds.size()) {
            live.auto_timer += dt;
            if (live.auto_timer >= tick_delay) {
                live.auto_timer = 0.0f;
                if (live.playback_speed == 3) {
                    // Legacy round-skip mode — whole round per tick.
                    live.round_cursor += 1;
                    live.event_cursor = 0;
                } else {
                    // Per-kill mode: tick events; advance round when
                    // current round exhausted.
                    auto& cur_round = rounds[(std::size_t)live.round_cursor];
                    int n_events = static_cast<int>(cur_round.events.size());
                    if (live.event_cursor < n_events) {
                        live.event_cursor += 1;
                    } else {
                        // End-of-round: small pause already absorbed by
                        // tick_delay; advance to next round.
                        live.round_cursor += 1;
                        live.event_cursor = 0;
                    }
                }
            }
        }
        int shown_idx = std::min<int>(live.round_cursor, (int)rounds.size());
        int t1_score_now = 0, t2_score_now = 0;
        if (shown_idx > 0) {
            t1_score_now = rounds[shown_idx - 1].t1_score;
            t2_score_now = rounds[shown_idx - 1].t2_score;
        }

        // Helper: derive (blue_tag, red_tag) comp tags for a recording by
        // counting roles of the agent each player ran. Returns {nullptr,
        // nullptr} when stats / team rosters are missing.
        auto comp_tags_for = [](const vlr::RecordedMatch& r)
            -> std::pair<const char*, const char*> {
            if (!r.history_record || !r.history_record->blue_team.size()
                || !r.history_record->red_team.size()) {
                return {nullptr, nullptr};
            }
            const auto& hr = *r.history_record;
            auto build_tag = [&](const std::vector<vlr::Player*>& roster)
                -> const char* {
                vlr::CompPlan plan{};
                int counted = 0;
                for (auto* pl : roster) {
                    if (!pl) continue;
                    auto it = hr.stats.find(pl);
                    if (it == hr.stats.end()) continue;
                    if (auto* ag = vlr::find_agent_by_name(it->second.agent)) {
                        plan.need[(int)ag->role] += 1;
                        ++counted;
                    }
                }
                if (counted == 0) return nullptr;
                return vlr::comp_tag_name(vlr::comp_tag_of(plan));
            };
            return {build_tag(hr.blue_team), build_tag(hr.red_team)};
        };

        // === Series Comp Evolution panel ===
        // Multi-map only — single-map opens hide it. Shows comp tag each
        // team ran on each map of the series, with the active row gold-
        // accented. Sits ABOVE the score banner so the user reads the
        // series narrative before the current-map detail.
        //
        // SPOILER GUARD: render rows only up to the currently-viewed map.
        // Showing all rows reveals series length (3 rows = BO3, 5 = BO5)
        // and pre-spoils future-map comp picks. Rows for not-yet-reached
        // maps stay hidden until the user navigates onto them.
        if (live.series.size() > 1) {
            ImGui::PushID("##series_comp_evolution");
            H2("SERIES COMP EVOLUTION");
            if (ImGui::BeginTable("##series_evo_tbl", 4,
                    ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
                ImGui::TableSetupColumn("MAP",  ImGuiTableColumnFlags_WidthFixed, 60);
                ImGui::TableSetupColumn("NAME", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("BLUE COMP", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("RED COMP",  ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();
                std::size_t evo_max = std::min<std::size_t>(
                    live.series.size(),
                    static_cast<std::size_t>(live.current_map_idx) + 1);
                for (std::size_t i = 0; i < evo_max; ++i) {
                    const auto& sm = live.series[i];
                    bool is_active = ((int)i == live.current_map_idx);
                    ImGui::TableNextRow();
                    if (is_active) ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));

                    ImGui::TableNextColumn();
                    ImGui::Text("Map %zu", i + 1);

                    ImGui::TableNextColumn();
                    if (sm) ImGui::Text("%s", sm->map_name.c_str());
                    else    ImGui::TextDisabled("—");

                    const char* btag = nullptr;
                    const char* rtag = nullptr;
                    if (sm) {
                        auto pair = comp_tags_for(*sm);
                        btag = pair.first;
                        rtag = pair.second;
                    }
                    ImGui::TableNextColumn();
                    if (btag) ImGui::Text("%s", btag);
                    else      ImGui::TextDisabled("—");

                    ImGui::TableNextColumn();
                    if (rtag) ImGui::Text("%s", rtag);
                    else      ImGui::TextDisabled("—");

                    if (is_active) ImGui::PopStyleColor();
                }
                ImGui::EndTable();
            }
            ImGui::PopID();
            ImGui::Spacing();
        }

        DrawScoreBanner(rec, t1_score_now, t2_score_now, shown_idx, rounds.size());

        // === POST-MATCH WRAP-UP ============================================
        // Shown ONLY once this map's playback is fully revealed (shown_idx at
        // the end) so it never spoils a match being watched live. MVP card +
        // derived match awards (from the finished recording's match_stats) +
        // a deterministic storyline banner (from score + round history). Pure
        // presentation — reads the immutable recording, touches no engine state.
        if (shown_idx >= (int)rounds.size() && !rounds.empty() && rec.match_stats) {
            const auto& ms = *rec.match_stats;
            const int bs = rec.blue_score, rs = rec.red_score;
            const int total_rounds = bs + rs;
            const bool blue_won = (bs >= rs);
            const std::string& win_name  = blue_won ? rec.blue_name : rec.red_name;
            const std::string& lose_name = blue_won ? rec.red_name  : rec.blue_name;
            const int win_sc  = std::max(bs, rs);
            const int lose_sc = std::min(bs, rs);
            const int margin  = win_sc - lose_sc;

            // Storyline: did the eventual winner ever trail by >=5 (comeback)?
            int max_winner_deficit = 0;
            for (const auto& r : rounds) {
                int wsc = blue_won ? r.t1_score : r.t2_score;
                int lsc = blue_won ? r.t2_score : r.t1_score;
                max_winner_deficit = std::max(max_winner_deficit, lsc - wsc);
            }
            // Upset: winner clearly weaker on paper (resolve live teams if alive).
            bool upset = false;
            {
                auto wt = find_team_by_name(s, win_name);
                auto lt = find_team_by_name(s, lose_name);
                if (wt && lt && (lt->ovr() - wt->ovr()) >= 4.0) upset = true;
            }
            const char* story_tag = "RESULT";
            ImU32 story_col = kAccent;
            std::string story;
            char sc[24]; std::snprintf(sc, sizeof(sc), "%d-%d", win_sc, lose_sc);
            if (lose_sc == 0) {
                story_tag = "FLAWLESS"; story_col = kVlrGold;
                story = win_name + " ran a perfect map, " + sc + ".";
            } else if (win_sc > 13) {
                story_tag = "OVERTIME"; story_col = kVlrGold;
                story = "Overtime thriller \xE2\x80\x94 " + win_name + " edged it " + sc + ".";
            } else if (max_winner_deficit >= 5) {
                story_tag = "COMEBACK"; story_col = kVlrGreen;
                story = win_name + " stormed back from -" + std::to_string(max_winner_deficit)
                      + " to take it " + sc + ".";
            } else if (margin >= 9) {
                story_tag = "DOMINATION"; story_col = kVlrGold;
                story = win_name + " dominated " + lose_name + " " + sc + ".";
            } else if (margin <= 2) {
                story_tag = "NAILBITER"; story_col = kAccent2;
                story = "Down to the wire \xE2\x80\x94 " + win_name + " took it " + sc + ".";
            } else {
                story = win_name + " def. " + lose_name + " " + sc + ".";
            }
            if (upset) { story = "UPSET! " + story; story_col = kVlrRed; }

            // Derived awards from the finished per-player stats.
            vlr::Player* topfrag = nullptr; vlr::Player* entryk = nullptr;
            vlr::Player* clutchp = nullptr; vlr::Player* acer = nullptr;
            int best_k = -1, best_fb = -1, best_cl = 0;
            for (const auto& kv : ms) {
                vlr::Player* p = kv.first; if (!p) continue;
                const auto& st = kv.second;
                if (st.k  > best_k)  { best_k  = st.k;  topfrag = p; }
                if (st.fb > best_fb) { best_fb = st.fb; entryk  = p; }
                if (st.clutch_pts > best_cl) { best_cl = st.clutch_pts; clutchp = p; }
                if (st.mk5 > 0 && !acer) acer = p;
            }

            BeginCard("##postmatch_wrap", kBgDeep, 14.0f, 12.0f);
            ImGui::PushFont(g_font_h2 ? g_font_h2 : g_font_body);
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
            ImGui::TextUnformatted("MATCH WRAP-UP");
            ImGui::PopStyleColor();
            ImGui::PopFont();
            VSpace(4);
            Pill(story_tag, story_col);
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrText));
            ImGui::TextWrapped("%s", story.c_str());
            ImGui::PopStyleColor();
            VSpace(8);

            // MVP + award tiles, laid out in one row (SameLine between, not after).
            struct WrapTile { const char* label; std::string val; ImU32 col; std::string sub; };
            std::vector<WrapTile> tiles;
            if (history_record.mvp) {
                std::string sub;
                auto it = ms.find(history_record.mvp);
                if (it != ms.end()) {
                    const auto& st = it->second;
                    double adr = total_rounds > 0 ? (double)st.damage / total_rounds : 0.0;
                    char mv[160];
                    std::snprintf(mv, sizeof(mv), "%.2f rating  \xC2\xB7  %d-%d-%d  \xC2\xB7  %.0f ADR",
                                  st.rating, st.k, st.d, st.a, adr);
                    sub = mv;
                }
                tiles.push_back({ "MATCH MVP", player_label(*history_record.mvp), kVlrGold, sub });
            }
            if (topfrag && best_k >= 0) {
                char b[40]; std::snprintf(b, sizeof(b), "%d kills", best_k);
                tiles.push_back({ "TOP FRAGGER", player_label(*topfrag), kVlrRed, b });
            }
            if (entryk && best_fb > 0) {
                char b[48]; std::snprintf(b, sizeof(b), "%d opening kills", best_fb);
                tiles.push_back({ "ENTRY KING", player_label(*entryk), kAccent, b });
            }
            if (clutchp && best_cl > 0) {
                char b[48]; std::snprintf(b, sizeof(b), "%d clutch pts", best_cl);
                tiles.push_back({ "CLUTCH", player_label(*clutchp), kVlrGreen, b });
            }
            for (std::size_t i = 0; i < tiles.size(); ++i) {
                StatTile(tiles[i].label, tiles[i].val, tiles[i].col, ImVec2(210, 92),
                         tiles[i].sub.empty() ? nullptr : tiles[i].sub.c_str());
                if (i + 1 < tiles.size()) ImGui::SameLine();
            }
            if (acer) {
                VSpace(4);
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                ImGui::Text("\xE2\x98\x85 ACE \xE2\x80\x94 %s dropped all five!",
                            player_label(*acer).c_str());
                ImGui::PopStyleColor();
            }
            EndCard();
            VSpace(8);
        }

        // WS-B: international style-clash banner. At a Masters/Champions between
        // two DIFFERENT regions, surface the meta contrast (the same signal that
        // feeds the bounded match tilt). Purely informational.
        {
            const bool intl_ev =
                (rec.event.find("MASTERS")   != std::string::npos ||
                 rec.event.find("CHAMPIONS") != std::string::npos);
            if (intl_ev) {
                auto bt = find_team_by_name(s, rec.blue_name);
                auto rt = find_team_by_name(s, rec.red_name);
                if (bt && rt) {
                    std::string note = vlr::region_clash_note(bt->region, rt->region);
                    if (!note.empty()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                        ImGui::TextWrapped("STYLE CLASH \xE2\x80\x94 %s", note.c_str());
                        ImGui::PopStyleColor();
                    }
                }
            }
        }

        // Per-team comp tag subtitle row — sits directly below the score
        // banner, mirroring its left/right team layout. Only shown when
        // we can derive both comps from the recording's history_record.
        {
            auto cur_tags = comp_tags_for(rec);
            if (cur_tags.first || cur_tags.second) {
                ImGui::PushID("##live_comp_tag_row");
                ImGui::PushFont(g_font_small);
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                if (cur_tags.first) {
                    ImGui::Text("%s comp: %s", rec.blue_name.c_str(), cur_tags.first);
                } else {
                    ImGui::Text("%s comp: —", rec.blue_name.c_str());
                }
                // Right-align the red team's comp tag on the same line.
                char red_buf[160];
                if (cur_tags.second) {
                    std::snprintf(red_buf, sizeof(red_buf), "%s comp: %s",
                                  rec.red_name.c_str(), cur_tags.second);
                } else {
                    std::snprintf(red_buf, sizeof(red_buf), "%s comp: —",
                                  rec.red_name.c_str());
                }
                ImVec2 sz = ImGui::CalcTextSize(red_buf);
                float right_x = ImGui::GetContentRegionAvail().x - sz.x;
                if (right_x < 0) right_x = 0;
                ImGui::SameLine(right_x);
                ImGui::TextUnformatted(red_buf);
                ImGui::PopStyleColor();
                ImGui::PopFont();
                ImGui::PopID();
            }
        }
        ImGui::Spacing();
        DrawRoundTimeline(live, rounds, rec);
        ImGui::Spacing();

        // Controls row — broadcast transport bar.
        ImGui::PushStyleColor(ImGuiCol_Button, toV4(live.auto_play ? kVlrRed : kVlrGreen));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toV4(kVlrRedDim));
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kBgDeep));
        if (ImGui::Button(live.auto_play ? "Pause" : "Auto Play", ImVec2(120, 34))) {
            live.auto_play = !live.auto_play;
        }
        ImGui::PopStyleColor(3);
        ImGui::SameLine();
        if (ImGui::Button("Next Round", ImVec2(120, 34))) {
            if (live.round_cursor < (int)rounds.size()) live.round_cursor++;
            live.event_cursor = 0;
        }
        ImGui::SameLine();
        if (ImGui::Button("Skip 5", ImVec2(80, 34))) {
            live.round_cursor = std::min<int>(live.round_cursor + 5, (int)rounds.size());
            live.event_cursor = 0;
        }
        ImGui::SameLine();
        if (ImGui::Button("Skip to End", ImVec2(120, 34))) {
            live.round_cursor = (int)rounds.size();
            live.event_cursor = 0;
        }
        ImGui::SameLine();
        ImGui::Dummy(ImVec2(14, 0)); ImGui::SameLine();
        // Segmented speed selector pill row — same playback_speed values
        // (0=Slow 1=Normal 2=Fast 3=Round-skip), just a cleaner control
        // than the dropdown. Selected segment uses the accent fill.
        {
            const char* speed_labels[] = {"Slow", "Normal", "Fast", "Round-skip"};
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 0));
            for (int sp = 0; sp < 4; ++sp) {
                bool sel = (live.playback_speed == sp);
                ImGui::PushStyleColor(ImGuiCol_Button,
                    toV4(sel ? kAccent : kSurfaceAlt));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                    toV4(sel ? kAccent : kSurfaceHi));
                ImGui::PushStyleColor(ImGuiCol_Text,
                    toV4(sel ? kBgDeep : kVlrSub));
                if (ImGui::Button(speed_labels[sp], ImVec2(0, 34))) {
                    live.playback_speed = sp;
                }
                ImGui::PopStyleColor(3);
                if (sp < 3) ImGui::SameLine();
            }
            ImGui::PopStyleVar();
        }
        // Series navigation: only shown when this is a multi-map series.
        // Safety: only honor the series if every entry is non-null AND
        // current_map_idx is in-range AND the current recording matches
        // the series slot. Otherwise treat as single-map (clear series).
        bool series_ok = live.series.size() > 1
                      && live.current_map_idx >= 0
                      && live.current_map_idx < (int)live.series.size();
        if (series_ok) {
            for (auto& m : live.series) {
                if (!m || !m->round_history) { series_ok = false; break; }
            }
        }
        if (!series_ok) {
            // Stale series state — drop it so map-switch buttons can't
            // touch dangling/mismatched recordings (the historic crash
            // when switching back to Map 1 after a tournament watch).
            live.series.clear();
            live.current_map_idx = 0;
        }
        if (live.series.size() > 1) {
            ImGui::SameLine();
            ImGui::Dummy(ImVec2(20, 0));
            ImGui::SameLine();
            bool can_prev = live.current_map_idx > 0;
            bool can_next = live.current_map_idx + 1 < (int)live.series.size();
            if (!can_prev) ImGui::BeginDisabled();
            if (ImGui::Button("< Prev Map", ImVec2(110, 32))) {
                int new_idx = live.current_map_idx - 1;
                if (new_idx >= 0 && new_idx < (int)live.series.size()
                    && live.series[new_idx] && live.series[new_idx]->round_history) {
                    live.current_map_idx = new_idx;
                    live.recording = live.series[new_idx];
                    live.round_cursor = 0;
                    live.auto_play = false;
                }
            }
            if (!can_prev) ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
            // Series length intentionally HIDDEN — showing "Map 1/3" tells
            // the user it's a BO3 (or BO5), spoiling whether further maps
            // remain. We just show the current map number; the Next/Prev
            // buttons grey out at series boundaries.
            ImGui::Text("Map %d", live.current_map_idx + 1);
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if (!can_next) ImGui::BeginDisabled();
            if (ImGui::Button("Next Map >", ImVec2(110, 32))) {
                int new_idx = live.current_map_idx + 1;
                if (new_idx >= 0 && new_idx < (int)live.series.size()
                    && live.series[new_idx] && live.series[new_idx]->round_history) {
                    live.current_map_idx = new_idx;
                    live.recording = live.series[new_idx];
                    live.round_cursor = 0;
                    live.auto_play = false;
                }
            }
            if (!can_next) ImGui::EndDisabled();
        }
        // === Slot 1: Export to JSON button (map-nav row) ===
        // Only visible when we're on the final map of a recorded series.
        // Sits in the controls row alongside Next Map / Prev Map.
        if (can_export_series) {
            ImGui::SameLine();
            ImGui::Dummy(ImVec2(20, 0));
            ImGui::SameLine();
            ImGui::PushID("export_json_navrow");
            ImGui::PushStyleColor(ImGuiCol_Button,        toV4(kVlrGold));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toV4(kVlrGold));
            ImGui::PushStyleColor(ImGuiCol_Text,          IM_COL32(0x10, 0x14, 0x1A, 0xFF));
            if (ImGui::Button("Export JSON", ImVec2(130, 32))) {
                run_export();
            }
            ImGui::PopStyleColor(3);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Export full series to JSON file\n"
                    "(every map, every player, full stats).");
            }
            ImGui::PopID();
        }
        ImGui::SameLine();
        // Spoiler-free round counter: total rounds hidden so the user
        // can't pre-read whether the match went to overtime. " — FINAL"
        // (below) signals completion once the cursor reaches the end.
        ImGui::TextDisabled("Round %d", shown_idx);
        if (live.round_cursor >= (int)rounds.size()) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
            ImGui::Text(" — FINAL");
            ImGui::PopStyleColor();
            // MVP highlight.
            if (history_record.mvp) {
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                ImGui::Text("  |  MVP: %s", player_label(*history_record.mvp).c_str());
                ImGui::PopStyleColor();
            }
        }

        ImGui::Spacing();

        // Responsive split: scoreboards | round feed. Side-by-side only
        // when the modal content region is wide enough for both panels;
        // otherwise stack them vertically so nothing overflows / drifts
        // off-screen on a narrow window. The modal window owns the single
        // vertical scroll — neither panel scrolls internally.
        float live_avail_w = ImGui::GetContentRegionAvail().x;
        bool live_two_col = (live_avail_w >= 1000.0f);
        // Replaced deprecated ImGui::Columns(2) with two side-by-side
        // BeginChild panels + SameLine. Preserves the prior 60/40 split
        // and the responsive stacked fallback when the modal is narrow.
        // BeginChild is safe to host arbitrary nested widgets (tables,
        // selectables, draw-list painting) — unlike inner BeginTable.
        float live_left_w  = live_two_col ? (live_avail_w * 0.60f - 6.0f) : 0.0f;
        float live_right_w = live_two_col ? (live_avail_w - live_left_w - 14.0f) : 0.0f;
        if (live_two_col) {
            ImGui::BeginChild("##live_left", ImVec2(live_left_w, 0),
                              ImGuiChildFlags_AutoResizeY);
        }

        // Live partial stats: only show what's been revealed up to the
        // current cursor. Final K/D/FB/KAST come from the event log; A/dmg
        // are scaled approximations until the match ends.
        auto live_stats = compute_partial_stats(rec, live.round_cursor);

        // Header label so the user knows whether they're seeing live data
        // or the finalized scoreboard.
        bool is_finalized = (live.round_cursor >= (int)rounds.size());
        ImGui::PushFont(g_font_small);
        if (is_finalized) {
            ImGui::TextColored(toV4(kVlrGold), "FINAL — actual match totals");
        } else {
            // Spoiler-free LIVE counter: drop "/ total".
            ImGui::TextColored(toV4(kVlrSub),
                               "LIVE — stats accurate as of round %d",
                               shown_idx);
        }
        ImGui::PopFont();

        // Left: side-by-side scoreboards stacked vertically.
        //
        // Build a (roster, name, tag, brand_color) bundle for each side.
        // For pro/league matches the TeamPtr survives (it's owned by the
        // world's `teams` vector) and we use its roster + identity. For
        // solo Q matches the synthetic Blue/Red Teams die when
        // force_solo_match() returns, so team1.lock() comes back null —
        // we fall back to the recording's history_record.blue_team /
        // red_team (raw vector<Player*>) plus rec.blue_name / red_name
        // and a default blue/red brand color. Without this fallback the
        // solo Q live viewer's scoreboard renders empty.
        vlr::TeamPtr rec_t1 = rec.team1.lock();
        vlr::TeamPtr rec_t2 = rec.team2.lock();

        auto resolve_side = [&](const vlr::TeamPtr& tp,
                                const std::string& fallback_name,
                                const std::vector<vlr::Player*>& fallback_roster,
                                ImU32 fallback_brand)
            -> std::tuple<std::vector<vlr::Player*>, std::string, std::string, ImU32> {
            if (tp) {
                std::vector<vlr::Player*> roster;
                roster.reserve(tp->roster.size());
                for (auto& sp : tp->roster) roster.push_back(sp.get());
                ImU32 brand = (tp->color_primary_r || tp->color_primary_g ||
                               tp->color_primary_b)
                    ? IM_COL32(tp->color_primary_r, tp->color_primary_g,
                               tp->color_primary_b, 0xFF)
                    : fallback_brand;
                return {std::move(roster), tp->name, tp->tag, brand};
            }
            // Solo Q / orphaned recording path. No tag — we never minted
            // one for the synthetic Team. Brand falls back to blue/red.
            return {fallback_roster, fallback_name, std::string{}, fallback_brand};
        };

        auto [t1_roster, t1_name, t1_tag, t1_brand] = resolve_side(
            rec_t1, rec.blue_name,
            history_record.blue_team, kVlrBlue);
        auto [t2_roster, t2_name, t2_tag, t2_brand] = resolve_side(
            rec_t2, rec.red_name,
            history_record.red_team, kVlrRed);

        DrawTeamScoreboard(t1_roster, t1_name, t1_tag, t1_brand,
                           live_stats, history_record, "##sb_blue");
        ImGui::Spacing();
        DrawTeamScoreboard(t2_roster, t2_name, t2_tag, t2_brand,
                           live_stats, history_record, "##sb_red");

        // === Slot 2: Export to JSON button (scoreboard panel footer) ===
        // Mirror of Slot 1, placed below both scoreboards so users at the
        // post-match results screen don't have to reach for the controls
        // row above. Same gating, same handler.
        if (can_export_series) {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::PushID("export_json_scoreboard");
            ImGui::PushStyleColor(ImGuiCol_Button,        toV4(kVlrGold));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toV4(kVlrGold));
            ImGui::PushStyleColor(ImGuiCol_Text,          IM_COL32(0x10, 0x14, 0x1A, 0xFF));
            if (ImGui::Button("Export Series to JSON", ImVec2(220, 32))) {
                run_export();
            }
            ImGui::PopStyleColor(3);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Export full series to JSON file\n"
                    "(every map, every player, full stats).");
            }
            ImGui::PopID();
        }

        if (live_two_col) {
            ImGui::EndChild();          // close ##live_left
            ImGui::SameLine();
            ImGui::BeginChild("##live_right", ImVec2(live_right_w, 0),
                              ImGuiChildFlags_AutoResizeY);
        } else {
            // Stacked layout — visual breather between the scoreboards
            // and the round feed instead of a column gutter.
            ImGui::Spacing();
            ThinDivider(6);
        }

        // === ROUND DETAIL: result card + economy + kill feed ===
        SectionHeader("ROUND FEED", nullptr, kAccent);
        // Round to display events for: the IN-PROGRESS round if we're
        // partway through it (event_cursor > 0), else the most recently
        // completed round. This produces real play-by-play in auto_play
        // mode — kills tick out one at a time as event_cursor advances.
        int events_round_idx = -1;
        bool round_in_progress = false;
        if (live.event_cursor > 0 && live.round_cursor < (int)rounds.size()) {
            events_round_idx = live.round_cursor;
            round_in_progress = true;
        } else if (shown_idx > 0) {
            events_round_idx = shown_idx - 1;
        }
        if (events_round_idx >= 0) {
            auto& r = rounds[events_round_idx];
            int events_to_show = round_in_progress
                ? std::min(live.event_cursor, (int)r.events.size())
                : (int)r.events.size();
            ImU32 c1 = rec_t1
                ? IM_COL32(rec_t1->color_primary_r, rec_t1->color_primary_g, rec_t1->color_primary_b, 0xFF)
                : kVlrBlue;
            ImU32 c2 = rec_t2
                ? IM_COL32(rec_t2->color_primary_r, rec_t2->color_primary_g, rec_t2->color_primary_b, 0xFF)
                : kVlrRed;
            bool t1_won = (r.winner_name == rec.blue_name);

            // --- Round result card -------------------------------------
            // round # | winning-side color chip | outcome | score-after.
            // We don't have an explicit outcome enum on RoundLog, so infer
            // a tasteful label from the alive counts of the final event:
            // a side wiped => "elimination", otherwise "round won".
            {
                ImGui::BeginChild("##rd_hdr", ImVec2(0, 48),
                                  ImGuiChildFlags_Border,
                                  ImGuiWindowFlags_NoScrollbar |
                                  ImGuiWindowFlags_NoScrollWithMouse);
                ImVec2 hp = ImGui::GetCursorScreenPos();
                ImVec2 hs = ImGui::GetContentRegionAvail();
                ImDrawList* hdl = ImGui::GetWindowDrawList();
                ImColor dim_w(t1_won ? c1 : c2);
                dim_w.Value.x *= 0.30f; dim_w.Value.y *= 0.30f;
                dim_w.Value.z *= 0.30f; dim_w.Value.w = 1.0f;
                hdl->AddRectFilled(hp, ImVec2(hp.x + hs.x, hp.y + hs.y),
                                   (ImU32)dim_w, 4.0f);
                // Winning-side color chip on the left.
                hdl->AddRectFilled(hp, ImVec2(hp.x + 6, hp.y + hs.y),
                                   t1_won ? c1 : c2);
                ImGui::SetCursorScreenPos(ImVec2(hp.x + 16, hp.y + 11));
                ImGui::PushFont(g_font_h2);
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                ImGui::Text("ROUND %d", r.round);
                ImGui::PopStyleColor();
                ImGui::PopFont();
                ImGui::SameLine();
                ImGui::PushFont(g_font_small);
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrText));
                if (round_in_progress) {
                    // Spoiler-safe: never reveal winner or total kills mid-round.
                    ImGui::Text("   in progress \xE2\x80\x94 %d kills so far",
                                events_to_show);
                } else {
                    // WS-C R2: narrate HOW the round ended from the deterministic
                    // R1 metadata; fall back to the alive-count heuristic for
                    // empty / legacy RoundLogs (end_kind defaults to Elimination).
                    const char* how;
                    switch (r.end_kind) {
                        case vlr::RoundEndKind::SpikeDetonation: how = "spike detonated"; break;
                        case vlr::RoundEndKind::Defuse:
                            how = r.was_retake ? "defused after retake" : "defused"; break;
                        case vlr::RoundEndKind::TimeExpiry:      how = "time expired";   break;
                        case vlr::RoundEndKind::Elimination:
                        default:
                            how = (!r.events.empty() &&
                                   (r.events.back().t1_alive == 0 || r.events.back().t2_alive == 0))
                                  ? "team eliminated" : "round won";
                            break;
                    }
                    ImGui::Text("   %s \xE2\x80\x94 %s   (%d - %d)",
                                r.winner_name.c_str(), how, r.t1_score, r.t2_score);
                    if (r.spike_planted) {
                        ImGui::SameLine();
                        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                        ImGui::TextUnformatted(" \xE2\x80\xA2 spike");
                        ImGui::PopStyleColor();
                    }
                }
                ImGui::PopStyleColor();
                ImGui::PopFont();
                ImGui::EndChild();
            }

            // --- Economy panel -----------------------------------------
            // Compact per-team loadout card derived from round invest.
            {
                auto loadout_label = [](int invest) -> const char* {
                    if (invest >= 18000) return "OP BUY";
                    if (invest >= 14000) return "FULL BUY";
                    if (invest >= 8000)  return "HALF BUY";
                    if (invest >= 4500)  return "FORCE";
                    return "ECO";
                };
                bool have_econ = (r.t1_invest > 0 || r.t2_invest > 0);
                ImGui::Spacing();
                ImGui::BeginChild("##econ", ImVec2(0, 66),
                                  ImGuiChildFlags_Border,
                                  ImGuiWindowFlags_NoScrollbar |
                                  ImGuiWindowFlags_NoScrollWithMouse);
                ImVec2 ep = ImGui::GetCursorScreenPos();
                ImVec2 es = ImGui::GetContentRegionAvail();
                ImDrawList* edl = ImGui::GetWindowDrawList();
                if (!have_econ) {
                    ImGui::PushFont(g_font_small);
                    ImGui::SetCursorScreenPos(ImVec2(ep.x + 12, ep.y + 22));
                    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kTextFaint));
                    ImGui::TextUnformatted(
                        "Economy data not recorded for this round.");
                    ImGui::PopStyleColor();
                    ImGui::PopFont();
                } else {
                    float bar_w = es.x - 24.0f;
                    float bar_h = 16.0f;
                    auto frac = [](int v){ return std::min(1.0f,
                        (float)v / 22000.0f); };
                    auto draw_row = [&](float yo, ImU32 col, int invest,
                                        const char* tag) {
                        edl->AddRectFilled(ImVec2(ep.x + 12, ep.y + yo),
                            ImVec2(ep.x + 12 + bar_w, ep.y + yo + bar_h),
                            IM_COL32(0x14, 0x1B, 0x26, 0xFF), 3.0f);
                        edl->AddRectFilled(ImVec2(ep.x + 12, ep.y + yo),
                            ImVec2(ep.x + 12 + bar_w * frac(invest),
                                   ep.y + yo + bar_h), col, 3.0f);
                        char b[96];
                        std::snprintf(b, sizeof(b), "%s  \xE2\x80\xA2  %s  ($%d)",
                            tag, loadout_label(invest), invest);
                        edl->AddText(g_font_small ? g_font_small : nullptr,
                            g_font_small ? g_font_small->FontSize : 13.0f,
                            ImVec2(ep.x + 18, ep.y + yo + 1), kVlrText, b);
                    };
                    draw_row(10.0f, c1,
                        r.t1_invest,
                        rec_t1 && !rec_t1->tag.empty() ? rec_t1->tag.c_str()
                            : rec.blue_name.c_str());
                    draw_row(36.0f, c2,
                        r.t2_invest,
                        rec_t2 && !rec_t2->tag.empty() ? rec_t2->tag.c_str()
                            : rec.red_name.c_str());
                }
                ImGui::EndChild();
            }

            // --- Kill feed ---------------------------------------------
            // Each kill is a self-contained, fixed-height card. Layout is
            // computed ONCE per row (no overlapping absolute cursor math —
            // the old draw-list text bleed bug) and laid out via the
            // window draw list inside a measured InvisibleButton slot so
            // ImGui owns the row advance and nothing overlaps.
            ImGui::Spacing();
            ImGui::BeginChild("##events", ImVec2(0, 0),
                              ImGuiChildFlags_Border |
                              ImGuiChildFlags_AutoResizeY,
                              ImGuiWindowFlags_NoScrollbar |
                              ImGuiWindowFlags_NoScrollWithMouse);

            std::unordered_map<vlr::Player*, int> kill_count;
            for (int k_i = 0; k_i < events_to_show && k_i < (int)r.events.size(); ++k_i) {
                auto& ev = r.events[k_i];
                if (ev.killer) kill_count[ev.killer]++;
            }

            const float kRowH = 30.0f;
            const float kRowGap = 4.0f;
            ImFont* fs = g_font_small;
            float fsz = fs ? fs->FontSize : 13.0f;
            int ev_i = 0;
            int last_clutch_kills = 0;          // for the callout banner
            std::string clutch_name, ace_name;
            // Running per-killer counter — replaces the inner O(N) re-scan
            // that made the kill-feed loop O(N^2). Increment as we iterate.
            std::unordered_map<vlr::Player*, int> kills_running;
            for (int e_idx = 0; e_idx < events_to_show && e_idx < (int)r.events.size(); ++e_idx) {
                auto& ev = r.events[e_idx];
                if (!ev.killer || !ev.victim) continue;
                bool killer_t1 = (ev.team_won == 1);
                ImU32 kcol = killer_t1 ? c1 : c2;
                ImU32 vcol = killer_t1 ? c2 : c1;
                // Trade: previous kill went the other way (mirrors the
                // partial-stats heuristic — RoundEvent has no trade flag).
                bool traded = (e_idx > 0 &&
                    r.events[e_idx - 1].team_won != ev.team_won);
                int this_kill_idx = ++kills_running[ev.killer];
                int total_kills = kill_count[ev.killer];
                int adv = killer_t1 ? (ev.t1_alive - ev.t2_alive)
                                    : (ev.t2_alive - ev.t1_alive);
                bool clutch = (adv < 0 && this_kill_idx >= 2);
                if (clutch && this_kill_idx > last_clutch_kills) {
                    last_clutch_kills = this_kill_idx;
                    clutch_name = player_label(*ev.killer);
                }
                if (total_kills >= 5) ace_name = player_label(*ev.killer);

                ImGui::PushID(ev_i++);
                // Reserve the row with a real ImGui item so the next row
                // advances correctly — this is the core overlap fix.
                ImVec2 rp = ImGui::GetCursorScreenPos();
                float rw = ImGui::GetContentRegionAvail().x;
                ImGui::InvisibleButton("##krow", ImVec2(rw, kRowH + kRowGap));
                ImDrawList* rdl = ImGui::GetWindowDrawList();
                ImVec2 a(rp.x, rp.y), b(rp.x + rw - 2, rp.y + kRowH);

                rdl->AddRectFilled(a, b, kBgDeep, 5.0f);
                float bgi = 0.18f;
                if (total_kills >= 3) bgi = 0.30f;
                if (total_kills >= 4) bgi = 0.42f;
                ImColor bg(kcol);
                bg.Value.x *= bgi; bg.Value.y *= bgi; bg.Value.z *= bgi;
                bg.Value.w = 1.0f;
                rdl->AddRectFilled(a, b, (ImU32)bg, 5.0f);
                if (total_kills >= 3)
                    rdl->AddRect(a, b, kVlrGold, 5.0f, 0, 1.0f);
                rdl->AddRectFilled(a, ImVec2(a.x + 4, b.y), kcol, 5.0f,
                                   ImDrawFlags_RoundCornersLeft);

                float ty = rp.y + (kRowH - fsz) * 0.5f;
                // alive badge (fixed slot)
                char ab[12];
                std::snprintf(ab, sizeof(ab), "%dv%d",
                              ev.t1_alive, ev.t2_alive);
                rdl->AddText(fs, fsz, ImVec2(rp.x + 12, ty), kVlrSub, ab);
                // killer (bold team color)
                float cx = rp.x + 64.0f;
                std::string kn = player_label(*ev.killer);
                rdl->AddText(fs, fsz, ImVec2(cx, ty), kcol, kn.c_str());
                cx += ImGui::CalcTextSize(kn.c_str()).x + 6.0f;
                if (total_kills >= 2) {
                    const char* mk = total_kills == 2 ? "[2K]"
                                  : total_kills == 3 ? "[3K]"
                                  : total_kills == 4 ? "[4K!]"
                                  : "[ACE]";
                    rdl->AddText(fs, fsz, ImVec2(cx, ty), kVlrGold, mk);
                    cx += ImGui::CalcTextSize(mk).x + 6.0f;
                }
                // Kill separator. The recording stores no weapon string
                // or per-kill headshot flag, so we use a clean directional
                // marker rather than fabricating data.
                rdl->AddText(fs, fsz, ImVec2(cx, ty), kTextFaint,
                             "\xE2\x96\xB8");
                cx += 18.0f;
                // victim (dim)
                std::string vn = player_label(*ev.victim);
                rdl->AddText(fs, fsz, ImVec2(cx, ty), vcol, vn.c_str());
                cx += ImGui::CalcTextSize(vn.c_str()).x + 8.0f;
                if (traded) {
                    rdl->AddText(fs, fsz, ImVec2(cx, ty), kVlrSub, "[trade]");
                    cx += ImGui::CalcTextSize("[trade]").x + 6.0f;
                }
                if (clutch) {
                    rdl->AddText(fs, fsz, ImVec2(cx, ty), kVlrRed, "CLUTCH!");
                }
                ImGui::PopID();
            }
            ImGui::EndChild();

            // --- Clutch / multi-kill callout banner --------------------
            if (!ace_name.empty() || !clutch_name.empty()) {
                ImGui::Spacing();
                ImGui::BeginChild("##callout", ImVec2(0, 36),
                                  ImGuiChildFlags_Border,
                                  ImGuiWindowFlags_NoScrollbar |
                                  ImGuiWindowFlags_NoScrollWithMouse);
                ImVec2 cp = ImGui::GetCursorScreenPos();
                ImVec2 cs = ImGui::GetContentRegionAvail();
                ImDrawList* cdl = ImGui::GetWindowDrawList();
                ImU32 cc = !ace_name.empty() ? kVlrGold : kVlrRed;
                ImColor dimc(cc);
                dimc.Value.x *= 0.22f; dimc.Value.y *= 0.22f;
                dimc.Value.z *= 0.22f; dimc.Value.w = 1.0f;
                cdl->AddRectFilled(cp, ImVec2(cp.x + cs.x, cp.y + cs.y),
                                   (ImU32)dimc, 4.0f);
                cdl->AddRectFilled(cp, ImVec2(cp.x + 5, cp.y + cs.y), cc);
                char cb[120];
                if (!ace_name.empty())
                    std::snprintf(cb, sizeof(cb), "ACE  \xE2\x80\x94  %s",
                                  ace_name.c_str());
                else
                    std::snprintf(cb, sizeof(cb), "CLUTCH  \xE2\x80\x94  %s",
                                  clutch_name.c_str());
                ImGui::SetCursorScreenPos(ImVec2(cp.x + 16, cp.y + 8));
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(cc));
                ImGui::TextUnformatted(cb);
                ImGui::PopStyleColor();
                ImGui::EndChild();
            }
        } else {
            ImGui::TextDisabled("Click Next Round / Auto Play to begin.");
        }

        if (live_two_col) ImGui::EndChild();  // close ##live_right

        ImGui::Separator();
        if (ImGui::Button("Close", ImVec2(120, 32))) {
            s.live.open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

