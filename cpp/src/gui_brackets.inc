// Tournament bracket UI: BracketCardLayout, connectors, matchup cards, DrawBrackets (extracted from gui_main.cpp).
// Unity-build fragment of gui_main.cpp: this file is #included inside the
// same anonymous namespace; it is NOT a standalone translation unit.
// Screen-space rect produced by drawing a card. Used by connector lines.
struct BracketCardLayout {
    ImVec2 tl;
    ImVec2 br;
    bool   is_winner_a = false;
    bool   is_winner_b = false;
    bool   played      = false;
    vlr::TeamPtr winner;
};

// Draw a single bracket card.
//   - 2-row layout: team A (top) / team B (bottom)
//   - Winner row tinted with team primary color + bold white tag/name + gold score
//   - Loser row dimmed
//   - Unplayed match: both rows at mid-tier opacity, no scores
//   - BO5 chip drawn in top-right corner when best_of >= 5
BracketCardLayout DrawBracketCardAt(AppState& s, ImVec2 origin,
                                     const vlr::TeamPtr& a, const vlr::TeamPtr& b,
                                     const vlr::TeamPtr& winner,
                                     int a_score, int b_score, bool played,
                                     int best_of,
                                     bool is_lower, bool highlight,
                                     const std::string& event_name,
                                     int unique_id) {
    using namespace bracket_ui;
    BracketCardLayout out;
    out.tl = origin;
    out.br = ImVec2(origin.x + kCardW, origin.y + kCardH);
    out.played = played;
    out.winner = winner;
    out.is_winner_a = (winner && winner == a);
    out.is_winner_b = (winner && winner == b);

    auto* dl = ImGui::GetWindowDrawList();
    ImU32 bg     = played ? kCardBg : kCardBgUnplayed;
    ImU32 border = highlight ? kCardBorderLive : kCardBorder;
    float border_thick = highlight ? 2.0f : 1.0f;
    dl->AddRectFilled(out.tl, out.br, bg, 4.0f);
    dl->AddRect(out.tl, out.br, border, 4.0f, 0, border_thick);

    auto draw_team_row = [&](const vlr::TeamPtr& t, float row_y, bool is_winner,
                              bool is_loser, int score) {
        ImVec2 row_br(out.br.x, row_y + kCardH * 0.5f);
        if (!t) {
            dl->AddText(ImVec2(out.tl.x + 12, row_y + 6),
                        kTextDim, "(TBD)");
            return;
        }
        ImU32 tc = IM_COL32(t->color_primary_r, t->color_primary_g, t->color_primary_b, 0xFF);
        // Team color stripe down the left side of this row.
        dl->AddRectFilled(ImVec2(out.tl.x + 1, row_y + 1),
                          ImVec2(out.tl.x + 1 + kStripeW, row_br.y - 1), tc);
        if (is_winner) {
            // Subtle tinted bg behind winner row.
            dl->AddRectFilled(ImVec2(out.tl.x + 1 + kStripeW, row_y + 1),
                              ImVec2(out.br.x - 1, row_br.y - 1), kRowWin);
        }
        std::string tag = t->tag.empty() ? std::string("???") : t->tag;
        ImU32 row_text_col;
        if (!played)          row_text_col = kTextMid;
        else if (is_winner)   row_text_col = kTextWhite;
        else                  row_text_col = kTextDim;
        dl->AddText(ImVec2(out.tl.x + 12, row_y + 8), row_text_col, tag.c_str());
        // Truncated team name (after tag).
        std::string nm = t->name;
        if (nm.size() > 13) nm = nm.substr(0, 11) + "..";
        dl->AddText(ImVec2(out.tl.x + 50, row_y + 8), row_text_col, nm.c_str());
        // Right-aligned score for played matches.
        if (played) {
            char sb[8]; std::snprintf(sb, sizeof(sb), "%d", score);
            ImVec2 sz = ImGui::CalcTextSize(sb);
            ImU32 score_col = is_winner ? kVlrGold : kTextDim;
            dl->AddText(ImVec2(out.br.x - sz.x - 10, row_y + 8), score_col, sb);
        }
    };

    bool a_loser = played && winner && winner != a;
    bool b_loser = played && winner && winner != b;
    draw_team_row(a, out.tl.y,                   out.is_winner_a, a_loser, a_score);
    draw_team_row(b, out.tl.y + kCardH * 0.5f,   out.is_winner_b, b_loser, b_score);
    // Center divider line between the two team rows.
    dl->AddLine(ImVec2(out.tl.x + 1 + kStripeW, out.tl.y + kCardH * 0.5f),
                ImVec2(out.br.x - 1,            out.tl.y + kCardH * 0.5f),
                kCardDivider, 1.0f);

    // BO5 chip — drawn last so it floats on top. Structural info, not a
    // live-match spoiler (per §8 pitfall 31 — bracket cards may surface
    // best_of since it's match metadata, not in-progress round state).
    if (best_of >= 5) {
        ImVec2 chip_br(out.br.x - 4, out.tl.y + 14);
        ImVec2 chip_tl(chip_br.x - 28, chip_br.y - 12);
        dl->AddRectFilled(chip_tl, chip_br, kVlrGold, 2.0f);
        dl->AddText(ImVec2(chip_tl.x + 4, chip_tl.y - 1),
                    IM_COL32(0x10, 0x10, 0x10, 0xFF), "BO5");
    }
    (void)is_lower;  // borders are uniform now; LB is distinguished by panel header

    // Invisible click target overlay.
    ImGui::PushID(unique_id);
    ImGui::SetCursorScreenPos(ImVec2(out.tl.x, out.tl.y));
    if (ImGui::InvisibleButton("##bcardclk", ImVec2(kCardW, kCardH))) {
        if (a && b && !played) {
            // Use the per-match best_of stamped by the Tournament at
            // scheduling time (Agent A contract). Fallback to 3 if it's 0.
            int bo = (best_of > 0) ? best_of : 3;
            OpenLiveMatchSeries(s, a, b, event_name, bo);
        } else if (winner) {
            OpenTeamProfile(s, winner);
        } else if (a) {
            OpenTeamProfile(s, a);
        }
    }
    if (ImGui::IsItemHovered()) {
        if (a && b) {
            if (played && winner) {
                ImGui::SetTooltip("%s %d - %d %s\nWinner: %s\n(click for team profile)",
                                  a->name.c_str(), a_score, b_score, b->name.c_str(),
                                  winner->name.c_str());
            } else {
                ImGui::SetTooltip("%s vs %s%s\n(click to Watch live)",
                                  a->name.c_str(), b->name.c_str(),
                                  best_of >= 5 ? "  [BO5]" : "");
            }
        }
    }
    ImGui::PopID();
    return out;
}

