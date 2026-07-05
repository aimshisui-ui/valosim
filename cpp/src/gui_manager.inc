// gui_manager.inc - unity-build fragment #included by gui_main.cpp (shares its namespace,
// helpers, colors, fonts, AppState). DrawManager.  NOT a standalone TU.

void DrawManager(AppState& s) {
    SectionHeader("MANAGER",
        "Run your organization — player development, head coach, and "
        "finances all in one place.",
        kAccent);
    VSpace(6);

    auto& t = s.gm.user_team;
    if (!t) { ImGui::TextDisabled("(no user team selected)"); return; }

    if (ImGui::BeginTabBar("##manager_tabs")) {   // guard

    // ===== PLAYER DEVELOPMENT =====
    if (ImGui::BeginTabItem("Player Development")) {
        H2("MONTHLY DEVELOPMENT REPORTS");
        Sub("Every 30 in-game days each player runs a development pass. "
            "Pros grow based on match performance + coaching; FAs grow from "
            "ranked performance + natural age curve.");
        ImGui::Spacing();

        auto& list = s.gm.recent_progression_reports;
        if (list.empty()) {
            ImGui::TextDisabled("(no recent monthly progression — advance ~30 days)");
        } else if (ImGui::BeginTable("##mgr_prog", 6,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                ImGuiTableFlags_ScrollY, ImVec2(-FLT_MIN, 280))) {
            ImGui::TableSetupColumn("PLAYER");
            ImGui::TableSetupColumn("TYPE");
            ImGui::TableSetupColumn("MATCHES");
            ImGui::TableSetupColumn("AVG RAT");
            ImGui::TableSetupColumn("CHANGES");
            ImGui::TableSetupColumn("EXPLANATION", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            int idx = 0;
            for (auto& r : list) {
                ImGui::PushID(idx++);
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%s", r.player_name.c_str());
                ImGui::TableNextColumn();
                ImGui::TextColored(toV4(r.was_pro ? kVlrGreen : kInfo),
                                   "%s", r.was_pro ? "PRO" : "FA");
                ImGui::TableNextColumn();
                if (r.was_pro) ImGui::Text("%d", r.matches_in_window);
                else           ImGui::Text("%d ranked", r.ranked_played);
                ImGui::TableNextColumn();
                if (r.matches_in_window > 0) ImGui::Text("%.2f", r.avg_rating);
                else                          ImGui::Text("-");
                ImGui::TableNextColumn();
                if (r.changes.empty()) {
                    ImGui::TextDisabled("-");
                } else {
                    for (auto& c : r.changes) {
                        ImU32 col = c.second > 0 ? kVlrGreen : kVlrRed;
                        ImGui::PushStyleColor(ImGuiCol_Text, toV4(col));
                        ImGui::Text("%s%+d ", vlr::attr_name(c.first), c.second);
                        ImGui::PopStyleColor();
                        ImGui::SameLine();
                    }
                    ImGui::NewLine();
                }
                ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", r.explanation.c_str());
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        ImGui::Spacing();
        H2("ROSTER FORM TRACKER");
        Sub("Snapshot of each starter's form, age, and trajectory. Players "
            "below 0.95 rating may attract mid-season replacement attention "
            "from strict coaches.");

        if (ImGui::BeginTable("##mgr_form", 8,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV,
                ImVec2(-FLT_MIN, 220))) {
            ImGui::TableSetupColumn("PLAYER", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("AGE");
            ImGui::TableSetupColumn("OVR");
            ImGui::TableSetupColumn("POT");
            ImGui::TableSetupColumn("AVG RAT");
            ImGui::TableSetupColumn("SEASON RAT");
            ImGui::TableSetupColumn("FORM");
            ImGui::TableSetupColumn("AT RISK");
            ImGui::TableHeadersRow();
            int ri = 0;
            for (auto& p : t->roster) {
                if (!p) continue;
                ImGui::PushID(ri++);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (TableRowSelectable(player_label(*p).c_str(), false,
                        ImGuiSelectableFlags_SpanAllColumns)) {
                    OpenPlayerModal(s, p);
                }
                ImGui::TableNextColumn(); ImGui::Text("%d", p->age);
                ImGui::TableNextColumn(); ImGui::Text("%.0f", p->ovr());
                ImGui::TableNextColumn(); ImGui::Text("%d", p->potential);
                ImGui::TableNextColumn(); ImGui::Text("%.2f", p->avg_match_rating());
                double srat = p->season_matches > 0
                    ? p->season_rating_total / p->season_matches : 0.0;
                ImGui::TableNextColumn();
                if (p->season_matches > 0) ImGui::Text("%.2f", srat);
                else                       ImGui::TextDisabled("-");
                ImGui::TableNextColumn();
                {
                    const char* form = "STEADY";
                    ImU32 col = kVlrText;
                    if (srat >= 1.20)      { form = "HOT";    col = kVlrGreen; }
                    else if (srat >= 1.05) { form = "GOOD";   col = kVlrText;  }
                    else if (srat >= 0.95) { form = "STEADY"; col = kVlrText;  }
                    else if (srat > 0.0)   { form = "COLD";   col = kVlrRed;   }
                    else                   { form = "-";      col = kVlrSub;   }
                    ImGui::PushStyleColor(ImGuiCol_Text, toV4(col));
                    ImGui::Text("%s", form);
                    ImGui::PopStyleColor();
                }
                ImGui::TableNextColumn();
                // Centralised "at risk" check on Player — replaces the
                // previously hard-coded UI threshold so engine + UI can't
                // silently diverge.
                bool at_risk = p->is_form_at_risk();
                if (at_risk) {
                    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrRed));
                    ImGui::TextUnformatted("YES");
                    ImGui::PopStyleColor();
                } else { ImGui::TextDisabled("no"); }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        if (s.god_mode) {
            ImGui::Spacing();
            H2("HIDDEN: PEAK ARCHETYPES (god mode)");
            Sub("Internal aging curve assignments. Players don't know which "
                "archetype they have; coaches infer it over time.");
            if (ImGui::BeginTable("##mgr_arch", 4, ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("PLAYER", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("ARCHETYPE");
                ImGui::TableSetupColumn("PEAK AGE");
                ImGui::TableSetupColumn("WORK ETHIC");
                ImGui::TableHeadersRow();
                for (auto& p : t->roster) {
                    if (!p) continue;
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::Text("%s", p->name.c_str());
                    ImGui::TableNextColumn();
                    const char* a = "Standard";
                    switch (p->growth_archetype) {
                        case vlr::GrowthArchetype::EarlyPeaker: a = "Early Peaker"; break;
                        case vlr::GrowthArchetype::LateBloomer: a = "Late Bloomer"; break;
                        case vlr::GrowthArchetype::Standard:    a = "Standard";     break;
                    }
                    ImGui::Text("%s", a);
                    ImGui::TableNextColumn(); ImGui::Text("%d", p->peak_age);
                    ImGui::TableNextColumn(); ImGui::Text("%d", p->work_ethic);
                }
                ImGui::EndTable();
            }
        }
        ImGui::EndTabItem();
    }

    // ===== HEAD COACH MANAGEMENT =====
    if (ImGui::BeginTabItem("Head Coach")) {
        if (!t->head_coach) {
            ImGui::TextDisabled("No coach signed.");
        } else {
            auto& c = t->head_coach;
            if (s.god_mode) {
                char nm[128];
                std::snprintf(nm, sizeof(nm), "%s", c->name.c_str());
                ImGui::SetNextItemWidth(360);
                if (ImGui::InputText("##mgr_coach_name", nm, sizeof(nm))) c->name = nm;
            } else {
                H2(c->name.c_str());
            }
            CountryFlag(c->country_iso); ImGui::SameLine();
            Sub("%s   |   age %d   |   $%dK/yr   |   %s",
                c->country.empty() ? c->region.c_str() : c->country.c_str(),
                c->age, c->salary_k,
                vlr::coach_personality_name(c->personality));
            ImGui::PushFont(g_font_small);
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
            ImGui::Text("Style: %s", vlr::coach_personality_blurb(c->personality));
            ImGui::PopStyleColor();
            ImGui::PopFont();
            ImGui::Spacing();

            auto stat = [&](const char* lbl, int& v) {
                if (s.god_mode) {
                    DrawAttrSlider(lbl, v);
                } else {
                    ImGui::Text("%-15s", lbl);
                    ImGui::SameLine(170);
                    ImGui::ProgressBar(v / 99.0f, ImVec2(280, 0), "");
                    ImGui::SameLine();
                    ImGui::Text("%d", v);
                }
            };
            stat("Tactical",    c->tactical);
            stat("Development", c->development);
            stat("Leadership",  c->leadership);
            stat("Experience",  c->experience);

            ImGui::Spacing();
            H2("CONTRACT");
            int years_left = c->contract_exp_year > 0
                ? std::max(0, c->contract_exp_year - s.gm.year + 1)
                : c->contract_years;
            ImGui::Text("Years remaining: %d   |   Expires after season %d",
                        years_left,
                        c->contract_exp_year > 0 ? c->contract_exp_year : (s.gm.year + years_left - 1));
            if (s.god_mode) {
                ImGui::Text("Edit contract length:");
                int yrs_in = years_left;
                ImGui::SetNextItemWidth(160);
                if (ImGui::SliderInt("##coach_contract_years", &yrs_in, 0, 8, "%d yrs")) {
                    c->contract_years = std::max(0, yrs_in);
                    c->contract_exp_year = s.gm.year + c->contract_years - 1;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("+1 yr")) {
                    c->contract_years = std::max(1, years_left + 1);
                    c->contract_exp_year = s.gm.year + c->contract_years - 1;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("-1 yr")) {
                    c->contract_years = std::max(0, years_left - 1);
                    c->contract_exp_year = s.gm.year + c->contract_years - 1;
                }
            }
            ImGui::Spacing();
            H2("DERIVED EFFECTS");
            ImGui::Text("Match synergy boost: +%.1f%%", (c->match_synergy_mult() - 1.0) * 100.0);
            ImGui::Text("Annual dev chance:   %.2fx",   c->dev_chance_mult());
            ImGui::Text("Asking salary now:   $%dK/yr", c->requested_salary_k());
            ImGui::Text("Replacement aggression: %.0f%%",
                vlr::personality_replacement_aggressiveness(c->personality) * 100.0);

            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Button, toV4(kVlrRed));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toV4(kVlrRedDim));
            if (ImGui::Button("Fire Coach", ImVec2(140, 32))) {
                t->head_coach.reset();
                s.log.emplace_back("Fired head coach. New coach hired from market at year-end.");
            }
            ImGui::PopStyleColor(2);
        }

        ImGui::Spacing();
        H2("COACH MARKET");
        Sub("Available free-agent coaches. Hire one to replace your current "
            "head coach immediately.");
        // Cached market in AppState (regenerate on demand). Use a static
        // local vector tied to the year so it refreshes once a year.
        static std::vector<vlr::CoachPtr> market;
        static int last_market_year = -1;
        if (last_market_year != s.gm.year) {
            market = vlr::generate_coach_market(6);
            last_market_year = s.gm.year;
        }
        if (ImGui::Button("Refresh Market", ImVec2(140, 28))) {
            market = vlr::generate_coach_market(6);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Refreshes annually + on demand");
        if (ImGui::BeginTable("##mgr_market", 8,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                ImGuiTableFlags_ScrollY, ImVec2(-FLT_MIN, 280))) {
            ImGui::TableSetupColumn("FLAG");
            ImGui::TableSetupColumn("NAME", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("REGION");
            ImGui::TableSetupColumn("AGE");
            ImGui::TableSetupColumn("PERSONALITY");
            ImGui::TableSetupColumn("OVR");
            ImGui::TableSetupColumn("ASK $K/yr");
            ImGui::TableSetupColumn("HIRE");
            ImGui::TableHeadersRow();
            int mi = 0;
            for (auto& c : market) {
                if (!c) continue;
                ImGui::PushID(mi++);
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); CountryFlag(c->country_iso);
                ImGui::TableNextColumn(); ImGui::Text("%s", c->name.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%s", c->region.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%d", c->age);
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                ImGui::Text("%s", vlr::coach_personality_name(c->personality));
                ImGui::PopStyleColor();
                int ovr = (c->tactical + c->development + c->leadership + c->experience) / 4;
                ImGui::TableNextColumn(); ImGui::Text("%d", ovr);
                ImGui::TableNextColumn(); ImGui::Text("$%dK/yr", c->requested_salary_k());
                ImGui::TableNextColumn();
                if (ImGui::SmallButton("Hire")) {
                    long long cost = static_cast<long long>(c->requested_salary_k()) * 1000LL;
                    if (t->budget < cost) {
                        s.log.emplace_back("Cannot afford coach " + c->name + ".");
                    } else {
                        c->salary_k = c->requested_salary_k();
                        c->team_name = t->name;
                        if (c->contract_years <= 0) c->contract_years = 2;
                        c->contract_exp_year = s.gm.year + c->contract_years - 1;
                        t->head_coach = c;
                        s.log.emplace_back("Hired " + c->name + " as head coach.");
                    }
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::EndTabItem();
    }

    // ===== HEAD SCOUT (WS-A) — cloned from Head Coach =====
    if (ImGui::BeginTabItem("Head Scout")) {
        if (!t->head_scout) {
            ImGui::TextDisabled("No scout signed. Hire one below to widen your view of the market.");
        } else {
            auto& sc = t->head_scout;
            if (s.god_mode) {
                char nm[128];
                std::snprintf(nm, sizeof(nm), "%s", sc->name.c_str());
                ImGui::SetNextItemWidth(360);
                if (ImGui::InputText("##mgr_scout_name", nm, sizeof(nm))) sc->name = nm;
            } else {
                H2(sc->name.c_str());
            }
            CountryFlag(sc->country_iso); ImGui::SameLine();
            Sub("%s   |   age %d   |   $%dK/yr   |   %s",
                sc->country.empty() ? sc->region.c_str() : sc->country.c_str(),
                sc->age, sc->salary_k,
                vlr::scout_personality_name(sc->personality));
            ImGui::PushFont(g_font_small);
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
            ImGui::Text("Style: %s", vlr::scout_personality_blurb(sc->personality));
            ImGui::PopStyleColor();
            ImGui::PopFont();
            ImGui::Spacing();

            auto stat = [&](const char* lbl, int& v) {
                if (s.god_mode) {
                    DrawAttrSlider(lbl, v);
                } else {
                    ImGui::Text("%-15s", lbl);
                    ImGui::SameLine(170);
                    ImGui::ProgressBar(v / 99.0f, ImVec2(280, 0), "");
                    ImGui::SameLine();
                    ImGui::Text("%d", v);
                }
            };
            stat("Judgement",  sc->judgement);
            stat("Network",    sc->network);
            stat("Projection", sc->projection);
            stat("Experience", sc->experience);

            ImGui::Spacing();
            H2("CONTRACT");
            int years_left = sc->contract_exp_year > 0
                ? std::max(0, sc->contract_exp_year - s.gm.year + 1)
                : sc->contract_years;
            ImGui::Text("Years remaining: %d   |   Expires after season %d",
                        years_left,
                        sc->contract_exp_year > 0 ? sc->contract_exp_year : (s.gm.year + years_left - 1));
            if (s.god_mode) {
                int yrs_in = years_left;
                ImGui::SetNextItemWidth(160);
                if (ImGui::SliderInt("##scout_contract_years", &yrs_in, 0, 8, "%d yrs")) {
                    sc->contract_years = std::max(0, yrs_in);
                    sc->contract_exp_year = s.gm.year + sc->contract_years - 1;
                }
            }
            ImGui::Spacing();
            H2("DERIVED EFFECTS");
            ImGui::Text("Scouting assignments / season: %d", sc->reveal_credits());
            ImGui::Text("Report accuracy (band tighten): +%.0f%%", sc->band_tighten_mult() * 100.0);
            ImGui::Text("Discovery quality:             %.2fx", sc->discovery_quality_mult());
            ImGui::Text("Career finds:                  %d", sc->career_finds);
            ImGui::Text("Asking salary now:             $%dK/yr", sc->requested_salary_k());

            // === ASSIGNMENT (increment D) — direct the scout's season focus ===
            ImGui::Spacing();
            H2("ASSIGNMENT");
            Sub("At each season start your scout auto-reveals the highest-ceiling "
                "unscouted prospects matching this focus (spending assignments). "
                "'None' = scout manually from the market.");
            const char* focuses[] = { "None (manual)", "Region", "Role", "My Watchlist" };
            int fc = static_cast<int>(s.gm.scout_focus_);
            ImGui::SetNextItemWidth(200);
            if (ImGui::Combo("Focus##scout_focus", &fc, focuses, 4))
                s.gm.scout_focus_ = static_cast<vlr::GameManager::ScoutFocus>(fc);
            if (s.gm.scout_focus_ == vlr::GameManager::ScoutFocus::Region) {
                const char* regions[] = { "Americas", "EMEA", "Pacific" };
                int ri = 0;
                for (int k = 0; k < 3; ++k) if (s.gm.scout_focus_region_ == regions[k]) ri = k;
                ImGui::SameLine(); ImGui::SetNextItemWidth(150);
                if (ImGui::Combo("##scout_focus_region", &ri, regions, 3) ||
                    s.gm.scout_focus_region_.empty())
                    s.gm.scout_focus_region_ = regions[ri];
            } else if (s.gm.scout_focus_ == vlr::GameManager::ScoutFocus::Role) {
                const char* roles[] = { "Duelist", "Initiator", "Controller", "Sentinel" };
                int roi = static_cast<int>(s.gm.scout_focus_role_);
                if (roi < 0 || roi > 3) roi = 0;
                ImGui::SameLine(); ImGui::SetNextItemWidth(150);
                if (ImGui::Combo("##scout_focus_role", &roi, roles, 4) ||
                    s.gm.scout_focus_role_ == vlr::Role::Count)
                    s.gm.scout_focus_role_ = static_cast<vlr::Role>(roi);
            }
            ImGui::Text("Assignments left this season: %d", s.gm.scout_credits());

            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Button, toV4(kVlrRed));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toV4(kVlrRedDim));
            if (ImGui::Button("Fire Scout", ImVec2(140, 32))) {
                t->head_scout.reset();
                s.log.emplace_back("Fired head scout. Your view of the market narrows until you hire one.");
            }
            ImGui::PopStyleColor(2);
        }

        ImGui::Spacing();
        H2("SCOUT MARKET");
        Sub("Available free-agent scouts. A sharper scout reveals more of the market "
            "(assignments) and reads player ceilings tighter.");
        static std::vector<vlr::ScoutPtr> smarket;
        static int last_smarket_year = -1;
        if (last_smarket_year != s.gm.year) {
            smarket = vlr::generate_scout_market(6);
            last_smarket_year = s.gm.year;
        }
        if (ImGui::Button("Refresh Scouts", ImVec2(140, 28))) {
            smarket = vlr::generate_scout_market(6);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Refreshes annually + on demand");
        if (ImGui::BeginTable("##mgr_scout_market", 8,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                ImGuiTableFlags_ScrollY, ImVec2(-FLT_MIN, 280))) {
            ImGui::TableSetupColumn("FLAG");
            ImGui::TableSetupColumn("NAME", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("REGION");
            ImGui::TableSetupColumn("AGE");
            ImGui::TableSetupColumn("PERSONALITY");
            ImGui::TableSetupColumn("OVR");
            ImGui::TableSetupColumn("ASK $K/yr");
            ImGui::TableSetupColumn("HIRE");
            ImGui::TableHeadersRow();
            int mi = 0;
            for (auto& sc : smarket) {
                if (!sc) continue;
                ImGui::PushID(20000 + mi++);
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); CountryFlag(sc->country_iso);
                ImGui::TableNextColumn(); ImGui::Text("%s", sc->name.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%s", sc->region.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%d", sc->age);
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                ImGui::Text("%s", vlr::scout_personality_name(sc->personality));
                ImGui::PopStyleColor();
                int ovr = (sc->judgement + sc->network + sc->projection + sc->experience) / 4;
                ImGui::TableNextColumn(); ImGui::Text("%d", ovr);
                ImGui::TableNextColumn(); ImGui::Text("$%dK/yr", sc->requested_salary_k());
                ImGui::TableNextColumn();
                if (ImGui::SmallButton("Hire")) {
                    long long cost = static_cast<long long>(sc->requested_salary_k()) * 1000LL;
                    if (t->budget < cost) {
                        s.log.emplace_back("Cannot afford scout " + sc->name + ".");
                    } else {
                        sc->salary_k = sc->requested_salary_k();
                        sc->team_name = t->name;
                        if (sc->contract_years <= 0) sc->contract_years = 2;
                        sc->contract_exp_year = s.gm.year + sc->contract_years - 1;
                        t->head_scout = sc;
                        s.log.emplace_back("Hired " + sc->name + " as head scout.");
                    }
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::EndTabItem();
    }

    // ===== HEAD ANALYST (Scouting&Match-Prep increment A) =====
    if (ImGui::BeginTabItem("Head Analyst")) {
        if (!t->head_analyst) {
            ImGui::TextDisabled("No analyst signed. Hire one below to deepen your opposition reports.");
        } else {
            auto& an = t->head_analyst;
            if (s.god_mode) {
                char nm[128];
                std::snprintf(nm, sizeof(nm), "%s", an->name.c_str());
                ImGui::SetNextItemWidth(360);
                if (ImGui::InputText("##mgr_analyst_name", nm, sizeof(nm))) an->name = nm;
            } else {
                H2(an->name.c_str());
            }
            CountryFlag(an->country_iso); ImGui::SameLine();
            Sub("%s   |   age %d   |   $%dK/yr   |   %s",
                an->country.empty() ? an->region.c_str() : an->country.c_str(),
                an->age, an->salary_k,
                vlr::analyst_personality_name(an->personality));
            ImGui::PushFont(g_font_small);
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
            ImGui::Text("Style: %s", vlr::analyst_personality_blurb(an->personality));
            ImGui::PopStyleColor();
            ImGui::PopFont();
            ImGui::Spacing();

            auto astat = [&](const char* lbl, int& v) {
                if (s.god_mode) {
                    DrawAttrSlider(lbl, v);
                } else {
                    ImGui::Text("%-15s", lbl);
                    ImGui::SameLine(170);
                    ImGui::ProgressBar(v / 99.0f, ImVec2(280, 0), "");
                    ImGui::SameLine();
                    ImGui::Text("%d", v);
                }
            };
            astat("Tactical Read",    an->tactical_read);
            astat("Opponent Insight", an->opponent_insight);
            astat("Prep",             an->prep);
            astat("Experience",       an->experience);

            ImGui::Spacing();
            H2("CONTRACT");
            int years_left = an->contract_exp_year > 0
                ? std::max(0, an->contract_exp_year - s.gm.year + 1)
                : an->contract_years;
            ImGui::Text("Years remaining: %d   |   Expires after season %d",
                        years_left,
                        an->contract_exp_year > 0 ? an->contract_exp_year : (s.gm.year + years_left - 1));
            if (s.god_mode) {
                int yrs_in = years_left;
                ImGui::SetNextItemWidth(160);
                if (ImGui::SliderInt("##analyst_contract_years", &yrs_in, 0, 8, "%d yrs")) {
                    an->contract_years = std::max(0, yrs_in);
                    an->contract_exp_year = s.gm.year + an->contract_years - 1;
                }
            }
            ImGui::Spacing();
            H2("DERIVED EFFECTS");
            ImGui::Text("Opposition report sections: %d", an->report_depth_sections());
            ImGui::Text("Report accuracy:            +%.0f%%", an->report_accuracy_mult() * 100.0);
            ImGui::Text("Per-map prep edge:          %.2f", an->prep_edge01());
            ImGui::Text("Career reports:             %d", an->career_reports);
            ImGui::Text("Asking salary now:          $%dK/yr", an->requested_salary_k());

            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Button, toV4(kVlrRed));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toV4(kVlrRedDim));
            if (ImGui::Button("Fire Analyst", ImVec2(140, 32))) {
                t->head_analyst.reset();
                s.log.emplace_back("Fired head analyst. Opposition reports lose depth until you hire one.");
            }
            ImGui::PopStyleColor(2);
        }

        ImGui::Spacing();
        H2("ANALYST MARKET");
        Sub("Available free-agent analysts. A sharper analyst gives deeper, more "
            "accurate opposition reports (and a stronger per-map prep edge).");
        static std::vector<vlr::AnalystPtr> amarket;
        static int last_amarket_year = -1;
        if (last_amarket_year != s.gm.year) {
            amarket = vlr::generate_analyst_market(6);
            last_amarket_year = s.gm.year;
        }
        if (ImGui::Button("Refresh Analysts", ImVec2(150, 28))) {
            amarket = vlr::generate_analyst_market(6);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Refreshes annually + on demand");
        if (ImGui::BeginTable("##mgr_analyst_market", 8,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                ImGuiTableFlags_ScrollY, ImVec2(-FLT_MIN, 280))) {
            ImGui::TableSetupColumn("FLAG");
            ImGui::TableSetupColumn("NAME", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("REGION");
            ImGui::TableSetupColumn("AGE");
            ImGui::TableSetupColumn("PERSONALITY");
            ImGui::TableSetupColumn("OVR");
            ImGui::TableSetupColumn("ASK $K/yr");
            ImGui::TableSetupColumn("HIRE");
            ImGui::TableHeadersRow();
            int mi = 0;
            for (auto& an : amarket) {
                if (!an) continue;
                ImGui::PushID(30000 + mi++);
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); CountryFlag(an->country_iso);
                ImGui::TableNextColumn(); ImGui::Text("%s", an->name.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%s", an->region.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%d", an->age);
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                ImGui::Text("%s", vlr::analyst_personality_name(an->personality));
                ImGui::PopStyleColor();
                int ovr = (an->tactical_read + an->opponent_insight + an->prep + an->experience) / 4;
                ImGui::TableNextColumn(); ImGui::Text("%d", ovr);
                ImGui::TableNextColumn(); ImGui::Text("$%dK/yr", an->requested_salary_k());
                ImGui::TableNextColumn();
                if (ImGui::SmallButton("Hire")) {
                    long long cost = static_cast<long long>(an->requested_salary_k()) * 1000LL;
                    if (t->budget < cost) {
                        s.log.emplace_back("Cannot afford analyst " + an->name + ".");
                    } else {
                        an->salary_k = an->requested_salary_k();
                        an->team_name = t->name;
                        if (an->contract_years <= 0) an->contract_years = 2;
                        an->contract_exp_year = s.gm.year + an->contract_years - 1;
                        t->head_analyst = an;
                        s.log.emplace_back("Hired " + an->name + " as head analyst.");
                    }
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::EndTabItem();
    }

    // ===== FINANCE =====
    if (ImGui::BeginTabItem("Finance")) {
        H2("ORG FINANCES");
        Sub("Budget, salary commitments, and revenue snapshot. Year-end "
            "payroll subtracts total salaries from your budget; sponsorships "
            "and prize winnings refill it.");
        ImGui::Spacing();

        long long budget = t->budget;
        int payroll = t->total_payroll_k();
        int player_salaries = 0;
        for (auto& p : t->roster) if (p) player_salaries += p->contract.amount_k;
        int coach_salary = t->head_coach ? t->head_coach->salary_k : 0;

        // REAL finance figures (FM-depth economy): the year-end finance pass
        // books sponsorship_k as income and projects next season's committed
        // payroll / wage envelope. No more fabricated "800 + wins*25".
        int revenue_k = t->last_revenue_k > 0 ? t->last_revenue_k : t->sponsorship_k;
        int net = t->projected_income_k - t->committed_payroll_k;  // projected operating balance
        ImGui::TextUnformatted("Wealth tier:"); ImGui::SameLine();
        WealthTierBadge(t->wealth_tier);
        ImGui::Spacing();
        StatTile("BUDGET",         fmt_money(budget),                   kVlrGreen);
        ImGui::SameLine();
        StatTile("ANNUAL PAYROLL", fmt_money_k(payroll),                kAccent);
        ImGui::SameLine();
        StatTile("REVENUE",        fmt_money_k(revenue_k),              kVlrGold);
        ImGui::SameLine();
        StatTile("WAGE BUDGET",    fmt_money_k(t->wage_envelope_k),     kInfo);
        ImGui::SameLine();
        StatTile("PROJ. BALANCE",  fmt_money_k(net), net >= 0 ? kVlrGreen : kVlrRed);

        ImGui::Spacing();
        ImGui::Spacing();

        H2("SALARY BREAKDOWN");
        if (ImGui::BeginTable("##mgr_salary", 6,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV,
                ImVec2(-FLT_MIN, 240))) {
            ImGui::TableSetupColumn("PLAYER", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("ROLE");
            ImGui::TableSetupColumn("AGE");
            ImGui::TableSetupColumn("OVR");
            ImGui::TableSetupColumn("SALARY $K");
            ImGui::TableSetupColumn("EXP YEAR");
            ImGui::TableHeadersRow();
            int fi = 0;
            for (auto& p : t->roster) {
                if (!p) continue;
                ImGui::PushID(fi++);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (TableRowSelectable(player_label(*p).c_str(), false,
                        ImGuiSelectableFlags_SpanAllColumns)) {
                    OpenPlayerModal(s, p);
                }
                ImGui::TableNextColumn();
                PositionBadge(vlr::position_of(*p));
                if (p->is_igl) { ImGui::SameLine(); IglBadge(); }
                ImGui::TableNextColumn(); ImGui::Text("%d", p->age);
                ImGui::TableNextColumn(); ImGui::Text("%.0f", p->ovr());
                ImGui::TableNextColumn(); ImGui::Text("$%dK/yr", p->contract.amount_k);
                ImGui::TableNextColumn(); ImGui::Text("%d", p->contract.exp_year);
                ImGui::PopID();
            }
            if (t->head_coach) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextColored(toV4(kVlrGold), "%s (Coach)", t->head_coach->name.c_str());
                ImGui::TableNextColumn(); ImGui::TextDisabled("-");
                ImGui::TableNextColumn(); ImGui::Text("%d", t->head_coach->age);
                ImGui::TableNextColumn(); ImGui::TextDisabled("-");
                ImGui::TableNextColumn(); ImGui::Text("$%dK/yr", t->head_coach->salary_k);
                ImGui::TableNextColumn(); ImGui::TextDisabled("-");
            }
            ImGui::EndTable();
        }

        ImGui::Spacing();
        H2("INVESTMENTS");
        Sub("(Future: facility upgrades, scouting network, analyst staff. "
            "Currently each costs $K from budget for a small recurring "
            "performance bonus.)");
        ImGui::TextDisabled("Player salaries:   $%dK/yr", player_salaries);
        ImGui::TextDisabled("Coach salary:      $%dK/yr", coach_salary);
        ImGui::TextDisabled("Total commitments: $%dK/yr", payroll);
        ImGui::EndTabItem();
    }

    // ===== COMMISSIONER (god-mode only) =====
    if (s.god_mode && ImGui::BeginTabItem("Commissioner")) {
        H2("LEAGUE-WIDE EDITOR");
        ImGui::SameLine();
        Pill("GOD MODE", kVlrGold);
        Sub("Edit any team's finances, prestige, sponsorship, and strategy. "
            "Inline edits apply immediately and are bounds-clamped.");
        ImGui::Spacing();

        // Team selection by region
        const char* regions[] = {"Americas", "EMEA", "Pacific"};
        static int reg_idx = 0;
        ImGui::SetNextItemWidth(160);
        ImGui::Combo("Region##commreg", &reg_idx, regions, 3);

        auto rit = s.gm.leagues.find(regions[reg_idx]);
        if (rit == s.gm.leagues.end()) { ImGui::EndTabItem(); ImGui::EndTabBar(); return; }
        auto& league = rit->second;

        if (ImGui::BeginTable("##comm_teams", 9,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                ImGuiTableFlags_ScrollY, ImVec2(-FLT_MIN, 540))) {
            ImGui::TableSetupColumn("FLAG");
            ImGui::TableSetupColumn("NAME", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("TAG");
            ImGui::TableSetupColumn("BUDGET ($K)");
            ImGui::TableSetupColumn("PRESTIGE");
            ImGui::TableSetupColumn("SPONSOR ($K)");
            ImGui::TableSetupColumn("STRATEGY");
            ImGui::TableSetupColumn("OVR");
            ImGui::TableSetupColumn("PAYROLL");
            ImGui::TableHeadersRow();
            int ti = 0;
            for (auto& t : league->teams()) {
                if (!t) continue;
                ImGui::PushID(ti++);
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); CountryFlag(t->home_country_iso);
                ImGui::TableNextColumn();
                if (TableRowSelectable(t->name.c_str(), false,
                        ImGuiSelectableFlags_SpanAllColumns)) {
                    OpenTeamProfile(s, t);
                }
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                ImGui::Text("%s", t->tag.c_str());
                ImGui::PopStyleColor();
                ImGui::TableNextColumn();
                int bud_k = (int)(t->budget / 1000LL);
                ImGui::SetNextItemWidth(110);
                if (ImGui::DragInt("##bud", &bud_k, 25.0f, -10000, 100000, "%d", ImGuiSliderFlags_AlwaysClamp)) {
                    t->budget = (long long)bud_k * 1000LL;
                }
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(80);
                ImGui::DragInt("##prst", &t->prestige, 1.0f, 0, 99, "%d", ImGuiSliderFlags_AlwaysClamp);
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(110);
                ImGui::DragInt("##spon", &t->sponsorship_k, 25.0f, 0, 9999, "%d", ImGuiSliderFlags_AlwaysClamp);
                ImGui::TableNextColumn();
                int strat_idx = static_cast<int>(t->strategy);
                const char* strats[] = {"Contender", "Rebuilding", "Bridge",
                                         "BudgetRoster", "DevelopmentFocus",
                                         "WinNow", "TalentFarm"};
                ImGui::SetNextItemWidth(140);
                if (ImGui::Combo("##strat", &strat_idx, strats, 7)) {
                    t->strategy = static_cast<vlr::Team::Strategy>(strat_idx);
                }
                ImGui::TableNextColumn(); ImGui::Text("%.0f", t->ovr());
                ImGui::TableNextColumn(); ImGui::Text("$%dK", t->total_payroll_k());
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        ImGui::Spacing();
        // === Bulk actions card — league-wide overrides, confirm-gated ======
        if (BeginCard("##comm_bulk", kBgDeep)) {
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrRed));
            ImGui::TextUnformatted("BULK ACTIONS");
            ImGui::PopStyleColor();
            Sub("League-wide overrides. Destructive actions require confirmation.");

            // Header tiles for at-a-glance context across ALL leagues.
            {
                long long total_budget = 0, prest_sum = 0; int tcount = 0;
                for (auto& kv : s.gm.leagues)
                    for (auto& t : kv.second->teams())
                        if (t) { total_budget += t->budget; prest_sum += t->prestige; ++tcount; }
                char b1[16]; std::snprintf(b1, sizeof(b1), "%d", tcount);
                StatTile("TEAMS", b1, kAccent); ImGui::SameLine();
                StatTile("TOTAL BUDGET", fmt_money(total_budget), kVlrGreen); ImGui::SameLine();
                char b3[16]; std::snprintf(b3, sizeof(b3), "%d", tcount ? (int)(prest_sum / tcount) : 0);
                StatTile("AVG PRESTIGE", b3, kVlrGold);
            }
            VSpace(8);

            // Destructive buttons set a pending flag; nothing fires until Confirm.
            static int pending = 0;   // 0 none, 1 reset budgets, 2 re-classify
            auto destructive = [&](const char* label, int id) {
                ImGui::PushStyleColor(ImGuiCol_Button,        toV4(kVlrRedDim));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toV4(kVlrRed));
                if (ImGui::Button(label, ImVec2(220, 28))) pending = id;
                ImGui::PopStyleColor(2);
            };
            destructive("Reset all budgets to $1.5M", 1);
            ImGui::SameLine();
            destructive("Re-classify all strategies", 2);
            ImGui::SameLine();
            if (ImGui::Button("+$1M to user team", ImVec2(180, 28))) {   // additive, no confirm
                if (s.gm.user_team) {
                    s.gm.user_team->budget += 1'000'000LL;
                    s.log.emplace_back("[CMSR] +$1M to user team.");
                }
            }

            if (pending != 0) {
                VSpace(6);
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrRed));
                ImGui::Text("%s This cannot be undone.",
                    pending == 1 ? "Reset EVERY team's budget to $1.5M?"
                                 : "Re-classify EVERY team's strategy?");
                ImGui::PopStyleColor();
                ImGui::PushStyleColor(ImGuiCol_Button,        toV4(kVlrRed));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toV4(kVlrRedDim));
                if (ImGui::Button("Confirm", ImVec2(120, 28))) {
                    if (pending == 1) {
                        for (auto& kv : s.gm.leagues)
                            for (auto& t : kv.second->teams())
                                if (t) t->budget = 1'500'000LL;
                        s.log.emplace_back("[CMSR] All team budgets reset to $1.5M");
                    } else {
                        int n = 0;
                        for (auto& kv : s.gm.leagues)
                            for (auto& t : kv.second->teams())
                                if (t) { t->strategy = vlr::classify_team_strategy(*t); ++n; }
                        char buf[64]; std::snprintf(buf, sizeof(buf), "[CMSR] Re-classified %d teams.", n);
                        s.log.emplace_back(buf);
                    }
                    pending = 0;
                }
                ImGui::PopStyleColor(2);
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(120, 28))) pending = 0;
            }
        } EndCard();
        ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
    }  // ##manager_tabs guard
}
