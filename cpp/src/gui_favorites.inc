// gui_favorites.inc - unity-build fragment #included by gui_main.cpp (shares its namespace,
// helpers, colors, fonts, AppState). Favorites / GOAT / Formula Lab.  NOT a standalone TU.

void DrawFavorites(AppState& s) {
    SectionHeader("FAVORITES",
        "Favorited players, Hall of Fame, all-time / single-season GOAT "
        "rankings, and the formula lab — all in one place.",
        kAccent);
    VSpace(6);

    if (ImGui::BeginTabBar("##favtabs")) {   // guard: skip cleanly if collapsed/clipped

    // MVP Race, Awards Recap and Awards History now live under their own
    // top-level "Awards" nav tab (DrawAwards). Favorites keeps Hall of Fame,
    // GOAT rankings, the formula lab and the favorited-players list.

    // ---- Hall of Fame ----
    if (ImGui::BeginTabItem("Hall of Fame")) {
        // Filter out anyone not currently retired — strictly retirees only.
        std::vector<vlr::PlayerPtr> hof;
        for (auto& p : s.gm.hall_of_fame) {
            if (p && p->is_retired) hof.push_back(p);
        }
        if (hof.empty()) {
            ImGui::TextDisabled("No inductees yet. Players must retire first, "
                                "have 3+ seasons played, and meet 4 of the 9 "
                                "Hall of Fame criteria (international/role/MVP "
                                "title, 1.20+ season rating, top-20 solo, "
                                "$1M earnings, 30+ kill match, 2.0 KD match, "
                                "or Grand Final clutch).");
        } else if (ImGui::BeginTable("##hof_table", 6,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                ImGuiTableFlags_ScrollY, ImVec2(-FLT_MIN, 540))) {
            ImGui::TableSetupColumn("FLAG");
            ImGui::TableSetupColumn("NAME", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("REGION");
            ImGui::TableSetupColumn("CAREER M");
            ImGui::TableSetupColumn("AVG RAT");
            // Replaces the old "AWARDS" count column (which was just an
            // integer with no context) — now shows colored pills for each
            // of the 4+ HoF criteria the player actually satisfies, so
            // users can see WHY someone is enshrined without opening the
            // profile. Mirrors the criteria check in GameManager's
            // run_end_of_year HoF qualification pass.
            ImGui::TableSetupColumn("WHY IN HoF",
                ImGuiTableColumnFlags_WidthStretch, 0, 1);
            ImGui::TableHeadersRow();
            int idx = 0;
            // Iterate the filtered `hof` (retired-only); never raw hall_of_fame.
            for (auto& p : hof) {
                if (!p) continue;
                ImGui::PushID(idx++);
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); CountryFlag(p->country_iso);
                ImGui::TableNextColumn();
                if (ImGui::Selectable(player_label(*p).c_str(), false,
                        ImGuiSelectableFlags_SpanAllColumns)) {
                    OpenPlayerModal(s, p);
                }
                ImGui::TableNextColumn(); ImGui::Text("%s", p->region.c_str());
                ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%d", p->career_matches); ImGui::PopFont();
                ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%.2f", p->avg_match_rating()); ImGui::PopFont();
                // === Criteria pills ===
                // Mirrors the 4-of-10 check used by GameManager at year-end.
                // Each satisfied criterion renders as a small colored pill.
                ImGui::TableNextColumn();
                {
                    auto chip = [&](const char* lbl, ImU32 bg) {
                        Pill(lbl, bg, kVlrText);
                        ImGui::SameLine();
                    };
                    int intl_M = p->award_count_by_prefix("[M] ");
                    int intl_W = p->award_count_by_prefix("[W] ");
                    if (intl_W >= 1) {
                        char b[24]; std::snprintf(b, sizeof(b), "%dx Champ", intl_W);
                        chip(b, IM_COL32(0x55, 0x42, 0x14, 0xFF));
                    } else if (intl_M >= 1) {
                        char b[24]; std::snprintf(b, sizeof(b), "%dx Masters", intl_M);
                        chip(b, IM_COL32(0x35, 0x35, 0x35, 0xFF));
                    }
                    if (p->ever_won_mvp())        chip("MVP",          IM_COL32(0x4A, 0x3E, 0x14, 0xFF));
                    if (p->ever_won_role_award()) chip("Role oTY",     IM_COL32(0x2A, 0x3E, 0x60, 0xFF));
                    // 1.20+ across a full season — walk MatchHistoryEntry
                    bool had_120 = false;
                    for (auto& h : p->history) {
                        if (h.rating >= 1.20 && h.matches >= 8) { had_120 = true; break; }
                    }
                    if (had_120) chip("1.20+ Season", IM_COL32(0x14, 0x4A, 0x32, 0xFF));
                    if (p->ever_top20_solo) chip("Top-20 Solo Q", IM_COL32(0x40, 0x20, 0x40, 0xFF));
                    int earnings_k = 0;
                    for (auto& sl : p->salary_log) earnings_k += sl.second;
                    if (earnings_k >= 1000) chip("$1M+ Career", IM_COL32(0x14, 0x42, 0x42, 0xFF));
                    if (p->career_max_match_kills    >= 30)  chip("30-kill Match", IM_COL32(0x60, 0x30, 0x20, 0xFF));
                    if (p->career_max_match_kd_x100  >= 200) chip("2.0 KD Match",  IM_COL32(0x60, 0x40, 0x20, 0xFF));
                    if (p->career_grand_final_clutches >= 1) chip("GF Clutch",     IM_COL32(0x50, 0x14, 0x14, 0xFF));
                    if (p->is_igl && p->igl_impact_total > 50.0)
                        chip("IGL 50+", IM_COL32(0x1E, 0x3A, 0x55, 0xFF));
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::EndTabItem();
    }

    // ---- Favorited list ----
    if (ImGui::BeginTabItem("Favorited")) {
        if (s.gm.favorite_players.empty()) {
            ImGui::TextDisabled("No favorited players yet. Open a player "
                                "profile and click 'Favorite' to add them here.");
        } else if (ImGui::BeginTable("##favs", 8,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                ImGuiTableFlags_ScrollY, ImVec2(-FLT_MIN, 540))) {
            ImGui::TableSetupColumn("FLAG");
            ImGui::TableSetupColumn("NAME", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("ROLE");
            ImGui::TableSetupColumn("AGE");
            ImGui::TableSetupColumn("OVR");
            ImGui::TableSetupColumn("CAREER M");
            ImGui::TableSetupColumn("AVG RAT");
            ImGui::TableSetupColumn("MMR");
            ImGui::TableHeadersRow();
            int idx = 0;
            for (auto& p : s.gm.favorite_players) {
                if (!p) continue;
                ImGui::PushID(idx++);
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); CountryFlag(p->country_iso);
                ImGui::TableNextColumn();
                if (TableRowSelectable(p->name.c_str(), false,
                        ImGuiSelectableFlags_SpanAllColumns)) {
                    OpenPlayerModal(s, p);
                }
                ImGui::TableNextColumn();
                PositionBadge(vlr::position_of(*p));
                if (p->is_igl) { ImGui::SameLine(); IglBadge(); }
                ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%d", p->age); ImGui::PopFont();
                ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%.0f", p->ovr()); ImGui::PopFont();
                ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%d", p->career_matches); ImGui::PopFont();
                ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%.2f", p->avg_match_rating()); ImGui::PopFont();
                ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%d", p->solo_mmr); ImGui::PopFont();
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::EndTabItem();
    }

    // Helper: gather all players once for the GOAT computations.
    auto collect_players = [&]() {
        std::vector<vlr::PlayerPtr> all;
        for (auto& kv : s.gm.solo_qs) {
            for (auto& p : kv.second->global_ladder()) all.push_back(p);
        }
        return all;
    };

    // ---- Career GOAT ----
    if (ImGui::BeginTabItem("GOAT Career")) {
        Sub("All-time greats ranked by your formula. Min 10 career matches.");
        auto all = collect_players();
        auto rows = vlr::compute_goat_career(all, s.goat_weights, 100);
        if (rows.empty()) {
            ImGui::TextDisabled("(No qualifying careers yet — sim a few seasons.)");
        } else if (ImGui::BeginTable("##gcareer", 9,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                ImGuiTableFlags_ScrollY, ImVec2(-FLT_MIN, 520))) {
            ImGui::TableSetupColumn("#");
            ImGui::TableSetupColumn("FLAG");
            ImGui::TableSetupColumn("NAME", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("ROLE");
            ImGui::TableSetupColumn("CAREER M");
            ImGui::TableSetupColumn("AVG RAT");
            ImGui::TableSetupColumn("AWARDS");
            ImGui::TableSetupColumn("PEAK MMR");
            ImGui::TableSetupColumn("GOAT SCORE");
            ImGui::TableHeadersRow();
            int rank = 1, idx = 0;
            for (auto& r : rows) {
                if (!r.player) continue;
                ImGui::PushID(idx++);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(placement_color(rank)));
                ImGui::Text("#%d", rank);
                ImGui::PopStyleColor();
                ImGui::TableNextColumn(); CountryFlag(r.player->country_iso);
                ImGui::TableNextColumn();
                if (ImGui::Selectable(player_label(*r.player).c_str(), false,
                        ImGuiSelectableFlags_SpanAllColumns)) {
                    OpenPlayerModal(s, r.player);
                }
                ImGui::TableNextColumn();
                PositionBadge(vlr::position_of(*r.player));
                if (r.player->is_igl) { ImGui::SameLine(); IglBadge(); }
                ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%d", r.player->career_matches); ImGui::PopFont();
                ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%.2f", r.player->avg_match_rating()); ImGui::PopFont();
                ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%zu", r.player->awards.size()); ImGui::PopFont();
                ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%d", r.player->peak_mmr); ImGui::PopFont();
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                ImGui::PushFont(g_font_mono);
                ImGui::Text("%.1f", r.score);
                ImGui::PopFont();
                ImGui::PopStyleColor();
                ImGui::PopID();
                ++rank;
            }
            ImGui::EndTable();
        }
        ImGui::EndTabItem();
    }

    // ---- Season GOAT ----
    if (ImGui::BeginTabItem("GOAT Season")) {
        Sub("All-time best individual seasons (every season ever played by anyone, "
            "including retirees). Old legendary years stay on the board forever — "
            "never reset between seasons.");
        auto all = collect_players();
        // Generous cap so historic seasons aren't pushed off by recent ones.
        auto rows = vlr::compute_goat_season(all, s.goat_weights, 500);
        if (rows.empty()) {
            ImGui::TextDisabled("(No season history yet — sim a few seasons.)");
        } else if (ImGui::BeginTable("##gseason", 9,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                ImGuiTableFlags_ScrollY, ImVec2(-FLT_MIN, 520))) {
            ImGui::TableSetupColumn("#");
            ImGui::TableSetupColumn("FLAG");
            ImGui::TableSetupColumn("NAME", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("YEAR");
            ImGui::TableSetupColumn("TEAM");
            ImGui::TableSetupColumn("RAT");
            ImGui::TableSetupColumn("KD");
            ImGui::TableSetupColumn("ROLE");
            ImGui::TableSetupColumn("GOAT SCORE");
            ImGui::TableHeadersRow();
            int rank = 1, idx = 0;
            for (auto& r : rows) {
                if (!r.player) continue;
                ImGui::PushID(idx++);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(placement_color(rank)));
                ImGui::Text("#%d", rank);
                ImGui::PopStyleColor();
                ImGui::TableNextColumn(); CountryFlag(r.player->country_iso);
                ImGui::TableNextColumn();
                if (ImGui::Selectable(player_label(*r.player).c_str(), false,
                        ImGuiSelectableFlags_SpanAllColumns)) {
                    OpenPlayerModal(s, r.player);
                }
                ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%d", r.season); ImGui::PopFont();
                ImGui::TableNextColumn(); ImGui::Text("%s", r.season_team.c_str());
                ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%.2f", r.season_rating); ImGui::PopFont();
                ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%.2f", r.season_kd); ImGui::PopFont();
                ImGui::TableNextColumn();
                PositionBadge(vlr::position_of(*r.player));
                if (r.player->is_igl) { ImGui::SameLine(); IglBadge(); }
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                ImGui::PushFont(g_font_mono);
                ImGui::Text("%.1f", r.score);
                ImGui::PopFont();
                ImGui::PopStyleColor();
                ImGui::PopID();
                ++rank;
            }
            ImGui::EndTable();
        }
        ImGui::EndTabItem();
    }

    // ---- Formula Lab: edit the weights live ----
    if (ImGui::BeginTabItem("Formula Lab")) {
        H2("EDIT GOAT WEIGHTS");
        Sub("Tweak the coefficients that go into the GOAT score. Bigger value "
            "= bigger contribution to the ranking. Other tabs update live.");
        ImGui::Spacing();

        auto& w = s.goat_weights;
        auto slider = [&](const char* lbl, double& v, double lo, double hi, const char* fmt = "%.3f") {
            float fv = (float)v;
            ImGui::SetNextItemWidth(360);
            if (ImGui::SliderFloat(lbl, &fv, (float)lo, (float)hi, fmt)) v = fv;
        };

        H2("CHAMPIONSHIPS");
        slider("World Champion (per title)",   w.champ_world,    0.0, 100.0, "%.1f");
        slider("Masters Champion (per title)", w.champ_masters,  0.0, 60.0,  "%.1f");
        slider("Regional Champion (per title)",w.champ_regional, 0.0, 30.0,  "%.1f");

        ImGui::Spacing();
        H2("INDIVIDUAL AWARDS");
        slider("MVP award",                    w.mvp_award,      0.0, 60.0,  "%.1f");
        slider("Role of the Year",             w.role_award,     0.0, 40.0,  "%.1f");
        slider("IGL of the Year",              w.igl_award,      0.0, 40.0,  "%.1f");

        ImGui::Spacing();
        H2("PRO PLAY");
        slider("Per career match",             w.per_match,        0.0, 0.5,  "%.3f");
        slider("Avg rating bonus (rating-1.0)*x", w.avg_rating_bonus, 0.0, 200.0, "%.1f");
        slider("Peak rating bonus",            w.peak_rating_bonus,0.0, 120.0, "%.1f");
        slider("Per kill",                     w.per_kill,         0.0, 0.05, "%.4f");
        slider("Per assist",                   w.per_assist,       0.0, 0.05, "%.4f");
        slider("Per first blood",              w.per_first_blood,  0.0, 0.10, "%.4f");
        slider("Per match-MVP",                w.per_mvp_match,    0.0, 1.0,  "%.3f");

        ImGui::Spacing();
        H2("HOF MILESTONES");
        slider("Bonus: 30+ kill match",        w.bonus_30_kill_match, 0.0, 30.0, "%.1f");
        slider("Bonus: 2.0+ KD match",         w.bonus_2kd_match,     0.0, 30.0, "%.1f");
        slider("Bonus: Grand Final clutch",    w.bonus_gf_clutch,     0.0, 50.0, "%.1f");
        slider("Bonus: Top 20 solo Q ever",    w.bonus_top20_solo,    0.0, 30.0, "%.1f");

        ImGui::Spacing();
        H2("RANKED");
        slider("Per peak-MMR pt above 2000",   w.peak_mmr_above_2k,0.0, 0.05, "%.4f");
        slider("Per solo Q win",               w.per_solo_win,     0.0, 0.05, "%.4f");

        ImGui::Spacing();
        if (ImGui::Button("Reset to defaults", ImVec2(180, 32))) {
            s.goat_weights = vlr::GoatWeights{};
        }
        ImGui::EndTabItem();
    }

    // ---- Phase 1 (Agent C) additions: three new tabs --------------------
    if (ImGui::BeginTabItem("Trophy Room")) {
        DrawTrophyRoomTab(s);
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Hall of Records")) {
        DrawHallOfRecordsTab(s);
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Greatest Seasons")) {
        DrawGreatestSeasonsTab(s);
        ImGui::EndTabItem();
    }

    // === C8: Frivolities (5 "fun stats" sub-views) ========================
    // Each sub-view is a sortable table backed by existing accessors.
    // Empty-state strings cover early saves where the data doesn't qualify
    // yet. NO engine edits — everything derived locally from
    // all_players_world(s) + Player::trophy_summary / top_n_seasons /
    // career_max_ovr / potential / is_retired.
    if (ImGui::BeginTabItem("Frivolities")) {
        SectionHeader("FRIVOLITIES",
            "Fun-stat oddities — players who orbit the spotlight without "
            "the obvious trophy haul.",
            kAccent2);
        VSpace(6);

        if (ImGui::BeginTabBar("##friv_tabs")) {
            auto all_friv = all_players_world(s);

            // ----- 1. Best Without a LAN ----------------------------------
            // Top players by career rating who NEVER won [M] Masters or [W]
            // Worlds. Sortable: Name / Team / Career Rating / Trophies /
            // Seasons.
            if (ImGui::BeginTabItem("Best Without a LAN")) {
                struct R {
                    vlr::PlayerPtr p;
                    double rating = 0.0;
                    int trophies = 0;
                    int seasons = 0;
                };
                std::vector<R> rows;
                for (auto& p : all_friv) {
                    if (!p) continue;
                    auto ts = p->trophy_summary();
                    if (ts.masters > 0 || ts.worlds > 0) continue;
                    if (p->career_matches < 30) continue;
                    R r;
                    r.p = p;
                    r.rating = p->avg_match_rating();
                    r.trophies = ts.total_trophies();
                    r.seasons = static_cast<int>(p->top_n_seasons(99).size());
                    rows.push_back(r);
                }
                std::sort(rows.begin(), rows.end(),
                    [](const R& a, const R& b) { return a.rating > b.rating; });
                if (rows.size() > 100) rows.resize(100);
                if (rows.empty()) {
                    MutedText("No qualifying players yet.");
                } else if (ImGui::BeginTable("##fr_bwl", 5,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                        ImGuiTableFlags_BordersInnerV,
                        ImVec2(0, 460))) {
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableSetupColumn("Name");
                    ImGui::TableSetupColumn("Team");
                    ImGui::TableSetupColumn("Career Rating");
                    ImGui::TableSetupColumn("Trophies");
                    ImGui::TableSetupColumn("Seasons");
                    ImGui::TableHeadersRow();
                    int ri = 0;
                    for (auto& r : rows) {
                        ImGui::PushID(ri++);
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        if (TableRowSelectable(player_label(*r.p).c_str(),
                                false, ImGuiSelectableFlags_SpanAllColumns)) {
                            OpenPlayerModal(s, r.p);
                        }
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(r.p->team_name.c_str());
                        ImGui::TableNextColumn();
                        ImGui::Text("%.2f", r.rating);
                        ImGui::TableNextColumn();
                        ImGui::Text("%d", r.trophies);
                        ImGui::TableNextColumn();
                        ImGui::Text("%d", r.seasons);
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
                ImGui::EndTabItem();
            }

            // ----- 2. Youngest MVPs ---------------------------------------
            // Players who won [A] MVP at the youngest ages. We parse the
            // award strings — every MVP line is "[A] MVP YYYY".
            // age_at_win = (YYYY - birth_year) when birth_year known, else
            // skip. Sort: age_at_win ASC, year ASC.
            if (ImGui::BeginTabItem("Youngest MVPs")) {
                struct R {
                    vlr::PlayerPtr p;
                    int age = 0;
                    int year = 0;
                };
                std::vector<R> rows;
                for (auto& p : all_friv) {
                    if (!p || p->birth_year <= 0) continue;
                    for (auto& aw : p->awards) {
                        if (aw.find("[A] MVP ") != 0) continue;
                        // Extract trailing 4-digit year.
                        if (aw.size() < 4) continue;
                        std::string yr_s = aw.substr(aw.size() - 4);
                        bool digits = true;
                        for (char c : yr_s) if (!std::isdigit((unsigned char)c)) {
                            digits = false; break;
                        }
                        if (!digits) continue;
                        int yr = std::atoi(yr_s.c_str());
                        if (yr <= 0) continue;
                        R r;
                        r.p = p;
                        r.year = yr;
                        r.age = yr - p->birth_year;
                        if (r.age < 14 || r.age > 50) continue;
                        rows.push_back(r);
                    }
                }
                std::sort(rows.begin(), rows.end(),
                    [](const R& a, const R& b) {
                        if (a.age != b.age) return a.age < b.age;
                        return a.year < b.year;
                    });
                if (rows.size() > 100) rows.resize(100);
                if (rows.empty()) {
                    MutedText("No MVP winners on file yet.");
                } else if (ImGui::BeginTable("##fr_ym", 4,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                        ImGuiTableFlags_BordersInnerV,
                        ImVec2(0, 460))) {
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableSetupColumn("Name");
                    ImGui::TableSetupColumn("Age at MVP");
                    ImGui::TableSetupColumn("Year");
                    ImGui::TableSetupColumn("Team");
                    ImGui::TableHeadersRow();
                    int ri = 0;
                    for (auto& r : rows) {
                        ImGui::PushID(ri++);
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        if (TableRowSelectable(player_label(*r.p).c_str(),
                                false, ImGuiSelectableFlags_SpanAllColumns)) {
                            OpenPlayerModal(s, r.p);
                        }
                        ImGui::TableNextColumn();
                        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                        ImGui::Text("%d", r.age);
                        ImGui::PopStyleColor();
                        ImGui::TableNextColumn();
                        ImGui::Text("%d", r.year);
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(r.p->team_name.c_str());
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
                ImGui::EndTabItem();
            }

            // ----- 3. Hall of Good ----------------------------------------
            // Retired players who hit 2-3 HoF criteria but NOT 4+. We don't
            // have a public criteria-met accessor, so derive a local "score"
            // from common HoF-style thresholds (see §7 cheat sheet, "HoF
            // criteria"). 4+ -> HoF; 2-3 -> Hall of Good.
            if (ImGui::BeginTabItem("Hall of Good")) {
                struct R {
                    vlr::PlayerPtr p;
                    int score = 0;
                    int retire_year = 0;
                };
                auto score_player = [](const vlr::Player& p,
                                       int& trophies_out,
                                       double& rating_out) -> int {
                    int n = 0;
                    auto ts = p.trophy_summary();
                    trophies_out = ts.total_trophies();
                    rating_out = p.avg_match_rating();
                    if (ts.total_trophies() >= 1)        ++n;
                    if (ts.worlds      >= 1)             ++n;
                    if (ts.mvps        >= 1)             ++n;
                    if (ts.role_awards >= 1)             ++n;
                    if (p.career_matches >= 200)         ++n;
                    if (p.career_kills   >= 5000)        ++n;
                    if (rating_out >= 1.15)              ++n;
                    if (p.career_max_match_kills >= 30)  ++n;
                    if (p.igl_impact_total >= 50.0)      ++n;
                    return n;
                };
                std::vector<R> rows;
                for (auto& p : all_friv) {
                    if (!p || !p->is_retired) continue;
                    // 2026-05-28 audit: require a meaningful career body of
                    // work — short-career flukes (e.g. one Champions run
                    // followed by retirement) were polluting the list. Real
                    // "Hall of Good" should mean a sustained pro tenure that
                    // fell just short of full HoF, not random 2-3 season
                    // careers with one trophy.
                    if (p->career_seasons_played < 5) continue;
                    if (p->career_matches < 150)      continue;
                    int trophies = 0; double rating = 0.0;
                    int n = score_player(*p, trophies, rating);
                    // Exactly 3 criteria (4+ is real HoF; <3 is just "had a
                    // pro career"). Was 2-3; the 2-band let single-criterion
                    // accidents (one MVP + one role award) qualify too easily.
                    if (n != 3) continue;
                    // Career rating floor — Hall of Good should imply at
                    // least a competent pro, not a journeyman who happened
                    // to ride a winning team.
                    if (rating < 1.00) continue;
                    R r;
                    r.p = p;
                    r.score = n;
                    r.retire_year = 0;
                    // Last career_timeline entry of category "retirement"
                    // gives the retire year (best-effort).
                    for (auto& ev : p->career_timeline()) {
                        if (ev.category == "retirement") r.retire_year = ev.year;
                    }
                    rows.push_back(r);
                }
                std::sort(rows.begin(), rows.end(),
                    [](const R& a, const R& b) {
                        if (a.score != b.score) return a.score > b.score;
                        return a.p->avg_match_rating() > b.p->avg_match_rating();
                    });
                if (rows.size() > 100) rows.resize(100);
                if (rows.empty()) {
                    MutedText("No retirees on the border-of-greatness list yet.");
                } else if (ImGui::BeginTable("##fr_hog", 5,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                        ImGuiTableFlags_BordersInnerV,
                        ImVec2(0, 460))) {
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableSetupColumn("Name");
                    ImGui::TableSetupColumn("Criteria Met");
                    ImGui::TableSetupColumn("Career Rating");
                    ImGui::TableSetupColumn("Trophies");
                    ImGui::TableSetupColumn("Retired Year");
                    ImGui::TableHeadersRow();
                    int ri = 0;
                    for (auto& r : rows) {
                        ImGui::PushID(ri++);
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        if (TableRowSelectable(player_label(*r.p).c_str(),
                                false, ImGuiSelectableFlags_SpanAllColumns)) {
                            OpenPlayerModal(s, r.p);
                        }
                        ImGui::TableNextColumn();
                        ImGui::Text("%d / 9", r.score);
                        ImGui::TableNextColumn();
                        ImGui::Text("%.2f", r.p->avg_match_rating());
                        ImGui::TableNextColumn();
                        ImGui::Text("%d",
                            r.p->trophy_summary().total_trophies());
                        ImGui::TableNextColumn();
                        if (r.retire_year > 0) ImGui::Text("%d", r.retire_year);
                        else ImGui::TextDisabled("\xE2\x80\x94");
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
                ImGui::EndTabItem();
            }

            // ----- 4. Biggest Busts ---------------------------------------
            // career_max_ovr < potential - 15 AND (is_retired OR age >= 28).
            // Reason chip: derived from consistency / work_ethic / age
            // heuristics. Sort by gap DESC.
            if (ImGui::BeginTabItem("Biggest Busts")) {
                struct R {
                    vlr::PlayerPtr p;
                    int gap = 0;
                    const char* reason = "Inconsistent";
                };
                std::vector<R> rows;
                for (auto& p : all_friv) {
                    if (!p) continue;
                    if (p->potential <= 0 || p->career_max_ovr <= 0) continue;
                    int gap = p->potential - p->career_max_ovr;
                    if (gap <= 15) continue;
                    if (!p->is_retired && p->age < 28) continue;
                    R r;
                    r.p = p;
                    r.gap = gap;
                    if (p->work_ethic < 50)      r.reason = "Off-meta";
                    else if (p->consistency < 50) r.reason = "Inconsistent";
                    else                          r.reason = "Injured";
                    rows.push_back(r);
                }
                std::sort(rows.begin(), rows.end(),
                    [](const R& a, const R& b) { return a.gap > b.gap; });
                if (rows.size() > 100) rows.resize(100);
                if (rows.empty()) {
                    MutedText("No qualifying players yet (need POT - peak >= 16).");
                } else if (ImGui::BeginTable("##fr_bb", 5,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                        ImGuiTableFlags_BordersInnerV,
                        ImVec2(0, 460))) {
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableSetupColumn("Name");
                    ImGui::TableSetupColumn("POT");
                    ImGui::TableSetupColumn("Peak OVR");
                    ImGui::TableSetupColumn("Gap");
                    ImGui::TableSetupColumn("Reason");
                    ImGui::TableHeadersRow();
                    int ri = 0;
                    for (auto& r : rows) {
                        ImGui::PushID(ri++);
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        if (TableRowSelectable(player_label(*r.p).c_str(),
                                false, ImGuiSelectableFlags_SpanAllColumns)) {
                            OpenPlayerModal(s, r.p);
                        }
                        ImGui::TableNextColumn();
                        ImGui::Text("%d", r.p->potential);
                        ImGui::TableNextColumn();
                        ImGui::Text("%d", r.p->career_max_ovr);
                        ImGui::TableNextColumn();
                        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrRed));
                        ImGui::Text("%d", r.gap);
                        ImGui::PopStyleColor();
                        ImGui::TableNextColumn();
                        Pill(r.reason, kAccent2);
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
                ImGui::EndTabItem();
            }

            // ----- 5. Best Single-Season Performances Without a Title -----
            // Top single-season ratings whose awards_that_year contains no
            // [W]/[M]/[T] trophy line — the team didn't win a championship
            // that year (proxy via the player's awards snapshot).
            if (ImGui::BeginTabItem("Seasons Without a Title")) {
                struct R {
                    vlr::PlayerPtr p;
                    int year = 0;
                    double rating = 0.0;
                    std::string team_name;
                    std::string finish;
                };
                std::vector<R> rows;
                for (auto& p : all_friv) {
                    if (!p) continue;
                    for (auto& h : p->top_n_seasons(3)) {
                        bool has_title = false;
                        for (auto& aw : h.awards_that_year) {
                            if (aw.find("[T] ") == 0
                                || aw.find("[M] ") == 0
                                || aw.find("[W] ") == 0) {
                                has_title = true; break;
                            }
                        }
                        if (has_title) continue;
                        R r;
                        r.p = p;
                        r.year = h.year;
                        r.rating = h.rating;
                        r.team_name = h.team_name;
                        r.finish = "(no title)";
                        rows.push_back(r);
                    }
                }
                std::sort(rows.begin(), rows.end(),
                    [](const R& a, const R& b) {
                        return a.rating > b.rating;
                    });
                if (rows.size() > 100) rows.resize(100);
                if (rows.empty()) {
                    MutedText("No qualifying seasons yet.");
                } else if (ImGui::BeginTable("##fr_swt", 5,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                        ImGuiTableFlags_BordersInnerV,
                        ImVec2(0, 460))) {
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableSetupColumn("Name");
                    ImGui::TableSetupColumn("Season");
                    ImGui::TableSetupColumn("Rating");
                    ImGui::TableSetupColumn("Team");
                    ImGui::TableSetupColumn("Finish");
                    ImGui::TableHeadersRow();
                    int ri = 0;
                    for (auto& r : rows) {
                        ImGui::PushID(ri++);
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        if (TableRowSelectable(player_label(*r.p).c_str(),
                                false, ImGuiSelectableFlags_SpanAllColumns)) {
                            OpenPlayerModal(s, r.p);
                        }
                        ImGui::TableNextColumn();
                        ImGui::Text("%d", r.year);
                        ImGui::TableNextColumn();
                        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGreen));
                        ImGui::Text("%.2f", r.rating);
                        ImGui::PopStyleColor();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(r.team_name.c_str());
                        ImGui::TableNextColumn();
                        MutedText("%s", r.finish.c_str());
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }
        ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
    }  // ##favtabs guard
}