// Stepped H-V-H connector line between two cards. Routes from the right
// edge of `from` to the left edge of `to`. Colored by whether the source
// match was played (white-ish) or not (muted gray).
void DrawBracketConnector(ImDrawList* dl, const BracketCardLayout& from,
                           const BracketCardLayout& to) {
    float fx = from.br.x;
    float fy = (from.tl.y + from.br.y) * 0.5f;
    float tx = to.tl.x;
    float ty = (to.tl.y + to.br.y) * 0.5f;
    float midx = fx + (tx - fx) * 0.5f;
    // Played connectors that resolved a winner subtly carry the accent so
    // the eye can trace the advancing path; unplayed stays muted.
    bool advanced = from.played && (bool)from.winner;
    float thick = advanced ? 2.2f : 1.6f;
    ImU32 col = !from.played ? bracket_ui::kLineUnplayed
              : advanced     ? kAccent
                             : bracket_ui::kLinePlayed;
    // Smooth H-V-H route: three segments with a small filled dot at each
    // bend so the corners read as rounded joints rather than hard kinks.
    dl->AddLine(ImVec2(fx, fy),   ImVec2(midx, fy), col, thick);
    dl->AddLine(ImVec2(midx, fy), ImVec2(midx, ty), col, thick);
    dl->AddLine(ImVec2(midx, ty), ImVec2(tx, ty),   col, thick);
    dl->AddCircleFilled(ImVec2(midx, fy), thick * 0.9f, col);
    dl->AddCircleFilled(ImVec2(midx, ty), thick * 0.9f, col);
}

// Slot record for one match in the rendered grid. Carries enough info to
// draw the card and route the click handler — best_of is stamped at
// scheduling time by the Tournament (Agent A contract on BracketMatch).
struct RoundSlot {
    vlr::TeamPtr a, b, winner;
    int  a_score   = 0;
    int  b_score   = 0;
    int  best_of   = 3;
    bool played    = false;
    bool placeholder = false;       // future round, no teams known yet
};

// Resolve a per-column label using a passed-in label list (one per
// round). Empty entry → fall back to a count-based generic. total_rounds
// is the total number of columns in this panel — needed so the LB fallback
// can distinguish "LB Semifinal" (1-card round NOT at the end) from
// "LB Final" (1-card round at the end), which both have teams_in_round==1.
static std::string round_label_at(const std::vector<std::string>& labels,
                                   std::size_t round_idx,
                                   std::size_t teams_in_round,
                                   bool is_lower,
                                   std::size_t total_rounds = 0) {
    if (round_idx < labels.size() && !labels[round_idx].empty()) {
        return labels[round_idx];
    }
    int remaining = static_cast<int>(teams_in_round) * 2;
    if (is_lower) {
        bool is_last = (total_rounds > 0)
            && (round_idx + 1 == total_rounds);
        if (teams_in_round <= 1 && is_last) return "LB FINAL";
        if (teams_in_round <= 1)            return "LB Semifinal";
        if (teams_in_round == 2)            return "LB R" +
            std::to_string(round_idx + 1);
        return "LB R" + std::to_string(round_idx + 1);
    }
    if (remaining == 16) return "UB R1";
    if (remaining == 8)  return "UB Quarterfinals";
    if (remaining == 4)  return "UB Semifinals";
    if (remaining == 2)  return "UB Final";
    return "Round " + std::to_string(round_idx + 1);
}

