// gui_team.inc - unity-build fragment #included by gui_main.cpp (shares its namespace,
// helpers, colors, fonts, AppState). DrawTeamProfile.  NOT a standalone TU.

void DrawTeamProfile(AppState& s) {
    if (!s.selected_team) {
        ImGui::TextDisabled("(no team selected)");
        return;
    }
    auto t = s.selected_team;

    // Tier-aware competition label: "VCT" for tier-1, "Challengers" /
    // "Challengers Ascension" etc. for lower tiers. A relegated user team
    // must NOT keep reading "VCT". Falls back to "VCT" only when the team
    // has no resolvable league (shouldn't happen for a rostered team).
    auto _div_league = s.gm.league_for(t);
    const char* div_label = (_div_league && !_div_league->division_name().empty())
                            ? _div_league->division_name().c_str() : "VCT";

    // Back button — pop the navigation stack so we step back through the
    // whole drill-down chain instead of jumping straight to the origin.
    if (ImGui::Button("< Back", ImVec2(80, 28))) {
        if (!s.nav_stack.empty()) {
            auto prev = s.nav_stack.back();
            s.nav_stack.pop_back();
            s.screen = prev.first;
            s.selected_team = prev.second;   // may be null for a non-profile screen
        } else {
            s.screen = Screen::Dashboard;
            s.selected_team.reset();
        }
        return;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("|  Team profile");

    ImGui::Spacing();

    // === Banner =========================================================
    // Phase 2: large 96px TeamLogo on the left, replacing the prior
    // tag-square. CountryFlag now sits between the logo and the name.
    ImGui::BeginChild("##team_banner", ImVec2(0, 140), ImGuiChildFlags_Border);
    ImGui::SetCursorPos(ImVec2(16, 12));
    TeamLogo(*t, 96.f);
    ImGui::SameLine();
    ImGui::Dummy(ImVec2(12, 0));
    ImGui::SameLine();
    ImGui::BeginGroup();
    CountryFlag(t->home_country_iso);
    ImGui::SameLine();
    ImGui::Dummy(ImVec2(6, 0));
    ImGui::SameLine();
    ImGui::PushFont(g_font_h1);
    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrRed));
    ImGui::TextUnformatted(t->name.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::PushFont(g_font_small);
    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
    ImGui::Text("%s %s   |   Based in %s, %s   |   Payroll $%dK/yr   |   Roster OVR %.0f",
                div_label, t->region.c_str(),
                t->home_city.empty() ? "?" : t->home_city.c_str(),
                t->home_country.empty() ? "?" : t->home_country.c_str(),
                t->total_payroll_k(),
                t->ovr());
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 4));
    Pill(vlr::identity_name(t->comp_identity), kAccent2);
    ImGui::SameLine();
    Pill(vlr::brand_tag_name(t->identity.brand), kVlrGold);   // WS-B: brand-tag identity
    ImGui::SameLine();
    {
        const char* pers = "Balanced";
        switch (t->personality) {
            case vlr::Personality::Aggressive: pers = "Aggressive"; break;
            case vlr::Personality::Tactical:   pers = "Tactical";   break;
            case vlr::Personality::Budget:     pers = "Budget";     break;
            case vlr::Personality::Balanced:   pers = "Balanced";   break;
        }
        PillOutline(pers, kBorderStrong, kVlrSub);
    }
    // P6.2: dynasty-tier badge — only when the club has earned one (titles/finals
    // in the trailing 5y window). The Contender/Champion Era tiers are newly
    // reachable via the P6.1 finals_history wiring.
    switch (t->dynasty_tier(s.gm.year)) {
        case vlr::Team::DynastyTier::Contender:   ImGui::SameLine(); Pill("Contender", kAccent);     break;
        case vlr::Team::DynastyTier::ChampionEra: ImGui::SameLine(); Pill("Champion Era", kVlrGold); break;
        case vlr::Team::DynastyTier::Dynasty:     ImGui::SameLine(); Pill("DYNASTY", kVlrRed);       break;
        case vlr::Team::DynastyTier::None: default: break;
    }
    ImGui::EndGroup();
    ImGui::EndChild();
    ImGui::Spacing();

    // === KPI row =========================================================
    int rank = 1;
    auto t_league = s.gm.league_for(t);   // rank vs the team's ACTUAL division
    if (t_league) {
        std::vector<vlr::TeamPtr> sorted_t;
        for (auto& tt : t_league->teams()) if (tt) sorted_t.push_back(tt);
        std::sort(sorted_t.begin(), sorted_t.end(),
                  [](const vlr::TeamPtr& a, const vlr::TeamPtr& b) { return a->phase_wins > b->phase_wins; });
        for (auto& tt : sorted_t) { if (tt == t) break; ++rank; }
    }

    {
        char b[32];
        std::snprintf(b, sizeof(b), "%dW-%dL", t->phase_wins, t->phase_losses);
        StatTile("PHASE RECORD", b, kAccent);
    }
    ImGui::SameLine();
    {
        char b[32]; std::snprintf(b, sizeof(b), "#%d", rank);
        // Tier-aware label: "CHALLENGERS RANK" for lower divisions.
        std::string rank_label = (t_league && t_league->tier() > 1)
            ? (t_league->division_name() + " RANK") : std::string("REGIONAL RANK");
        StatTile(rank_label.c_str(), b, kVlrGold);
    }
    ImGui::SameLine();
    {
        char b[32]; std::snprintf(b, sizeof(b), "%d-%d", t->wins, t->losses);
        StatTile("SEASON RECORD", b, kAccent);
    }
    ImGui::SameLine();
    {
        char b[32]; std::snprintf(b, sizeof(b), "%.0f", t->ovr());
        StatTile("ROSTER OVR", b, kAccent2);
    }
    ImGui::SameLine();
    StatTile("BUDGET", fmt_money(t->budget), kVlrGreen);

    ImGui::Spacing();
    ImGui::Spacing();

    // === QW: HONOURS / trophy cabinet ===================================
    // trophy_case() was shipped "for the trophy ribbon UI" but had zero render
    // sites — a dynasty sim with invisible silverware. Render it.
    {
        auto tc = t->trophy_case();
        int total = tc.regional_titles + tc.masters_titles + tc.world_titles;
        if (BeginCard("##honours")) {
            SectionHeader("HONOURS", nullptr, kVlrGold);
            if (total == 0 && tc.total_finals == 0) {
                ImGui::TextDisabled("No silverware yet \xE2\x80\x94 the cabinet awaits.");
            } else {
                bool any = false;
                if (tc.world_titles > 0) {
                    char b[48]; std::snprintf(b, sizeof(b), "%d World", tc.world_titles);
                    Pill(b, kVlrGold); ImGui::SameLine(); any = true;
                }
                if (tc.masters_titles > 0) {
                    char b[48]; std::snprintf(b, sizeof(b), "%d Masters", tc.masters_titles);
                    Pill(b, kAccent2); ImGui::SameLine(); any = true;
                }
                if (tc.regional_titles > 0) {
                    char b[48]; std::snprintf(b, sizeof(b), "%d Regional", tc.regional_titles);
                    Pill(b, kAccent); ImGui::SameLine(); any = true;
                }
                if (tc.total_finals > 0) {
                    char b[48]; std::snprintf(b, sizeof(b), "%d Finals", tc.total_finals);
                    PillOutline(b, kBorderStrong, kVlrSub); ImGui::SameLine(); any = true;
                }
                if (any) ImGui::NewLine();
                // Chronological ribbon, most recent first (cap at 12 lines).
                ImGui::PushFont(g_font_small);
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
                int shown = 0;
                for (auto it = tc.ordered.rbegin();
                     it != tc.ordered.rend() && shown < 12; ++it, ++shown)
                    ImGui::Text("%d   \xE2\x80\xA2   %s", it->first, it->second.c_str());
                ImGui::PopStyleColor();
                ImGui::PopFont();
            }
        } EndCard();
        ImGui::Spacing();
    }

    // === Tabs: Overview / Recent / Upcoming / Maps / Bio =================
    if (ImGui::BeginTabBar("##teamtabs")) {
        if (ImGui::BeginTabItem("Roster")) {
            // === 4 canonical + 1 flexible-5th display layout ============
            // Mirror DrawRoster: walk roster[0..4], first occupant per
            // canonical position locks that slot, the leftover starter
            // takes the "5th / Flex" slot. Everything past roster[4] (or
            // displaced duplicates) drops to the bench.
            std::array<vlr::PlayerPtr,
                       (size_t)vlr::Position::Count> tp_canonical{};
            vlr::PlayerPtr tp_fifth;
            std::vector<vlr::PlayerPtr> tp_bench;
            {
                std::size_t starter_count =
                    std::min<std::size_t>(t->roster.size(), 5);
                std::vector<vlr::PlayerPtr> displaced;
                for (std::size_t i = 0; i < starter_count; ++i) {
                    auto& pp = t->roster[i];
                    if (!pp) continue;
                    std::size_t idx = (std::size_t)vlr::position_of(*pp);
                    if (!tp_canonical[idx])      tp_canonical[idx] = pp;
                    else if (!tp_fifth)          tp_fifth = pp;
                    else                         displaced.push_back(pp);
                }
                for (std::size_t i = starter_count; i < t->roster.size(); ++i)
                    if (t->roster[i]) tp_bench.push_back(t->roster[i]);
                for (auto& d : displaced) tp_bench.push_back(d);
                std::sort(tp_bench.begin(), tp_bench.end(),
                          [](const vlr::PlayerPtr& a, const vlr::PlayerPtr& b) {
                              return a->ovr() > b->ovr();
                          });
            }

            auto tp_row = [&](vlr::Position slot,
                              const vlr::PlayerPtr& p,
                              bool dup_warn,
                              int row_id,
                              const char* slot_label = nullptr) {
                ImGui::PushID(row_id);
                ImGui::TableNextRow();
                if (!p) {
                    ImGui::TableNextColumn(); ImGui::TextDisabled("--");
                    ImGui::TableNextColumn();
                    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrRed));
                    if (slot_label && slot_label[0])
                        ImGui::Text("[empty %s]", slot_label);
                    else
                        ImGui::Text("[empty %s]", vlr::position_name(slot));
                    ImGui::PopStyleColor();
                    ImGui::TableNextColumn();
                    if (slot_label && slot_label[0])
                        ImGui::TextDisabled("--");
                    else
                        PositionBadge(slot);
                    ImGui::TableNextColumn(); ImGui::TextDisabled("--");
                    ImGui::TableNextColumn(); ImGui::TextDisabled("--");
                    ImGui::TableNextColumn(); ImGui::TextDisabled("--");
                    ImGui::TableNextColumn(); ImGui::TextDisabled("--");
                    ImGui::TableNextColumn(); ImGui::TextDisabled("--");
                    ImGui::PopID();
                    return;
                }
                ImGui::TableNextColumn(); CountryFlag(p->country_iso);
                ImGui::TableNextColumn();
                if (TableRowSelectable(player_label(*p).c_str(), false,
                        ImGuiSelectableFlags_SpanAllColumns)) {
                    OpenPlayerModal(s, p);
                }
                // Signature agent chip beside gamertag (matches DrawRoster).
                ImGui::SameLine();
                SigBadge(*p);
                ImGui::TableNextColumn();
                // Canonical chip order: PositionBadge → FlexBadge → IglBadge.
                PositionBadge(vlr::position_of(*p));
                if (p->is_flex) { ImGui::SameLine(); FlexBadge(); }
                if (p->is_igl)  { ImGui::SameLine(); IglBadge();  }
                if (dup_warn) {
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrRed));
                    ImGui::PushFont(g_font_small);
                    ImGui::Text("[duplicate]");
                    ImGui::PopFont();
                    ImGui::PopStyleColor();
                }
                ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%d", p->age); ImGui::PopFont();
                ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%.0f", p->ovr()); ImGui::PopFont();
                ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%.2f", p->avg_match_rating()); ImGui::PopFont();
                ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%.1f", p->kast()); ImGui::PopFont();
                ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("$%dK", p->contract.amount_k); ImGui::PopFont();
                ImGui::PopID();
            };

            H2("STARTERS");
            if (ImGui::BeginTable("##tp_roster", 8,
                    ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                    ImGuiTableFlags_ScrollY, ImVec2(-FLT_MIN, 220))) {
                ImGui::TableSetupColumn("FLAG");
                ImGui::TableSetupColumn("PLAYER", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("POSITION");
                ImGui::TableSetupColumn("AGE");
                ImGui::TableSetupColumn("OVR");
                ImGui::TableSetupColumn("AVG RAT");
                ImGui::TableSetupColumn("KAST");
                ImGui::TableSetupColumn("SALARY");
                ImGui::TableHeadersRow();
                int row_id = 0;
                for (size_t i = 0; i < (size_t)vlr::Position::Count; ++i) {
                    auto slot = (vlr::Position)i;
                    tp_row(slot, tp_canonical[i], false, row_id++);
                }
                // 5th / Flex row (no PositionBadge in the empty-state).
                tp_row(vlr::Position::Initiator, tp_fifth, false, row_id++,
                       "5th slot");
                ImGui::EndTable();
            }
            if (!tp_bench.empty()) {
                ImGui::Spacing();
                H2("BENCH");
                if (ImGui::BeginTable("##tp_roster_bench", 8,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                        ImGuiTableFlags_ScrollY, ImVec2(-FLT_MIN, 150))) {
                    ImGui::TableSetupColumn("FLAG");
                    ImGui::TableSetupColumn("PLAYER", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("POSITION");
                    ImGui::TableSetupColumn("AGE");
                    ImGui::TableSetupColumn("OVR");
                    ImGui::TableSetupColumn("AVG RAT");
                    ImGui::TableSetupColumn("KAST");
                    ImGui::TableSetupColumn("SALARY");
                    ImGui::TableHeadersRow();
                    int row_id = 2000;
                    for (auto& bp : tp_bench)
                        tp_row(vlr::position_of(*bp), bp, false, row_id++);
                    ImGui::EndTable();
                }
            }
            if (t->head_coach) {
                ImGui::Spacing();
                ImGui::BeginChild("##tp_coach", ImVec2(0, 110), ImGuiChildFlags_Border);
                H2("HEAD COACH");
                ImGui::Text("%s   age %d   |   Tactical %d  Dev %d  Leadership %d  |  $%dK/yr",
                            t->head_coach->name.c_str(), t->head_coach->age,
                            t->head_coach->tactical, t->head_coach->development,
                            t->head_coach->leadership, t->head_coach->salary_k);
                // Coach personality — drives mid-season replacement
                // aggressiveness + dev style. Engine has it; UI surfaces it.
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                ImGui::Text("Personality: %s",
                            vlr::coach_personality_name(t->head_coach->personality));
                ImGui::PopStyleColor();
                ImGui::PushFont(g_font_small);
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
                ImGui::TextWrapped("%s",
                    vlr::coach_personality_blurb(t->head_coach->personality));
                ImGui::PopStyleColor();
                ImGui::PopFont();
                ImGui::EndChild();
            }
            ImGui::EndTabItem();
        }

        // === Recent matches: pull from any roster member's pro history ===
        if (ImGui::BeginTabItem("Recent Matches")) {
            std::vector<vlr::RecordedMatchPtr> recent;
            if (!t->roster.empty() && t->roster.front()) {
                for (auto& r : t->roster.front()->pro_match_history) recent.push_back(r);
            }
            if (recent.empty()) {
                ImGui::TextDisabled("(no recorded matches yet)");
            } else if (ImGui::BeginTable("##tp_recent", 6,
                    ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                    ImGuiTableFlags_ScrollY, ImVec2(-FLT_MIN, 380))) {
                ImGui::TableSetupColumn("EVENT", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("MAP");
                ImGui::TableSetupColumn("OPPONENT");
                ImGui::TableSetupColumn("SCORE");
                ImGui::TableSetupColumn("RESULT");
                ImGui::TableSetupColumn("WATCH");
                ImGui::TableHeadersRow();
                int idx = 0;
                for (auto& rec : recent) {
                    if (!rec) continue;
                    ImGui::PushID(idx++);
                    bool is_blue = (rec->blue_name == t->name);
                    std::string opp = is_blue ? rec->red_name : rec->blue_name;
                    bool won = is_blue
                        ? (rec->blue_score > rec->red_score)
                        : (rec->red_score  > rec->blue_score);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::Text("%s", rec->event.c_str());
                    ImGui::TableNextColumn(); ImGui::Text("%s", rec->map_name.c_str());
                    ImGui::TableNextColumn(); ImGui::Text("%s", opp.c_str());
                    ImGui::TableNextColumn(); ImGui::Text("%s", rec->score.c_str());
                    ImGui::TableNextColumn();
                    ImGui::PushStyleColor(ImGuiCol_Text, toV4(won ? kVlrGreen : kVlrRed));
                    ImGui::Text("%s", won ? "W" : "L");
                    ImGui::PopStyleColor();
                    ImGui::TableNextColumn();
                    if (ImGui::SmallButton("Watch")) OpenReplay(s, rec);
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }

        // === Upcoming matches: filter the calendar to this team =========
        if (ImGui::BeginTabItem("Upcoming")) {
            auto fixtures = s.gm.upcoming_fixtures(30);
            std::vector<vlr::UpcomingFixture> mine;
            for (auto& f : fixtures) if (f.a == t || f.b == t) mine.push_back(f);
            if (mine.empty()) {
                ImGui::TextDisabled("(no fixtures scheduled in the next 30 days)");
            } else if (ImGui::BeginTable("##tp_upc", 4,
                    ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
                ImGui::TableSetupColumn("DAY");
                ImGui::TableSetupColumn("EVENT");
                ImGui::TableSetupColumn("OPPONENT", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("WHEN");
                ImGui::TableHeadersRow();
                int upc_idx = 0;
                for (auto& f : mine) {
                    ImGui::PushID(upc_idx++);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    int delta = f.day_in_year - s.gm.day_in_year;
                    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                    if (delta == 0) ImGui::Text("TODAY");
                    else            ImGui::Text("+%dd", delta);
                    ImGui::PopStyleColor();
                    ImGui::TableNextColumn(); ImGui::Text("%s", f.label.c_str());
                    ImGui::TableNextColumn();
                    {
                        vlr::TeamPtr opp = (f.a == t) ? f.b : f.a;
                        if (opp) {
                            CountryFlag(opp->home_country_iso); ImGui::SameLine();
                            if (TableRowSelectable(opp->name.c_str(), false,
                                    ImGuiSelectableFlags_SpanAllColumns)) {
                                OpenTeamProfile(s, opp);
                            }
                        }
                    }
                    ImGui::TableNextColumn();
                    ImGui::Text("Day %d/%d", f.day_in_year + 1,
                                vlr::GameManager::kDaysPerYear);
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }

        // === Maps: aggregate roster's per-map ratings ===================
        if (ImGui::BeginTabItem("Maps")) {
            if (ImGui::BeginTable("##tp_maps", 4,
                    ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
                ImGui::TableSetupColumn("MAP", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("MATCHES PLAYED");
                ImGui::TableSetupColumn("AVG TEAM RATING");
                ImGui::TableSetupColumn("FAVORED ROLE");
                ImGui::TableHeadersRow();
                for (auto& m : vlr::maps()) {
                    int total_count = 0;
                    double total_rating = 0.0;
                    for (auto& p : t->roster) {
                        if (!p) continue;
                        auto it = p->map_stats.find(m.name);
                        if (it == p->map_stats.end()) continue;
                        total_count += it->second.count;
                        total_rating += it->second.rating_total;
                    }
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::Text("%s", m.name.c_str());
                    ImGui::TableNextColumn(); ImGui::Text("%d", total_count);
                    ImGui::TableNextColumn();
                    if (total_count > 0) {
                        double avg = total_rating / total_count;
                        ImU32 col = avg >= 1.10 ? kVlrGreen : (avg >= 0.95 ? kVlrText : kVlrRed);
                        ImGui::PushStyleColor(ImGuiCol_Text, toV4(col));
                        ImGui::Text("%.2f", avg);
                        ImGui::PopStyleColor();
                    } else {
                        ImGui::TextDisabled("—");
                    }
                    ImGui::TableNextColumn();
                    ImGui::PushStyleColor(ImGuiCol_Text, toV4(role_color(m.favored_role)));
                    ImGui::Text("%s", vlr::role_name(m.favored_role));
                    ImGui::PopStyleColor();
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }

        // === Bio / org info =============================================
        if (ImGui::BeginTabItem("Bio")) {
            H2("ORGANIZATION");
            ImGui::Text("Name:");        ImGui::SameLine(180); ImGui::Text("%s", t->name.c_str());
            ImGui::Text("Region:");      ImGui::SameLine(180); ImGui::Text("%s %s", div_label, t->region.c_str());
            ImGui::Text("Home country:"); ImGui::SameLine(180);
            CountryFlag(t->home_country_iso); ImGui::SameLine();
            ImGui::Text("%s", t->home_country.empty() ? "?" : t->home_country.c_str());
            ImGui::Text("Headquarters:"); ImGui::SameLine(180);
            ImGui::Text("%s", t->home_city.empty() ? "?" : t->home_city.c_str());
            ImGui::Text("Personality:"); ImGui::SameLine(180);
            switch (t->personality) {
                case vlr::Personality::Aggressive: ImGui::Text("Aggressive"); break;
                case vlr::Personality::Tactical:   ImGui::Text("Tactical"); break;
                case vlr::Personality::Budget:     ImGui::Text("Budget"); break;
                case vlr::Personality::Balanced:   ImGui::Text("Balanced"); break;
            }
            // Comp shape — every team is 4 canonical gameplay slots plus
            // a flexible 5th. Flex and IGL are independent OVERLAY flags
            // that any starter may carry (Team::enforce_one_flex ensures
            // >=1 is_flex player exists in the starting 5; multiples are
            // allowed). Flex tendency below derives the concrete rotation
            // list from the is_flex player's agent_pool roles.
            ImGui::Text("Comp shape:"); ImGui::SameLine(180);
            ImGui::TextWrapped(
                "1D + 1C + 1S + 1I + 1 flexible 5th "
                "(Flex/secondary I/secondary C). "
                "1 IGL flag overlaid on any starter.");

            // === Flex tendency — concrete rotation list =================
            // Algorithm:
            //   1. Collect every roster member with is_flex == true. Per
            //      the new model multiple Flex starters are allowed
            //      (Team::enforce_one_flex only ensures >=1 exists).
            //   2. Pull each Flex player's agent_pool, collect distinct
            //      roles (primary_role first, drop primary_role if it
            //      never appears in the pool).
            //   3. Format:
            //        - 0 Flex players in roster -> "No designated Flex
            //                                      player (rare)."
            //        - 1 Flex player, 0 agents  -> "not yet set"
            //        - 1 Flex player, 1 role    -> "locked to {Role}"
            //                                      + "(low rotation player)"
            //        - 1 Flex player, 2-3 roles -> "{Name} rotates between
            //                                      {Role A / Role B [/ C]}"
            //        - 2+ Flex players          -> "Flex players: {names},
            //                                      rotating between
            //                                      {Role A / Role B [/ C]}"
            ImGui::Text("Flex tendency:"); ImGui::SameLine(180);
            {
                std::vector<vlr::PlayerPtr> flex_players;
                for (auto& pp : t->roster) {
                    if (pp && pp->is_flex) flex_players.push_back(pp);
                }
                if (flex_players.empty()) {
                    ImGui::TextDisabled("No designated Flex player (rare).");
                } else if (flex_players.size() == 1) {
                    auto& flex_p = flex_players.front();
                    if (flex_p->agent_pool.empty()) {
                        ImGui::TextDisabled("not yet set");
                    } else {
                        std::vector<vlr::Role> roles_seen;
                        auto push_unique = [&](vlr::Role r) {
                            for (auto x : roles_seen) if (x == r) return;
                            roles_seen.push_back(r);
                        };
                        push_unique(flex_p->primary_role);
                        for (auto* a : flex_p->agent_pool) {
                            if (a) push_unique(a->role);
                        }
                        bool primary_in_pool = false;
                        for (auto* a : flex_p->agent_pool) {
                            if (a && a->role == flex_p->primary_role) {
                                primary_in_pool = true; break;
                            }
                        }
                        if (!primary_in_pool && !roles_seen.empty()
                            && roles_seen.front() == flex_p->primary_role) {
                            roles_seen.erase(roles_seen.begin());
                        }
                        if (roles_seen.empty()) {
                            ImGui::TextDisabled("not yet set");
                        } else if (roles_seen.size() == 1) {
                            ImGui::Text("locked to %s",
                                        vlr::role_name(roles_seen[0]));
                            ImGui::PushFont(g_font_small);
                            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
                            ImGui::SameLine();
                            ImGui::Text("(low rotation player)");
                            ImGui::PopStyleColor();
                            ImGui::PopFont();
                        } else {
                            if (roles_seen.size() > 3) roles_seen.resize(3);
                            std::string list;
                            for (size_t i = 0; i < roles_seen.size(); ++i) {
                                if (i) list += " / ";
                                list += vlr::role_name(roles_seen[i]);
                            }
                            ImGui::Text("%s rotates between %s",
                                        flex_p->name.c_str(), list.c_str());
                        }
                    }
                } else {
                    // Multiple Flex players — show names + union of roles.
                    std::vector<vlr::Role> roles_seen;
                    auto push_unique = [&](vlr::Role r) {
                        for (auto x : roles_seen) if (x == r) return;
                        roles_seen.push_back(r);
                    };
                    std::string names;
                    for (size_t i = 0; i < flex_players.size(); ++i) {
                        if (i) names += ", ";
                        names += flex_players[i]->name;
                        push_unique(flex_players[i]->primary_role);
                        for (auto* a : flex_players[i]->agent_pool) {
                            if (a) push_unique(a->role);
                        }
                    }
                    if (roles_seen.size() > 3) roles_seen.resize(3);
                    std::string list;
                    for (size_t i = 0; i < roles_seen.size(); ++i) {
                        if (i) list += " / ";
                        list += vlr::role_name(roles_seen[i]);
                    }
                    if (list.empty()) {
                        ImGui::Text("Flex players: %s", names.c_str());
                    } else {
                        ImGui::Text("Flex players: %s, rotating between %s",
                                    names.c_str(), list.c_str());
                    }
                }
            }

            // Comp Identity — distinct from "personality" and "strategy".
            // Surfaces the new CompIdentity classification on Team. Display
            // only; value is engine-driven.
            ImGui::Text("Comp Identity:"); ImGui::SameLine(180);
            ImGui::Text("%s", vlr::identity_name(t->comp_identity));

            // === WS-B TEAM IDENTITY panel (B1) — playstyle fingerprint =====
            ImGui::Spacing();
            H2("TEAM IDENTITY");
            {
                const auto& id = t->identity;
                Pill(vlr::brand_tag_name(id.brand), kVlrGold);
                ImGui::SameLine();
                ImGui::TextDisabled(id.user_chosen ? "(chosen)" : "(emergent)");
                ImGui::PushFont(g_font_small);
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
                ImGui::TextUnformatted(vlr::brand_tag_blurb(id.brand));
                ImGui::PopStyleColor();
                ImGui::PopFont();
                ImGui::Spacing();
                ImGui::Text("Methodical"); ImGui::SameLine(180);
                ImGui::ProgressBar(static_cast<float>(id.aggression), ImVec2(200, 0), "");
                ImGui::SameLine(); ImGui::TextUnformatted("Aggressive");
                ImGui::Text("Win-Now"); ImGui::SameLine(180);
                ImGui::ProgressBar(static_cast<float>(id.dev_youth), ImVec2(200, 0), "");
                ImGui::SameLine(); ImGui::TextUnformatted("Youth-First");
                ImGui::Text("Preferred comp:"); ImGui::SameLine(180);
                ImGui::Text("%s", vlr::comp_tag_name(id.comp_lean));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Identity gives a small bounded match tilt (<=~1%% win rate)\n"
                                      "and biases comp picks; it never overrides the engine's caps.");
                // Region meta — the league-wide style this club is shaped by.
                ImGui::Text("Region style:"); ImGui::SameLine(180);
                Pill(vlr::region_meta(t->region).brand, kAccent2);
                ImGui::SameLine();
                ImGui::TextDisabled("(%s)", t->region.c_str());
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s's meta nudges this club's identity. At internationals,\n"
                                      "a style clash vs another region gives a tiny edge/disadvantage.",
                                      t->region.c_str());
            }

            // === C3: Strategy + inertia ====================================
            // Surfaces Agent A's `t->strategy` plus a from-previous shift
            // indicator. If `strategy == previous_strategy` (stable) the
            // trailing label is suppressed. A local lambda maps the enum
            // to a display name — kept inside the function body per the
            // "no new top-level functions" rule.
            auto strategy_name = [](vlr::Team::Strategy s) -> const char* {
                switch (s) {
                    case vlr::Team::Strategy::Contender:        return "Contender";
                    case vlr::Team::Strategy::Rebuilding:       return "Rebuilding";
                    case vlr::Team::Strategy::Bridge:           return "Bridge";
                    case vlr::Team::Strategy::BudgetRoster:     return "Budget Roster";
                    case vlr::Team::Strategy::DevelopmentFocus: return "Development Focus";
                    case vlr::Team::Strategy::WinNow:           return "Win Now";
                    case vlr::Team::Strategy::TalentFarm:       return "Talent Farm";
                }
                return "?";
            };
            ImGui::Text("Strategy:"); ImGui::SameLine(180);
            PillOutline(strategy_name(t->strategy), kAccent, kVlrText);
            if (t->previous_strategy != t->strategy) {
                ImGui::SameLine();
                ImGui::PushFont(g_font_small);
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
                ImGui::Text("\xE2\x86\x90 from %s",
                            strategy_name(t->previous_strategy));
                ImGui::PopStyleColor();
                ImGui::PopFont();
            }

            // === Phase B (C1): Championship Window indicator ===============
            // Surfaces Agent A's `t->window` lifecycle classifier. Pill is
            // colour-coded by window state and gets a directional glyph
            // (rising / peak / hourglass / falling). A one-line MutedText
            // below explains the behavioural implication for AI decisions.
            {
                ImU32 wcol = kAccent;
                const char* glyph = "\xE2\x97\x8F"; // ●
                const char* blurb = "";
                switch (t->window) {
                    case vlr::TeamWindow::Opening:
                        wcol = kVlrGreen;
                        glyph = "\xE2\x96\xB2"; // ▲
                        blurb = "Building a young core "
                                "\xE2\x80\x94 patient development, "
                                "regional focus.";
                        break;
                    case vlr::TeamWindow::Open:
                        wcol = kAccent;
                        glyph = "\xE2\x97\x8F"; // ●
                        blurb = "Prime contention years "
                                "\xE2\x80\x94 re-sign cores, push for "
                                "titles.";
                        break;
                    case vlr::TeamWindow::Closing:
                        wcol = kVlrGold;
                        glyph = "\xE2\x8F\xB3"; // ⏳
                        blurb = "Window closing "
                                "\xE2\x80\x94 aggressive imports, "
                                "win-now spending.";
                        break;
                    case vlr::TeamWindow::Closed:
                        wcol = kVlrRedDim;
                        glyph = "\xE2\x96\xBC"; // ▼
                        blurb = "Rebuild phase "
                                "\xE2\x80\x94 cycling out vets, "
                                "betting on potential.";
                        break;
                }
                ImGui::Text("Window:"); ImGui::SameLine(180);
                char wlabel[64];
                std::snprintf(wlabel, sizeof(wlabel), "%s WINDOW: %s",
                              glyph, vlr::team_window_name(t->window));
                Pill(wlabel, wcol);
                MutedText("%s", blurb);
            }

            // === C2: Org Memory grid =======================================
            // Surfaces Agent A's six rolling memory metrics. Each tile shows
            // a small-caps label, signed value (green/red/text by sign +
            // magnitude) and a thin centered horizontal bar that extends
            // left/right from the midpoint to encode the [-100,+100] value.
            // 3 columns x 2 rows; tile ~ 200x60.
            ImGui::Spacing();
            SectionHeader("ORG MEMORY",
                          "Hidden rolling metrics that steer long-term AI",
                          kAccent2);
            {
                struct MemMetric { const char* label; int value; };
                MemMetric mm[6] = {
                    { "ROOKIE SUCCESS",      t->memory.rookie_success       },
                    { "IMPORT SUCCESS",      t->memory.import_success       },
                    { "VETERAN SUCCESS",     t->memory.veteran_success      },
                    { "FINANCIAL DISCIPLINE",t->memory.financial_discipline },
                    { "STABILITY CULTURE",   t->memory.stability_culture    },
                    { "STAR DEPENDENCY",     t->memory.star_dependency      },
                };
                const ImVec2 tile_size(200.0f, 60.0f);
                for (int i = 0; i < 6; ++i) {
                    if (i % 3 != 0) ImGui::SameLine();
                    ImGui::PushID(900 + i);
                    if (BeginCardSized("##memtile", tile_size, kSurface,
                                       8.0f, 10.0f)) {
                        int v = mm[i].value;
                        if (v < -100) v = -100; else if (v > 100) v = 100;
                        ImU32 vcol = (v >=  30) ? kVlrGreen
                                   : (v <= -30) ? kVlrRed
                                   :              kVlrText;
                        // Label
                        ImGui::PushFont(g_font_small);
                        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
                        ImGui::TextUnformatted(mm[i].label);
                        ImGui::PopStyleColor();
                        ImGui::PopFont();
                        // Signed value
                        ImGui::PushStyleColor(ImGuiCol_Text, toV4(vcol));
                        ImGui::Text("%+d", v);
                        ImGui::PopStyleColor();
                        // Inline bar — centered on midpoint, extends per sign.
                        {
                            ImVec2 cp = ImGui::GetCursorScreenPos();
                            float avail = ImGui::GetContentRegionAvail().x;
                            if (avail < 40.0f) avail = 40.0f;
                            float bar_h = 5.0f;
                            float mid_x = cp.x + avail * 0.5f;
                            ImDrawList* dl = ImGui::GetWindowDrawList();
                            // Track
                            dl->AddRectFilled(
                                ImVec2(cp.x, cp.y),
                                ImVec2(cp.x + avail, cp.y + bar_h),
                                kBorder, bar_h * 0.5f);
                            // Filled signed bar
                            float half = avail * 0.5f;
                            float ext  = (std::abs(v) / 100.0f) * half;
                            if (ext > 0.0f) {
                                ImVec2 a = (v >= 0)
                                    ? ImVec2(mid_x, cp.y)
                                    : ImVec2(mid_x - ext, cp.y);
                                ImVec2 b = (v >= 0)
                                    ? ImVec2(mid_x + ext, cp.y + bar_h)
                                    : ImVec2(mid_x, cp.y + bar_h);
                                dl->AddRectFilled(a, b, vcol, bar_h * 0.5f);
                            }
                            // Midpoint tick
                            dl->AddLine(ImVec2(mid_x, cp.y - 1.0f),
                                        ImVec2(mid_x, cp.y + bar_h + 1.0f),
                                        kBorderStrong, 1.0f);
                            ImGui::Dummy(ImVec2(avail, bar_h + 2.0f));
                        }
                    }
                    EndCardSized();
                    ImGui::PopID();
                }
                MutedText("Org memory drives long-term AI decisions. "
                          "High Rookie Success -> more youth signings. "
                          "Negative Import Success -> AI shies away from "
                          "foreign FAs.");
            }

            // === Phase B (C2): Roster chemistry panel ======================
            // Surfaces Agent A's `top_chemistry_pairs(n)` — co-play synergy
            // values learned from match outcomes. Each pair gets a coloured
            // value (green = cohesive, red = toxic, otherwise muted) and a
            // small inline horizontal bar in the [-2, +2] range so the user
            // can visually compare magnitudes at a glance.
            ImGui::Spacing();
            SectionHeader("ROSTER CHEMISTRY",
                          "Co-play synergy built from match outcomes",
                          kAccent2);
            {
                auto pairs = t->top_chemistry_pairs(5);
                if (pairs.empty()) {
                    MutedText("No co-play history yet "
                              "\xE2\x80\x94 chemistry develops over "
                              "the season.");
                } else {
                    int ci = 0;
                    for (auto& tup : pairs) {
                        auto* pa = std::get<0>(tup);
                        auto* pb = std::get<1>(tup);
                        double v = std::get<2>(tup);
                        if (!pa || !pb) continue;
                        ImGui::PushID(8200 + ci++);
                        if (BeginCard("##chempair", kSurface, 10.0f, 8.0f)) {
                            ImU32 vcol = (v >=  1.0) ? kVlrGreen
                                       : (v <= -0.5) ? kVlrRed
                                       :               kVlrSub;
                            // Row label: "A <-> B"
                            ImGui::TextUnformatted(pa->name.c_str());
                            ImGui::SameLine();
                            ImGui::TextDisabled("\xE2\x86\x94");
                            ImGui::SameLine();
                            ImGui::TextUnformatted(pb->name.c_str());
                            // Signed value, coloured.
                            ImGui::SameLine();
                            ImGui::Dummy(ImVec2(10, 0));
                            ImGui::SameLine();
                            ImGui::PushStyleColor(ImGuiCol_Text, toV4(vcol));
                            ImGui::Text("%+.2f", v);
                            ImGui::PopStyleColor();
                            // Inline bar — [-2, +2] range, centered on mid.
                            {
                                ImVec2 cp = ImGui::GetCursorScreenPos();
                                float avail =
                                    ImGui::GetContentRegionAvail().x;
                                if (avail < 60.0f) avail = 60.0f;
                                float bar_h = 5.0f;
                                float mid_x = cp.x + avail * 0.5f;
                                ImDrawList* dl =
                                    ImGui::GetWindowDrawList();
                                dl->AddRectFilled(
                                    ImVec2(cp.x, cp.y),
                                    ImVec2(cp.x + avail, cp.y + bar_h),
                                    kBorder, bar_h * 0.5f);
                                double vc = v;
                                if (vc < -2.0) vc = -2.0;
                                if (vc >  2.0) vc =  2.0;
                                float half = avail * 0.5f;
                                float ext  =
                                    (float)(std::abs(vc) / 2.0) * half;
                                if (ext > 0.0f) {
                                    ImVec2 a = (vc >= 0)
                                        ? ImVec2(mid_x, cp.y)
                                        : ImVec2(mid_x - ext, cp.y);
                                    ImVec2 b = (vc >= 0)
                                        ? ImVec2(mid_x + ext,
                                                 cp.y + bar_h)
                                        : ImVec2(mid_x, cp.y + bar_h);
                                    dl->AddRectFilled(
                                        a, b, vcol, bar_h * 0.5f);
                                }
                                dl->AddLine(
                                    ImVec2(mid_x, cp.y - 1.0f),
                                    ImVec2(mid_x, cp.y + bar_h + 1.0f),
                                    kBorderStrong, 1.0f);
                                ImGui::Dummy(ImVec2(avail, bar_h + 2.0f));
                            }
                        }
                        EndCard();
                        ImGui::PopID();
                    }
                }
            }

            ImGui::Spacing();
            H2("HISTORICAL RECORD");
            if (t->history.empty()) {
                ImGui::TextDisabled("(no completed seasons yet)");
            } else if (ImGui::BeginTable("##tp_hist", 4,
                    ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
                ImGui::TableSetupColumn("YEAR");
                ImGui::TableSetupColumn("WINS");
                ImGui::TableSetupColumn("LOSSES");
                ImGui::TableSetupColumn("WIN RATE");
                ImGui::TableHeadersRow();
                for (auto& h : t->history) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::Text("%d", h.year);
                    ImGui::TableNextColumn(); ImGui::Text("%d", h.wins);
                    ImGui::TableNextColumn(); ImGui::Text("%d", h.losses);
                    ImGui::TableNextColumn();
                    int total = h.wins + h.losses;
                    if (total > 0) ImGui::Text("%.0f%%", 100.0 * h.wins / total);
                    else           ImGui::TextDisabled("—");
                }
                ImGui::EndTable();
            }

            // === Rivalries (C10 — uses Pack B h2h split accessors) ====
            // Lists every frequent opponent (h2h_total >= 3 this season)
            // with reg/playoff split + finals-title count between teams.
            ImGui::Spacing();
            H2("RIVALRIES (this season)");
            {
                struct RivRow {
                    vlr::Team* opp = nullptr;
                    int total = 0;
                    int reg = 0;
                    int po = 0;
                    int finals_titles_vs = 0;
                };
                std::vector<RivRow> rivs;
                for (auto& kv : s.gm.leagues) {
                    if (!kv.second) continue;
                    for (auto& tt : kv.second->teams()) {
                        if (!tt || tt.get() == t.get()) continue;
                        int total = s.gm.h2h_total(t.get(), tt.get());
                        if (total < 3) continue;
                        RivRow r;
                        r.opp = tt.get();
                        r.total = total;
                        r.reg = s.gm.h2h_regular(t.get(), tt.get());
                        r.po  = s.gm.h2h_playoff(t.get(), tt.get());
                        auto finals = s.gm.h2h_finals_between(
                            t.get(), tt.get());
                        for (auto& fF : finals) {
                            if (fF.winner == t.get()) r.finals_titles_vs++;
                        }
                        rivs.push_back(r);
                    }
                }
                std::sort(rivs.begin(), rivs.end(),
                    [](const RivRow& a, const RivRow& b) {
                        return a.total > b.total;
                    });
                if (rivs.empty()) {
                    MutedText("No frequent rivals this season "
                              "(need >= 3 meetings to qualify).");
                } else if (ImGui::BeginTable("##tp_rivs", 5,
                        ImGuiTableFlags_RowBg |
                        ImGuiTableFlags_BordersInnerV)) {
                    ImGui::TableSetupColumn("VS");
                    ImGui::TableSetupColumn("Total");
                    ImGui::TableSetupColumn("Regular");
                    ImGui::TableSetupColumn("Playoff");
                    ImGui::TableSetupColumn("Finals (won by this team)");
                    ImGui::TableHeadersRow();
                    int ri = 0;
                    for (auto& r : rivs) {
                        if (!r.opp) continue;
                        ImGui::PushID(ri++);
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        if (!r.opp->tag.empty()) {
                            ImU32 tc = IM_COL32(r.opp->color_primary_r,
                                r.opp->color_primary_g,
                                r.opp->color_primary_b, 0xFF);
                            ImGui::PushStyleColor(ImGuiCol_Text, toV4(tc));
                            ImGui::TextUnformatted(r.opp->tag.c_str());
                            ImGui::PopStyleColor();
                            ImGui::SameLine();
                        }
                        ImGui::TextUnformatted(r.opp->name.c_str());
                        ImGui::TableNextColumn(); ImGui::Text("%d", r.total);
                        ImGui::TableNextColumn(); ImGui::Text("%d", r.reg);
                        ImGui::TableNextColumn(); ImGui::Text("%d", r.po);
                        ImGui::TableNextColumn();
                        if (r.finals_titles_vs > 0) {
                            ImGui::PushStyleColor(ImGuiCol_Text,
                                toV4(kVlrGold));
                            ImGui::Text("%d", r.finals_titles_vs);
                            ImGui::PopStyleColor();
                        } else {
                            ImGui::TextDisabled("0");
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
            }
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
}

// ===== Calendar screen: upcoming fixtures grouped by day =================
// =========================================================================
// Calendar (Phase 2): month-grid layout
// =========================================================================
// Replaces the legacy table view. Renders a 5x7 grid of 80x56 day cells
// centred on `today`, navigable with +/- 30 day buttons. Cells are tinted
// by phase / matchday status; hover shows a tooltip with all matchups for
// that day across regions; click opens a modal listing the day's matchups
// with click-through into the live viewer.
//
// Schedule data: we drain `gm.upcoming_fixtures(max_days_ahead)` which returns
// already-paired stage round-robin fixtures across all regions for the
// requested look-ahead window. That's the canonical accessor and works
// for any positive look-ahead. Tournament matchdays are tinted via the
// phase background (REGIONALS/MASTERS/CHAMPIONS), but per-matchup data
// for those isn't returned by `upcoming_fixtures` — the bracket UI is the
// dedicated surface for that.
namespace {

struct CalDayInfo {
    int  day = -1;        // day_in_year, or -1 for empty trailing cells
    int  year = 0;
    bool is_today = false;
    bool is_user_match = false;
    bool is_matchday = false;
    bool is_tournament = false;
    std::string phase;          // e.g. "STAGE 1", "REGIONALS 1", "OFFSEASON"
    std::string short_tag;      // small per-day note ("vs FNX" / "MASTERS R1")
    std::vector<vlr::UpcomingFixture> fixtures;
};

// Background tint per phase. Returns 0 if no tint (keeps default cell bg).
ImU32 phase_tint(const std::string& phase) {
    if (phase.find("STAGE") == 0)        return IM_COL32(0x16, 0x22, 0x30, 0xFF);
    if (phase.find("REGIONALS") == 0)    return IM_COL32(0x40, 0x18, 0x18, 0x60);
    if (phase.find("MASTERS") == 0)      return IM_COL32(0x4A, 0x2A, 0x60, 0x60);
    if (phase.find("CHAMPIONS") == 0)    return IM_COL32(0x5C, 0x46, 0x16, 0x80);
    if (phase == "AWARDS")               return IM_COL32(0x60, 0x4A, 0x16, 0x90);
    if (phase == "OFFSEASON")            return IM_COL32(0x1A, 0x2B, 0x1A, 0xFF);
    return 0;
}
