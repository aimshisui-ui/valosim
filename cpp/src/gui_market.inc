// DrawMarket: transfer-market screen (extracted from gui_main.cpp).
// Unity-build fragment of gui_main.cpp: this file is #included inside the
// same anonymous namespace; it is NOT a standalone translation unit.
void DrawMarket(AppState& s) {
    auto& gm = s.gm;
    SectionHeader("TRANSFER MARKET",
        "Free agents' asking-prices reflect mood; offer too low and they refuse.",
        kAccent);
    VSpace(6);

    // === OFFSEASON banner (C10) ===
    // FA price-decay runs internally during OFFSEASON ticks (Pack B). Surface
    // a small header banner so users understand WHY ask-prices drift down
    // each day they hold. No engine state needed — just phase-gated text.
    bool offseason_now = (gm.current_phase() == "OFFSEASON");
    if (offseason_now) {
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
        ImGui::TextUnformatted("[*] OFFSEASON");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        MutedText("\xE2\x80\x94 Free Agent prices decay daily during this "
                  "window. Hold out, save money.");
        VSpace(4);
    }

    // === Filter bar (Group B) — search + role/region/OVR/POT/ask gates =====
    ImGui::SetNextItemWidth(210);
    ImGui::InputTextWithHint("##search", "Search name...", s.search, sizeof(s.search));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    {
        // index 0 = "All"; 1..4 map to Position D/C/S/I (enum order).
        const char* roles[] = {"All roles", "Duelist", "Controller", "Sentinel", "Initiator"};
        int rc = s.market_role + 1;
        if (ImGui::Combo("##mrole", &rc, roles, IM_ARRAYSIZE(roles))) s.market_role = rc - 1;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    {
        const char* regions[] = {"All regions", "Americas", "EMEA", "Pacific"};
        int rg = s.market_region + 1;
        if (ImGui::Combo("##mregion", &rg, regions, IM_ARRAYSIZE(regions))) s.market_region = rg - 1;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(108);
    ImGui::DragInt("##movr", &s.market_min_ovr, 0.3f, 0, 99, "OVR>=%d");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(108);
    ImGui::DragInt("##mpot", &s.market_min_pot, 0.3f, 0, 99, "POT>=%d");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(132);
    ImGui::DragInt("##mask", &s.market_max_ask, 3.0f, 0, 3000,
                   s.market_max_ask > 0 ? "ASK<=$%dK" : "ASK: any");
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) {
        s.search[0] = '\0'; s.market_role = -1; s.market_region = -1;
        s.market_min_ovr = 0; s.market_min_pot = 0; s.market_max_ask = 0;
    }
    VSpace(6);

    // FA listing — per PROJECT_GUIDE §5.11, recomputed every frame from
    // every regional solo Q ladder, filtered ONLY by `team_name == "Free
    // Agent"` (and !is_retired). No role / position filter is applied
    // here: Flex is an INDEPENDENT overlay flag (Player::is_flex) and
    // every FA — including Flex players — has a canonical primary_role
    // that resolves to one of the 4 Position values via position_of().
    //
    // Earlier round of code bucketed Flex players into their own
    // `Position::Flex` group; when Position::Flex was removed and
    // position_of() became pure-primary_role, any code path that filtered
    // FAs by Position would have dropped Flex players entirely. This
    // unfiltered loop is intentional — DO NOT re-add a Position-keyed
    // filter unless you also accept Flex players via the is_flex flag.
    // Lowercased search query (computed once, used in collection below).
    std::string q = s.search;
    for (auto& cc : q) cc = (char)std::tolower(cc);

    // Collect FAs with ALL active filters applied up front (search + role +
    // region + min-OVR + min-POT + max-ask). The ask uses the user-club
    // mood-adjusted demand so the ASK<= filter matches the number shown.
    auto demand_of = [&](const vlr::PlayerPtr& p) {
        return gm.user_team
            ? p->amount_with_mood(p->contract.amount_k, gm.user_team->name)
            : p->contract.amount_k;
    };
    std::vector<vlr::PlayerPtr> fas;
    for (auto& kv : gm.solo_qs) {
        for (auto& p : kv.second->global_ladder()) {
            if (!p || p->is_retired || p->team_name != "Free Agent") continue;
            // WS-A INC-0: a cross-region ("international") FA only belongs on the
            // market if they're established; raw foreign youth come up through the
            // soloq -> Tier-3 pipeline, not as common free agents.
            if (gm.user_team && p->region != gm.user_team->region &&
                !vlr::is_internationally_marketable(*p)) continue;
            if (s.market_role >= 0 && (int)vlr::position_of(*p) != s.market_role) continue;
            if (s.market_region >= 0 &&
                p->region != vlr::kRegions[(std::size_t)s.market_region]) continue;
            if (s.market_min_ovr > 0 && p->ovr() < (double)s.market_min_ovr) continue;
            if (s.market_min_pot > 0 && effective_potential(gm, *p) < s.market_min_pot) continue;
            if (s.market_max_ask > 0 && demand_of(p) > s.market_max_ask) continue;
            if (!q.empty()) {
                std::string nm = p->name;
                for (auto& cc : nm) cc = (char)std::tolower(cc);
                if (nm.find(q) == std::string::npos) continue;
            }
            fas.push_back(p);
        }
    }

    // WS-A INC-0: keep internationals RARE even when several clear the established
    // bar — cap them to intl_fa_market_share of the list, keeping the best by OVR
    // (deterministic id tiebreak so the set never flickers). Domestic FAs are
    // never capped, so the list never empties.
    if (gm.user_team && !fas.empty()) {
        std::vector<vlr::PlayerPtr> dom, intl;
        for (auto& p : fas)
            (p->region == gm.user_team->region ? dom : intl).push_back(p);
        int cap = std::max(2, (int)(fas.size() * vlr::config().intl_fa_market_share));
        if (static_cast<int>(intl.size()) > cap) {
            std::sort(intl.begin(), intl.end(),
                      [](const vlr::PlayerPtr& a, const vlr::PlayerPtr& b) {
                          if (a->ovr() != b->ovr()) return a->ovr() > b->ovr();
                          return a->id < b->id;
                      });
            intl.resize(cap);
        }
        fas.clear();
        fas.insert(fas.end(), dom.begin(), dom.end());
        fas.insert(fas.end(), intl.begin(), intl.end());
    }

    // Result-count + active-filter readout so the user knows the list is gated.
    {
        bool filtered = q.size() || s.market_role >= 0 || s.market_region >= 0 ||
                        s.market_min_ovr || s.market_min_pot || s.market_max_ask;
        MutedText("%d free agent%s%s", (int)fas.size(), fas.size() == 1 ? "" : "s",
                  filtered ? " match your filters" : " available");
        if (gm.user_team) {
            ImGui::SameLine(); ImGui::TextDisabled("  |  "); ImGui::SameLine();
            int sc = gm.scout_credits();
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(sc > 0 ? kVlrGold : kVlrSub));
            ImGui::Text("Scouting: %d assignment%s left", sc, sc == 1 ? "" : "s");
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Spend an assignment (Scout button) to reveal an unscouted\n"
                                  "player's exact potential. Refills each new season.");
        }
        VSpace(4);
    }

    BeginCard("##market_card");
    if (ImGui::BeginTable("##market", 10,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_Sortable,
            ImVec2(-FLT_MIN, 540))) {
        ImGui::TableSetupColumn("FLAG", ImGuiTableColumnFlags_NoSort);
        ImGui::TableSetupColumn("NAME", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("ROLE");
        ImGui::TableSetupColumn("REGION");
        ImGui::TableSetupColumn("AGE");
        ImGui::TableSetupColumn("OVR",
            ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending);
        ImGui::TableSetupColumn("POT");
        ImGui::TableSetupColumn("ASK $K/Y");
        ImGui::TableSetupColumn("MOOD");
        ImGui::TableSetupColumn("SIGN", ImGuiTableColumnFlags_NoSort);
        ImGui::TableHeadersRow();

        // Sort by the active column spec (defaults to OVR-desc via DefaultSort).
        if (ImGuiTableSortSpecs* sp = ImGui::TableGetSortSpecs()) {
            if (sp->SpecsCount > 0) {
                const ImGuiTableColumnSortSpecs& c = sp->Specs[0];
                bool asc = (c.SortDirection == ImGuiSortDirection_Ascending);
                std::stable_sort(fas.begin(), fas.end(),
                    [&](const vlr::PlayerPtr& a, const vlr::PlayerPtr& b) {
                        double va = 0, vb = 0; bool str = false;
                        const std::string* sa = nullptr; const std::string* sb = nullptr;
                        switch (c.ColumnIndex) {
                            case 1: str = true; sa = &a->name; sb = &b->name; break;
                            case 2: va = (double)vlr::position_of(*a); vb = (double)vlr::position_of(*b); break;
                            case 3: str = true; sa = &a->region; sb = &b->region; break;
                            case 4: va = a->age; vb = b->age; break;
                            case 5: va = a->ovr(); vb = b->ovr(); break;
                            case 6: va = effective_potential(gm, *a); vb = effective_potential(gm, *b); break;
                            case 7: va = demand_of(a); vb = demand_of(b); break;
                            case 8: va = gm.user_team ? a->mood_for(gm.user_team->name) : 0.0;
                                    vb = gm.user_team ? b->mood_for(gm.user_team->name) : 0.0; break;
                            default: va = a->ovr(); vb = b->ovr(); break;
                        }
                        if (str) return asc ? (*sa < *sb) : (*sa > *sb);
                        return asc ? (va < vb) : (va > vb);
                    });
            }
        }

        int shown = 0; int idx = 0;
        for (auto& p : fas) {
            if (++shown > 200) break;
            ImGui::PushID(idx++);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); CountryFlag(p->country_iso);
            ImGui::TableNextColumn();
            if (TableRowSelectable(player_label(*p).c_str(), false,
                    ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
                OpenPlayerModal(s, p);
            }
            // Small Flex chip on the gamertag column — surfaces is_flex
            // FAs at a glance so the user can spot rotation pieces in the
            // listing without opening the profile modal. Plus the signature
            // agent chip (same helper as Roster + Team Profile rows) — so
            // a Cypher main jumps out before the user even clicks them.
            if (p->is_flex) { ImGui::SameLine(); FlexBadge(); }
            ImGui::SameLine();
            SigBadge(*p);
            ImGui::TableNextColumn();
            PositionBadge(vlr::position_of(*p));
            if (p->is_flex) { ImGui::SameLine(); FlexBadge(); }
            if (p->is_igl)  { ImGui::SameLine(); IglBadge();  }
            ImGui::TableNextColumn(); ImGui::Text("%s", p->region.c_str());
            ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%d", p->age); ImGui::PopFont();
            ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%.0f", p->ovr()); ImGui::PopFont();
            ImGui::TableNextColumn();
            ImGui::PushFont(g_font_mono);
            if (potential_known(gm, *p)) {
                ImGui::Text("%d", p->potential);
            } else {
                int plo, phi; potential_band(gm, *p, plo, phi);
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
                ImGui::Text("%d-%d", plo, phi);
                ImGui::PopStyleColor();
            }
            ImGui::PopFont();
            if (!potential_known(gm, *p) && ImGui::IsItemHovered())
                ImGui::SetTooltip("Unscouted \xE2\x80\x94 potential is an estimate.\n"
                                  "Scout from the player profile to reveal it.");
            int demand = gm.user_team
                ? p->amount_with_mood(p->contract.amount_k, gm.user_team->name)
                : p->contract.amount_k;
            ImGui::TableNextColumn();
            // The ask price IS the signal — no vague "decayed" tag. The engine
            // moves a player's raw ask off their value metrics (OVR, potential,
            // an age curve, and offseason drift). We surface that on the NUMBER:
            // green when the ask sits below market value (a bargain), gold when
            // it's above, with a tooltip itemizing exactly what drives it.
            {
                // Fog-of-war (Group C): for an unscouted player, derive every
                // value signal here (bargain color + "market value") from the
                // potential BAND MIDPOINT, never the true value — otherwise the
                // tooltip would hand the exact ceiling away for free.
                bool pk = potential_known(gm, *p);
                int plo = 0, phi = 0, eff_pot = p->potential;
                if (!pk) { potential_band(gm, *p, plo, phi); eff_pot = (plo + phi) / 2; }
                double age_mult = (p->age <= 23) ? 1.08
                                : (p->age <= 27) ? 1.00
                                : (p->age <= 30) ? 0.90 : 0.78;
                int baseline = static_cast<int>(
                    (0.55 * p->ovr() + 0.45 * eff_pot) * age_mult);
                ImU32 col = kVlrText;
                if (baseline > 5) {
                    if      (p->contract.amount_k < baseline - 5) col = kVlrGreen;
                    else if (p->contract.amount_k > baseline + 5) col = kVlrGold;
                }
                ImGui::PushFont(g_font_mono);
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(col));
                ImGui::Text("$%dK", demand);
                ImGui::PopStyleColor();
                ImGui::PopFont();
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::TextColored(toV4(kVlrGold), "What drives this ask");
                    ImGui::Separator();
                    if (pk) ImGui::Text("  OVR %.0f  /  POT %d", p->ovr(), p->potential);
                    else    ImGui::Text("  OVR %.0f  /  POT %d-%d (est)", p->ovr(), plo, phi);
                    ImGui::Text("  Age %d  (%s)", p->age,
                        p->age <= 23 ? "upside premium" :
                        p->age <= 27 ? "prime years"   :
                        p->age <= 30 ? "slight discount": "veteran discount");
                    ImGui::Text("  Market value  ~$%dK%s", baseline, pk ? "" : " (est)");
                    ImGui::Text("  Current ask    $%dK", p->contract.amount_k);
                    if (offseason_now)
                        ImGui::TextDisabled("  offseason: unsold asks drift toward value");
                    if (!pk) ImGui::TextDisabled("  scout to confirm the ceiling");
                    ImGui::EndTooltip();
                }
            }
            double m = gm.user_team ? p->mood_for(gm.user_team->name) : 0.0;
            ImGui::TableNextColumn();
            {
                ImU32 c = m > 0.6 ? kVlrRed : (m > 0.3 ? kVlrGold : kVlrGreen);
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(c));
                ImGui::PushFont(g_font_mono);
                ImGui::Text("%.0f%%", m * 100.0);
                ImGui::PopFont();
                ImGui::PopStyleColor();
            }
            ImGui::TableNextColumn();
            // Surface roster-full state explicitly so the user understands
            // why the Sign button is missing rather than the row simply
            // looking different.
            if (!gm.user_team) {
                ImGui::TextDisabled("—");
            } else {
                // Scout (Group C): reveal an unscouted FA's exact potential for
                // one assignment. Shown disabled when the season's pool is dry.
                if (!potential_known(gm, *p)) {
                    bool can = gm.scout_credits() > 0;
                    if (!can) ImGui::BeginDisabled();
                    ImGui::SetNextItemAllowOverlap();
                    if (ImGui::SmallButton("Scout")) gm.scout_player(p);
                    if (!can) ImGui::EndDisabled();
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(can ? "Reveal exact potential (costs 1 assignment)"
                                              : "No scouting assignments left this season");
                    ImGui::SameLine();
                }
                if (gm.user_team->roster.size() >= 7) {
                    ImGui::TextDisabled("FULL");
                } else if (ImGui::SetNextItemAllowOverlap(), ImGui::SmallButton("Negotiate")) {
                    // 2026-05-28: replaced direct sign with the negotiation
                    // modal so FA signings have the same interactive depth as
                    // re-signings (salary, length, role offer, breakdown).
                    s.show_fa_modal      = true;
                    s.fa_target          = p;
                    s.fa_years_choice    = 0;   // 0 sentinel -> fill from cached offer
                    s.fa_amount_choice   = 0;
                    s.fa_role_choice     = -1;  // -1 -> natural
                    s.fa_cached_for      = nullptr;
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    EndCard();

    // === FA Sign negotiation modal — shared with Re-sign =================
    // Calls DrawNegotiationModal (defined above DrawDashboard) in
    // FreeAgentSign mode. The helper handles all UI, the offer cache, the
    // role combo, the breakdown, AND the pre-flight (budget / import cap /
    // roster size) + sign_player commit path.
    if (gm.user_team) {
        DrawNegotiationModal(
            s, NegotiationMode::FreeAgentSign, "Sign Free Agent##fa_modal",
            s.fa_target, gm.user_team,
            s.fa_years_choice, s.fa_amount_choice, s.fa_role_choice,
            s.fa_cached_offer, s.fa_cached_mkt_k, s.fa_cached_for,
            s.show_fa_modal,
            s.fa_bonus_choice, s.fa_promise_starter, s.fa_offers_left);
    }
#if 0
    // Legacy inline body — kept #if 0'd as a fallback reference during
    // helper rollout; will be deleted in a follow-up after stability has
    // been confirmed across a full session.
    if (s.show_fa_modal && s.fa_target && gm.user_team) {
        auto p = s.fa_target;
        auto ut = gm.user_team;
        ImGui::OpenPopup("Sign Free Agent##fa_modal");
        ImVec2 rc = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(rc, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(860, 560), ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal("Sign Free Agent##fa_modal",
                                   &s.show_fa_modal,
                                   ImGuiWindowFlags_NoSavedSettings)) {
            if (s.fa_cached_for != p.get()) {
                s.fa_cached_offer = p->propose_resign_offer(*ut, gm.year);
                s.fa_cached_mkt_k = ut->market_value_estimate(*p);
                s.fa_cached_for   = p.get();
                if (s.fa_years_choice  <= 0) s.fa_years_choice  = s.fa_cached_offer.years;
                if (s.fa_amount_choice <= 0) s.fa_amount_choice = s.fa_cached_offer.amount_k;
            }
            const auto& offer = s.fa_cached_offer;
            int mkt_k = s.fa_cached_mkt_k;

            int years_lo = 1;
            int years_hi = std::max(1, offer.max_acceptable_years);
            int amt_lo   = std::max((int)vlr::kSalaryFloorK,
                                    offer.min_acceptable_k);
            int amt_hi   = std::max(amt_lo + 1,
                                    (int)std::min<long long>(
                                        vlr::kSalaryCapK,
                                        (long long)std::lround(offer.amount_k * 1.6)));
            s.fa_years_choice  = vlr::clamp_v(s.fa_years_choice,  years_lo, years_hi);
            s.fa_amount_choice = vlr::clamp_v(s.fa_amount_choice, amt_lo, amt_hi);

            {
                std::string title = std::string("SIGN ") + p->name;
                H2(title.c_str());
            }
            {
                char tag[80];
                std::snprintf(tag, sizeof(tag),
                    "%d | %s | %s | OVR %.0f",
                    p->age, vlr::role_name(p->primary_role),
                    p->region.c_str(), p->ovr());
                Pill(tag, kVlrSub);
                if (p->region != ut->region) {
                    ImGui::SameLine();
                    Pill("IMPORT", kVlrGold);
                }
            }
            ThinDivider(6);

            ImGui::Columns(2, "##fa_cols", false);

            // LEFT: player's ask
            SectionHeader("PLAYER IS ASKING", nullptr, kVlrGold);
            ImGui::Text("Years requested: %d", offer.years);
            ImGui::Text("Amount requested: $%dK/yr", offer.amount_k);
            {
                float w01 = (float)vlr::clamp_v(offer.willingness, 0.0, 1.0);
                ImU32 wcol = (w01 >= 0.70f) ? kVlrGreen
                           : (w01 >= 0.40f) ? kVlrGold : kVlrRed;
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, toV4(wcol));
                char wbuf[32];
                std::snprintf(wbuf, sizeof(wbuf), "%.0f%% willing",
                              w01 * 100.0f);
                ImGui::ProgressBar(w01, ImVec2(-FLT_MIN, 0), wbuf);
                ImGui::PopStyleColor();
            }
            if (!offer.explainer.empty()) MutedText("%s", offer.explainer.c_str());
            VSpace(6);
            ImGui::Text("Min acceptable: $%dK/yr", offer.min_acceptable_k);
            ImGui::Text("Max acceptable years: %d", offer.max_acceptable_years);
            VSpace(4);
            MutedText("Preferred role: %s", vlr::role_name(p->primary_role));

            // RIGHT: market + your offer
            ImGui::NextColumn();
            SectionHeader("MARKET & YOUR OFFER", nullptr, kAccent);
            ImGui::Text("Market value: $%dK/yr", mkt_k);
            ImGui::Text("Team budget: %s", fmt_money(ut->budget).c_str());
            VSpace(4);

            ImGui::SetNextItemWidth(220);
            ImGui::SliderInt("Years", &s.fa_years_choice, years_lo, years_hi);
            ImGui::SetNextItemWidth(220);
            ImGui::SliderInt("Salary $K/yr", &s.fa_amount_choice, amt_lo, amt_hi);

            // Role offer combo — same UX as Re-sign modal.
            {
                const vlr::Role role_opts[4] = {
                    vlr::Role::Duelist, vlr::Role::Initiator,
                    vlr::Role::Controller, vlr::Role::Sentinel
                };
                int natural_idx = 0;
                for (int i = 0; i < 4; ++i)
                    if (role_opts[i] == p->primary_role) natural_idx = i;
                int cur_idx = (s.fa_role_choice < 0) ? natural_idx : s.fa_role_choice;
                if (cur_idx < 0 || cur_idx > 3) cur_idx = natural_idx;
                ImGui::SetNextItemWidth(220);
                char preview[80];
                std::snprintf(preview, sizeof(preview), "%s%s",
                    vlr::role_name(role_opts[cur_idx]),
                    (cur_idx == natural_idx) ? " (Natural)" : "");
                if (ImGui::BeginCombo("Offered Role", preview)) {
                    for (int i = 0; i < 4; ++i) {
                        char item[96];
                        std::snprintf(item, sizeof(item), "%s  -  %s",
                            vlr::role_name(role_opts[i]),
                            p->role_fit_verdict(role_opts[i]));
                        bool selected = (i == cur_idx);
                        if (ImGui::Selectable(item, selected)) {
                            s.fa_role_choice = i;
                        }
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            }
            vlr::Role fa_offered_role = p->primary_role;
            if (s.fa_role_choice >= 0 && s.fa_role_choice < 4) {
                const vlr::Role role_opts[4] = {
                    vlr::Role::Duelist, vlr::Role::Initiator,
                    vlr::Role::Controller, vlr::Role::Sentinel
                };
                fa_offered_role = role_opts[s.fa_role_choice];
            }

            // Breakdown — same evaluator as Re-sign for parity.
            auto bd = p->evaluate_resign_offer(s.fa_amount_choice,
                                               s.fa_years_choice,
                                               *ut, fa_offered_role);
            ImU32 verdict_col = kVlrSub;
            if      (bd.total >= 85) verdict_col = kVlrGold;
            else if (bd.total >= 70) verdict_col = kVlrGreen;
            else if (bd.total >= 55) verdict_col = kAccent;
            else if (bd.total >= 35) verdict_col = kRoleFlex;
            else                     verdict_col = kVlrRed;
            char vbuf[80];
            std::snprintf(vbuf, sizeof(vbuf), "%s  (%d/100)", bd.verdict, bd.total);
            Pill(vbuf, verdict_col);
            ImGui::SameLine();
            if (bd.will_accept) Pill("WILL ACCEPT", kVlrGreen);
            else                Pill("WILL REJECT", kVlrRed);

            VSpace(4);
            if (g_font_small) ImGui::PushFont(g_font_small);
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
            ImGui::Text("Base interest: %d", bd.base_score);
            ImGui::PopStyleColor();
            for (auto& kv : bd.labels) {
                ImU32 col = (kv.second > 0) ? kVlrGreen
                          : (kv.second < 0) ? kVlrRed : kVlrSub;
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(col));
                ImGui::Text("  %s: %+d", kv.first.c_str(), kv.second);
                ImGui::PopStyleColor();
            }
            if (g_font_small) ImGui::PopFont();
            if (!bd.will_accept && !bd.reject_reason.empty()) {
                VSpace(4);
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrRedDim));
                ImGui::TextWrapped("Reason: %s", bd.reject_reason.c_str());
                ImGui::PopStyleColor();
            }

            ImGui::Columns(1);
            ThinDivider(8);

            // === Action buttons =======================================
            ImGui::PushStyleColor(ImGuiCol_Button,        toV4(kAccent));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toV4(kAccent2));
            if (ImGui::Button("Send Offer", ImVec2(150, 32))) {
                int yrs  = s.fa_years_choice;
                int amt  = s.fa_amount_choice;
                long long cost = static_cast<long long>(amt) * 1000LL;
                // Pre-flight gates (unchanged from legacy direct sign).
                bool ok = bd.will_accept;
                std::string reject;
                if (ok && !s.gm.transfers_open()) {   // WS-A INC-4: window gate
                    ok = false; reject = "The transfer window is closed.";
                }
                if (ok && ut->budget < cost) {
                    ok = false;
                    reject = "Insufficient budget ($" +
                             std::to_string(ut->budget / 1000) +
                             "K available, $" + std::to_string(amt) + "K offered).";
                }
                if (ok && p->region != ut->region) {
                    int imports = 0;
                    for (auto& rp : ut->roster)
                        if (rp && rp->region != ut->region) ++imports;
                    if (imports >= vlr::config().max_imports) {
                        ok = false;
                        reject = "Import cap reached (" +
                                 std::to_string(vlr::config().max_imports) + ").";
                    }
                }
                if (ok && ut->roster.size() >= 7) {
                    ok = false; reject = "Roster full.";
                }
                if (!ok) {
                    s.log.emplace_back(p->name + ": " +
                        (reject.empty() ? std::string("Offer rejected.") : reject));
                } else {
                    std::size_t before_size = ut->roster.size();
                    ut->budget -= cost;
                    p->contract.amount_k = amt;
                    int base_yr = (gm.current_phase() == "OFFSEASON")
                                      ? gm.year : gm.year + 1;
                    ut->sign_player(p, yrs, base_yr, fa_offered_role);
                    bool added = (ut->roster.size() == before_size + 1)
                              && (p->team_name == ut->name);
                    if (!added) {
                        ut->budget += cost;
                        s.log.emplace_back("Engine rejected sign of " + p->name + ".");
                    } else {
                        gm.sync_player_ranked_region(p, ut->region);
                        char msg[256];
                        std::snprintf(msg, sizeof(msg),
                            "Signed %s as %s (%dy / $%dK/yr).",
                            p->name.c_str(),
                            vlr::role_name(fa_offered_role), yrs, amt);
                        s.log.emplace_back(msg);
                        s.show_fa_modal = false;
                        s.fa_target.reset();
                    }
                }
            }
            ImGui::PopStyleColor(2);
            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button,        toV4(kVlrGold));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toV4(kVlrGold));
            if (ImGui::Button("Match Their Ask", ImVec2(170, 32))) {
                s.fa_years_choice  = vlr::clamp_v(offer.years, years_lo, years_hi);
                s.fa_amount_choice = vlr::clamp_v(offer.amount_k, amt_lo, amt_hi);
            }
            ImGui::PopStyleColor(2);
            ImGui::SameLine();

            if (ImGui::Button("Walk Away", ImVec2(120, 32))) {
                s.show_fa_modal = false;
                s.fa_target.reset();
            }

            ImGui::EndPopup();
        }
        if (!s.show_fa_modal) {
            s.fa_target.reset();
            s.fa_cached_for = nullptr;
        }
    }
#endif
}