// Render the UB or LB rounds as a horizontal sequence of columns. Returns
// the bottom-Y of the lowest card drawn so the caller can place the next
// section beneath it.
float DrawBracketRounds(AppState& s, const std::vector<std::vector<RoundSlot>>& rounds,
                        const std::vector<std::string>& round_labels,
                        ImVec2 origin, bool is_lower,
                        const std::string& event_name, int id_base,
                        std::vector<BracketCardLayout>* out_finals = nullptr) {
    using namespace bracket_ui;
    auto* dl = ImGui::GetWindowDrawList();
    float content_y0 = origin.y + kRoundLabelH + 4.0f;

    if (rounds.empty()) return content_y0;

    std::vector<std::vector<BracketCardLayout>> rendered;
    rendered.resize(rounds.size());

    for (std::size_t r = 0; r < rounds.size(); ++r) {
        auto& slots = rounds[r];
        float col_x = origin.x + static_cast<float>(r) * (kCardW + kColGap);

        // Column header — H2 font, muted gold for UB, red for LB Final,
        // soft slate for everything else.
        std::string lbl = round_label_at(round_labels, r, slots.size(),
                                          is_lower, rounds.size());
        bool is_lb_final = is_lower && (r + 1 == rounds.size()) && slots.size() == 1;
        ImU32 lbl_col = is_lb_final ? kVlrGold
                       : (is_lower ? IM_COL32(0xE0, 0x9C, 0x9C, 0xFF)
                                   : IM_COL32(0xE3, 0xC5, 0x6D, 0xFF));
        if (is_lb_final) {
            std::string upper = lbl;
            for (auto& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            // Append "(BO5)" suffix per design spec
            if (upper.find("BO5") == std::string::npos) upper += " (BO5)";
            lbl = upper;
        }
        // SectionHeader-style: a short accent tick precedes the label so
        // each round column reads as its own titled block.
        {
            float lblfs = g_font_h2 ? g_font_h2->FontSize * 0.78f : 16.0f;
            dl->AddRectFilled(ImVec2(col_x, origin.y + 2.0f),
                              ImVec2(col_x + 3.0f, origin.y + lblfs),
                              is_lb_final ? kVlrGold : kAccent, 1.5f);
            if (g_font_h2) {
                dl->AddText(g_font_h2, lblfs,
                            ImVec2(col_x + 10.0f, origin.y), lbl_col, lbl.c_str());
            } else {
                dl->AddText(ImVec2(col_x + 10.0f, origin.y), lbl_col, lbl.c_str());
            }
        }

        // Vertical layout: in round 0, stack at base pitch. In subsequent
        // rounds, place each card at the midpoint of its feeder(s). UB rounds
        // always halve (2 feeders per card). LB rounds alternate between
        // "merge" (LB winner + UB drop, prev_n == cur_n → straight 1:1 pair)
        // and "pure halve" (prev_n == cur_n*2 → 2 feeders per card). The
        // straight-pair branch fixes the long-standing bug where LB R2
        // cards 0/1 both clamped onto R1 card 1, collapsing the panel.
        std::vector<float> card_ys;
        card_ys.reserve(slots.size());
        if (r == 0) {
            for (std::size_t i = 0; i < slots.size(); ++i) {
                card_ys.push_back(content_y0 + i * kBasePitch);
            }
        } else {
            auto& prev = rendered[r - 1];
            std::size_t prev_n = prev.size();
            std::size_t cur_n  = slots.size();
            bool straight_pair = (prev_n == cur_n) && prev_n > 0;
            for (std::size_t i = 0; i < cur_n; ++i) {
                std::size_t f1, f2;
                if (straight_pair) {
                    f1 = f2 = std::min(prev_n - 1, i);
                } else {
                    f1 = std::min(prev_n - 1, i * 2);
                    f2 = std::min(prev_n - 1, i * 2 + 1);
                }
                float y1 = (prev[f1].tl.y + prev[f1].br.y) * 0.5f;
                float y2 = (prev[f2].tl.y + prev[f2].br.y) * 0.5f;
                float mid = (y1 + y2) * 0.5f;
                card_ys.push_back(mid - kCardH * 0.5f);
            }
        }

        for (std::size_t i = 0; i < slots.size(); ++i) {
            auto& sl = slots[i];
            BracketCardLayout layout;
            if (sl.placeholder) {
                ImVec2 tl(col_x, card_ys[i]);
                ImVec2 br(col_x + kCardW, card_ys[i] + kCardH);
                // Crisp empty card — deep fill, hairline border, centered
                // muted "TBD" so future rounds still read as real slots.
                dl->AddRectFilled(tl, br, bracket_ui::kCardBgUnplayed, 5.0f);
                dl->AddRect(tl, br, kCardBorder, 5.0f, 0, 1.0f);
                const char* tbd = "TBD";
                ImVec2 tsz = ImGui::CalcTextSize(tbd);
                dl->AddText(ImVec2((tl.x + br.x) * 0.5f - tsz.x * 0.5f,
                                   (tl.y + br.y) * 0.5f - tsz.y * 0.5f),
                            kTextDim, tbd);
                layout.tl = tl; layout.br = br;
                layout.played = false;
            } else {
                layout = DrawBracketCardAt(
                    s, ImVec2(col_x, card_ys[i]),
                    sl.a, sl.b, sl.winner, sl.a_score, sl.b_score, sl.played,
                    sl.best_of,
                    is_lower, /*highlight=*/(!sl.played && sl.a && sl.b),
                    event_name, id_base + static_cast<int>(r * 100 + i));
            }
            rendered[r].push_back(layout);

            // Connectors from this round back into prev round. Mirrors the
            // feeder rule used by the vertical layout above so the visual
            // wires actually trace the correct advancement path.
            if (r > 0 && !rendered[r - 1].empty()) {
                auto& prev_r = rendered[r - 1];
                std::size_t prev_n = prev_r.size();
                std::size_t cur_n  = slots.size();
                bool straight_pair = (prev_n == cur_n) && prev_n > 0;
                std::size_t f1, f2;
                if (straight_pair) {
                    f1 = f2 = std::min(prev_n - 1, i);
                } else {
                    f1 = std::min(prev_n - 1, i * 2);
                    f2 = std::min(prev_n - 1, i * 2 + 1);
                }
                DrawBracketConnector(dl, prev_r[f1], layout);
                if (f1 != f2) DrawBracketConnector(dl, prev_r[f2], layout);
            }
        }
    }

    if (out_finals && !rendered.empty()) {
        *out_finals = rendered.back();
    }

    float bottom_y = content_y0;
    for (auto& rrr : rendered)
        for (auto& c : rrr) bottom_y = std::max(bottom_y, c.br.y);
    return bottom_y;
}

// Convert a Tournament's history + scheduled matchups into a slot grid.
// Future UB rounds (after the latest scheduled) get TBD placeholders so
// the visual bracket extends all the way to the final. LB structure is
// irregular (drop-ins from UB merge unpredictably with LB winners) so we
// do NOT pad LB placeholders — that was a major source of the "glitchy
// lower bracket" complaints.
std::vector<std::vector<RoundSlot>>
build_round_grid(const std::vector<std::vector<vlr::BracketMatch>>& history,
                 const std::vector<std::pair<vlr::TeamPtr, vlr::TeamPtr>>& scheduled,
                 int scheduled_best_of,
                 bool is_lower,
                 int target_total_rounds) {
    std::vector<std::vector<RoundSlot>> out;
    // 1. Played history rounds.
    for (auto& round : history) {
        std::vector<RoundSlot> r;
        for (auto& m : round) {
            RoundSlot s;
            s.a = m.a; s.b = m.b; s.winner = m.winner;
            s.a_score = m.a_score; s.b_score = m.b_score; s.played = m.played;
            s.best_of = m.best_of > 0 ? m.best_of : 3;
            r.push_back(s);
        }
        out.push_back(std::move(r));
    }
    // 2. Currently-scheduled (not-yet-played) round.
    if (!scheduled.empty()) {
        std::vector<RoundSlot> r;
        for (auto& mu : scheduled) {
            RoundSlot s;
            s.a = mu.first; s.b = mu.second; s.played = false;
            s.best_of = scheduled_best_of > 0 ? scheduled_best_of : 3;
            r.push_back(s);
        }
        out.push_back(std::move(r));
    }
    // 3. UB-only: pad with TBD placeholders down to the final.
    if (!is_lower && !out.empty()) {
        std::size_t last = out.back().size();
        int safety = 0;
        while (last > 1 && safety++ < 6) {
            std::size_t next = (last + 1) / 2;
            std::vector<RoundSlot> r;
            for (std::size_t i = 0; i < next; ++i) {
                RoundSlot s; s.placeholder = true; s.best_of = 3;
                r.push_back(s);
            }
            out.push_back(std::move(r));
            last = next;
        }
    }
    // 4. LB: pad with TBD placeholders so the column count matches across
    //    renders. Each LB round alternates between MERGE (LB winners +
    //    UB drops → next size == current size) and PURE HALVE (no drops →
    //    next size == current / 2). For an 8-team bracket the shape is
    //    [2, 2, 1, 1]; 16-team is [4, 4, 2, 2, 1, 1]; etc. Driven by the
    //    index of the round being added — even indices (R2, R4...) merge,
    //    odd indices (R3, R5...) halve. The LB Final placeholder is
    //    stamped BO5 so the gold "BO5" badge renders consistently.
    if (is_lower && target_total_rounds > 0 &&
        static_cast<int>(out.size()) < target_total_rounds && !out.empty()) {
        int pad = target_total_rounds - static_cast<int>(out.size());
        if (pad > 6) pad = 6;
        for (int p = 0; p < pad; ++p) {
            std::size_t next_idx = out.size();          // 0-based round idx
            std::size_t prev_n   = out.back().size();
            std::size_t next_n;
            // R1 itself is base; index 1 (R2) is merge; index 2 (R3) halves;
            // index 3 (R4) merges; etc.
            if (next_idx % 2 == 1) {
                next_n = prev_n;
            } else {
                next_n = std::max<std::size_t>(1, (prev_n + 1) / 2);
            }
            bool last_round = (static_cast<int>(next_idx) + 1
                               == target_total_rounds);
            int bo = last_round ? 5 : 3;
            std::vector<RoundSlot> r;
            r.reserve(next_n);
            for (std::size_t i = 0; i < next_n; ++i) {
                RoundSlot s; s.placeholder = true; s.best_of = bo;
                r.push_back(s);
            }
            out.push_back(std::move(r));
        }
    }
    return out;
}

// Backwards-compat shim: keeps DrawMatchupCard signature for the live-match
// tile rendering on the sidebar / dashboard. Returns true if Watch clicked.
bool DrawMatchupCard(AppState& s, const std::pair<vlr::TeamPtr, vlr::TeamPtr>& mu,
                     const std::string& event_name,
                     bool is_lower_bracket,
                     const std::unordered_set<vlr::Team*>& eliminated_set) {
    bool watch_clicked = false;
    auto draw_team_row = [&](const vlr::TeamPtr& t) {
        if (!t) {
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
            ImGui::Text("  BYE");
            ImGui::PopStyleColor();
            return;
        }
        ImVec2 cur = ImGui::GetCursorScreenPos();
        ImU32 tc = IM_COL32(t->color_primary_r, t->color_primary_g, t->color_primary_b, 0xFF);
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(cur.x, cur.y), ImVec2(cur.x + 4, cur.y + 22), tc, 1.5f);
        ImGui::SetCursorScreenPos(ImVec2(cur.x + 10, cur.y));
        TeamLogo(*t, 20.f);
        ImGui::SameLine();
        CountryFlag(t->home_country_iso);
        ImGui::SameLine();
        if (!t->tag.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(tc));
            ImGui::Text("%s", t->tag.c_str());
            ImGui::PopStyleColor();
            ImGui::SameLine();
        }
        if (ImGui::Selectable(t->name.c_str(), false, 0, ImVec2(130, 0))) {
            OpenTeamProfile(s, t);
        }
    };
    BeginCardSized("##muc", ImVec2(280, 70), kSurface, 8.0f, 8.0f);
    draw_team_row(mu.first);
    draw_team_row(mu.second);
    if (mu.first && mu.second) {
        ImGui::PushFont(g_font_small);
        ImGui::PushStyleColor(ImGuiCol_Button, toV4(kAccent));
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kBgDeep));
        if (ImGui::SmallButton("Watch")) {
            OpenLiveMatch(s, mu.first, mu.second, event_name);
            watch_clicked = true;
        }
        ImGui::PopStyleColor(2);
        ImGui::PopFont();
    }
    EndCardSized();
    (void)eliminated_set;
    (void)is_lower_bracket;
    return watch_clicked;
}

