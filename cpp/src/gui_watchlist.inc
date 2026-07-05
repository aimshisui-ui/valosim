// DrawWatchlist: transfer/scouting target board (extracted increment-C screen).
// Unity-build fragment of gui_main.cpp: this file is #included inside the same
// anonymous namespace; it is NOT a standalone translation unit.
void DrawWatchlist(AppState& s) {
    ImGui::PushFont(g_font_h1);
    ImGui::PushStyleColor(ImGuiCol_Text, toV4(kVlrRed));
    ImGui::TextUnformatted("WATCHLIST");
    ImGui::PopStyleColor();
    ImGui::PopFont();
    MutedText("Players you're tracking as transfer / scouting targets \xE2\x80\x94 distinct "
              "from Favorites (your GOAT / Hall-of-Fame bookmarks). Set a status, jot a "
              "note, and open a profile to scout or make an offer. (Not saved between runs yet.)");
    VSpace(10);

    auto& wl = s.gm.watchlist();
    if (wl.empty()) {
        ImGui::TextDisabled("(empty) \xE2\x80\x94 open any player and hit [ ] Watch to start tracking them.");
        return;
    }

    const char* statuses[] = { "Watching", "Scouting", "Bidding" };
    vlr::PlayerPtr to_open, to_remove;

    if (BeginCard("##watchlist_card")) {
        SectionHeader("TRACKED TARGETS", nullptr, kAccent2);
        if (ImGui::BeginTable("##watchlist_tbl", 7,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                ImGuiTableFlags_ScrollY, ImVec2(-FLT_MIN, 0))) {
            ImGui::TableSetupColumn("NAME", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("TEAM");
            ImGui::TableSetupColumn("ROLE");
            ImGui::TableSetupColumn("OVR");
            ImGui::TableSetupColumn("STATUS");
            ImGui::TableSetupColumn("NOTE", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("");
            ImGui::TableHeadersRow();
            int wi = 0;
            for (auto& w : wl) {
                if (!w.player) continue;
                auto& p = w.player;
                ImGui::PushID(40000 + wi++);
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(player_label(*p).c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(p->team_name.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(vlr::role_name(p->primary_role));
                ImGui::TableNextColumn(); ImGui::Text("%d", static_cast<int>(std::lround(p->ovr())));
                ImGui::TableNextColumn();
                int st = static_cast<int>(w.status);
                ImGui::SetNextItemWidth(110);
                if (ImGui::Combo("##st", &st, statuses, 3))
                    w.status = static_cast<vlr::GameManager::WatchStatus>(st);
                ImGui::TableNextColumn();
                char nb[128];
                std::snprintf(nb, sizeof(nb), "%s", w.note.c_str());
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::InputText("##note", nb, sizeof(nb))) w.note = nb;
                ImGui::TableNextColumn();
                if (ImGui::SmallButton("View")) to_open = p;
                ImGui::SameLine();
                if (ImGui::SmallButton("X")) to_remove = p;
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    EndCard();

    // Deferred mutations (don't touch the vector / open modals mid-iteration).
    if (to_open) OpenPlayerModal(s, to_open);
    if (to_remove) s.gm.unwatch_player(to_remove);
}
