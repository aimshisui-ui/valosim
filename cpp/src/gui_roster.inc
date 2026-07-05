// DrawRoster: roster-management screen (extracted from gui_main.cpp).
// Unity-build fragment of gui_main.cpp: this file is #included inside the
// same anonymous namespace; it is NOT a standalone translation unit.
void DrawRoster(AppState& s) {
    auto t = s.gm.user_team;
    if (!t) return;
    ImGui::PushFont(g_font_h1);
    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrRed));
    ImGui::TextUnformatted("MY ROSTER");
    ImGui::PopStyleColor();
    ImGui::PopFont();
    // Comp shape — every starting 5 is 4 canonical gameplay slots plus a
    // flexible 5th. Flex and IGL are independent overlay flags (any
    // starter can carry zero, one, or both).
    MutedText("Comp shape: 1D + 1C + 1S + 1I + 1 flexible 5th "
        "(Flex/secondary I/secondary C). 1 IGL flag overlaid on any starter.   "
        "(Tends toward %s comps)",
        vlr::comp_tag_name(vlr::comp_tag_of(t->target_comp)));
    VSpace(14);

    // === Roster KPI ribbon =================================================
    // A performance/squad snapshot that complements (rather than duplicates)
    // the Manager screen's finance/standing ribbon. Top rated + top fragger
    // spotlight in-form starters; avg rating and squad age read overall depth;
    // best duo surfaces the strongest co-play chemistry pairing.
    {
        vlr::PlayerPtr top_rated, top_frag;
        double best_rating = -1.0, best_kd = -1.0, sum_rating = 0.0;
        int rated_n = 0, age_sum = 0, squad_n = 0;
        for (auto& p : t->roster) {
            if (!p) continue;
            ++squad_n;
            age_sum += p->age;
            double r = p->avg_match_rating();
            if (r > 0.0) {
                sum_rating += r; ++rated_n;
                if (r > best_rating) { best_rating = r; top_rated = p; }
            }
            double kd = p->kd_ratio();
            if (kd > best_kd) { best_kd = kd; top_frag = p; }
        }
        auto duo = t->top_chemistry_pairs(1);
        const char* kDash = "\xE2\x80\x94";   // em dash for "no data"
        char buf[48];

        // TOP RATED — highest VLR match rating on the roster.
        if (top_rated) std::snprintf(buf, sizeof(buf), "%.2f", best_rating);
        StatTile("TOP RATED", top_rated ? std::string(buf) : std::string(kDash),
                 kVlrGold, ImVec2(0, 0), top_rated ? top_rated->name.c_str() : nullptr);
        ImGui::SameLine();

        // TOP FRAGGER — best K/D on the roster.
        if (top_frag) std::snprintf(buf, sizeof(buf), "%.2f", best_kd);
        StatTile("TOP FRAGGER", top_frag ? std::string(buf) : std::string(kDash),
                 kVlrGreen, ImVec2(0, 0), top_frag ? top_frag->name.c_str() : nullptr);
        ImGui::SameLine();

        // AVG RATING — mean match rating across players who have played.
        std::string avg_v = kDash;
        if (rated_n > 0) { std::snprintf(buf, sizeof(buf), "%.2f", sum_rating / rated_n); avg_v = buf; }
        char rated_sub[32];
        std::snprintf(rated_sub, sizeof(rated_sub), "%d player%s rated", rated_n, rated_n == 1 ? "" : "s");
        StatTile("AVG RATING", avg_v, kAccent, ImVec2(0, 0), rated_sub);
        ImGui::SameLine();

        // SQUAD — headcount + average age.
        std::snprintf(buf, sizeof(buf), "%d", squad_n);
        char age_sub[32]; age_sub[0] = '\0';
        if (squad_n > 0) std::snprintf(age_sub, sizeof(age_sub), "avg age %.0f", (double)age_sum / squad_n);
        StatTile("SQUAD", std::string(buf), kAccent2, ImVec2(0, 0), squad_n > 0 ? age_sub : nullptr);
        ImGui::SameLine();

        // BEST DUO — strongest co-play chemistry pairing (signed, [-2,+2]).
        std::string duo_v = kDash, duo_sub;
        if (!duo.empty() && std::get<0>(duo[0]) && std::get<1>(duo[0])) {
            std::snprintf(buf, sizeof(buf), "%+.2f", std::get<2>(duo[0]));
            duo_v = buf;
            duo_sub = std::string(std::get<0>(duo[0])->name) + " & " + std::get<1>(duo[0])->name;
        }
        StatTile("BEST DUO", duo_v, kVlrGold, ImVec2(0, 0), duo_sub.empty() ? nullptr : duo_sub.c_str());
    }
    VSpace(16);

    // === Bucket the roster into the canonical 4 + flexible 5th ==========
    // Layout: 4 named slots (Duelist / Controller / Sentinel / Initiator)
    // each filled by the first roster member matching that position, plus
    // a 5th "5th / Flex" slot for whoever's left over from the starting 5.
    // Bucketing logic: walk roster[0..4]. Track which Position has been
    // filled. First occupant per canonical position locks that slot; any
    // subsequent starter goes into the 5th slot. Everyone past roster[4]
    // (or any extras displaced by the bucketing pass) drops to bench,
    // sorted by OVR descending.
    std::array<vlr::PlayerPtr, (size_t)vlr::Position::Count> canonical{};
    vlr::PlayerPtr fifth;       // the flexible 5th occupant
    std::vector<vlr::PlayerPtr> bench;
    {
        // First pass: only starters (roster[0..4]) compete for the 5 slots.
        std::size_t starter_count = std::min<std::size_t>(t->roster.size(), 5);
        std::vector<vlr::PlayerPtr> displaced;
        for (std::size_t i = 0; i < starter_count; ++i) {
            auto& p = t->roster[i];
            if (!p) continue;
            std::size_t idx = (std::size_t)vlr::position_of(*p);
            if (!canonical[idx]) {
                canonical[idx] = p;
            } else if (!fifth) {
                fifth = p;
            } else {
                // Two starters share a canonical position AND the 5th
                // slot is already taken (e.g. 2D + 2C in the same 5).
                // The duplicate falls to the bench so the user can see
                // the invariant violation.
                displaced.push_back(p);
            }
        }
        // Bench = everyone past roster[4] + any displaced duplicates.
        for (std::size_t i = starter_count; i < t->roster.size(); ++i) {
            if (t->roster[i]) bench.push_back(t->roster[i]);
        }
        for (auto& d : displaced) bench.push_back(d);
        std::sort(bench.begin(), bench.end(),
                  [](const vlr::PlayerPtr& a, const vlr::PlayerPtr& b) {
                      return a->ovr() > b->ovr();
                  });
    }

    std::vector<vlr::PlayerPtr> to_release;
    // Deferred bench/start swaps {starter_to_bench, bench_to_start}, applied
    // after the tables so we don't mutate the roster mid-render.
    std::vector<std::pair<vlr::PlayerPtr, vlr::PlayerPtr>> to_swap;

    // Lambda: render one player row. nullptr = "[empty]" placeholder.
    // `dup_warn` = mark the position column with a duplicate warning.
    // `slot_label` overrides the empty-slot label; pass non-empty to render
    // e.g. "[empty 5th slot]" when slot_pos is meaningless (5th / Flex /
    // bench rows).
    auto render_row = [&](vlr::Position slot_pos,
                          const vlr::PlayerPtr& p,
                          bool dup_warn,
                          int row_id,
                          const char* slot_label = nullptr) {
        ImGui::PushID(row_id);
        ImGui::TableNextRow();

        if (!p) {
            // Empty slot — surface the gap loudly so the user can see
            // they're short a starter for this position (or 5th slot).
            ImGui::TableNextColumn(); ImGui::TextDisabled("--");
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrRed));
            if (slot_label && slot_label[0]) {
                ImGui::Text("[empty %s]", slot_label);
            } else {
                ImGui::Text("[empty %s]", vlr::position_name(slot_pos));
            }
            ImGui::PopStyleColor();
            ImGui::TableNextColumn();
            if (slot_label && slot_label[0]) {
                ImGui::TextDisabled("--");      // no PositionBadge for 5th
            } else {
                PositionBadge(slot_pos);
            }
            ImGui::TableNextColumn(); ImGui::TextDisabled("--");   // REGION
            ImGui::TableNextColumn(); ImGui::TextDisabled("--");   // AGE
            ImGui::TableNextColumn(); ImGui::TextDisabled("--");   // OVR
            ImGui::TableNextColumn(); ImGui::TextDisabled("--");   // POT
            ImGui::TableNextColumn(); ImGui::TextDisabled("--");   // FORM
            ImGui::TableNextColumn(); ImGui::TextDisabled("--");   // MORALE
            ImGui::TableNextColumn(); ImGui::TextDisabled("--");   // TRAJ
            ImGui::TableNextColumn(); ImGui::TextDisabled("--");   // CONTRACT
            ImGui::TableNextColumn(); ImGui::TextDisabled("--");   // SALARY
            ImGui::TableNextColumn(); // ACTIONS column blank
            ImGui::PopID();
            return;
        }

        ImGui::TableNextColumn(); CountryFlag(p->country_iso);
        ImGui::TableNextColumn();
        // NOTE 1: do NOT use SpanAllColumns — it covers the Release
        //         button in the last column, swallowing its clicks.
        // NOTE 2: Do NOT add an inline "[I]" prefix for imports here —
        //         it collides visually with player_label()'s "[I]"
        //         IGL prefix, making imports look like duplicate IGLs.
        //         Import status is shown as " (Import)" in the REGION
        //         column instead.
        if (ImGui::Selectable(player_label(*p).c_str(), false)) {
            OpenPlayerModal(s, p);
        }
        // Signature agent inline chip — small gold pill beside the gamertag
        // showing the player's most-mastered agent. Empty signature_agent()
        // = no-op. Same helper used on Market FA rows + Team Profile.
        ImGui::SameLine();
        SigBadge(*p);
        // Transfer-request flag — this player has demanded a move. Release or
        // re-sign (with a starter promise / better deal) to resolve it.
        if (p->transfer_requested) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrRed));
            ImGui::TextUnformatted("[WANTS OUT]");
            ImGui::PopStyleColor();
        }
        ImGui::TableNextColumn();
        // Canonical order: PositionBadge → FlexBadge (if is_flex) →
        // IglBadge (if is_igl). Both overlay flags are independent and a
        // single player can carry zero, one, or both.
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
        ImGui::TableNextColumn();
        if (p->region != t->region) {
            ImGui::Text("%s", p->region.c_str());
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
            ImGui::PushFont(g_font_small);
            ImGui::Text("(Import)");
            ImGui::PopFont();
            ImGui::PopStyleColor();
        } else {
            ImGui::Text("%s", p->region.c_str());
        }
        ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%d", p->age); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%.0f", p->ovr()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%d", p->potential); ImGui::PopFont();
        // === Quick-columns (Group B): FORM / MORALE / TRAJ =================
        {
            double form = p->avg_match_rating();
            ImU32 fcol = form >= 1.10 ? kVlrGreen : (form < 0.95 ? kVlrRed : kVlrText);
            ImGui::TableNextColumn();
            ImGui::PushFont(g_font_mono); ImGui::PushStyleColor(ImGuiCol_Text, toV4(fcol));
            ImGui::Text("%.2f", form);
            ImGui::PopStyleColor(); ImGui::PopFont();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Recent form (avg match rating)");
        }
        {
            int mood = p->overall_mood(*t, s.gm.year);
            ImU32 mcol = mood >= 62 ? kVlrGreen : (mood < 42 ? kVlrRed : kVlrGold);
            ImGui::TableNextColumn();
            ImGui::PushFont(g_font_mono); ImGui::PushStyleColor(ImGuiCol_Text, toV4(mcol));
            ImGui::Text("%d", mood);
            ImGui::PopStyleColor(); ImGui::PopFont();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Morale 0-100 (open profile for the breakdown)");
        }
        {
            auto tp = traj_display(p->trajectory_class);
            ImGui::TableNextColumn();
            ImGui::PushFont(g_font_small); ImGui::PushStyleColor(ImGuiCol_Text, toV4(tp.second));
            ImGui::TextUnformatted(tp.first);
            ImGui::PopStyleColor(); ImGui::PopFont();
        }
        ImGui::TableNextColumn();
        ImGui::PushFont(g_font_mono); ImGui::Text("exp %d", p->contract.exp_year); ImGui::PopFont();
        // C3: Contract status pill — surfaces expiring deals so the user
        // can spot re-sign candidates at a glance. Long-term deals
        // (>=3y) render no pill to keep the row uncluttered.
        {
            int yl = p->years_left(s.gm.year);
            if (yl <= 0) {
                ImGui::SameLine(); Pill("EXPIRED", kVlrRedDim);
            } else if (yl == 1) {
                ImGui::SameLine(); Pill("FINAL YEAR", kVlrGold);
            } else if (yl == 2) {
                ImGui::SameLine(); Pill("Expiring soon", kVlrSub);
            }
        }
        ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("$%dK", p->contract.amount_k); ImGui::PopFont();
        ImGui::TableNextColumn();
        // C1: Re-sign / Extend button — any active rostered player can
        // open the negotiation modal at any time. Final-year deals use
        // "Re-sign" wording; mid-deal extensions read "Extend". Was
        // previously gated to `years_left <= 1` which silently locked
        // bench + mid-contract players out of negotiations — user spec
        // 2026-05-28: "all rostered players can be re-signed regardless
        // of starting/bench status". Defensive guard still hides this on
        // any row that somehow shows a FA team_name.
        {
            int yl = p->years_left(s.gm.year);
            bool can_resign = (yl >= 1)
                              && (p->team_name == t->name)
                              && p->team_name != "Free Agent"
                              && p->team_name != "Retired";
            if (can_resign) {
                ImGui::PushStyleColor(ImGuiCol_Button,        toV4(kAccent));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toV4(kAccent2));
                const char* label = (yl <= 1) ? "Re-sign" : "Extend";
                if (ImGui::SmallButton(label)) {
                    s.show_resign_modal    = true;
                    s.resign_target        = p;
                    // Sentinel 0 -> modal fills both from cached offer on
                    // first frame (years = offer.years, amount = offer.amount_k).
                    s.resign_years_choice  = 0;
                    s.resign_amount_choice = 0;
                    s.resign_role_choice   = -1;       // default to natural
                    s.resign_cached_for    = nullptr;  // force recompute
                }
                ImGui::PopStyleColor(2);
                ImGui::SameLine();
            }
        }
        ImGui::PushStyleColor(ImGuiCol_Button,        toV4(kVlrRed));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toV4(kVlrRedDim));
        // QW: gate the irreversible release behind a confirm modal (no save to
        // fall back on). The actual release happens in the confirm below.
        if (ImGui::SmallButton("Release")) s.release_confirm_target = p;
        ImGui::PopStyleColor(2);
        // Bench / Start toggle — keeps the player rostered AND paid, just
        // changes who plays. A starter shows "Bench" (the top reserve takes
        // their spot); a reserve shows "Start" (displaces the same-position
        // starter so the 1D/1C/1I/1S + flex shape is preserved).
        if (p) {
            bool p_is_bench = std::find(bench.begin(), bench.end(), p) != bench.end();
            if (p_is_bench) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Start")) {
                    std::size_t pos = (std::size_t)vlr::position_of(*p);
                    vlr::PlayerPtr displaced = canonical[pos] ? canonical[pos] : fifth;
                    if (displaced) to_swap.push_back({displaced, p});
                }
            } else if (!bench.empty()) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Bench")) {
                    to_swap.push_back({p, bench.front()});  // top reserve comes in
                }
            }
        }
        ImGui::PopID();
    };

    // === Starters table — fixed 5-row order by Position =================
    if (BeginCard("##roster_starters")) {
    SectionHeader("STARTERS", "Four canonical slots + a flexible fifth", kAccent);
    if (ImGui::BeginTable("##fullroster", 13,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
            ImVec2(-FLT_MIN, 220))) {
        ImGui::TableSetupColumn("FLAG");
        ImGui::TableSetupColumn("NAME", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("POSITION");
        ImGui::TableSetupColumn("REGION");
        ImGui::TableSetupColumn("AGE");
        ImGui::TableSetupColumn("OVR");
        ImGui::TableSetupColumn("POT");
        ImGui::TableSetupColumn("FORM");
        ImGui::TableSetupColumn("MOOD");
        ImGui::TableSetupColumn("TRAJ");
        ImGui::TableSetupColumn("CONTRACT");
        ImGui::TableSetupColumn("SALARY");
        ImGui::TableSetupColumn("ACTIONS");
        ImGui::TableHeadersRow();
        int row_id = 0;
        // 4 canonical rows in enum order (D / C / S / I).
        for (size_t i = 0; i < (size_t)vlr::Position::Count; ++i) {
            auto slot_pos = (vlr::Position)i;
            const auto& occupant = canonical[i];
            render_row(slot_pos, occupant, false, row_id++);
        }
        // 5th / Flex row — whichever starter didn't fill a canonical
        // slot. Empty label is "[empty 5th slot]" (no PositionBadge
        // since the 5th can be any position).
        render_row(vlr::Position::Initiator, fifth, false, row_id++,
                   "5th slot");
        ImGui::EndTable();
    }
    } EndCard();

    // === Bench (overflow / duplicates) ==================================
    if (!bench.empty()) {
        VSpace(14);
        if (BeginCard("##roster_bench", kBgDeep)) {
        SectionHeader("BENCH", "Reserves, overflow & duplicate-position players", kVlrSub);
        if (ImGui::BeginTable("##fullroster_bench", 13,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                ImVec2(-FLT_MIN, 180))) {
            ImGui::TableSetupColumn("FLAG");
            ImGui::TableSetupColumn("NAME", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("POSITION");
            ImGui::TableSetupColumn("REGION");
            ImGui::TableSetupColumn("AGE");
            ImGui::TableSetupColumn("OVR");
            ImGui::TableSetupColumn("POT");
            ImGui::TableSetupColumn("FORM");
            ImGui::TableSetupColumn("MOOD");
            ImGui::TableSetupColumn("TRAJ");
            ImGui::TableSetupColumn("CONTRACT");
            ImGui::TableSetupColumn("SALARY");
            ImGui::TableSetupColumn("ACTIONS");
            ImGui::TableHeadersRow();
            int row_id = 1000;
            for (auto& p : bench) {
                render_row(vlr::position_of(*p), p, false, row_id++);
            }
            ImGui::EndTable();
        }
        } EndCard();
    }

    for (auto& sw : to_swap) {
        if (sw.first && sw.second && t->swap_roster_slots(sw.first, sw.second)) {
            s.log.emplace_back(sw.first->name + " benched; " + sw.second->name + " starts.");
        }
    }
    for (auto& r : to_release) {
        t->release_player(r);
        s.log.emplace_back("Released " + r->name + " from " + t->name + ".");
    }

    // QW: Release confirmation modal — releasing a player is irreversible and
    // there is no save to undo it, so gate it behind an explicit confirm.
    if (s.release_confirm_target && !ImGui::IsPopupOpen("Release Player##confirm"))
        ImGui::OpenPopup("Release Player##confirm");
    if (ImGui::BeginPopupModal("Release Player##confirm", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        auto rp = s.release_confirm_target;
        ImGui::PushFont(g_font_h2);
        ImGui::Text("Release %s?", rp ? rp->name.c_str() : "?");
        ImGui::PopFont();
        if (rp) {
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
            ImGui::Text("%s, %d \xE2\x80\x94 %s, OVR %d", vlr::role_name(rp->primary_role),
                        rp->age, rp->country.c_str(), rp->ovr());
            ImGui::PopStyleColor();
        }
        ImGui::TextColored(toV4(kVlrRed),
            "They become a free agent immediately. This cannot be undone.");
        VSpace(6);
        ImGui::PushStyleColor(ImGuiCol_Button,        toV4(kVlrRed));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toV4(kVlrRedDim));
        if (ImGui::Button("Release", ImVec2(130, 0))) {
            if (rp) {
                t->release_player(rp);
                s.log.emplace_back("Released " + rp->name + " from " + t->name + ".");
            }
            s.release_confirm_target.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(130, 0))) {
            s.release_confirm_target.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // === C2: Re-sign negotiation modal ==================================
    // Now a thin call into the shared DrawNegotiationModal helper. Both
    // Re-sign (here) and FA Sign (in DrawMarket) share ~250 lines of UI;
    // the helper is the single source of truth. See DrawNegotiationModal
    // defined just above DrawDashboard.
    DrawNegotiationModal(
        s, NegotiationMode::Resign, "Re-sign Player##resign_modal",
        s.resign_target, t,
        s.resign_years_choice, s.resign_amount_choice, s.resign_role_choice,
        s.resign_cached_offer, s.resign_cached_mkt_k, s.resign_cached_for,
        s.show_resign_modal,
        s.resign_bonus_choice, s.resign_promise_starter, s.resign_offers_left);
#if 0
    // Legacy inline body — kept #if 0'd as a fallback reference during the
    // helper rollout; will be deleted in a follow-up after stability has
    // been confirmed across a full session.
    if (s.show_resign_modal && s.resign_target) {
        ImGui::OpenPopup("Re-sign Player##resign_modal");
        ImVec2 rc = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(rc, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(860, 540), ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal("Re-sign Player##resign_modal",
                                   &s.show_resign_modal,
                                   ImGuiWindowFlags_NoSavedSettings)) {
            auto p = s.resign_target;

            // Cache the offer + market value on FIRST frame for this target.
            // `propose_resign_offer` -> `gen_contract` uses randomize_amount,
            // so recomputing each frame jitters the slider's max bound and
            // the value gets re-clamped, causing visible oscillation. Stable
            // snapshot per modal session is the correct UX anyway — once the
            // player has stated their ask, the user negotiates against THAT.
            if (s.resign_cached_for != p.get()) {
                s.resign_cached_offer = p->propose_resign_offer(*t, s.gm.year);
                s.resign_cached_mkt_k = t->market_value_estimate(*p);
                s.resign_cached_for   = p.get();
                if (s.resign_years_choice <= 0)
                    s.resign_years_choice = s.resign_cached_offer.years;
                if (s.resign_amount_choice <= 0)
                    s.resign_amount_choice = s.resign_cached_offer.amount_k;
            }
            const vlr::Player::ResignOffer& offer = s.resign_cached_offer;
            int mkt_k = s.resign_cached_mkt_k;

            // Clamp slider defaults to the offer's tolerance window on
            // first open. resign_amount_choice == 0 = "use ask".
            if (s.resign_amount_choice <= 0) {
                s.resign_amount_choice = offer.amount_k;
            }
            int years_lo = 1;
            int years_hi = std::max(1, offer.max_acceptable_years);
            int amt_lo   = std::max((int)vlr::kSalaryFloorK,
                                    offer.min_acceptable_k);
            int amt_hi   = std::max(amt_lo + 1,
                                    (int)std::min<long long>(
                                        vlr::kSalaryCapK,
                                        (long long)std::lround(
                                            offer.amount_k * 1.5)));
            s.resign_years_choice  = vlr::clamp_v(s.resign_years_choice,
                                                  years_lo, years_hi);
            s.resign_amount_choice = vlr::clamp_v(s.resign_amount_choice,
                                                  amt_lo, amt_hi);

            // === Header ================================================
            {
                std::string title = std::string("RE-SIGN ") + p->name;
                H2(title.c_str());
            }
            {
                std::string sig = p->signature_agent();
                if (!sig.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                    ImGui::Text("[*] Signature %s", sig.c_str());
                    ImGui::PopStyleColor();
                }
            }
            // Status pills row.
            {
                char exp_buf[32];
                std::snprintf(exp_buf, sizeof(exp_buf),
                              "Expires %d", p->contract.exp_year);
                Pill(exp_buf, kVlrSub);
                ImGui::SameLine();
                ImU32 wcol = kAccent;
                const char* wname = "Open";
                switch (t->window) {
                    case vlr::TeamWindow::Opening:
                        wcol = kVlrGreen;  wname = "Opening"; break;
                    case vlr::TeamWindow::Open:
                        wcol = kAccent;    wname = "Open";    break;
                    case vlr::TeamWindow::Closing:
                        wcol = kVlrGold;   wname = "Closing"; break;
                    case vlr::TeamWindow::Closed:
                        wcol = kVlrRedDim; wname = "Closed";  break;
                }
                Pill(wname, wcol);
            }
            ThinDivider(6);

            // === Two-column body ======================================
            ImGui::Columns(2, "##resign_cols", false);

            // ---- LEFT: player's ask -----------------------------------
            SectionHeader("PLAYER IS ASKING", nullptr, kVlrGold);
            ImGui::Text("Years requested: %d", offer.years);
            ImGui::Text("Amount requested: $%dK/yr", offer.amount_k);
            // Willingness bar — 0..100% color-tiered.
            {
                float w01 = (float)vlr::clamp_v(offer.willingness,
                                                0.0, 1.0);
                ImU32 wcol = (w01 >= 0.70f) ? kVlrGreen
                           : (w01 >= 0.40f) ? kVlrGold
                           :                  kVlrRed;
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, toV4(wcol));
                char wbuf[32];
                std::snprintf(wbuf, sizeof(wbuf), "%.0f%% willing",
                              w01 * 100.0f);
                ImGui::ProgressBar(w01, ImVec2(-FLT_MIN, 0), wbuf);
                ImGui::PopStyleColor();
            }
            if (!offer.explainer.empty()) {
                MutedText("%s", offer.explainer.c_str());
            }
            VSpace(6);
            ImGui::Text("Min acceptable: $%dK/yr", offer.min_acceptable_k);
            ImGui::Text("Max acceptable years: %d",
                        offer.max_acceptable_years);

            // ---- RIGHT: market + your offer ---------------------------
            ImGui::NextColumn();
            SectionHeader("MARKET & YOUR OFFER", nullptr, kAccent);
            ImGui::Text("Market value: $%dK/yr", mkt_k);
            ImGui::Text("Team budget: %s", fmt_money(t->budget).c_str());
            VSpace(4);

            ImGui::SetNextItemWidth(220);
            ImGui::SliderInt("Years", &s.resign_years_choice,
                             years_lo, years_hi);
            ImGui::SetNextItemWidth(220);
            ImGui::SliderInt("Salary $K/yr",
                             &s.resign_amount_choice,
                             amt_lo, amt_hi);

            // Role offer combo — the role the team is offering for the
            // new deal. Natural role is the default (no fit penalty); off-
            // role offers take a role_fit_score-derived score modifier.
            // Each entry surfaces a fit verdict so the user sees up-front
            // whether the offered role is plausible for this player.
            {
                const vlr::Role role_opts[4] = {
                    vlr::Role::Duelist, vlr::Role::Initiator,
                    vlr::Role::Controller, vlr::Role::Sentinel
                };
                int natural_idx = 0;
                for (int i = 0; i < 4; ++i)
                    if (role_opts[i] == p->primary_role) natural_idx = i;
                int cur_idx = (s.resign_role_choice < 0)
                                ? natural_idx : s.resign_role_choice;
                if (cur_idx < 0 || cur_idx > 3) cur_idx = natural_idx;
                ImGui::SetNextItemWidth(220);
                char combo_preview[80];
                std::snprintf(combo_preview, sizeof(combo_preview),
                    "%s%s",
                    vlr::role_name(role_opts[cur_idx]),
                    (cur_idx == natural_idx) ? " (Natural)" : "");
                if (ImGui::BeginCombo("Offered Role", combo_preview)) {
                    for (int i = 0; i < 4; ++i) {
                        char item[96];
                        std::snprintf(item, sizeof(item),
                            "%s  -  %s",
                            vlr::role_name(role_opts[i]),
                            p->role_fit_verdict(role_opts[i]));
                        bool selected = (i == cur_idx);
                        if (ImGui::Selectable(item, selected)) {
                            s.resign_role_choice = i;
                        }
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            }
            vlr::Role offered_role = p->primary_role;
            if (s.resign_role_choice >= 0 && s.resign_role_choice < 4) {
                const vlr::Role role_opts[4] = {
                    vlr::Role::Duelist, vlr::Role::Initiator,
                    vlr::Role::Controller, vlr::Role::Sentinel
                };
                offered_role = role_opts[s.resign_role_choice];
            }

            // Transparent breakdown — single source of truth with the engine.
            // evaluate_resign_offer is the same function the AI uses to
            // decide acceptance, so the displayed pill never disagrees with
            // what "Send Offer" actually does.
            auto bd = p->evaluate_resign_offer(s.resign_amount_choice,
                                                s.resign_years_choice,
                                                *t, offered_role);

            // Verdict pill — colored by total score band.
            ImU32 verdict_col = kVlrSub;
            if      (bd.total >= 85) verdict_col = kVlrGold;       // OVERPAY
            else if (bd.total >= 70) verdict_col = kVlrGreen;      // STRONG
            else if (bd.total >= 55) verdict_col = kAccent;        // FAIR
            else if (bd.total >= 35) verdict_col = kRoleFlex;      // WEAK (amber)
            else                     verdict_col = kVlrRed;        // INSULTING
            char vbuf[80];
            std::snprintf(vbuf, sizeof(vbuf), "%s  (%d/100)", bd.verdict, bd.total);
            Pill(vbuf, verdict_col);
            ImGui::SameLine();
            if (bd.will_accept) Pill("WILL ACCEPT", kVlrGreen);
            else                Pill("WILL REJECT", kVlrRed);

            // Factor breakdown — small mono list.
            VSpace(4);
            if (g_font_small) ImGui::PushFont(g_font_small);
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
            ImGui::Text("Base interest: %d", bd.base_score);
            ImGui::PopStyleColor();
            for (auto& kv : bd.labels) {
                ImU32 col = (kv.second > 0) ? kVlrGreen
                          : (kv.second < 0) ? kVlrRed
                                            : kVlrSub;
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(col));
                ImGui::Text("  %s: %+d", kv.first.c_str(), kv.second);
                ImGui::PopStyleColor();
            }
            if (g_font_small) ImGui::PopFont();

            // Reject reason (only when relevant).
            if (!bd.will_accept && !bd.reject_reason.empty()) {
                VSpace(4);
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrRedDim));
                ImGui::TextWrapped("Reason: %s", bd.reject_reason.c_str());
                ImGui::PopStyleColor();
            }

            ImGui::Columns(1);
            ThinDivider(8);

            // === Action buttons =======================================
            // "Send Offer" — primary; commits via Team::resign_player.
            ImGui::PushStyleColor(ImGuiCol_Button,        toV4(kAccent));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toV4(kAccent2));
            if (ImGui::Button("Send Offer", ImVec2(150, 32))) {
                int yrs = s.resign_years_choice;
                int amt = s.resign_amount_choice;
                // Use the role-aware acceptance check via the breakdown
                // (same source of truth — the user-visible verdict is
                // exactly what gets committed).
                bool ok = bd.will_accept;
                if (ok) {
                    int base_yr = (s.gm.current_phase() == "OFFSEASON")
                                      ? s.gm.year : s.gm.year + 1;
                    bool committed = t->resign_player(p, yrs, amt,
                                                     base_yr,
                                                     offered_role);
                    if (committed) {
                        char lb[160];
                        std::snprintf(lb, sizeof(lb),
                            "Re-signed %s for %dY at $%dK/yr.",
                            p->name.c_str(), yrs, amt);
                        s.log.emplace_back(lb);
                        s.show_resign_modal = false;
                        s.resign_target.reset();
                    } else {
                        char lb[160];
                        std::snprintf(lb, sizeof(lb),
                            "Re-sign of %s failed (budget / cap).",
                            p->name.c_str());
                        s.log.emplace_back(lb);
                    }
                } else {
                    char lb[160];
                    std::snprintf(lb, sizeof(lb),
                        "%s refused at $%dK/y for %dY.",
                        p->name.c_str(), amt, yrs);
                    s.log.emplace_back(lb);
                }
            }
            ImGui::PopStyleColor(2);
            ImGui::SameLine();

            // "Match Their Ask" — gold quick-fill.
            ImGui::PushStyleColor(ImGuiCol_Button,        toV4(kVlrGold));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toV4(kVlrGold));
            if (ImGui::Button("Match Their Ask", ImVec2(170, 32))) {
                s.resign_years_choice =
                    vlr::clamp_v(offer.years, years_lo, years_hi);
                s.resign_amount_choice =
                    vlr::clamp_v(offer.amount_k, amt_lo, amt_hi);
            }
            ImGui::PopStyleColor(2);
            ImGui::SameLine();

            // "Walk Away" — close without action.
            if (ImGui::Button("Walk Away", ImVec2(120, 32))) {
                s.show_resign_modal = false;
                s.resign_target.reset();
            }

            ImGui::EndPopup();
        }
        // Defensive: if user closed via the [x] window button, clear the
        // target pointer + cache so a stale ptr/offer can't survive into
        // next frame OR leak into a fresh open of a different player.
        if (!s.show_resign_modal) {
            s.resign_target.reset();
            s.resign_cached_for = nullptr;
        }
    }
#endif
}