void DrawBrackets(AppState& s) {
    H1("TOURNAMENT BRACKETS", kVlrRed);
    ImGui::Separator();
    if (s.gm.active_tournaments.empty()) {
        ImGui::TextDisabled("No active tournaments. Advance the phase to enter playoffs.");
        return;
    }

    // Group tournaments into tabs. Regional events go in their region tab;
    // international events (MASTERS / CHAMPIONS) go in an "International" tab.
    // This keeps the page navigable instead of a single endless scroll.
    auto tour_tab_for = [](const std::shared_ptr<vlr::Tournament>& t) -> std::string {
        const std::string& n = t->name();
        // International events go in their own tab regardless of word order.
        if (n.find("MASTERS")   != std::string::npos) return "International";
        if (n.find("CHAMPIONS") != std::string::npos) return "International";
        // Match known region names explicitly (rather than splitting on the
        // first space) so multi-word region tags don't get truncated and
        // unknown tournament names don't pollute the tab bar.
        for (auto* known : {"Americas", "EMEA", "Pacific"}) {
            if (n.find(known) != std::string::npos) return known;
        }
        // Unknown / future events land in the International bucket so the tab
        // bar stays compact.
        return "International";
    };

    // Build deterministic tab order: known regions first, then International,
    // then any leftover/unknown.
    std::vector<std::string> tab_order;
    auto push_tab = [&](const std::string& nm) {
        if (std::find(tab_order.begin(), tab_order.end(), nm) == tab_order.end())
            tab_order.push_back(nm);
    };
    for (auto* known : {"Americas", "EMEA", "Pacific"}) push_tab(known);
    push_tab("International");
    for (auto& t : s.gm.active_tournaments) push_tab(tour_tab_for(t));

    if (!ImGui::BeginTabBar("##tour_tabs", ImGuiTabBarFlags_FittingPolicyScroll)) return;
    for (auto& tab_name : tab_order) {
        // Skip tabs with no active tournament (avoids empty tabs).
        bool has_any = false;
        for (auto& t : s.gm.active_tournaments) {
            if (tour_tab_for(t) == tab_name) { has_any = true; break; }
        }
        if (!has_any) continue;
        if (!ImGui::BeginTabItem(tab_name.c_str())) continue;

        int tour_index = 0;
        for (auto& tour : s.gm.active_tournaments) {
            if (tour_tab_for(tour) != tab_name) continue;
        // Scope ImGui IDs to this tournament so the bracket card buttons
        // don't collide with cards in another tournament's bracket
        // (which all share the same id_base offsets internally).
        ImGui::PushID(("##tour_" + std::to_string(tour_index++)).c_str());
        // Tournament title strip
        ImGui::PushFont(g_font_h2);
        ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
        ImGui::Text("%s", tour->name().c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::SameLine();
        Sub("  |  Round %d  |  %s", tour->round_num(),
            tour->current_round_label().c_str());

        // Per-tournament sub-tabs: "Bracket" (existing view) and
        // "Player Stats" (event-wide aggregate). Lives INSIDE the
        // per-tournament PushID scope so widget IDs stay namespaced.
        if (ImGui::BeginTabBar("##tour_subtabs")) {
        if (ImGui::BeginTabItem("Bracket")) {
        // The existing bracket/group/champion rendering. Wrapped in a
        // lambda so its several early-exit paths can `return` cleanly
        // instead of `continue`-ing past the Player Stats tab below.
        auto render_bracket = [&]() {
        // Champion banner
        if (tour->finished()) {
            ImGui::Spacing();
            {
                ImVec2 cb_p = ImGui::GetCursorScreenPos();
                ImVec2 cb_a = ImGui::GetContentRegionAvail();
                float cb_h = 68.0f;
                ImDrawList* cdl = ImGui::GetWindowDrawList();
                cdl->AddRectFilled(cb_p, ImVec2(cb_p.x + cb_a.x, cb_p.y + cb_h),
                                   kBgDeep, 8.0f);
                cdl->AddRectFilled(cb_p, ImVec2(cb_p.x + cb_a.x, cb_p.y + cb_h),
                                   IM_COL32(0x2A, 0x22, 0x08, 0xFF), 8.0f);
                cdl->AddRect(cb_p, ImVec2(cb_p.x + cb_a.x, cb_p.y + cb_h),
                             kVlrGold, 8.0f, 0, 1.5f);
                cdl->AddRectFilled(cb_p, ImVec2(cb_p.x + 5, cb_p.y + cb_h),
                                   kVlrGold);
            }
            ImGui::BeginChild("##champ_banner", ImVec2(0, 68), ImGuiChildFlags_None);
            ImGui::Dummy(ImVec2(0, 14));
            ImGui::Indent(18.0f);
            ImGui::PushFont(g_font_h1);
            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
            ImGui::Text("CHAMPION: %s", tour->champion()->name.c_str());
            ImGui::PopStyleColor();
            ImGui::PopFont();
            ImGui::Unindent(18.0f);
            ImGui::EndChild();
            ImGui::Spacing();

            // Final placement strip — runner_up_/semifinalists_ are only
            // populated when the tournament reaches Done, so this is
            // final-state only and shown alongside the champion banner.
            if (!tour->eliminated().empty() || !tour->semifinalists().empty()
                || tour->runner_up()) {
                ImGui::PushFont(g_font_small);
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrSub));
                ImGui::Text("FINAL PLACEMENT:");
                ImGui::PopStyleColor();
                ImGui::PopFont();
                auto pill = [&](const vlr::TeamPtr& t, const char* tag, ImU32 col) {
                    if (!t) return;
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Text, toV4(col));
                    ImGui::PushFont(g_font_small);
                    std::string label = std::string(tag) + " " +
                                        (t->tag.empty() ? t->name : t->tag);
                    if (ImGui::SmallButton(label.c_str())) OpenTeamProfile(s, t);
                    ImGui::PopFont();
                    ImGui::PopStyleColor();
                };
                if (tour->champion())
                    pill(tour->champion(), "CHAMP", kVlrGold);
                if (tour->runner_up())
                    pill(tour->runner_up(), "FINAL", IM_COL32(0xC0, 0xC0, 0xC0, 0xFF));
                for (auto& sf : tour->semifinalists())
                    pill(sf, "SF", IM_COL32(0xCD, 0x7F, 0x32, 0xFF));
                std::unordered_set<vlr::Team*> shown;
                if (tour->champion())  shown.insert(tour->champion().get());
                if (tour->runner_up()) shown.insert(tour->runner_up().get());
                for (auto& sf : tour->semifinalists()) if (sf) shown.insert(sf.get());
                for (auto& t : tour->eliminated()) {
                    if (!t || shown.count(t.get())) continue;
                    pill(t, "OUT", kVlrRed);
                }
                ImGui::Spacing();
            }

            return;   // finished tournament — no bracket grid to draw
        }

        // Build elimination set for visual indicators
        std::unordered_set<vlr::Team*> elim_set;
        for (auto& t : tour->eliminated()) if (t) elim_set.insert(t.get());

        // Group stage view
        if (!tour->groups().empty() && !tour->group_stage_complete()) {
            ImGui::Spacing();
            ImGui::PushFont(g_font_h2);
            ImGui::Text("GROUP STAGE");
            ImGui::PopFont();
            char gname[2] = {'A', 0};
            for (auto& g : tour->groups()) {
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGold));
                ImGui::Text("Group %s", gname);
                ImGui::PopStyleColor();
                ++gname[0];
                if (ImGui::BeginTable(("##grp" + std::to_string(gname[0])).c_str(), 5,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
                    ImGui::TableSetupColumn("TEAM", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("W");
                    ImGui::TableSetupColumn("L");
                    ImGui::TableSetupColumn("MAP DIFF");
                    ImGui::TableSetupColumn("STATUS");
                    ImGui::TableHeadersRow();
                    for (auto& gs : g) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        if (gs.team) {
                            CountryFlag(gs.team->home_country_iso); ImGui::SameLine();
                            if (TableRowSelectable(gs.team->name.c_str(), false)) {
                                OpenTeamProfile(s, gs.team);
                            }
                        }
                        ImGui::TableNextColumn(); ImGui::Text("%d", gs.wins);
                        ImGui::TableNextColumn(); ImGui::Text("%d", gs.losses);
                        ImGui::TableNextColumn(); ImGui::Text("%+d", gs.map_diff);
                        ImGui::TableNextColumn();
                        if (gs.eliminated) {
                            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrRed));
                            ImGui::Text("ELIMINATED");
                            ImGui::PopStyleColor();
                        } else if (gs.wins >= 2) {
                            ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrGreen));
                            ImGui::Text("ADVANCING");
                            ImGui::PopStyleColor();
                        } else {
                            ImGui::TextDisabled("alive");
                        }
                    }
                    ImGui::EndTable();
                }
            }
            ImGui::Spacing();
            ImGui::Spacing();
            return;  // group stage doesn't show bracket yet
        }

        // === Bracket view (VCT / NBA playoff style — clean redesign) ===
        // Two visually separated panels (UB top, LB bottom) inside a single
        // scrollable canvas. The Grand Final lives in its own column to the
        // right of the UB panel, vertically centered as a single BO5 card
        // (modern VCT format — no bracket reset).
        //
        // Lower bracket is rendered as its own dedicated horizontal strip
        // below the UB panel. We do NOT try to interleave UB drops into the
        // LB column flow — that was the previous design's biggest source of
        // visual glitches (rounds misaligning after every UB elimination).
        ImGui::Spacing();

        // === Empty-bracket guard ===
        // If we somehow landed here with no UB history, no scheduled UB,
        // and no GF (e.g. partial state mid-transition from groups),
        // render a friendly placeholder instead of an empty canvas.
        bool ub_has_data = !tour->ub_history().empty()
            || !tour->current_matchups().empty()
            || !tour->gf_history().empty()
            || tour->in_grand_final();
        if (!ub_has_data) {
            ImGui::TextDisabled("Bracket starts after group stage.");
            return;
        }

        // === Round-label sourcing ===
        // For the latest scheduled round we use Tournament::current_round_label()
        // (Agent A contract — returns labels like "Upper Bracket R1",
        // "Lower Bracket Final", "Grand Final").
        // Historical / placeholder rounds get a count-based fallback inside
        // build_round_grid → round_label_at.
        std::string live_label = tour->current_round_label();
        bool live_in_gf = tour->in_grand_final();

        // Per-match BO for the currently-scheduled UB/LB rounds. The played
        // history's per-match best_of comes off BracketMatch::best_of, but
        // the SCHEDULED round's pairs are raw (TeamPtr, TeamPtr) — Agent A
        // stamps BO3 everywhere except LB Final / GF (BO5).
        // Mirror that here so the live card can render its BO5 chip
        // correctly even before play_round runs.
        auto sched_bo_for = [&](bool is_lower, std::size_t scheduled_count) -> int {
            if (live_in_gf) return 5;  // GF is BO5
            // Lower Bracket Final: scheduled LB count == 1
            if (is_lower && scheduled_count == 1
                && live_label.find("Final") != std::string::npos) return 5;
            return 3;
        };

        // Build UB grid (use scheduled current_matchups ONLY if not in GF
        // phase — in GF the current_matchups are the GF series, which we
        // render in the dedicated GF column).
        std::vector<std::pair<vlr::TeamPtr, vlr::TeamPtr>> ub_sched;
        if (!live_in_gf) ub_sched = tour->current_matchups();
        int ub_sched_bo = sched_bo_for(/*is_lower=*/false, ub_sched.size());

        std::vector<std::pair<vlr::TeamPtr, vlr::TeamPtr>> lb_sched;
        if (!live_in_gf) lb_sched = tour->lower_matchups();
        int lb_sched_bo = sched_bo_for(/*is_lower=*/true, lb_sched.size());

        // Target round counts. The Tournament accessors return the count of
        // ALREADY-PLAYED rounds — fine for UB (its padding halves until 1
        // and self-terminates) but useless for LB (we need to know the full
        // expected depth to pad future placeholders). Derive the canonical
        // LB depth from the initial bracket size: for N teams in a
        // double-elim, LB has 2*log2(N) - 2 rounds (8→4, 16→6, 4→2).
        int ub_target = tour->upper_bracket_round_count();
        int lb_target = tour->lower_bracket_round_count();
        {
            const auto& seed = tour->initial_seeding();
            int n_teams = static_cast<int>(seed.size());
            if (n_teams >= 4) {
                int log2n = 0;
                for (int x = n_teams; x > 1; x >>= 1) ++log2n;
                int canonical_lb = std::max(1, 2 * log2n - 2);
                if (canonical_lb > lb_target) lb_target = canonical_lb;
            }
        }

        auto ub_grid = build_round_grid(tour->ub_history(), ub_sched,
                                         ub_sched_bo, /*is_lower=*/false,
                                         ub_target);
        auto lb_grid = build_round_grid(tour->lb_history(), lb_sched,
                                         lb_sched_bo, /*is_lower=*/true,
                                         lb_target);

        // === Defensive layout audit (C9) ===
        // UB rounds MUST shrink monotonically (single-elim invariant). If we
        // ever see a wider next round it means history was mis-ordered or
        // double-fired (see pitfall 8: schedule_next_bracket_round
        // snapshot+clear). Surface a small muted warning so user knows the
        // visual layout may misrepresent the actual standings — engine fix
        // belongs in Pack B (Tournament::validate_bracket_state).
        bool bracket_warn = false;
        for (std::size_t i = 1; i < ub_grid.size(); ++i) {
            if (ub_grid[i].size() > ub_grid[i - 1].size()) {
                bracket_warn = true; break;
            }
        }
        if (bracket_warn) {
            MutedText("[bracket layout warning] UB round sizes are not "
                      "monotonically shrinking. Display may misrepresent "
                      "standings.");
        }

        // === Pre-compute per-round labels for both panels ===
        // Played UB rounds: pull from history[i][0].label if present, else
        // empty (count-based fallback inside the column renderer kicks in).
        // The single LATEST scheduled round gets `live_label` patched in.
        auto labels_for = [&](const std::vector<std::vector<vlr::BracketMatch>>& hist,
                              std::size_t scheduled_present,
                              std::size_t total_columns,
                              bool is_lower) -> std::vector<std::string> {
            std::vector<std::string> out_lbl;
            out_lbl.reserve(total_columns);
            for (auto& round : hist) {
                if (!round.empty() && !round.front().label.empty()) {
                    out_lbl.push_back(round.front().label);
                } else {
                    out_lbl.emplace_back();
                }
            }
            if (scheduled_present) {
                // The latest scheduled column gets Tournament's current label,
                // but only if it matches this panel's bracket side. Otherwise
                // leave empty so the count-based fallback names it.
                bool live_is_lower = live_label.find("Lower") != std::string::npos;
                bool live_is_upper = live_label.find("Upper") != std::string::npos;
                if ((is_lower && live_is_lower) || (!is_lower && live_is_upper)) {
                    out_lbl.push_back(live_label);
                } else {
                    out_lbl.emplace_back();
                }
            }
            // Pad to total_columns with empties.
            while (out_lbl.size() < total_columns) out_lbl.emplace_back();
            return out_lbl;
        };

        std::vector<std::string> ub_labels = labels_for(
            tour->ub_history(),
            ub_sched.empty() ? 0u : 1u,
            ub_grid.size(),
            /*is_lower=*/false);
        std::vector<std::string> lb_labels = labels_for(
            tour->lb_history(),
            lb_sched.empty() ? 0u : 1u,
            lb_grid.size(),
            /*is_lower=*/true);

        // === Canvas sizing ===
        // Width = max(UB cols, LB cols) * column + GF column on the right.
        // Height: estimate generously — connectors must not clip even when
        // round-0 has 8 cards (Champions UB R1).
        std::size_t ub_cols = ub_grid.size();
        std::size_t lb_cols = lb_grid.size();
        std::size_t ub_r0   = ub_grid.empty() ? 0u : ub_grid.front().size();
        std::size_t lb_r0   = lb_grid.empty() ? 0u : lb_grid.front().size();
        float ub_height = std::max<float>(200.0f,
            static_cast<float>(ub_r0) * bracket_ui::kBasePitch + 80.0f);
        float lb_height = lb_grid.empty() ? 60.0f
            : std::max<float>(160.0f,
                  static_cast<float>(lb_r0) * bracket_ui::kBasePitch + 80.0f);
        float canvas_h = ub_height + lb_height + 100.0f;
        std::size_t widest_cols = std::max(ub_cols, lb_cols) + 1u;  // +1 for GF
        float canvas_w = std::max<float>(900.0f,
            static_cast<float>(widest_cols)
                * (bracket_ui::kCardW + bracket_ui::kColGap) + 80.0f);

        ImGui::BeginChild("##bracket_canvas", ImVec2(0, canvas_h),
                          ImGuiChildFlags_Border,
                          ImGuiWindowFlags_HorizontalScrollbar);
        ImVec2 cv_origin = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(canvas_w, canvas_h - 16.0f));
        ImGui::SetCursorScreenPos(cv_origin);
        auto* dl = ImGui::GetWindowDrawList();

        // === Panel A: UPPER BRACKET ===
        // Panel-level header (gold, H2) painted with full canvas width so it
        // anchors the visual hierarchy.
        ImVec2 ub_panel_tl(cv_origin.x + 6.0f, cv_origin.y + 4.0f);
        ImVec2 ub_panel_br(cv_origin.x + canvas_w - 6.0f, cv_origin.y + ub_height);
        dl->AddRectFilled(ub_panel_tl, ub_panel_br, bracket_ui::kPanelBg, 6.0f);
        dl->AddRect(ub_panel_tl, ub_panel_br, bracket_ui::kPanelBorder, 6.0f, 0, 1.0f);
        {
            float hfs = g_font_h2 ? g_font_h2->FontSize : 22.0f;
            dl->AddRectFilled(ImVec2(ub_panel_tl.x + 14.0f, ub_panel_tl.y + 7.0f),
                              ImVec2(ub_panel_tl.x + 18.0f, ub_panel_tl.y + 6.0f + hfs),
                              kVlrGold, 2.0f);
            if (g_font_h2) {
                dl->AddText(g_font_h2, hfs,
                            ImVec2(ub_panel_tl.x + 28.0f, ub_panel_tl.y + 6.0f),
                            kVlrGold, "UPPER BRACKET");
            } else {
                dl->AddText(ImVec2(ub_panel_tl.x + 28.0f, ub_panel_tl.y + 6.0f),
                            kVlrGold, "UPPER BRACKET");
            }
        }

        std::vector<BracketCardLayout> ub_finals;
        float ub_rounds_top = ub_panel_tl.y + 40.0f;
        DrawBracketRounds(s, ub_grid, ub_labels,
                          ImVec2(ub_panel_tl.x + bracket_ui::kPanelPad, ub_rounds_top),
                          /*is_lower=*/false,
                          tour->name() + " - UB",
                          /*id_base=*/1000,
                          &ub_finals);

        // === GF column (to the right of UB) ===
        ImVec2 gf_origin(
            ub_panel_tl.x + bracket_ui::kPanelPad
                + static_cast<float>(ub_cols)
                    * (bracket_ui::kCardW + bracket_ui::kColGap),
            ub_rounds_top);

        // Vertically center GF against UB final card (or fall back to mid-panel).
        float gf_y = ub_rounds_top + ub_height * 0.25f;
        if (!ub_finals.empty()) {
            gf_y = (ub_finals.back().tl.y + ub_finals.back().br.y) * 0.5f
                 - bracket_ui::kCardH * 0.5f;
        }

        // GF column header — wider gold treatment so it reads as the apex.
        if (g_font_h2) {
            dl->AddText(g_font_h2, g_font_h2->FontSize * 0.78f,
                        ImVec2(gf_origin.x, gf_y - 28.0f),
                        kVlrGold, "GRAND FINAL");
        } else {
            dl->AddText(ImVec2(gf_origin.x, gf_y - 28.0f),
                        kVlrGold, "GRAND FINAL");
        }

        std::vector<BracketCardLayout> gf_layouts;
        // Modern VCT format: exactly one Grand Final card (BO5, no bracket
        // reset). Render the played GF if it's in history, otherwise render
        // the live scheduled GF card.
        if (!tour->gf_history().empty()) {
            auto& g = tour->gf_history().front();
            BracketCardLayout layout = DrawBracketCardAt(
                s, ImVec2(gf_origin.x, gf_y),
                g.a, g.b, g.winner, g.a_score, g.b_score, g.played,
                g.best_of > 0 ? g.best_of : 5,
                /*is_lower=*/false, /*highlight=*/false,
                tour->name() + " - GF",
                /*unique_id=*/9000);
            gf_layouts.push_back(layout);
            // Mini-label above the card — prefer the stamped BracketMatch.label.
            std::string mini = g.label.empty() ? std::string("Grand Final") : g.label;
            dl->AddText(ImVec2(layout.tl.x, layout.tl.y - 16.0f),
                        bracket_ui::kTextMid, mini.c_str());
        } else if (live_in_gf && !tour->current_matchups().empty()) {
            // Live GF card (when GF is scheduled but not yet played).
            auto& mu = tour->current_matchups().front();
            BracketCardLayout layout = DrawBracketCardAt(
                s, ImVec2(gf_origin.x, gf_y),
                mu.first, mu.second, vlr::TeamPtr{}, 0, 0, /*played=*/false,
                /*best_of=*/5,
                /*is_lower=*/false, /*highlight=*/true,
                tour->name() + " - GF", /*unique_id=*/9100);
            gf_layouts.push_back(layout);
            dl->AddText(ImVec2(layout.tl.x, layout.tl.y - 16.0f),
                        kVlrGold, live_label.c_str());
        }

        // Connector: UB final → GF card. (LB → GF connector drawn after LB.)
        if (!gf_layouts.empty() && !ub_finals.empty()) {
            DrawBracketConnector(dl, ub_finals.back(), gf_layouts.front());
        }

        // === Visual divider between UB and LB panels ===
        float divider_y = ub_panel_br.y + 14.0f;
        dl->AddLine(ImVec2(ub_panel_tl.x + 8.0f, divider_y),
                    ImVec2(ub_panel_br.x - 8.0f, divider_y),
                    bracket_ui::kPanelBorder, 1.0f);

        // === Panel B: LOWER BRACKET ===
        if (!lb_grid.empty()) {
            ImVec2 lb_panel_tl(cv_origin.x + 6.0f, divider_y + 14.0f);
            ImVec2 lb_panel_br(cv_origin.x + canvas_w - 6.0f,
                               lb_panel_tl.y + lb_height);
            dl->AddRectFilled(lb_panel_tl, lb_panel_br, bracket_ui::kPanelBg, 6.0f);
            dl->AddRect(lb_panel_tl, lb_panel_br, bracket_ui::kPanelBorder, 6.0f, 0, 1.0f);
            {
                ImU32 lbc = IM_COL32(0xE0, 0x9C, 0x9C, 0xFF);
                float hfs = g_font_h2 ? g_font_h2->FontSize : 22.0f;
                dl->AddRectFilled(ImVec2(lb_panel_tl.x + 14.0f, lb_panel_tl.y + 7.0f),
                                  ImVec2(lb_panel_tl.x + 18.0f, lb_panel_tl.y + 6.0f + hfs),
                                  kVlrRed, 2.0f);
                if (g_font_h2) {
                    dl->AddText(g_font_h2, hfs,
                                ImVec2(lb_panel_tl.x + 28.0f, lb_panel_tl.y + 6.0f),
                                lbc, "LOWER BRACKET");
                } else {
                    dl->AddText(ImVec2(lb_panel_tl.x + 28.0f, lb_panel_tl.y + 6.0f),
                                lbc, "LOWER BRACKET");
                }
            }
            // Caption explaining UB-loser inflow.
            if (g_font_small) {
                dl->AddText(g_font_small, g_font_small->FontSize,
                            ImVec2(lb_panel_tl.x + 220.0f, lb_panel_tl.y + 11.0f),
                            bracket_ui::kTextDim, "(UB losers drop in here)");
            }

            std::vector<BracketCardLayout> lb_finals;
            DrawBracketRounds(s, lb_grid, lb_labels,
                              ImVec2(lb_panel_tl.x + bracket_ui::kPanelPad,
                                     lb_panel_tl.y + 40.0f),
                              /*is_lower=*/true,
                              tour->name() + " - LB",
                              /*id_base=*/2000,
                              &lb_finals);

            // LB final → GF connector. Draw it long-form across the panel
            // gap (the connector helper handles any vertical delta).
            if (!lb_finals.empty() && !gf_layouts.empty()) {
                DrawBracketConnector(dl, lb_finals.back(), gf_layouts.front());
            }
        }
        ImGui::EndChild();

        // (Final-placement strip is rendered above in the finished/champion
        // branch — runner_up_/semifinalists_ are populated only at Done, so
        // there is nothing meaningful to show during active brackets.)
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        };   // end render_bracket lambda
        render_bracket();
        ImGui::EndTabItem();
        }   // end "Bracket" tab item

        // === Player Stats sub-tab ==================================
        // Event-wide aggregate (groups + playoffs accumulated by the
        // engine accessor). Sortable table; default sort = Rating desc.
        if (ImGui::BeginTabItem("Player Stats")) {
            // Persistent per-tournament copy so sort order survives
            // frames. Previously the cache was overwritten unconditionally
            // every frame and re-sorted ONLY when SpecsDirty fired — so
            // Pull fresh aggregated stats EVERY frame so per-map
            // accumulation (more kills/maps/rating) propagates immediately
            // as the tournament progresses. The previous version-keyed
            // cache (row_count + last player ptr) caught "roster changed"
            // but NOT "existing players played more maps", so stats froze
            // after the first map. aggregate_player_stats() is O(players)
            // (~16 per bracket) — trivial cost at 60fps. Sort spec is
            // re-applied unconditionally below so the user's chosen
            // column order survives across the per-frame rebuild.
            auto rows = tour->aggregate_player_stats();
            if (rows.empty()) {
                ImGui::Spacing();
                float availw = ImGui::GetContentRegionAvail().x;
                const char* msg = "Stats available once the event begins.";
                ImGui::SetCursorPosX(ImGui::GetCursorPosX()
                    + std::max(0.0f,
                        (availw - ImGui::CalcTextSize(msg).x) * 0.5f));
                MutedText("%s", msg);
            } else {
                enum PsCol { P_NAME, P_TEAM, P_AGENT, P_MAPS, P_K, P_D,
                             P_A, P_KD, P_RAT, P_FK, P_FD, P_RND, P_HS,
                             P_ADR, P_KAST, P_CLU, P_NCOL };
                if (ImGui::BeginTable("##tour_pstats", P_NCOL,
                        ImGuiTableFlags_Sortable | ImGuiTableFlags_SortMulti |
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                        ImGuiTableFlags_BordersInnerV |
                        ImGuiTableFlags_SizingStretchProp,
                        ImVec2(0, 460))) {
                    auto desc = ImGuiTableColumnFlags_PreferSortDescending;
                    ImGui::TableSetupScrollFreeze(1, 1);
                    ImGui::TableSetupColumn("Player",
                        ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Team",
                        ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Agent",
                        ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Maps", desc);
                    ImGui::TableSetupColumn("K", desc);
                    ImGui::TableSetupColumn("D", desc);
                    ImGui::TableSetupColumn("A", desc);
                    ImGui::TableSetupColumn("KD", desc);
                    ImGui::TableSetupColumn("Rating",
                        desc | ImGuiTableColumnFlags_DefaultSort);
                    ImGui::TableSetupColumn("FK", desc);
                    ImGui::TableSetupColumn("FD", desc);
                    ImGui::TableSetupColumn("Rounds", desc);
                    ImGui::TableSetupColumn("HS%", desc);
                    ImGui::TableSetupColumn("ADR", desc);
                    ImGui::TableSetupColumn("KAST", desc);
                    ImGui::TableSetupColumn("Clutches", desc);
                    ImGui::TableHeadersRow();

                    auto kd_of = [](const vlr::TournamentPlayerStat& r) {
                        return (double)r.kills /
                               (double)std::max(1, r.deaths);
                    };
                    if (ImGuiTableSortSpecs* sp =
                            ImGui::TableGetSortSpecs()) {
                        // rows is rebuilt every frame, so we must re-apply
                        // the active sort every frame too. Drop the dirty
                        // gate.
                        if (sp->SpecsCount > 0) {
                            std::stable_sort(rows.begin(), rows.end(),
                              [&](const vlr::TournamentPlayerStat& a,
                                  const vlr::TournamentPlayerStat& b) {
                                for (int i = 0; i < sp->SpecsCount; ++i) {
                                    const auto& sc = sp->Specs[i];
                                    bool asc = (sc.SortDirection ==
                                        ImGuiSortDirection_Ascending);
                                    double va = 0, vb = 0; int cmp = 0;
                                    switch (sc.ColumnIndex) {
                                    case P_NAME:
                                        cmp = a.display_name.compare(
                                              b.display_name); break;
                                    case P_TEAM:
                                        cmp = a.team_name.compare(
                                              b.team_name); break;
                                    case P_AGENT:
                                        cmp = a.top_agent.compare(
                                              b.top_agent); break;
                                    case P_MAPS: va=a.maps; vb=b.maps; break;
                                    case P_K: va=a.kills; vb=b.kills; break;
                                    case P_D: va=a.deaths; vb=b.deaths; break;
                                    case P_A: va=a.assists; vb=b.assists; break;
                                    case P_KD: va=kd_of(a); vb=kd_of(b); break;
                                    case P_RAT: va=a.rating; vb=b.rating; break;
                                    case P_FK: va=a.first_kills;
                                               vb=b.first_kills; break;
                                    case P_FD: va=a.first_deaths;
                                               vb=b.first_deaths; break;
                                    case P_RND: va=a.rounds;
                                                vb=b.rounds; break;
                                    case P_HS: va=a.hs_pct;
                                               vb=b.hs_pct; break;
                                    case P_ADR: va=a.adr; vb=b.adr; break;
                                    case P_KAST: va=a.kast;
                                                 vb=b.kast; break;
                                    case P_CLU: va=a.clutches;
                                                vb=b.clutches; break;
                                    default: break;
                                    }
                                    if (cmp == 0 && va != vb)
                                        cmp = (va < vb) ? -1 : 1;
                                    if (cmp != 0)
                                        return asc ? (cmp<0) : (cmp>0);
                                }
                                return a.display_name < b.display_name;
                              });
                            sp->SpecsDirty = false;
                        }
                    }

                    for (auto& r : rows) {
                        if (r.display_name.empty()) continue;
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        // Render display name from the snapshot — NEVER deref
                        // r.player (lifetime not guaranteed beyond the world
                        // reset boundary). IGL prefix mirrors player_label().
                        std::string label_text = r.is_igl
                            ? (std::string("[I] ") + r.display_name)
                            : r.display_name;
                        if (TableRowSelectable(
                                label_text.c_str(), false,
                                ImGuiSelectableFlags_SpanAllColumns)) {
                            // Resolve a PlayerPtr by walking live containers.
                            // Match on pointer identity first (cheap), then
                            // fall back to gamertag (handles retired/cut
                            // players who no longer share the original ptr).
                            vlr::PlayerPtr found;
                            auto try_match = [&](const vlr::PlayerPtr& pp) {
                                if (!pp) return false;
                                if (pp.get() == r.player) { found = pp; return true; }
                                if (pp->name == r.display_name) { found = pp; return true; }
                                return false;
                            };
                            for (auto& kv : s.gm.leagues) {
                                if (!kv.second || found) break;
                                for (auto& tt : kv.second->teams()) {
                                    if (!tt || found) break;
                                    for (auto& pp : tt->roster)
                                        if (try_match(pp)) break;
                                }
                            }
                            if (!found) {
                                for (auto& kv : s.gm.solo_qs) {
                                    if (!kv.second || found) break;
                                    for (auto& pp : kv.second->global_ladder())
                                        if (try_match(pp)) break;
                                }
                            }
                            if (found) OpenPlayerModal(s, found);
                        }
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(r.team_name.c_str());
                        ImGui::TableNextColumn();
                        if (r.top_agent.empty())
                            ImGui::TextDisabled("\xE2\x80\x94");
                        else
                            ImGui::TextUnformatted(r.top_agent.c_str());
                        ImGui::PushFont(g_font_mono);
                        ImGui::TableNextColumn();
                        ImGui::Text("%d", r.maps);
                        ImGui::TableNextColumn();
                        ImGui::Text("%d", r.kills);
                        ImGui::TableNextColumn();
                        ImGui::Text("%d", r.deaths);
                        ImGui::TableNextColumn();
                        ImGui::Text("%d", r.assists);
                        double kd = kd_of(r);
                        ImGui::TableNextColumn();
                        ImGui::PushStyleColor(ImGuiCol_Text, toV4(
                            kd >= 1.1 ? kVlrGreen
                            : (kd < 0.9 ? kVlrRed : kVlrText)));
                        ImGui::Text("%.2f", kd);
                        ImGui::PopStyleColor();
                        ImGui::TableNextColumn();
                        ImGui::PushStyleColor(ImGuiCol_Text, toV4(
                            r.rating >= 1.1 ? kVlrGreen
                            : (r.rating < 0.9 ? kVlrRed : kVlrText)));
                        ImGui::Text("%.2f", r.rating);
                        ImGui::PopStyleColor();
                        ImGui::TableNextColumn();
                        ImGui::Text("%d", r.first_kills);
                        ImGui::TableNextColumn();
                        ImGui::Text("%d", r.first_deaths);
                        ImGui::TableNextColumn();
                        ImGui::Text("%d", r.rounds);
                        ImGui::TableNextColumn();
                        ImGui::Text("%.0f%%", r.hs_pct);
                        ImGui::TableNextColumn();
                        ImGui::Text("%.0f", r.adr);
                        ImGui::TableNextColumn();
                        ImGui::Text("%.0f%%", r.kast);
                        ImGui::TableNextColumn();
                        ImGui::Text("%d", r.clutches);
                        ImGui::PopFont();
                    }
                    ImGui::EndTable();
                }
            }
            ImGui::EndTabItem();
        }   // end "Player Stats" tab item
        ImGui::EndTabBar();
        }   // end per-tournament sub-tab bar

        ImGui::PopID();   // close per-tournament ID scope
        }   // end inner per-tournament for-loop
        ImGui::EndTabItem();
    }   // end tab-order for-loop
    ImGui::EndTabBar();
}
