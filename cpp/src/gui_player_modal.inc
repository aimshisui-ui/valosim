// gui_player_modal.inc - unity-build fragment #included by gui_main.cpp.
// Shares gui_main's anonymous namespace, helpers, colors, fonts, AppState.
// NOT a standalone translation unit. Edit freely; it is part of gui_main.cpp.

void DrawPlayerProfileModal(AppState& s) {
    if (!s.show_player_modal || !s.selected_player) return;
    ImGui::OpenPopup("Player Profile##modal");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(820, 720), ImGuiCond_Appearing);

    if (ImGui::BeginPopupModal("Player Profile##modal", &s.show_player_modal,
                               ImGuiWindowFlags_NoSavedSettings)) {
        auto p = s.selected_player;

        // Header strip
        // === Header strip: handle, flag, role, identity, action buttons ===
        // Hero header card — premium framing around the existing handle /
        // flag / badges / identity rows. Pure visual wrapper; every widget,
        // label and ImGui ID inside is unchanged.
        BeginCard("##pp_hero", kBgDeep, 16.0f, 12.0f);
        CountryFlag(p->country_iso);
        ImGui::SameLine();
        if (s.god_mode) {
            char name_buf[128];
            std::snprintf(name_buf, sizeof(name_buf), "%s", p->name.c_str());
            ImGui::SetNextItemWidth(280);
            if (ImGui::InputText("##nameedit", name_buf, sizeof(name_buf))) {
                p->name = name_buf;
            }
        } else {
            H1(player_label(*p).c_str(), kVlrRed);
        }
        ImGui::SameLine(); ImGui::Dummy(ImVec2(10, 0)); ImGui::SameLine();
        PositionBadge(vlr::position_of(*p));
        if (p->is_flex) { ImGui::SameLine(); FlexBadge(); }
        if (p->is_igl)  { ImGui::SameLine(); IglBadge();  }

        // Favorite + queue-ranked actions on the right side of the header.
        ImGui::SameLine();
        ImGui::Dummy(ImVec2(20, 0));
        ImGui::SameLine();
        bool favorited = s.gm.is_favorited(p);
        if (favorited) ImGui::PushStyleColor(ImGuiCol_Button, toV4(kVlrGold));
        if (ImGui::Button(favorited ? "[*] Favorited" : "[ ] Favorite", ImVec2(140, 28))) {
            if (favorited) s.gm.unfavorite_player(p);
            else            s.gm.favorite_player(p);
        }
        if (favorited) ImGui::PopStyleColor();
        ImGui::SameLine();
        // Watchlist toggle (increment C) — a transfer/scouting target, distinct
        // from the GOAT Favorite above.
        bool watched = s.gm.is_watched(p);
        if (watched) ImGui::PushStyleColor(ImGuiCol_Button, toV4(kAccent2));
        if (ImGui::Button(watched ? "[*] Watching" : "[ ] Watch", ImVec2(130, 28))) {
            if (watched) s.gm.unwatch_player(p);
            else         s.gm.watch_player(p);
        }
        if (watched) ImGui::PopStyleColor();
        ImGui::SameLine();
        // Force solo Q match: pop the live viewer with this player as host.
        // Retired players have left competition — no ranked queue for them.
        bool clicked_queue = false;
        if (!p->is_retired) {
            ImGui::PushStyleColor(ImGuiCol_Button,        toV4(kVlrRed));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toV4(kVlrRedDim));
            clicked_queue = ImGui::Button("Queue Ranked Match", ImVec2(180, 28));
            ImGui::PopStyleColor(2);
        }

        // Identity strip.
        if (!p->first.empty() || !p->last.empty() || !p->country.empty()) {
            Sub("%s %s  |  %s  |  Age %d  |  %s  |  $%dK/yr (exp %d)  |  %dY left",
                p->first.c_str(), p->last.c_str(),
                p->country.empty() ? "?" : p->country.c_str(),
                p->age,
                p->team_name.c_str(),
                p->contract.amount_k, p->contract.exp_year,
                p->years_left(s.gm.year));
        } else {
            Sub("%s  |  Age %d  |  Team: %s  |  %dY left  |  $%dK/yr (exp %d)",
                p->region.c_str(), p->age, p->team_name.c_str(),
                p->years_left(s.gm.year), p->contract.amount_k, p->contract.exp_year);
        }

        // Signature agent strip — gold accent identifying this player's
        // mastery main. Empty until the player has 5+ pro matches on a
        // single agent, so most rookies won't see this line.
        {
            std::string sig = p->signature_agent();
            if (!sig.empty()) {
                auto sit = p->agent_mastery.find(sig);
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                if (sit != p->agent_mastery.end() && sit->second.matches > 0) {
                    ImGui::Text("[*] Signature %s  -  %d matches  -  %.2f avg  -  peak %.2f",
                        sig.c_str(), sit->second.matches,
                        sit->second.avg_rating, sit->second.peak_rating);
                } else {
                    ImGui::Text("[*] Signature %s", sig.c_str());
                }
                ImGui::PopStyleColor();

                // Phase 1 (Agent C) — regional rank flavor line. Compute
                // LOCALLY by scanning every player in this region for the
                // same signature agent, ranked by agent_mastery score.
                double my_score = 0.0;
                if (sit != p->agent_mastery.end()) {
                    // Use matches * avg_rating as a robust mastery proxy
                    // (works whether the engine exposes a method or not).
                    my_score = (double)sit->second.matches * sit->second.avg_rating;
                }
                int rank = 1;     // 1-indexed
                int tied_above = 0;
                auto it_eng = s.gm.solo_qs.find(p->region);
                if (it_eng != s.gm.solo_qs.end() && it_eng->second) {
                    for (auto& other : it_eng->second->global_ladder()) {
                        if (!other || other.get() == p.get()) continue;
                        if (other->signature_agent() != sig) continue;
                        auto oit = other->agent_mastery.find(sig);
                        if (oit == other->agent_mastery.end()) continue;
                        double os = (double)oit->second.matches * oit->second.avg_rating;
                        if (os > my_score) ++tied_above;
                    }
                }
                rank = tied_above + 1;
                const char* flavor = nullptr;
                if      (rank == 1)  flavor = "Best %s player in %s";
                else if (rank <= 3)  flavor = "Top %s player in %s";
                else if (rank <= 10) flavor = "Notable %s main";
                else                 flavor = "Strong %s player";
                ImGui::PushFont(g_font_small);
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                if (rank <= 3) {
                    ImGui::Text(flavor, sig.c_str(), p->region.c_str());
                } else {
                    // "Notable %s main" / "Strong %s player" — no region token.
                    ImGui::Text(flavor, sig.c_str());
                }
                ImGui::PopStyleColor();
                ImGui::PopFont();
            }
        }

        // Playstyle = the deep 32-archetype (Player::archetype). This is the
        // stable, simulation-driving identity assigned at spawn — one of 32,
        // never a generic "Generalist". Rendered prominently in the archetype
        // accent (purple). The OLD emergent playstyle_identity() is kept only
        // as a small secondary "tendency" note, and ONLY when it surfaces
        // something meaningful — the generic "Generalist" fallback is
        // suppressed entirely so it never reads as bland.
        {
            ImGui::PushFont(g_font_h2 ? g_font_h2 : g_font_body);
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kAccent2));
            ImGui::Text("PLAYSTYLE: %s", vlr::archetype_name(p->archetype));
            ImGui::PopStyleColor();
            ImGui::PopFont();

            std::string ident = p->playstyle_identity();
            if (!ident.empty() && ident != "Generalist") {
                bool volatile_ident = (ident == "LAN Choker"
                                    || ident == "Momentum Player");
                ImU32 col = volatile_ident ? kVlrRedDim : kVlrSub;
                ImGui::PushFont(g_font_small ? g_font_small : g_font_body);
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(col));
                ImGui::Text("Tendency: %s", ident.c_str());
                ImGui::PopStyleColor();
                ImGui::PopFont();
            }
        }

        // Position chip — canonical 4-value gameplay slot (Duelist /
        // Controller / Sentinel / Initiator), derived purely from
        // primary_role via position_of(). Flex and IGL are INDEPENDENT
        // overlay flags appended as suffix chips when set, in canonical
        // order: position first, then • Flex, then • IGL. A "Flex IGL"
        // renders as e.g. "POSITION: Initiator • Flex • IGL".
        {
            vlr::Position pos = vlr::position_of(*p);
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(position_color(pos)));
            ImGui::Text("POSITION: %s", vlr::position_name(pos));
            ImGui::PopStyleColor();
            if (p->is_flex) {
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kRoleFlex));
                ImGui::TextUnformatted("\xE2\x80\xA2 Flex");
                ImGui::PopStyleColor();
            }
            if (p->is_igl) {
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kIglBadge));
                ImGui::TextUnformatted("\xE2\x80\xA2 IGL");
                ImGui::PopStyleColor();
            }
        }

        // === Archetype + development row ===============================
        // Deep simulation-driving playstyle (distinct from the emergent
        // PLAYSTYLE label above) shown as a prominent purple Pill, plus an
        // OVR -> POT line and a development-trajectory Pill so the user can
        // instantly read whether this is a rising star, an aging vet, etc.
        // POT is shown directly here (user explicitly requested visibility
        // for squad planning) regardless of God Mode.
        //
        // Trajectory rule: derive from age + (potential - ovr) gap +
        // consistency (inconsistency => high-risk prospect).
        // Reads the engine-stamped trajectory class (item 8 fix) instead of
        // recomputing a static gap/age guess every frame.
        auto dev_trajectory = [](const vlr::Player& pl)
            -> std::pair<const char*, ImU32> {
            return traj_display(pl.trajectory_class);
        };
        {
            int ovr_i = (int)std::lround(p->ovr());
            // (Archetype is shown as the prominent PLAYSTYLE line above —
            // no duplicate pill here.) OVR -> POT pill (gold = prestige).
            char ovrpot[64];
            if (potential_known(s.gm, *p)) {
                std::snprintf(ovrpot, sizeof(ovrpot), "OVR %d \xE2\x86\x92 POT %d",
                              ovr_i, p->potential);
            } else {
                int plo, phi; potential_band(s.gm, *p, plo, phi);
                std::snprintf(ovrpot, sizeof(ovrpot), "OVR %d \xE2\x86\x92 POT %d-%d (est)",
                              ovr_i, plo, phi);
            }
            Pill(ovrpot, kSurfaceHi, kVlrText);
            ImGui::SameLine();
            // Development-trajectory pill (color-coded).
            auto traj = dev_trajectory(*p);
            Pill(traj.first, traj.second);
            ImGui::SameLine();
            // Desire pill — Agent B's NEGOTIATION-driving preference system.
            // Colour mapping per Phase A spec: Greedy=gold (money), Loyal=blue
            // (team), RingChaser=red (trophy), Mercenary=amber (hired-gun),
            // StabilitySeeker=green (steady), Competitor=electric (fire).
            {
                ImU32 dcol = kAccent;
                switch (p->desire) {
                    case vlr::Desire::Greedy:          dcol = kVlrGold;   break;
                    case vlr::Desire::Loyal:           dcol = kInfo;      break;
                    case vlr::Desire::RingChaser:      dcol = kVlrRed;    break;
                    case vlr::Desire::Mercenary:       dcol = kRoleFlex;  break;
                    case vlr::Desire::StabilitySeeker: dcol = kVlrGreen;  break;
                    case vlr::Desire::Competitor:      dcol = kAccent;    break;
                    default:                           dcol = kAccent;    break;
                }
                Pill(vlr::desire_name(p->desire), dcol);
            }
        }
        // One-line desire blurb — gives the user a single-sentence read on
        // WHY this player negotiates the way they do. Suppressed when empty.
        {
            const char* db = vlr::desire_blurb(p->desire);
            if (db && db[0]) MutedText("Desire: %s", db);
        }

        if (clicked_queue) {
            // Find the SoloQEngine for this player's region (defensive: fall
            // back to any region if their region isn't found).
            std::shared_ptr<vlr::SoloQEngine> engine;
            auto it = s.gm.solo_qs.find(p->region);
            if (it != s.gm.solo_qs.end()) engine = it->second;
            if (!engine && !s.gm.solo_qs.empty()) engine = s.gm.solo_qs.begin()->second;
            if (engine) {
                auto rec = engine->force_solo_match(p);
                if (rec) {
                    s.show_player_modal = false;
                    ImGui::CloseCurrentPopup();
                    OpenReplay(s, rec);
                    s.log.emplace_back("Queued ranked match for " + p->name + " — opening live viewer.");
                } else {
                    s.log.emplace_back("Could not queue match (not enough nearby-MMR players).");
                }
            }
        }

        EndCard();
        VSpace(8);

        // === Award-watch strip ===============================================
        // If this player is currently among the tracked award-race candidates
        // (overall MVP and/or a role-of-the-year race), surface their live
        // standing — rank within the field plus the momentum blurb the engine
        // already computes. Rendered ONLY when in contention, so the vast
        // majority of players (who aren't) see no clutter here.
        {
            struct Standing { std::string cat; int rank; int field; std::string blurb; int delta; };
            std::vector<Standing> standings;
            for (auto& lb : s.gm.mvp_race) {
                int field = (int)lb.candidates.size();
                for (int i = 0; i < field; ++i) {
                    if (lb.candidates[i].player.get() == p.get()) {
                        standings.push_back({ lb.category, i + 1, field,
                                              lb.candidates[i].blurb,
                                              lb.candidates[i].delta });
                        break;
                    }
                }
            }
            if (!standings.empty()) {
                BeginCard("##pp_awardwatch", kBgDeep, 12.0f, 8.0f);
                ImGui::PushFont(g_font_small ? g_font_small : g_font_body);
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                ImGui::TextUnformatted("AWARD WATCH");
                ImGui::PopStyleColor();
                ImGui::PopFont();
                for (auto& st : standings) {
                    ImU32 rc = (st.rank == 1) ? kVlrGold
                             : (st.rank <= 3) ? kAccent
                             :                  kVlrSub;
                    Pill(st.cat.c_str(), rc);
                    ImGui::SameLine();
                    ImGui::Text("#%d of %d", st.rank, st.field);
                    if (!st.blurb.empty()) {
                        ImGui::SameLine();
                        ImU32 bc = (st.delta > 0) ? kVlrGreen
                                 : (st.delta < 0) ? kVlrRed
                                 :                  kVlrSub;
                        ImGui::PushStyleColor(ImGuiCol_Text, toV4(bc));
                        ImGui::Text("(%s)", st.blurb.c_str());
                        ImGui::PopStyleColor();
                    }
                }
                EndCard();
                VSpace(8);
            }
        }

        if (ImGui::BeginTabBar("##playertabs")) {
            // === Overview ===
            if (ImGui::BeginTabItem("Overview")) {
                // === Phase B (C4): Best on-team chemistry partner =========
                // For active roster players, walk current team and ask
                // Agent A's O(1) `chemistry_between` for each teammate.
                // Render a single MutedText if a strong duo OR friction
                // exists; suppress entirely for FA / Retired / no team.
                if (!p->is_retired
                    && p->team_name != "Free Agent"
                    && !p->team_name.empty()
                    && p->team_name != "Retired") {
                    vlr::TeamPtr cur_team;
                    for (auto& kv : s.gm.leagues) {
                        if (!kv.second) continue;
                        for (auto& tt : kv.second->teams()) {
                            if (tt && tt->name == p->team_name) {
                                cur_team = tt; break;
                            }
                        }
                        if (cur_team) break;
                    }
                    if (cur_team) {
                        // === Player MOOD — composite happiness + why ==========
                        auto mb = p->mood_breakdown(*cur_team, s.gm.year);
                        ImU32 mcol = mb.total >= 62 ? kVlrGreen
                                   : mb.total >= 42 ? kVlrGold : kVlrRed;
                        ImGui::PushStyleColor(ImGuiCol_Text, toV4(mcol));
                        ImGui::Text("Mood: %s  (%d/100)", mb.verdict, mb.total);
                        ImGui::PopStyleColor();
                        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, toV4(mcol));
                        ImGui::ProgressBar(mb.total / 100.0f, ImVec2(-FLT_MIN, 0));
                        ImGui::PopStyleColor();
                        if (g_font_small) ImGui::PushFont(g_font_small);
                        for (auto& kv : mb.labels) {
                            ImU32 fc = (kv.second > 0) ? kVlrGreen
                                     : (kv.second < 0) ? kVlrRed : kVlrSub;
                            ImGui::PushStyleColor(ImGuiCol_Text, toV4(fc));
                            ImGui::Text("   %s: %+d", kv.first.c_str(), kv.second);
                            ImGui::PopStyleColor();
                        }
                        if (g_font_small) ImGui::PopFont();
                        if (p->transfer_requested) {
                            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrRed));
                            ImGui::TextUnformatted("Has handed in a TRANSFER REQUEST.");
                            ImGui::PopStyleColor();
                        }
                        ThinDivider(6);

                        const vlr::Player* best_mate = nullptr;
                        double best_val = 0.0;
                        for (auto& mate : cur_team->roster) {
                            if (!mate || mate.get() == p.get()) continue;
                            double v = cur_team->chemistry_between(
                                *p, *mate);
                            if (best_mate == nullptr
                                || std::abs(v) > std::abs(best_val)) {
                                best_mate = mate.get();
                                best_val  = v;
                            }
                        }
                        if (best_mate && best_val >= 0.5) {
                            MutedText("Strong duo: \xE2\x86\x94 %s (%+.2f)",
                                      best_mate->name.c_str(),
                                      best_val);
                        } else if (best_mate && best_val <= -0.5) {
                            ImGui::PushStyleColor(ImGuiCol_Text,
                                                  toV4(kVlrRedDim));
                            if (g_font_small) ImGui::PushFont(g_font_small);
                            ImGui::Text("Friction: \xE2\x86\x94 %s (%+.2f)",
                                        best_mate->name.c_str(),
                                        best_val);
                            if (g_font_small) ImGui::PopFont();
                            ImGui::PopStyleColor();
                        }
                    }
                }

                ImGui::Columns(2, nullptr, false);

                H2("OVERALL");
                // OVR / POT side-by-side tiles so long-term ceiling reads
                // at a glance next to current ability.
                {
                    char ovv[12], potv[16];
                    std::snprintf(ovv,  sizeof(ovv),  "%d",
                                  (int)std::lround(p->ovr()));
                    bool known = potential_known(s.gm, *p);
                    if (known) {
                        std::snprintf(potv, sizeof(potv), "%d", p->potential);
                    } else {
                        int plo, phi; potential_band(s.gm, *p, plo, phi);
                        std::snprintf(potv, sizeof(potv), "%d-%d", plo, phi);
                    }
                    bool grows = (p->potential > (int)std::lround(p->ovr()));
                    StatTile("OVR", ovv, kAccent, ImVec2(120, 78));
                    ImGui::SameLine();
                    StatTile(known ? "POT" : "POT (est)", potv,
                             (known && grows) ? kVlrGreen : kVlrSub, ImVec2(120, 78));
                    // Scout action (Group C): spend one assignment to reveal the
                    // exact ceiling. Hidden for owned/already-scouted players.
                    if (!known && s.gm.user_team) {
                        ImGui::SameLine();
                        int sc = s.gm.scout_credits();
                        bool can = sc > 0;
                        if (!can) ImGui::BeginDisabled();
                        if (ImGui::Button("Scout", ImVec2(90, 78)))
                            s.gm.scout_player(p);
                        if (!can) ImGui::EndDisabled();
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Reveal exact potential\n(%d scouting assignment%s left)",
                                              sc, sc == 1 ? "" : "s");
                    }
                }
                // Form proxy: recent-vs-career rating delta (no dedicated
                // morale/form field on Player — avg_match_rating is the
                // honest signal available).
                {
                    double r = p->avg_match_rating();
                    const char* fl = r >= 1.10 ? "Hot" :
                                     r >= 0.95 ? "Steady" : "Cold";
                    ImGui::Text("Form: %s (%.2f avg)", fl, r);
                }
                ImGui::Text("Age: %d   Work Ethic: %d   Consistency: %d",
                            p->age, p->work_ethic, p->consistency);
                ImGui::Text("Contract: %dY left  -  $%dK/yr (exp %d)",
                            p->years_left(s.gm.year), p->contract.amount_k,
                            p->contract.exp_year);

                // === Make Offer (Group D) — bid for a contracted player ====
                {
                    auto ut = s.gm.user_team;
                    bool buyable = ut && p->team_name != "Free Agent" &&
                                   p->team_name != "Retired" && p->team_name != ut->name;
                    if (buyable) {
                        vlr::TeamPtr seller = find_team_by_name(s, p->team_name);
                        int asking = seller ? seller->sell_threshold_k(*p, s.gm.year) : 0;
                        bool open = s.offer_panel_open && s.offer_for == p.get();
                        ImGui::Spacing();
                        if (!open) {
                            if (ImGui::Button("Make Transfer Offer")) {
                                s.offer_panel_open  = true;
                                s.offer_for         = p.get();
                                s.offer_fee_choice  = std::max(asking, 10);
                                s.offer_result_code = -1;
                                s.offer_result_msg.clear();
                            }
                            ImGui::SameLine();
                            MutedText("%s value him at ~$%dK", p->team_name.c_str(), asking);
                        } else if (seller) {
                            if (BeginCard("##offer_panel", kBgDeep)) {
                            char sub[160];
                            std::snprintf(sub, sizeof(sub),
                                "%s value him at ~$%dK  |  he earns $%dK/yr",
                                p->team_name.c_str(), asking, p->contract.amount_k);
                            SectionHeader("MAKE AN OFFER", sub, kAccent);
                            ImGui::SetNextItemWidth(300);
                            ImGui::SliderInt("##offer_fee", &s.offer_fee_choice, 10,
                                             std::max(asking * 2, 200), "Bid: $%dK fee");
                            bool can_afford = ut->within_wage_envelope(
                                p->contract.amount_k, s.offer_fee_choice);
                            if (!can_afford) {
                                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                                ImGui::TextWrapped("Your wage envelope won't cover this fee + his wage right now.");
                                ImGui::PopStyleColor();
                            }
                            ImGui::Spacing();
                            if (ImGui::Button("Submit Bid", ImVec2(140, 0))) {
                                auto out = s.gm.user_transfer_bid(seller, p, s.offer_fee_choice, 0);
                                s.offer_result_code = (int)out.code;
                                s.offer_result_msg  = out.message;
                                if (out.code == vlr::GameManager::TransferBidOutcome::Accepted) {
                                    s.log.emplace_back(out.message);
                                    s.offer_panel_open = false;   // deal done
                                }
                            }
                            ImGui::SameLine();
                            if (ImGui::Button("Cancel", ImVec2(100, 0))) s.offer_panel_open = false;
                            if (s.offer_result_code >= 0 && !s.offer_result_msg.empty()) {
                                ImGui::Spacing();
                                bool ok = s.offer_result_code ==
                                          (int)vlr::GameManager::TransferBidOutcome::Accepted;
                                ImGui::PushStyleColor(ImGuiCol_Text, toV4(ok ? kVlrGreen : kVlrRed));
                                ImGui::TextWrapped("%s", s.offer_result_msg.c_str());
                                ImGui::PopStyleColor();
                            }
                            } EndCard();
                        }
                    }
                }
                ImGui::Spacing();

                H2("RANKED");
                ImGui::Text("MMR %d  (%s)  Peak %d", p->solo_mmr, p->rank_name().c_str(), p->peak_mmr);
                ImGui::Text("K/D %.2f  W %d  L %d", p->solo_kd(), p->solo_wins, p->solo_losses);
                ImGui::Spacing();

                H2("CAREER");
                ImGui::Text("Matches: %d", p->career_matches);
                ImGui::Text("K/D: %.2f", p->kd_ratio());
                ImGui::Text("Avg Rating: %.2f", p->avg_match_rating());
                ImGui::Text("KAST: %.1f%%", p->kast());

                ImGui::NextColumn();

                H2("ATTRIBUTES");
                for (std::size_t i = 0; i < vlr::kAttrCount; ++i) {
                    const char* nm = vlr::attr_name(static_cast<vlr::Attr>(i));
                    if (s.god_mode) {
                        DrawAttrSlider(nm, p->attributes[i]);
                    } else {
                        ImGui::Text("%-15s", nm);
                        ImGui::SameLine(170);
                        ImGui::ProgressBar(p->attributes[i] / 99.0f, ImVec2(180, 0), "");
                        ImGui::SameLine();
                        ImGui::Text("%d", p->attributes[i]);
                    }
                }

                ImGui::Columns(1);

                if (s.god_mode) {
                    // === C5: Negotiation dev-tools reveal ====================
                    // God-mode-only block surfacing Agent B's Desire enum +
                    // blurb so the user can see WHY a sign was refused or a
                    // re-sign succeeded. Read-only — Desire is generation-
                    // assigned and stable for the career.
                    ThinDivider(8);
                    SectionHeader("DEV TOOLS \xE2\x80\x94 NEGOTIATION",
                        "Drives gen_contract, refuses_to_negotiate, "
                        "and re-sign acceptance.", kVlrGold);
                    {
                        ImU32 dcol = kAccent;
                        switch (p->desire) {
                            case vlr::Desire::Greedy:          dcol = kVlrGold;   break;
                            case vlr::Desire::Loyal:           dcol = kInfo;      break;
                            case vlr::Desire::RingChaser:      dcol = kVlrRed;    break;
                            case vlr::Desire::Mercenary:       dcol = kRoleFlex;  break;
                            case vlr::Desire::StabilitySeeker: dcol = kVlrGreen;  break;
                            case vlr::Desire::Competitor:      dcol = kAccent;    break;
                            default:                           dcol = kAccent;    break;
                        }
                        Pill(vlr::desire_name(p->desire), dcol);
                        const char* db = vlr::desire_blurb(p->desire);
                        if (db && db[0]) MutedText("%s", db);
                    }
                    ThinDivider(8);
                    SectionHeader("ATTRIBUTE & CONTRACT OVERRIDES",
                        "Direct edits — every field bounds-clamped (Ctrl+Click "
                        "can't exceed the range; Salary obeys the legal cap).", kVlrGold);
                    ImGui::SameLine();
                    Pill("GOD MODE", kVlrGold);
                    // Clean 3-up grid (was magic SameLine(260/500) offsets). Every
                    // DragInt is AlwaysClamp so typed entry can't escape its range.
                    if (ImGui::BeginTable("##god_edit_grid", 3,
                            ImGuiTableFlags_SizingStretchSame)) {
                        auto cell = [&]() { ImGui::TableNextColumn(); ImGui::SetNextItemWidth(120); };
                        cell(); ImGui::DragInt("Age", &p->age, 1.0f, 14, 50, "%d", ImGuiSliderFlags_AlwaysClamp);
                        cell(); ImGui::DragInt("Potential", &p->potential, 1.0f, 1, 99, "%d", ImGuiSliderFlags_AlwaysClamp);
                        cell(); ImGui::DragInt("Work Ethic", &p->work_ethic, 1.0f, 1, 99, "%d", ImGuiSliderFlags_AlwaysClamp);
                        cell(); ImGui::DragInt("Consistency", &p->consistency, 1.0f, 1, 99, "%d", ImGuiSliderFlags_AlwaysClamp);
                        cell(); ImGui::DragInt("MMR", &p->solo_mmr, 5.0f, 1100, 5000, "%d", ImGuiSliderFlags_AlwaysClamp);
                        cell(); ImGui::DragInt("Salary $K", &p->contract.amount_k, 1.0f, vlr::kSalaryFloorK, vlr::kSalaryCapK, "%d", ImGuiSliderFlags_AlwaysClamp);
                        cell(); ImGui::DragInt("Signed Yrs", &p->contract_years, 1.0f, 1, 6, "%d", ImGuiSliderFlags_AlwaysClamp);
                        cell(); ImGui::DragInt("Exp Year", &p->contract.exp_year, 1.0f, s.gm.year, 2100, "%d", ImGuiSliderFlags_AlwaysClamp);
                        cell(); ImGui::DragInt("Peak Age", &p->peak_age, 1.0f, 18, 35, "%d", ImGuiSliderFlags_AlwaysClamp);
                        ImGui::EndTable();
                    }
                    // Cross-field validation: potential is a CEILING, never below
                    // the player's current OVR.
                    if (p->potential < static_cast<int>(p->ovr()))
                        p->potential = static_cast<int>(p->ovr());

                    int role_idx = static_cast<int>(p->primary_role);
                    const char* roles[] = {"Duelist", "Initiator", "Controller", "Sentinel"};
                    ImGui::SetNextItemWidth(140);
                    if (ImGui::Combo("Primary Role", &role_idx, roles, 4)) {
                        p->primary_role = static_cast<vlr::Role>(role_idx);
                    }
                    ImGui::SameLine(260);
                    int arch_idx = static_cast<int>(p->growth_archetype);
                    const char* archs[] = {"Standard", "Early Peaker", "Late Bloomer"};
                    ImGui::SetNextItemWidth(140);
                    if (ImGui::Combo("Growth Curve", &arch_idx, archs, 3)) {
                        p->growth_archetype = static_cast<vlr::GrowthArchetype>(arch_idx);
                    }
                    // Rookie archetype combo (god-mode reveal/edit). 17 entries
                    // (None + 16). Combo's labels generated from helper.
                    ImGui::SameLine(420);
                    int rk_idx = static_cast<int>(p->rookie_archetype);
                    static std::vector<const char*> rk_labels;
                    if (rk_labels.empty()) {
                        for (int i = 0; i <= 16; ++i) {
                            rk_labels.push_back(vlr::rookie_archetype_name(
                                static_cast<vlr::RookieArchetype>(i)));
                        }
                    }
                    ImGui::SetNextItemWidth(180);
                    if (ImGui::Combo("Rookie Archetype", &rk_idx,
                                     rk_labels.data(), (int)rk_labels.size())) {
                        p->rookie_archetype = static_cast<vlr::RookieArchetype>(rk_idx);
                    }
                    if (p->rookie_archetype != vlr::RookieArchetype::None) {
                        ImGui::PushFont(g_font_small);
                        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
                        ImGui::TextWrapped("Style: %s",
                            vlr::rookie_archetype_blurb(p->rookie_archetype));
                        ImGui::PopStyleColor();
                        ImGui::PopFont();
                    }
                    ImGui::Spacing();
                    if (ImGui::Button("Refresh agent pool")) p->update_agent_pool();

                    // Morale per team display + edit
                    ImGui::Spacing();
                    H2("MORALE / MOOD");
                    Sub("Per-team mood: 0 = happy, 1 = will refuse to negotiate.");
                    if (p->mood.per_team.empty()) {
                        ImGui::TextDisabled("(no team-specific mood yet)");
                    } else {
                        int mi = 0;
                        for (auto& kv : p->mood.per_team) {
                            ImGui::PushID(mi++);
                            float mv = (float)kv.second;
                            ImGui::SetNextItemWidth(180);
                            if (ImGui::SliderFloat(kv.first.c_str(), &mv, 0.0f, 1.0f)) {
                                kv.second = vlr::clamp_v((double)mv, 0.0, 1.0);
                            }
                            ImGui::PopID();
                        }
                    }

                    // IGL toggle with proper stat shift
                    ImGui::Spacing();
                    bool was_igl = p->is_igl;
                    if (ImGui::Checkbox("Is IGL", &p->is_igl)) {
                        if (p->is_igl && !was_igl) {
                            // Apply IGL shift
                            p->apply_attribute_delta(vlr::Attr::Aim, -8);
                            p->apply_attribute_delta(vlr::Attr::Headshot, -8);
                            p->apply_attribute_delta(vlr::Attr::Entry, -5);
                            p->apply_attribute_delta(vlr::Attr::Aggressiveness, -4);
                            p->apply_attribute_delta(vlr::Attr::Leadership, +14);
                            p->apply_attribute_delta(vlr::Attr::Intelligence, +10);
                            p->apply_attribute_delta(vlr::Attr::Communication, +10);
                            p->apply_attribute_delta(vlr::Attr::GameSense, +8);
                            p->apply_attribute_delta(vlr::Attr::DecisionMaking, +8);
                            p->apply_attribute_delta(vlr::Attr::MidRoundCalling, +12);
                            p->apply_attribute_delta(vlr::Attr::EconomyMgmt, +12);
                            p->apply_attribute_delta(vlr::Attr::AntiStrat, +10);
                        } else if (!p->is_igl && was_igl) {
                            // Revert IGL shift
                            p->apply_attribute_delta(vlr::Attr::Aim, +8);
                            p->apply_attribute_delta(vlr::Attr::Headshot, +8);
                            p->apply_attribute_delta(vlr::Attr::Entry, +5);
                            p->apply_attribute_delta(vlr::Attr::Aggressiveness, +4);
                            p->apply_attribute_delta(vlr::Attr::Leadership, -14);
                            p->apply_attribute_delta(vlr::Attr::Intelligence, -10);
                            p->apply_attribute_delta(vlr::Attr::Communication, -10);
                            p->apply_attribute_delta(vlr::Attr::GameSense, -8);
                            p->apply_attribute_delta(vlr::Attr::DecisionMaking, -8);
                            p->apply_attribute_delta(vlr::Attr::MidRoundCalling, -12);
                            p->apply_attribute_delta(vlr::Attr::EconomyMgmt, -12);
                            p->apply_attribute_delta(vlr::Attr::AntiStrat, -10);
                        }
                        p->update_agent_pool();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Force retire")) {
                        p->is_retired = true;
                        p->team_name = "Retired";
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Reset MMR to 1100")) p->solo_mmr = 1100;
                }

                ImGui::EndTabItem();
            }

            // === Detail (badges, awards, agent pool, history) ===
            if (ImGui::BeginTabItem("Detail")) {
                // === Personal info block ===
                H2("PERSONAL");
                ImGui::Columns(2, nullptr, false);
                ImGui::SetColumnWidth(0, 320);
                ImGui::Text("Real name:"); ImGui::SameLine(140);
                ImGui::Text("%s %s", p->first.c_str(), p->last.c_str());
                ImGui::Text("Born:"); ImGui::SameLine(140);
                if (p->birth_year > 0) {
                    ImGui::Text("%d  (age %d)", p->birth_year, p->age);
                } else {
                    ImGui::Text("age %d", p->age);
                }
                ImGui::Text("Hometown:"); ImGui::SameLine(140);
                ImGui::Text("%s, %s",
                            p->birth_city.empty() ? "?" : p->birth_city.c_str(),
                            p->country.empty() ? "?" : p->country.c_str());
                ImGui::Text("Country:"); ImGui::SameLine(140);
                CountryFlag(p->country_iso); ImGui::SameLine();
                ImGui::Text("%s", p->country.empty() ? "?" : p->country.c_str());
                ImGui::Text("Region:"); ImGui::SameLine(140);
                ImGui::Text("%s", p->region.c_str());
                ImGui::NextColumn();
                ImGui::Text("Status:"); ImGui::SameLine(140);
                if (p->is_igl) {
                    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                    ImGui::Text("[I] In-Game Leader");
                    ImGui::PopStyleColor();
                } else {
                    ImGui::Text("Player");
                }
                ImGui::Text("Role:"); ImGui::SameLine(140);
                ImGui::TextColored(toV4(role_color(p->primary_role)),
                                   "%s", vlr::role_name(p->primary_role));
                ImGui::Text("Team:"); ImGui::SameLine(140);
                ImGui::Text("%s", p->team_name.c_str());
                ImGui::Text("Contract:"); ImGui::SameLine(140);
                ImGui::Text("%dY left  $%dK/yr (exp %d)",
                            p->years_left(s.gm.year), p->contract.amount_k, p->contract.exp_year);
                ImGui::Columns(1);
                ImGui::Spacing();

                if (!p->agent_pool.empty()) {
                    H2("PRIMARY AGENT POOL");
                    for (auto* a : p->agent_pool) {
                        ImGui::SameLine();
                        ImGui::PushStyleColor(ImGuiCol_Button, toV4(role_color(a->role)));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toV4(role_color(a->role)));
                        ImGui::SmallButton(a->name.c_str());
                        ImGui::PopStyleColor(2);
                    }
                    ImGui::NewLine();
                }
                if (!p->badges.empty() || s.god_mode) {
                    H2("BADGES");
                    int bi = 0;
                    for (auto it = p->badges.begin(); it != p->badges.end(); ) {
                        ImGui::SameLine();
                        ImGui::PushID(bi++);
                        ImGui::PushStyleColor(ImGuiCol_Button, toV4(kVlrPanel2));
                        ImGui::SmallButton(it->c_str());
                        ImGui::PopStyleColor();
                        // God mode: right-click to remove the badge
                        bool removed = false;
                        if (s.god_mode && ImGui::BeginPopupContextItem("##badge_ctx")) {
                            if (ImGui::MenuItem("Remove badge")) {
                                it = p->badges.erase(it);
                                removed = true;
                            }
                            ImGui::EndPopup();
                        }
                        ImGui::PopID();
                        if (!removed) ++it;
                    }
                    if (s.god_mode) {
                        ImGui::NewLine();
                        ImGui::PushFont(g_font_small);
                        ImGui::TextDisabled("Right-click a badge to remove. Add new:");
                        ImGui::PopFont();
                        // Combo of all known badges; "Add" button appends.
                        static int s_badge_pick = 0;
                        const auto& BAD = vlr::badges();
                        std::vector<const char*> labels;
                        labels.reserve(BAD.size());
                        for (auto& b : BAD) labels.push_back(b.name.c_str());
                        if (s_badge_pick >= (int)labels.size()) s_badge_pick = 0;
                        ImGui::SetNextItemWidth(220);
                        ImGui::Combo("##add_badge_pick", &s_badge_pick,
                                     labels.data(), (int)labels.size());
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Add")) {
                            const auto& bd = BAD[(std::size_t)s_badge_pick];
                            // Don't double-add the same badge
                            bool exists = false;
                            for (auto& b : p->badges) if (b == bd.name) { exists = true; break; }
                            if (!exists) {
                                p->badges.push_back(bd.name);
                                for (auto& m : bd.mods)
                                    p->apply_attribute_delta(m.stat, m.delta);
                            }
                        }
                    }
                    ImGui::NewLine();
                }
                if (!p->awards.empty()) {
                    H2("AWARDS");
                    // === Trophy audit indicator =================================
                    // Sums [T]+[M]+[W] championship wins using the helper
                    // award_count_by_prefix (NOT raw awards.size(), which would
                    // also include [A] season awards). Surfaces total trophies
                    // at a glance so the user can spot anomalies (phantom
                    // championships, double-pinned awards, etc.).
                    int n_regional = p->award_count_by_prefix("[T] ");
                    int n_masters  = p->award_count_by_prefix("[M] ");
                    int n_world    = p->award_count_by_prefix("[W] ");
                    int n_trophies = n_regional + n_masters + n_world;

                    // Anomaly flag: >10 championships for a player under 25
                    // is unrealistic. Tint the totals line red so it screams.
                    bool age_anomaly = (n_trophies > 10 && p->age < 25);
                    ImU32 total_col = age_anomaly ? kVlrRed : kVlrSub;
                    ImGui::PushStyleColor(ImGuiCol_Text, toV4(total_col));
                    ImGui::Text("Total trophies: %d  (T:%d  M:%d  W:%d)",
                                n_trophies, n_regional, n_masters, n_world);
                    ImGui::PopStyleColor();

                    // === Optional integrity cross-check =========================
                    // Count how many currently-active tournaments list this
                    // player in their winning_roster_snapshot(). Compare against
                    // this season's [T]/[M]/[W] count for the player. Mismatch
                    // = either a phantom pin or a missing one — surface a small
                    // warning. By year-end active_tournaments may be empty, in
                    // which case skip (nothing to verify against).
                    int cur_year = s.gm.year;
                    int snap_hits = 0;
                    int snapshots_seen = 0;
                    for (auto& tour : s.gm.active_tournaments) {
                        if (!tour || !tour->finished()) continue;
                        const auto& snap = tour->winning_roster_snapshot();
                        if (snap.empty()) continue;
                        ++snapshots_seen;
                        for (vlr::Player* pp : snap) {
                            if (pp == p.get()) { ++snap_hits; break; }
                        }
                    }
                    if (snapshots_seen > 0) {
                        // Count this player's trophies stamped with the current
                        // year suffix — those are the ones an active tournament
                        // would still hold a snapshot for.
                        std::string ystr = std::to_string(cur_year);
                        int year_trophies = 0;
                        for (auto& a : p->awards) {
                            if (a.size() < 4) continue;
                            if (a[0] != '[' || a[2] != ']') continue;
                            char k = a[1];
                            if (k != 'T' && k != 'M' && k != 'W') continue;
                            if (a.size() >= ystr.size()
                                && a.compare(a.size() - ystr.size(),
                                             ystr.size(), ystr) == 0) {
                                ++year_trophies;
                            }
                        }
                        if (snap_hits != year_trophies) {
                            ImGui::SameLine();
                            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrRed));
                            ImGui::PushFont(g_font_small);
                            ImGui::Text("  (integrity check failed: snap=%d vs year=%d)",
                                        snap_hits, year_trophies);
                            ImGui::PopFont();
                            ImGui::PopStyleColor();
                        }
                    }

                    // Color-coded by category prefix:
                    //   [W] World/Champions  = gold
                    //   [M] Masters          = silver-grey
                    //   [T] Regional         = bronze (orange)
                    //   [A] Season Award     = green
                    //   default              = subtext
                    for (auto& a : p->awards) {
                        ImU32 col = kVlrSub;
                        const char* cat = "";
                        if (a.rfind("[W]", 0) == 0) {
                            col = kVlrGold; cat = "WORLD ";
                        } else if (a.rfind("[M]", 0) == 0) {
                            col = IM_COL32(0xC0, 0xC0, 0xC8, 0xFF); cat = "MASTERS ";
                        } else if (a.rfind("[T]", 0) == 0) {
                            col = IM_COL32(0xCD, 0x7F, 0x32, 0xFF); cat = "REGIONAL ";
                        } else if (a.rfind("[A]", 0) == 0) {
                            col = kVlrGreen; cat = "AWARD ";
                        }
                        ImGui::Bullet();
                        ImGui::PushStyleColor(ImGuiCol_Text, toV4(col));
                        ImGui::PushFont(g_font_small);
                        ImGui::TextUnformatted(cat);
                        ImGui::PopFont();
                        ImGui::PopStyleColor();
                        ImGui::SameLine();
                        // Strip the [X] prefix in the displayed text
                        std::string display = a;
                        if (display.size() > 3 && display[0] == '[' && display[2] == ']')
                            display = display.substr(4);
                        ImGui::Text("%s", display.c_str());
                    }
                }
                if (!p->history.empty()) {
                    H2("SEASON HISTORY");

                    // === Career arc (Group F): rating + OVR trend lines ====
                    // Rating is always stamped; OVR is only on entries recorded
                    // after the field was added, so legacy 0s are skipped.
                    {
                        std::vector<float> rat, ovrs;
                        rat.reserve(p->history.size());
                        for (auto& e : p->history) {
                            rat.push_back(static_cast<float>(e.rating));
                            if (e.ovr > 0) ovrs.push_back(static_cast<float>(e.ovr));
                        }
                        if (rat.size() >= 2) {
                            ImGui::PushStyleColor(ImGuiCol_PlotLines, toV4(kAccent));
                            ImGui::PlotLines("##rat_arc", rat.data(), static_cast<int>(rat.size()),
                                             0, "Rating by season", 0.6f, 1.6f, ImVec2(-FLT_MIN, 50));
                            ImGui::PopStyleColor();
                        }
                        if (ovrs.size() >= 2) {
                            ImGui::PushStyleColor(ImGuiCol_PlotLines, toV4(kVlrGreen));
                            ImGui::PlotLines("##ovr_arc", ovrs.data(), static_cast<int>(ovrs.size()),
                                             0, "OVR by season", FLT_MAX, FLT_MAX, ImVec2(-FLT_MIN, 50));
                            ImGui::PopStyleColor();
                        }
                        VSpace(4);
                    }

                    int total_earnings_k = 0;
                    if (ImGui::BeginTable("##hist", 7,
                            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
                        ImGui::TableSetupColumn("YEAR");
                        ImGui::TableSetupColumn("AGE");
                        ImGui::TableSetupColumn("TEAM");
                        ImGui::TableSetupColumn("RAT");
                        ImGui::TableSetupColumn("KD");
                        ImGui::TableSetupColumn("KAST");
                        ImGui::TableSetupColumn("EARNED");
                        ImGui::TableHeadersRow();
                        for (auto& e : p->history) {
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn(); ImGui::Text("%d", e.year);
                            ImGui::TableNextColumn(); ImGui::Text("%d", e.age);
                            ImGui::TableNextColumn(); ImGui::Text("%s", e.team.c_str());
                            ImGui::TableNextColumn(); ImGui::Text("%.2f", e.rating);
                            ImGui::TableNextColumn(); ImGui::Text("%.2f", e.kd);
                            ImGui::TableNextColumn(); ImGui::Text("%.1f", e.kast);
                            ImGui::TableNextColumn();
                            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGreen));
                            ImGui::Text("$%dK", e.salary_k);
                            ImGui::PopStyleColor();
                            total_earnings_k += e.salary_k;
                        }
                        // Career total footer row
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                        ImGui::Text("TOTAL");
                        ImGui::PopStyleColor();
                        ImGui::TableNextColumn(); ImGui::TextDisabled("-");
                        ImGui::TableNextColumn(); ImGui::TextDisabled("Career");
                        ImGui::TableNextColumn(); ImGui::TextDisabled("-");
                        ImGui::TableNextColumn(); ImGui::TextDisabled("-");
                        ImGui::TableNextColumn(); ImGui::TextDisabled("-");
                        ImGui::TableNextColumn();
                        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                        if (total_earnings_k >= 1000) {
                            ImGui::Text("$%.2fM", total_earnings_k / 1000.0);
                        } else {
                            ImGui::Text("$%dK", total_earnings_k);
                        }
                        ImGui::PopStyleColor();
                        ImGui::EndTable();
                    }
                }
                ImGui::EndTabItem();
            }

            // === Match History ===
            if (ImGui::BeginTabItem("Match History")) {
                ImGui::RadioButton("Pro Matches",  &s.history_filter, 0); ImGui::SameLine();
                ImGui::RadioButton("Solo Q",       &s.history_filter, 1);
                ImGui::Separator();

                auto& list = (s.history_filter == 0) ? p->pro_match_history : p->solo_match_history;
                ImGui::TextDisabled("Click any row to watch it round-by-round.");
                if (list.empty()) {
                    ImGui::TextDisabled("(no matches yet)");
                } else if (ImGui::BeginTable("##matches", 9,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                        ImGuiTableFlags_ScrollY, ImVec2(-FLT_MIN, 440))) {
                    ImGui::TableSetupColumn("EVENT", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("MAP");
                    ImGui::TableSetupColumn("MATCHUP");
                    ImGui::TableSetupColumn("SCORE");
                    ImGui::TableSetupColumn("W/L");
                    ImGui::TableSetupColumn("K-D-A");
                    ImGui::TableSetupColumn("HS%");
                    ImGui::TableSetupColumn("RATING");
                    ImGui::TableSetupColumn("MVP");
                    ImGui::TableHeadersRow();

                    // Capture which row was clicked this frame; act on it
                    // AFTER the table closes so we can safely close the
                    // outer popup before opening the live-match popup.
                    vlr::RecordedMatchPtr clicked_rec;
                    int i = 0;
                    for (auto& rec : list) {
                        if (!rec) continue;
                        ImGui::PushID(i++);
                        int my_k = 0, my_d = 0, my_a = 0;
                        double my_rating = 1.0;
                        if (rec->match_stats) {
                            auto sit = rec->match_stats->find(p.get());
                            if (sit != rec->match_stats->end()) {
                                my_k = sit->second.k;
                                my_d = sit->second.d;
                                my_a = sit->second.a;
                                my_rating = sit->second.rating;
                            }
                        }
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        bool clicked = TableRowSelectable(rec->event.c_str(), false,
                                ImGuiSelectableFlags_SpanAllColumns);
                        ImGui::TableNextColumn(); ImGui::Text("%s", rec->map_name.c_str());
                        ImGui::TableNextColumn();
                        {
                            char buf[80];
                            std::snprintf(buf, sizeof(buf), "%s vs %s",
                                          rec->blue_name.c_str(), rec->red_name.c_str());
                            ImGui::Text("%s", buf);
                        }
                        ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%s", rec->score.c_str()); ImGui::PopFont();
                        // W/L from THE PLAYER'S perspective at the time —
                        // use the recording's snapshotted blue/red rosters
                        // (history_record->blue_team / red_team) to figure
                        // out which side the player was actually on for
                        // this specific map. Rosters change over time, so
                        // checking by team_name string would be wrong.
                        ImGui::TableNextColumn();
                        {
                            bool was_blue = false, was_red = false;
                            if (rec->history_record) {
                                // blue_team / red_team are vector<Player*>
                                // (raw pointers), not shared_ptr.
                                for (auto* bp : rec->history_record->blue_team)
                                    if (bp == p.get()) { was_blue = true; break; }
                                if (!was_blue) {
                                    for (auto* rp : rec->history_record->red_team)
                                        if (rp == p.get()) { was_red = true; break; }
                                }
                            }
                            const char* result = "—";
                            ImU32 col = kVlrSub;
                            if (was_blue || was_red) {
                                bool won = was_blue
                                    ? (rec->blue_score > rec->red_score)
                                    : (rec->red_score  > rec->blue_score);
                                result = won ? "W" : "L";
                                col = won ? kVlrGreen : kVlrRed;
                            }
                            ImGui::PushStyleColor(ImGuiCol_Text, toV4(col));
                            ImGui::Text("%s", result);
                            ImGui::PopStyleColor();
                        }
                        ImGui::TableNextColumn(); ImGui::PushFont(g_font_mono); ImGui::Text("%d / %d / %d", my_k, my_d, my_a); ImGui::PopFont();
                        ImGui::TableNextColumn();
                        // Per-match HS% from PlayerLine.hs (stored in history_record).
                        // Note: NOT career_hs_pct() — that would show the cumulative
                        // value on every row, which defeats the point of a per-match
                        // breakdown.
                        {
                            int my_hs = -1;
                            if (rec->history_record) {
                                auto hit = rec->history_record->stats.find(p.get());
                                if (hit != rec->history_record->stats.end()) {
                                    my_hs = hit->second.hs;
                                }
                            }
                            if (my_hs >= 0) ImGui::Text("%d%%", my_hs);
                            else            ImGui::TextDisabled("—");
                        }
                        ImGui::TableNextColumn();
                        {
                            ImU32 col = my_rating >= 1.10 ? kVlrGreen : (my_rating >= 0.95 ? kVlrText : kVlrRed);
                            ImGui::PushStyleColor(ImGuiCol_Text, toV4(col));
                            ImGui::PushFont(g_font_mono);
                            ImGui::Text("%.2f", my_rating);
                            ImGui::PopFont();
                            ImGui::PopStyleColor();
                        }
                        ImGui::TableNextColumn(); ImGui::Text("%s", rec->mvp_name.c_str());
                        if (clicked) clicked_rec = rec;
                        ImGui::PopID();
                    }
                    ImGui::EndTable();

                    // Close the player profile modal now (after EndTable)
                    // so the live-match popup that we open next frame
                    // doesn't get stacked under us.
                    if (clicked_rec) {
                        s.show_player_modal = false;
                        ImGui::CloseCurrentPopup();
                        OpenReplay(s, clicked_rec);
                    }
                }
                ImGui::EndTabItem();
            }

            // === Map Mastery ===
            // Mirrors the agent-mastery flow but on the per-map dimension.
            // Comfort map emerges from sustained pro play; map_mastery_bonus
            // (±12) feeds per-map agent selection in Team::build_round_selection.
            if (ImGui::BeginTabItem("Map Mastery")) {
                ImGui::PushID("##mapmastery_tab");
                Sub("Per-map career stats. Comfort maps emerge from sustained "
                    "pro play. Decays on disuse.");
                ImGui::Spacing();

                // Comfort-map strip — gold accent if established, otherwise
                // "not yet established" hint.
                {
                    std::string comfort = p->comfort_map();
                    if (!comfort.empty()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                        ImGui::Text("[*] Comfort Map: %s", comfort.c_str());
                        ImGui::PopStyleColor();
                    } else {
                        Sub("Comfort map: not yet established (need 5+ matches "
                            "on a single map).");
                    }
                }
                ImGui::Spacing();

                if (p->map_mastery.empty()) {
                    ImGui::TextDisabled("No pro matches played yet.");
                } else {
                    struct MapRow {
                        std::string name;
                        int    matches = 0;
                        double avg_rating = 0.0;
                        double peak_rating = 0.0;
                        int    last_year = 0;
                        double bonus = 0.0;
                        bool   is_comfort = false;
                    };
                    std::string comfort = p->comfort_map();
                    std::vector<MapRow> mrows;
                    mrows.reserve(vlr::maps().size());
                    // Iterate ALL canonical maps so the user sees every map in
                    // the pool — even the ones with 0 matches show as "—".
                    for (auto& gm : vlr::maps()) {
                        MapRow r;
                        r.name = gm.name;
                        auto it = p->map_mastery.find(gm.name);
                        if (it != p->map_mastery.end()) {
                            r.matches = it->second.matches;
                            r.avg_rating = it->second.avg_rating;
                            r.peak_rating = it->second.peak_rating;
                            r.last_year = it->second.last_played_year;
                        }
                        r.bonus = p->map_mastery_bonus(gm.name);
                        r.is_comfort = !comfort.empty() && comfort == gm.name;
                        mrows.push_back(r);
                    }
                    // Sort by matches desc, then peak_rating desc as tiebreak.
                    std::sort(mrows.begin(), mrows.end(),
                        [](const MapRow& a, const MapRow& b) {
                            if (a.matches != b.matches) return a.matches > b.matches;
                            return a.peak_rating > b.peak_rating;
                        });

                    if (ImGui::BeginTable("##map_mastery", 7,
                            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                            ImGuiTableFlags_ScrollY, ImVec2(-FLT_MIN, 320))) {
                        ImGui::TableSetupColumn("#",       ImGuiTableColumnFlags_WidthFixed, 32);
                        ImGui::TableSetupColumn("MAP",     ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("MATCHES", ImGuiTableColumnFlags_WidthFixed, 80);
                        ImGui::TableSetupColumn("AVG RATING", ImGuiTableColumnFlags_WidthFixed, 90);
                        ImGui::TableSetupColumn("PEAK",    ImGuiTableColumnFlags_WidthFixed, 70);
                        ImGui::TableSetupColumn("LAST PLAYED", ImGuiTableColumnFlags_WidthFixed, 100);
                        ImGui::TableSetupColumn("BONUS",   ImGuiTableColumnFlags_WidthFixed, 70);
                        ImGui::TableHeadersRow();
                        int rank = 1;
                        for (auto& r : mrows) {
                            ImGui::TableNextRow();
                            if (r.is_comfort) ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));

                            ImGui::TableNextColumn();
                            if (!r.is_comfort) ImGui::PushStyleColor(ImGuiCol_Text, toV4(placement_color(rank)));
                            ImGui::Text("%d", rank);
                            if (!r.is_comfort) ImGui::PopStyleColor();
                            ++rank;

                            ImGui::TableNextColumn();
                            ImGui::Text("%s", r.name.c_str());

                            ImGui::TableNextColumn();
                            if (r.matches > 0) ImGui::Text("%d", r.matches);
                            else               ImGui::TextDisabled("—");

                            ImGui::TableNextColumn();
                            if (r.matches > 0) ImGui::Text("%.2f", r.avg_rating);
                            else               ImGui::TextDisabled("—");

                            ImGui::TableNextColumn();
                            if (r.peak_rating > 0.0) ImGui::Text("%.2f", r.peak_rating);
                            else                     ImGui::TextDisabled("—");

                            ImGui::TableNextColumn();
                            if (r.last_year > 0) ImGui::Text("%d", r.last_year);
                            else                 ImGui::TextDisabled("—");

                            ImGui::TableNextColumn();
                            if (r.bonus > 0.05 || r.bonus < -0.05)
                                ImGui::Text("%+.1f", r.bonus);
                            else
                                ImGui::TextDisabled("—");

                            if (r.is_comfort) ImGui::PopStyleColor();
                        }
                        ImGui::EndTable();
                    }
                    Sub("BONUS feeds per-map agent selection (+12 cap). Mastery "
                        "decays on disuse.");
                }

                // === God-mode map-mastery editor ===
                // Mirrors the agent-mastery editor — direct backfill of the
                // map_mastery table. Same gating, same buttons.
                if (s.god_mode) {
                    ImGui::Spacing();
                    H2("EDIT MAP MASTERY (God Mode)");
                    Sub("Backfill map mastery directly. Matches > 0 marks the "
                        "player as having played that map; avg_rating and "
                        "peak_rating shape the map_mastery_bonus contribution.");

                    if (ImGui::BeginTable("##gm_map_mastery", 4,
                            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                            ImGuiTableFlags_ScrollY, ImVec2(-FLT_MIN, 320))) {
                        ImGui::TableSetupColumn("MAP", ImGuiTableColumnFlags_WidthFixed, 130);
                        ImGui::TableSetupColumn("MATCHES",    ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("AVG RATING", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("PEAK",       ImGuiTableColumnFlags_WidthFixed, 90);
                        ImGui::TableHeadersRow();
                        int gmi = 0;
                        for (auto& gm : vlr::maps()) {
                            ImGui::PushID(gmi++);
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::Text("%s", gm.name.c_str());

                            auto& m = p->map_mastery[gm.name];
                            int    matches = m.matches;
                            float  avg     = static_cast<float>(m.avg_rating);
                            float  peak    = static_cast<float>(m.peak_rating);

                            ImGui::TableNextColumn();
                            ImGui::SetNextItemWidth(180);
                            if (ImGui::SliderInt("##mm_matches", &matches, 0, 300)) {
                                m.matches = matches;
                                if (matches == 0 && m.avg_rating == 0.0 && m.peak_rating == 0.0) {
                                    p->map_mastery.erase(gm.name);
                                } else if (m.last_played_year == 0) {
                                    m.last_played_year = vlr::current_world_year();
                                }
                            }

                            ImGui::TableNextColumn();
                            ImGui::SetNextItemWidth(180);
                            if (ImGui::SliderFloat("##mm_avg", &avg, 0.0f, 2.0f, "%.2f")) {
                                m.avg_rating = avg;
                                if (m.peak_rating < m.avg_rating) m.peak_rating = m.avg_rating;
                            }

                            ImGui::TableNextColumn();
                            ImGui::SetNextItemWidth(80);
                            if (ImGui::SliderFloat("##mm_peak", &peak, 0.0f, 2.5f, "%.2f")) {
                                m.peak_rating = peak;
                            }

                            ImGui::PopID();
                        }
                        ImGui::EndTable();
                    }
                    if (ImGui::Button("Refresh agent pool from attrs", ImVec2(260, 28))) {
                        p->update_agent_pool();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Clear all map mastery", ImVec2(200, 28))) {
                        p->map_mastery.clear();
                    }
                }
                ImGui::PopID();
                ImGui::EndTabItem();
            }

            // === Agents & Mastery ===
            // The new scoring system: weighted attribute fit (a1 0.45, a2 0.32,
            // a3 0.23) + role match + mastery experience. Pool is auto-derived
            // from this score; manual override removed since it desynced from
            // the simulation contract.
            if (ImGui::BeginTabItem("Agents & Mastery")) {
                Sub("Each agent has 3 signature attributes — the player's "
                    "values for those attributes drive FIT. MASTERY is built "
                    "by playing pro matches on that agent (capped, decays on "
                    "disuse). SCORE = weighted FIT + role match + mastery, "
                    "and the top scorers fill the player's agent pool.");
                ImGui::Spacing();

                std::string sig = p->signature_agent();

                // Pool summary strip + signature
                {
                    std::string pool_str = "Pool: ";
                    if (p->agent_pool.empty()) {
                        pool_str += "(empty)";
                    } else {
                        for (std::size_t i = 0; i < p->agent_pool.size(); ++i) {
                            if (i) pool_str += ", ";
                            pool_str += p->agent_pool[i]->name;
                        }
                    }
                    ImGui::TextUnformatted(pool_str.c_str());
                    if (!sig.empty()) {
                        auto sit = p->agent_mastery.find(sig);
                        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                        if (sit != p->agent_mastery.end()) {
                            ImGui::Text("[*] Signature: %s  (%d matches, %.2f avg, peak %.2f)",
                                sig.c_str(), sit->second.matches,
                                sit->second.avg_rating, sit->second.peak_rating);
                        } else {
                            ImGui::Text("[*] Signature: %s", sig.c_str());
                        }
                        ImGui::PopStyleColor();
                    } else {
                        Sub("Signature agent: not yet established (need 5+ pro "
                            "matches on the same agent).");
                    }
                }
                ImGui::Spacing();

                struct Row {
                    const vlr::Agent* agent;
                    double fit;        // weighted attribute fit, 0..99
                    double mastery;    // mastery_bonus_for (0..12)
                    double score;      // full update_agent_pool score
                    int    matches;
                    double avg_rating;
                    double peak_rating;
                    int    last_year;
                    bool   in_pool;
                };
                std::vector<Row> rows;
                rows.reserve(vlr::agents().size());
                for (auto& a : vlr::agents()) {
                    Row r;
                    r.agent = &a;
                    int v1 = vlr::at(p->attributes, a.a1);
                    int v2 = vlr::at(p->attributes, a.a2);
                    int v3 = vlr::at(p->attributes, a.a3);
                    r.fit = v1 * 0.45 + v2 * 0.32 + v3 * 0.23;
                    r.mastery = p->mastery_bonus_for(a.name);
                    r.score = r.fit + (a.role == p->primary_role ? 6.0 : 0.0) + r.mastery;
                    auto mit = p->agent_mastery.find(a.name);
                    if (mit != p->agent_mastery.end()) {
                        r.matches = mit->second.matches;
                        r.avg_rating = mit->second.avg_rating;
                        r.peak_rating = mit->second.peak_rating;
                        r.last_year = mit->second.last_played_year;
                    } else {
                        r.matches = 0; r.avg_rating = 0.0; r.peak_rating = 0.0; r.last_year = 0;
                    }
                    r.in_pool = false;
                    for (auto* ap : p->agent_pool) if (ap == &a) { r.in_pool = true; break; }
                    rows.push_back(r);
                }
                std::sort(rows.begin(), rows.end(),
                          [](const Row& x, const Row& y) { return x.score > y.score; });

                if (ImGui::BeginTable("##agents_mastery", 9,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                        ImGuiTableFlags_ScrollY, ImVec2(-FLT_MIN, 380))) {
                    ImGui::TableSetupColumn("#",       ImGuiTableColumnFlags_WidthFixed, 32);
                    ImGui::TableSetupColumn("AGENT",   ImGuiTableColumnFlags_WidthFixed, 96);
                    ImGui::TableSetupColumn("ROLE",    ImGuiTableColumnFlags_WidthFixed, 80);
                    ImGui::TableSetupColumn("KEY ATTRS", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("FIT",     ImGuiTableColumnFlags_WidthFixed, 50);
                    ImGui::TableSetupColumn("MAT",     ImGuiTableColumnFlags_WidthFixed, 50);
                    ImGui::TableSetupColumn("AVG",     ImGuiTableColumnFlags_WidthFixed, 60);
                    ImGui::TableSetupColumn("LAST",    ImGuiTableColumnFlags_WidthFixed, 60);
                    ImGui::TableSetupColumn("SCORE",   ImGuiTableColumnFlags_WidthFixed, 80);
                    ImGui::TableHeadersRow();
                    int rank = 1;
                    for (auto& r : rows) {
                        ImGui::TableNextRow();
                        bool is_sig = !sig.empty() && sig == r.agent->name;
                        if (is_sig) ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));

                        ImGui::TableNextColumn();
                        if (!is_sig) ImGui::PushStyleColor(ImGuiCol_Text, toV4(placement_color(rank)));
                        ImGui::PushFont(g_font_mono);
                        if (r.in_pool) ImGui::Text("[%d]", rank);
                        else           ImGui::Text(" %d ", rank);
                        ImGui::PopFont();
                        if (!is_sig) ImGui::PopStyleColor();
                        ++rank;

                        ImGui::TableNextColumn();
                        if (!is_sig) ImGui::PushStyleColor(ImGuiCol_Text, toV4(role_color(r.agent->role)));
                        ImGui::Text("%s", r.agent->name.c_str());
                        if (!is_sig) ImGui::PopStyleColor();

                        ImGui::TableNextColumn();
                        ImGui::Text("%s", vlr::role_name(r.agent->role));

                        ImGui::TableNextColumn();
                        // Compact "Aim:88 HS:81 React:78" — abbreviates the
                        // most common long names so the column doesn't wrap.
                        auto abbrev_attr = [](vlr::Attr a) -> const char* {
                            const char* nm = vlr::attr_name(a);
                            // Common shortenings.
                            if (std::string_view(nm) == "Headshot")           return "HS";
                            if (std::string_view(nm) == "DecisionMaking")     return "Decision";
                            if (std::string_view(nm) == "Aggressiveness")     return "Aggro";
                            if (std::string_view(nm) == "Communication")      return "Comm";
                            if (std::string_view(nm) == "Adaptability")       return "Adapt";
                            if (std::string_view(nm) == "Positioning")        return "Pos";
                            if (std::string_view(nm) == "GameSense")          return "GS";
                            if (std::string_view(nm) == "Intelligence")       return "Intel";
                            if (std::string_view(nm) == "MidRoundCalling")    return "MidCall";
                            if (std::string_view(nm) == "CrosshairPlacement") return "Cross";
                            if (std::string_view(nm) == "EconomyMgmt")        return "Econ";
                            if (std::string_view(nm) == "AntiStrat")          return "AntiS";
                            if (std::string_view(nm) == "SpikeHandle")        return "Spike";
                            if (std::string_view(nm) == "Reaction")           return "React";
                            if (std::string_view(nm) == "Movement")           return "Move";
                            if (std::string_view(nm) == "Leadership")         return "Lead";
                            return nm;
                        };
                        char attrs_buf[128];
                        std::snprintf(attrs_buf, sizeof(attrs_buf),
                            "%s:%d  %s:%d  %s:%d",
                            abbrev_attr(r.agent->a1), vlr::at(p->attributes, r.agent->a1),
                            abbrev_attr(r.agent->a2), vlr::at(p->attributes, r.agent->a2),
                            abbrev_attr(r.agent->a3), vlr::at(p->attributes, r.agent->a3));
                        ImGui::TextUnformatted(attrs_buf);

                        ImGui::TableNextColumn();
                        ImGui::Text("%.0f", r.fit);

                        ImGui::TableNextColumn();
                        if (r.matches > 0) ImGui::Text("%d", r.matches);
                        else               ImGui::TextDisabled("—");

                        ImGui::TableNextColumn();
                        if (r.matches > 0) ImGui::Text("%.2f", r.avg_rating);
                        else               ImGui::TextDisabled("—");

                        ImGui::TableNextColumn();
                        if (r.last_year > 0) ImGui::Text("%d", r.last_year);
                        else                 ImGui::TextDisabled("—");

                        ImGui::TableNextColumn();
                        ImGui::Text("%.1f", r.score);

                        if (is_sig) ImGui::PopStyleColor();
                    }
                    ImGui::EndTable();
                }

                Sub("[N] in the # column = currently in agent pool. Mastery "
                    "decays 15%%/yr on disuse and prunes after 4 unused years.");

                // === God-mode mastery editor + pool refresh ===
                if (s.god_mode) {
                    ImGui::Spacing();
                    H2("EDIT MASTERY (God Mode)");
                    Sub("Backfill agent mastery directly. Matches > 0 marks the "
                        "player as having played that agent; avg_rating and "
                        "peak_rating shape the mastery bonus contribution.");

                    if (ImGui::BeginTable("##gm_mastery", 5,
                            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                            ImGuiTableFlags_ScrollY, ImVec2(-FLT_MIN, 320))) {
                        ImGui::TableSetupColumn("AGENT", ImGuiTableColumnFlags_WidthFixed, 110);
                        ImGui::TableSetupColumn("ROLE",  ImGuiTableColumnFlags_WidthFixed, 80);
                        ImGui::TableSetupColumn("MATCHES", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("AVG RATING", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("PEAK", ImGuiTableColumnFlags_WidthFixed, 90);
                        ImGui::TableHeadersRow();
                        int gi = 0;
                        for (auto& a : vlr::agents()) {
                            ImGui::PushID(gi++);
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::PushStyleColor(ImGuiCol_Text, toV4(role_color(a.role)));
                            ImGui::Text("%s", a.name.c_str());
                            ImGui::PopStyleColor();

                            ImGui::TableNextColumn();
                            ImGui::Text("%s", vlr::role_name(a.role));

                            auto& m = p->agent_mastery[a.name];
                            int matches = m.matches;
                            float avg = static_cast<float>(m.avg_rating);
                            float peak = static_cast<float>(m.peak_rating);

                            ImGui::TableNextColumn();
                            ImGui::SetNextItemWidth(180);
                            if (ImGui::SliderInt("##matches", &matches, 0, 300)) {
                                m.matches = matches;
                                if (matches == 0 && m.avg_rating == 0.0 && m.peak_rating == 0.0) {
                                    p->agent_mastery.erase(a.name);
                                } else if (m.last_played_year == 0) {
                                    m.last_played_year = vlr::current_world_year();
                                }
                            }

                            ImGui::TableNextColumn();
                            ImGui::SetNextItemWidth(180);
                            if (ImGui::SliderFloat("##avg", &avg, 0.0f, 2.0f, "%.2f")) {
                                m.avg_rating = avg;
                                if (m.peak_rating < m.avg_rating) m.peak_rating = m.avg_rating;
                            }

                            ImGui::TableNextColumn();
                            ImGui::SetNextItemWidth(80);
                            if (ImGui::SliderFloat("##peak", &peak, 0.0f, 2.5f, "%.2f")) {
                                m.peak_rating = peak;
                            }

                            ImGui::PopID();
                        }
                        ImGui::EndTable();
                    }
                    if (ImGui::Button("Refresh agent pool from attrs", ImVec2(260, 28))) {
                        p->update_agent_pool();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Clear all mastery", ImVec2(180, 28))) {
                        p->agent_mastery.clear();
                        p->update_agent_pool();
                    }
                }
                ImGui::EndTabItem();
            }

            // === IGL tab (only meaningful for IGL-flagged players) ===
            if (p->is_igl && ImGui::BeginTabItem("IGL Profile")) {
                ImGui::PushFont(g_font_h2);
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                ImGui::TextUnformatted("[I]  In-Game Leader");
                ImGui::PopStyleColor();
                ImGui::PopFont();
                Sub("IGLs trade fragging power for mental ceiling. They call "
                    "the shots mid-round, manage the team economy, and read "
                    "opponent tendencies. The hidden tendencies below shape "
                    "how this IGL likes to run a system.");
                ImGui::Spacing();

                bool god = s.god_mode;

                ImGui::Columns(2, nullptr, false);
                H2("LEADERSHIP STATS");
                auto stat_bar = [&](const char* lbl, vlr::Attr a) {
                    if (god) {
                        int& vr = vlr::at(p->attributes, a);
                        ImGui::PushID(lbl);
                        ImGui::AlignTextToFramePadding();
                        ImGui::Text("%-20s", lbl);
                        ImGui::SameLine(220);
                        ImGui::SetNextItemWidth(240);
                        ImGui::SliderInt("##gs", &vr, 1, 99);
                        ImGui::PopID();
                    } else {
                        int v = vlr::at(p->attributes, a);
                        ImGui::Text("%-20s", lbl);
                        ImGui::SameLine(220);
                        ImGui::ProgressBar(v / 99.0f, ImVec2(220, 0), "");
                        ImGui::SameLine();
                        ImGui::Text("%d", v);
                    }
                };
                stat_bar("Mid-round Calling",  vlr::Attr::MidRoundCalling);
                stat_bar("Economy Management", vlr::Attr::EconomyMgmt);
                stat_bar("Anti-Strat",         vlr::Attr::AntiStrat);
                stat_bar("Leadership",         vlr::Attr::Leadership);
                stat_bar("Communication",      vlr::Attr::Communication);
                stat_bar("Game Sense",         vlr::Attr::GameSense);
                stat_bar("Decision Making",    vlr::Attr::DecisionMaking);
                stat_bar("Intelligence",       vlr::Attr::Intelligence);
                if (god) {
                    stat_bar("Adaptability",   vlr::Attr::Adaptability);
                }

                ImGui::NextColumn();
                H2("TENDENCIES");
                Sub("Sliders are bidirectional — show preferred style.");
                ImGui::Spacing();

                auto tendency = [&](const char* low, const char* high, int& vr) {
                    ImGui::PushFont(g_font_small);
                    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
                    ImGui::Text("%-15s", low);
                    ImGui::PopStyleColor();
                    ImGui::SameLine(160);
                    ImGui::Text("%s", high);
                    ImGui::PopFont();
                    if (god) {
                        ImGui::PushID(low);
                        ImGui::SetNextItemWidth(-FLT_MIN);
                        ImGui::SliderInt("##gt", &vr, 0, 100);
                        ImGui::PopID();
                    } else {
                        ImGui::ProgressBar(vr / 100.0f, ImVec2(-FLT_MIN, 0), "");
                    }
                };
                tendency("Passive",       "Aggressive system",   p->tend_play_aggressive);
                tendency("Lurk-heavy",    "Default-execute",     p->tend_lurk_vs_execute);
                tendency("Quiet leader",  "Vocal leader",        p->tend_vocal);
                tendency("Structured",    "Adaptive",            p->tend_adaptive);

                ImGui::Columns(1);

                // === IGL IMPACT — strategic contribution counters =========
                // These are populated by GameManager's MVP/IGL-impact overhaul
                // (parallel agent B). Wrapped in PushID so the section's own
                // child widgets never collide with surrounding tab IDs.
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::PushID("igl_impact");
                H2("IGL IMPACT");
                if (p->igl_match_count == 0) {
                    ImGui::TextDisabled("No pro matches played yet.");
                } else {
                    ImGui::Text("SEASON IMPACT: %.2f  (peak %.2f)",
                                p->igl_impact_season, p->igl_impact_season_peak);
                    ImGui::Text("CAREER IMPACT: %.2f across %d matches",
                                p->igl_impact_total, p->igl_match_count);
                    double avg_per_match = p->igl_impact_total
                        / static_cast<double>(p->igl_match_count > 0
                                              ? p->igl_match_count : 1);
                    ImGui::Text("Average impact per match: %.3f", avg_per_match);
                    ImGui::Text("PRESSURE MATCHES THIS SEASON: %d",
                                p->season_pressure_matches);
                }
                Sub("IGL Impact tracks strategic contribution (mid-round calls, "
                    "anti-strat, adaptation, comeback creation) independent of "
                    "frag stats. Drives IGL of the Year, MVP race, and contract "
                    "value.");

                if (god) {
                    ImGui::Spacing();
                    SectionHeader("DEV TOOLS \xE2\x80\x94 IGL", nullptr, kVlrGold);
                    ImGui::Spacing();
                    {
                        float impt = static_cast<float>(p->igl_impact_total);
                        ImGui::SetNextItemWidth(220);
                        if (ImGui::DragFloat("Career Impact (total)", &impt,
                                             0.05f, 0.0f, 500.0f, "%.2f",
                                             ImGuiSliderFlags_AlwaysClamp)) {
                            p->igl_impact_total = impt;
                        }
                        float imps = static_cast<float>(p->igl_impact_season);
                        ImGui::SetNextItemWidth(220);
                        if (ImGui::DragFloat("Season Impact", &imps,
                                             0.05f, 0.0f, 60.0f, "%.2f",
                                             ImGuiSliderFlags_AlwaysClamp)) {
                            p->igl_impact_season = imps;
                        }
                        float impp = static_cast<float>(p->igl_impact_season_peak);
                        ImGui::SetNextItemWidth(220);
                        if (ImGui::DragFloat("Season Peak Impact", &impp,
                                             0.05f, 0.0f, 60.0f, "%.2f",
                                             ImGuiSliderFlags_AlwaysClamp)) {
                            p->igl_impact_season_peak = impp;
                        }
                        ImGui::SetNextItemWidth(220);
                        ImGui::InputInt("IGL Match Count", &p->igl_match_count);
                        if (p->igl_match_count < 0) p->igl_match_count = 0;
                        ImGui::SetNextItemWidth(220);
                        ImGui::InputInt("Pressure Matches (season)",
                                        &p->season_pressure_matches);
                        if (p->season_pressure_matches < 0)
                            p->season_pressure_matches = 0;
                        // NOTE: Is-IGL toggle intentionally LIVES on the
                        // Overview tab (god-mode block). Putting it here
                        // produced a catch-22 — IGL Profile only opens when
                        // is_igl is already true, so promoting a non-IGL
                        // required first being an IGL. Toggle from Overview
                        // instead; that path runs the proper attribute
                        // shift (pitfall 4), this raw assignment did not.
                        MutedText("God Mode: raw edits, no clamping beyond valid "
                                  "ranges. Attribute & tendency sliders above are "
                                  "live-editable. (Promote/demote IGL on the "
                                  "Overview tab — it applies the correct "
                                  "attribute shift.)");
                    }
                }
                ImGui::PopID();

                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        ImGui::Separator();
        if (ImGui::Button("Close", ImVec2(120, 32))) {
            s.show_player_modal = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}
