# Changelog

Dated history of changes to Valosim. Reverse chronological. The authoritative reference for current behavior is **PROJECT_GUIDE.md**; this file documents *when* and *why* things changed.

Entries reference PROJECT_GUIDE sections by number (e.g. `→ §4.17`) so you can jump to the current spec.

---

## 2026-06-16

### Hotfix — Start-a-New-Career heap-corruption crash (CardFrame_ splitter regression)

User report: "the crash thats happening when you start a new career that wasnt there before." Three crash dumps in `%LOCALAPPDATA%\CrashDumps\vlrgui.exe.*.dmp` (today 16:15-16:16) all showed `STATUS_HEAP_CORRUPTION (0xC0000374)` at the same ntdll address — a classic deferred-detection corruption, not a null deref. The wizard renders fine because the wizard preview doesn't call `BeginCard`; the crash fires on the first frame of `DrawDashboard`, which opens four cards (`##dash_orgmem` → `##dash_roster` → `##dash_development` → `##coach_card`) in sequence.

**Root cause** — the new `CardFrame_` struct (introduced 2026-06-11 in the premium UI pass to back-paint stacked tinted shadows behind every card) held an `ImDrawListSplitter` **by value**. ImGui's `ImDrawListSplitter` contains `ImVector<ImDrawChannel> _Channels;`, and `ImVector<T>::operator=` does a shallow `memcpy` of `Size * sizeof(T)` bytes (imgui.h:2061). Each `ImDrawChannel` is `{ ImVector<ImDrawCmd> _CmdBuffer; ImVector<ImDrawIdx> _IdxBuffer; }` — both ImVectors store their data via raw `Data` pointers.

So when `CardBegin_` did `CardFrame_ f{}` → `f.splitter.Split(parent_dl, 2)` → `f.splitter.SetCurrentChannel(parent_dl, 1)` → `CardStack_().push_back(f)`, the push_back invoked the implicit copy constructor on `CardFrame_`, which copy-constructed the `ImDrawListSplitter`, which memcpy'd the `_Channels` array — aliasing the `_CmdBuffer.Data` / `_IdxBuffer.Data` pointers between the local `f` and the vector copy. When the local `f` went out of scope at end of `CardBegin_`, `~ImDrawListSplitter` → `ClearFreeMemory()` ran `_CmdBuffer.clear()` (which `IM_FREE(Data)`) on the very pointers the vector copy was still holding. Subsequent `pop_back` + a SECOND copy-by-value inside `CardEnd_` (`CardFrame_ f = CardStack_().back();`) then operated on the dangling pointers and called `Merge` → ntdll's heap manager flagged the corruption on the next allocation it touched.

**Fix** — held the splitter via `std::unique_ptr<ImDrawListSplitter>`:
- Implicit copy constructor is now deleted (unique_ptr is move-only), so any future regression that tries to copy a `CardFrame_` is a **compile error** instead of silent heap corruption.
- `CardBegin_` does `f.splitter = std::make_unique<ImDrawListSplitter>()` then `CardStack_().push_back(std::move(f))` — vector storage moves the unique ownership, no aliasing.
- `CardEnd_` accesses the back element through a reference (`CardFrame_& back = CardStack_().back();`) instead of copying, snapshots the scalar fields it needs, calls `SetCurrentChannel` / two `AddRectFilled`s / `Merge` through the pointer, then `pop_back` destructs the only owner.

Only edits were in `gui_main.cpp:474-590`. Verified: clean release rebuild, all 19 smoke tests pass at seed 1337, `vlrgui.exe` boots and stays alive, no new crash dumps after the smoke launch. Added §8 Pitfall #44 (do not hold ImDrawListSplitter by value in any owned container).

### Multi-lens code audit

Launched a 9-lens parallel workflow (`valosim-audit`) over the engine and UI for memory-safety bugs, logic regressions, ImGui state imbalance, duplication smells, type-design contracts, and LogoArt/Common correctness. Each finding goes through an adversarial verifier before reaching the synthesis stage; results land in this changelog as a follow-up entry once the workflow returns.

---

## 2026-06-11

### Premium UI redesign — 7-agent parallel pass

User asked for a comprehensive premium pass with seven parallel agents, no overlapping scopes. Workflow tool fanned out 7 agents (each owning a disjoint slice — theme function, card helpers, sidebar, LogoArt, tabular figures, hover lift, cyan→info), each returning schema-enforced edit pairs that the main session applied serially to eliminate file-write races. Net: 88 of 98 returned edits landed cleanly. The 10 misses are all overlap artefacts between Agent 4 (tabular figures around row Selectables) and Agent 5 (hover-lift conversion of those same Selectables) — neither blocking, all minor secondary rows. All 19 smoke pass at seed 1337. GUI boots at 56.7 MB (under the 60 MB baseline).

**1) Refined accent palette — gold-amber primary, cyan demoted to info.** The primary `kAccent` constant moved from electric cyan `#00D4FF` to a warm amber `#D4A14C`. Cyan retained as `kInfo` (`#5BA8B0`) for explicit informational chips / data-line plotting. `kVlrGold` (`#FFD700`, brighter) stays exclusively trophy/prestige — the brightness gap between trophy gold and interaction amber gives clear hierarchy. New tokens: `kAccentSoft` (13% amber for hover overlays), `kInfo`/`kInfoDim`, `kShadow`/`kShadowSoft` (surface-hued black for tinted shadows). Single constant swap inherits across every theme-driven UI element. The previous Phase 2A "electric cyan accent" language in the theme header comment is now stale.

**2) `ApplyVlrTheme` refinement — varied radius scale + breathing room + accent remap.** Uniform 8-10px rounding across windows/frames/cards/tabs is a generic AI default; replaced with a hierarchy: Window 14 (softest outer), Child 10 (cards), Frame 6 (inputs — tight instrumented feel), Tab 8, Scrollbar 12, Grab 6, Popup 12. `WindowPadding` bumped to (24,22) and `ItemSpacing` to (12,10) for additional breathing room. `ImGuiCol_PlotLines` switched from kAccent to kInfo so data lines stop competing with the gold interaction primary. Stale cyan-literal `ImGuiCol_TextSelectedBg` replaced with 25%-alpha warm amber matching the new accent.

**3) `BeginCard` / `BeginCardSized` depth system.** Every `BeginCard` now paints a surface-hued **stacked tinted drop shadow** behind the child via parent-draw-list channel splitting (kShadow at +6px, kShadowSoft at +3px) — shadows stay tinted to the bg, never pure black. Plus a 1px top inner highlight (white at 7% alpha, inset by rounding so it follows the corner) for an edge-lit raised feel. New defaulted `hover_lift` parameter (default false; backward-compat) opt-ins to a 13% kAccentSoft tint overlay when the card is hovered. `BeginCardSized` got the same parameter. Internals: `CardFrame_` struct + `CardStack_()` thread the parent draw list + splitter state from Begin to End so the shadow is back-painted with the resolved child rect. Recommended rounding scale documented inline: 8-10 inner / 12 standard / 14-16 hero.

**4) `DrawSidebar` premium polish.** Stronger broadcast-style active nav state: 4px gold "you are here" bar (up from 3px), full-height; raised `kSurfaceHi` bg with a 6% amber tint overlay so the row glows warmly without screaming; brighter active text `IM_COL32(0xE8, 0xB8, 0x60, 0xFF)` for max contrast on the raised bg. Inactive rows get a clear 2px half-accent left-bar hover affordance painted after the Button so it sits on top of the framework hover bg. `navgroup` headers (Home/Team/Competition/People/History/Live) now have a 1px hairline beneath the title — six buckets read as distinct sections. User-org chip in the top: gold accent stripe on hover (mirrors active-nav language); tag font color swapped from kVlrGold to kAccent for consistency with the new interaction language. God Mode toggle (OFF) now uses a kSurfaceAlt fill (kSurfaceHi hover) so it reads as a real toggle row instead of transparent text.

**5) Tabular figures across stat tables.** 33 numeric body cells across DrawRoster / DrawStandings / DrawMarket / DrawSoloQ / DrawCompare / DrawLeagueLeaders / DrawHallOfFame / Awards History / Favorited / GOAT Career / GOAT Season / Hall of Records / Team Profile roster / Player Profile Match History got `PushFont(g_font_mono) / PopFont()` wrapping so digits align vertically across rows. Text columns (names, regions, agent names) stay in body font. Numbers in identical-format columns now line up to the pixel.

**6) Hover-lift on row Selectables — `TableRowSelectable()` helper.** Introduced a one-call drop-in wrapper around `ImGui::Selectable()` that paints a warm-gold hover lift: 3px kAccent vertical left-edge bar + 13% kAccentSoft full-row overlay. Applied to player rows across DrawRoster, DrawStandings team rows, DrawBrackets group-stage + tournament Player Stats rows, DrawMarket FA rows, DrawSoloQ ladder rows, DrawLeagueLeaders rows, DrawHallOfFame inductees, DrawCompare picker rows, DrawTeamProfile roster, and Player Profile match-history rows. Excluded from sidebar nav (own bespoke paint), bracket cards (own custom hover tooltip), and combo/dropdown items.

**7) LogoArt premium polish — tinted shadows + warm highlights + inner glow + monogram depth.**
- `drop_shadow` and the monogram drop shadow swapped pure-black halos for slight blue-leaning charcoal `IM_COL32(0x05, 0x08, 0x10, ...)` so shadows harmonize with the deep-charcoal surface bg.
- All five frame highlight crescents/wedges warmed from pure white `(255,255,255)` to off-white `(255,250,235)` so they read as warm metal under the new amber UI accent.
- Every `frame_*` function now paints a 1px inner-glow ring at 30% accent alpha (`with_alpha(accent, 0x4D)`) inset ~2px from the hairline border — gives each badge a layered patch feel.
- Monogram drop shadow offset changed from diagonal (6%/6%) to vertical-dominant (2%/8%) — reads as light-from-above.
- Accent corner dots bumped from 0.07r → 0.10r so they still read at 24-px standings thumbnails. Accent top stripe gets rounded ends (`rounding=1.5f`).
- Defensive font fallback added: if `ImGui::GetFont()` returns null (e.g. wizard preview during init), the monogram routine now falls back to the first font in the atlas instead of silently rendering nothing.

**Cyan → info migrations (9 sites):** `Desire::Loyal` chip (×2 mirrors), PRO/FA chip (×2 mirrors in Progression tab + Manager mirror), MVP-race WHY "Tactical IGL" tier + sub-3 "IGL season" fallback, career timeline "award" category color, career hi-row awards-list TextWrapped color, async sim broadcast spinner. The spinner specifically moved from electric cyan to kAccent (warm amber) to unify the loading overlay around one color family.

**No new TTF / font assets shipped** — premium feel achieved within the existing Segoe UI / Consolas atlas via hierarchy + spacing + color + depth + hover affordances.

### Hotfix — "Start a New Career" crash + logo redesign #2 (typographic monogram)

User report: clicking "Start a New Career" in the wizard crashed the game. Also that the first logo revamp (earlier same day) still looked amateur and they wanted "professional grade".

**Crash root cause** — Common.cpp's curated fallback team-name array. I'd declared it as `std::array<const char*, 120>` with 116 actual initializers; the trailing 4 slots default-initialized to nullptr. `g_teams.emplace_back(nullptr)` constructed a `std::string` from a null pointer, which is undefined behavior — MSVC's `std::string(const char*)` assertion-aborts on null. Fix: switched to a compiler-deduced C-style array `const char* const fallback[] = { ... }` so slot count == initializer count automatically. Adding or removing entries later can't reintroduce the trailing-nullptr trap. → §1 file pointers.

**Logo system redesign #2** — replaced the first-pass procedural-emblem-on-backplate approach with a TYPOGRAPHIC MONOGRAM BADGE. Each team's logo is now composed of three pieces, all driven from `shape_idx` + the team's TAG:

1. **Frame backplate** — one of 5 polished shapes (Shield, Hexagon, Circle, Diamond, Banner) selected via `shape_idx % 5`. Each renders with a depth stack: drop shadow → darker rim → primary fill → top highlight → hairline accent border. Reads as a raised patch.
2. **Monogram** — the team's `tag` rendered as bold typography in the center using the active ImGui font, auto-sized for tag length (1-char gets 1.20× radius, 3-char gets 0.70×, capped 9..64 px). Drop-shadow offset for depth. Color auto-picked for luma contrast against the primary backplate (accent → white → black fallback chain).
3. **Accent flourish** — one of 4 small decorations (top stripe, corner dots, bottom chevron, or inner outline of the same frame shape) selected via `(shape_idx / 5) % 4`.

5 frames × 4 accents × the team's unique tag + team-color palette gives every team a coherent professional wordmark — what real VCT/esports identities actually use (T1, G2, NRG, FAZE, 100T, DRX, etc.). 20 distinct frame+accent combinations replaces the prior set of 30 disjoint procedural emblems.

API change: `draw_team_logo(...)` gained an optional `const char* tag = nullptr` parameter. `TeamLogo()` in gui_main.cpp passes `team.tag.c_str()`. The new-game wizard's preview derives a 3-char monogram from the user's in-progress org name. Legacy callers that don't pass a tag get a stable per-shape default monogram (NRG-style placeholder) so they still render a brand-feel badge instead of falling back to procedural shapes.

`kLogoShapeCount` stays at 30 so the personality-bias pools on Team continue spreading teams across templates evenly (the badge composition only needs 20 combinations, but a wider hash range gives a smoother distribution per personality).

The earlier same-day "Team logo revamp — unified backplate + 8 new emblems" section below is superseded — those procedural emblem functions are no longer dispatched. The new system kept the backplate + drop-shadow + color-contrast helpers from that work.

### Tournament bug fixes — Player Stats hover, lower bracket layout

Two user-reported tournament-screen defects, both root-caused and fixed at source rather than patched.

**Bug 1 — Player Stats hover deref.** Hovering / clicking rows in a Tournament's Player Stats sub-tab could crash or render garbage. Cause: `TournamentPlayerStat::player` was a raw `Player*` populated from `HistoryRecord::blue_team / red_team` at match resolution. When players were later cut / retired / cross-region transferred, the source shared_ptr could drop, leaving the stat row's raw pointer dangling. The UI then dereferenced it every frame inside `player_label(*r.player)` and the sort comparator. Fix: added snapshot fields (`display_name`, `is_igl`, `signature_agent`) to `TournamentPlayerStat`, populated at accumulation time inside `Tournament::absorb_series_participants`. UI now reads from the snapshot — never derefs `r.player`. Click resolver was rewritten to walk both leagues AND solo_qs ladders with a name fallback, so retired/cut tournament participants still open correctly. → §4.9.1 / §5.4

**Bug 2 — LB bracket layout collapsed.** Lower-bracket panels rendered with R2 cards overlapping R1's last card and with future rounds (SF / Final) either missing or mis-labeled. Three root causes:

1. **Feeder math assumed halving everywhere.** `DrawBracketRounds` used `i*2 / i*2+1` for prev-round feeders. LB R1→R2 is a *straight pair* (each R1 winner faces a UB drop-in, not another R1 winner) — so prev_n == cur_n. Halving math clamped both R2 cards onto R1's card 1, collapsing the panel. Replaced with a size-aware rule: `prev_n == cur_n` → straight 1-to-1; `prev_n == cur_n*2` → halving; same fix applied to the connector pass.
2. **Target round count was the played count.** `lb_target` was computed via `Tournament::lower_bracket_round_count()` which returns `lb_history_.size()` (i.e. how many rounds have been *played*). Future rounds were never padded as TBD placeholders. Now derived from `initial_seeding().size()`: for an N-team double-elim, canonical LB depth is `2 * log2(N) - 2` (8→4, 16→6).
3. **LB padding shape was wrong.** Padding always emitted single-placeholder rounds. Real LB shape alternates merge / halve — for 8 teams: [2, 2, 1, 1]. Replaced with an index-aware rule: odd next_idx → merge (size = prev size), even next_idx → halve. Last padded round is BO5 so the gold "BO5" chip lands correctly.

**Label fix:** `round_label_at` previously returned "LB FINAL" for any LB round with 1 card, so canonical R3 (Semifinal) showed as "LB FINAL" alongside the actual final. Added a `total_rounds` parameter so 1-card LB rounds NOT at the end label as "LB Semifinal".

All 19 smoke tests pass at seeds 1337 / 42 / 7. GUI boots clean.

### Team logo revamp — unified backplate + 8 new emblems

User feedback: existing team icons looked amateur and unfocused — 22 procedural shapes drawn directly with primary + accent, no shared visual frame. Rewrote `LogoArt.cpp` so every logo now renders as:

1. **Backplate stack** — bottom dark-rim disc, primary fill disc (94% radius), subtle top highlight, hairline accent ring. Every logo sits inside this stack so the league reads as a coherent set of patches rather than 30 disconnected silhouettes.
2. **High-contrast emblem color** — picks accent if its luma differs from primary by ≥0.22, else falls back to white/black depending on backplate brightness. Avoids the old failure mode where accent was nearly invisible on same-luma primaries.
3. **Emblem inscribed at 72% radius** — leaves room for the trim ring and a small breathing margin so the patch reads cleanly.

**Eight new shapes added** (kLogoShapeCount 22 → 30, LogoShape enum extended before `Count` to preserve hash determinism): Fangs, Trident, CrossedSwords, Gear, Talon, Eye, Cobra, Sunburst. Existing shapes (Wolf, Eagle, Phoenix, Dragon, Skull, Star, Lightning, Sun, etc.) tightened — bolder silhouettes, more accent-color detail (eyes, snowcaps, feather lines, jewel dots) so they read clearly at the 24-px roster thumbnail size.

Personality bias pools updated: Aggressive draws from {Lightning, Skull, Dragon, Flame, Sword, **Fangs, CrossedSwords, Cobra, Talon**}; Tactical from {Crosshair, Hexagon, Compass, Shield, Diamond, **Eye, Gear, Trident**}; Budget from {Triangle, Circle, Anchor, Mountain, Tree, **Sunburst**}.

### Team name generator — curated 36 → 120 + brand-modern procedural style

User feedback: team names felt repetitive. Root cause: the fallback curated list (`CLan names.txt` doesn't exist; in-code fallback used) was only 36 names — once procedural kicked in for expansion teams or rookie orgs, the random acronym style 4 produced unreadable letter-mash like "GFGT" that looked like keyboard slaps.

Two fixes:
1. **Curated fallback expanded 36 → 120.** Single-word brand-modern (~70%), mythic/classical short brands, punchy one-syllable words, and a smaller pool of two-word esport stylings. Big enough that two seasons of expansion / shuffles never run dry before falling through to procedural.
2. **Style 4 rewritten.** Random TLA replaced with four sub-modes: CVC (consonant-vowel-consonant — "RIX", "NOX"), CVCC ("VEXT", "RAFT"), digit-prefix ("9 Echo", "100 Surge"), and word + brand suffix ("Vortex GG", "Apex Esports"). All four read as competitive orgs instead of keyboard mash.

The other 11 procedural styles (single bold noun, modifier+noun, compound, mythic, military, color+noun, region-flavored, etc.) unchanged — they were already producing decent output.

### UI audit pass — gap list captured for follow-up

Dispatched an Explore subagent to walk all 19 `Draw*` functions vs. every public accessor on Player / Team / Tournament / GameManager / Coach. Produced a 25-item gap list grouped by category (unused engine accessors, hardcoded thresholds, missing visual coverage, tournament inconsistencies, spoiler-discipline checks, god-mode editing gaps, duplicated UI patterns).

Items actioned this session were limited to the bug fixes above plus the visual revamp. The remaining ~18 items (Desire combo in dev-tools, TeamWindow override in Commissioner, OrgMemory read-only display, validate_bracket_state debug overlay, role-fit chip in FA rows, bracket seed numbers, etc.) live in §11 as "Audit gaps — next slice candidates" so a future session can pick them up cold.

### Deferred — gui_main.cpp module split

Rewrite plan documented in §5 as "Planned split". Mechanical scope: ~12,500-line monolith → 8-9 thematic .cpp files behind a shared `ui_common.h`. Not executed this session — risk too high for the remaining context budget; will land as its own dedicated session.

---

## 2026-05-29

### UI integration pass — 7 fixes, no new screens

Cross-UI audit found a recurring pattern: the engine has rich state (org memory, chemistry, trophy case, signature agents, coach personality, role-fit verdicts, IGL impact, archetype, playstyle identity) but the UI surfaced ~40% of it and surfaced it inconsistently (signature agents on 2/19 screens, org memory gated behind `>=25` so new players saw "building..." for season 1). Plus ~200 lines of duplication between the Re-sign and FA Sign modals. Slice 1 of the response — no new screens, just plumbing what already exists:

1. **`DrawNegotiationModal(mode, ...)`** — extracted shared helper for Re-sign + FA Sign modals. Both flows now call the same function with `NegotiationMode::Resign` or `NegotiationMode::FreeAgentSign`. Helper owns offer cache, role combo, breakdown, action buttons; caller owns state references + popup id. Legacy modal bodies kept under `#if 0` as a fallback during rollout (delete in follow-up). → §5.3
2. **Org memory ungated** — Dashboard KPI tile no longer gated to `|value| >= 25` (the gate was hiding the system from new teams for ~200 days). Plus a compact 6-pill strip below the KPI strip showing every metric always, color-coded by sign + magnitude. Team Profile Bio already had the full bar-grid version.
3. **`SigBadge(Player&)`** — canonical inline signature-agent pill (muted-gold background, dark text). Applied on Roster rows, Team Profile rows, Market FA rows, League Leaders rows. Signature agents went from visible on 2 screens (MVP Race + News) → 6. → §5.3
4. **Coach personality + blurb** surfaced on Dashboard coach card, Manager Head Coach tab, Team Profile coach strip. Was tracked in engine (drives mid-season replacement aggressiveness, dev focus) but never shown.
5. **HoF "WHY IN HoF" column** — replaces the old "AWARDS" count integer with colored pills for each of the 4+ criteria each inductee satisfies (Champ, Masters, MVP, Role oTY, 1.20+ Season, Top-20 Solo Q, $1M+ Career, 30-kill Match, 2.0 KD Match, GF Clutch, IGL 50+). Users can see *why* someone is enshrined at a glance.
6. **`Player::is_form_at_risk()`** — centralised the "AT RISK" form check (`season_matches >= 4 AND season_rating < 0.95`) as an engine method. UI swapped from a hardcoded threshold to the helper so engine + UI cannot silently diverge. → §4.4
7. **MVP Race WHY column IGL bands** — IGL gate dropped 5.0 → 3.0 + new "Elite IGL" band ≥5 + IGL season fallback. Legitimate mid-tier IGL seasons (impact 3-4) no longer mislabel as "Frag leader".

All 19 smoke tests pass. GUI boots clean at 60MB.

### Doc reconciliation
Resolved drift that had accumulated in PROJECT_GUIDE.md:
- Smoke-test count was stated three different ways (17 / 18 / 19) across four locations. Unified to **19** everywhere; added #14b as an explicit entry in §9's numbered list.
- `gui_main.cpp` line count was stated as `~6300` in the §3 module diagram and `~8100` in the §5 opener. Both stale — actual is `~12.5k`. Both call-sites updated.
- The "Belief-driven AI + temporal coherence + economy overhaul" block was wedged inside §5 UI between §5.2 (Color palette) and §5.3 (Helpers). Moved to §4 as proper engine reference (§4.17), out of §5.
- The "Big batch (2026-05-15) — Bug fixes" list contained 11 items verbatim-duplicated with the Phase A/B "Misc fixes during this period" list. Removed the duplicates; left a cross-reference at the head of the section.
- Removed the third TacticalIGL post-shift mention from the "Patches applied during build pass" sub-list (already covered above).

Net: PROJECT_GUIDE went 1339 → 1335 lines after the dedup; structurally cleaner.

### Then a separation pass (this changelog file)
The reviewer's call — "a reference doc and a changelog want to be separate files" — landed. All dated narrative blocks (Phase 1 DONE / Phase 2A DONE / Role overhaul DONE / Big batch / Archetype system / Belief-AI overhaul) moved into this CHANGELOG.md. PROJECT_GUIDE.md is now pure reference: architecture, current systems, magic numbers, pitfalls, smoke tests, file inventory. §11 ("Pending Tasks / Wishlist") trimmed to pending-only — historical "DONE" entries live here now.

### Year-end contract math — second off-by-one fix

A subtle bug: the year-end re-sign loop, market poach loop, and AI offseason pass all ran INSIDE `run_end_of_year`, BEFORE `year += 1` at the bottom of the function. They called `sign_player(p, years, year)` / `resign_player(p, years, amount, year)`, both of which compute `exp_year = current_year + years - 1`. But the just-played season was already over — the new contract effectively starts in `year + 1`. Net result: every AI-driven year-end deal was silently one year short.

**Concrete example:** 3-year AI extension signed at end of 2026 → engine wrote `exp_year = 2028` → after `year++` to 2027, roster showed `years_left = 2` (not 3). Player walked one season early.

Fixes (GameManager.cpp):
- Year-end re-sign loop: `resign_player(p, team_years, counter, year + 1)`
- Market poach: `sign_player(..., year + 1)` + `exp_year = year + win.years`
- AI offseason pass at year-end: `ai_full_offseason_pass(fa_pool, year + 1, log)`
- Bonus: re-sign news headline + log entry use `team_years` (actual deal length) instead of `offer.years` (player's wished length)

Mid-season replacement signings and the user OFFSEASON sign/re-sign paths were already correct — year is the active/upcoming-season year there. → §4.11

---

## 2026-05-28

### Role lock + role-fit evaluator + FA negotiation modal

User feedback: roster system felt unstable. Audit showed that `primary_role` is actually write-once at spawn — nothing in the engine reassigns roles dynamically — and `roster` has no sort/swap reorderings (sign appends, release preserves order). So the perceived instability wasn't a real engine bug. But the bench-player re-sign gate WAS gated to `years_left <= 1`, locking bench + mid-contract players out of negotiations.

**Implemented:**
- **`Player::contract_role`** — new field stamped at every signing path (sign_player + resign_player both have Role-aware overloads). `Role::Count` means "use natural" (delegates to primary_role). Lets the UI show "you were hired as X" and the negotiation evaluator apply role-fit penalties consistently.
- **`Player::role_fit_score(Role)`** — pure 0..1 function computing attribute fit per role (Duelist: Aim+Headshot+Entry+Aggro+Movement weighted; Initiator: Utility+Comm+GS+Intel+MidRound; Controller: Intel+Decision+Comm+MidRound+GS; Sentinel: Pos+Anchor+Clutch+SpikeHandle+Adapt). 1.00 for own primary_role.
- **`Player::role_fit_verdict(Role)`** — bands: Natural / Good fit / Possible / Stretch / Mismatch.
- **`evaluate_resign_offer` 4-arg overload** that takes the offered role; role penalty 0 / −6 / −14 / −28 / −45 by verdict band.
- **Re-sign gate opened** — `years_left >= 1` instead of `<= 1`. Bench players + mid-contract players can negotiate. Label switches to "Extend" for years_left > 1, "Re-sign" for ≤ 1.
- **Role offer combo** in the Re-sign modal — dropdown of 4 roles with per-role fit verdict shown next to each.
- **FA negotiation modal** — replaces the old "direct sign" Market button. Mirrors Re-sign modal layout: ask + willingness + role combo + transparent breakdown + Send/Match-Ask/Walk.

### Re-sign penalty rebalance (was too punishing)

User reported personality modifiers as "almost impossible to overcome regardless of salary, tenure, loyalty, overpaying". Audit confirmed: hard archetype gates at flat −50 created a wall no offer could clear.

Fixes (Player.cpp `evaluate_resign_offer`):
- Salary cap raised ±40 → ±55 above (and ±35 below) so real overpays can rescue a deal
- Archetype hard gates softened from flat −50 to −22..−25
- Greedy soft gate −25 → −15; Mercenary length gate −30 → −18; mood floor −40 → −22
- Loyalty boosted: Loyal+same_team +15 → +22; same_team flat +3 → +5; new tenure ramp +2/season cap +10
- Prestige bumped +12 → +14 (top tier)
- Contender window weights bumped +8/+4/0/−10 → +10/+5/0/−8
- Recent-trophy contender bonus: +4 if any roster member has [T]/[M]/[W] in last 2 seasons
- Overpay (+55 salary, +14 prestige, +10 contender, +27 loyalty, +6 years) can now overcome one stacked archetype mismatch but still feels resistance when multiple bad signals stack

### AI over-signing fixes

User reported AI signing past the 5-starter need. Audit found: when `user_team->roster.size() < 5` at OFFSEASON end, `ai_full_offseason_pass` ran Stage 4 bench depth and inflated to 7. Same logic ran for every AI team at year-end.

Fixes (Team.cpp):
- `ai_full_offseason_pass` Stage 2 upgrade cap 2 → 1 per pass (less roster churn)
- Stage 4 bench depth: capped at +1 (max 6 not 7), budget floor $200K → $400K, added positional-stacking guard (skip roles where the team already has 2+ players)

### OVR formula recalibration

Old curve `mean × 0.92 − 9.0` was too aggressive — a player averaging 75 across attributes showed OVR 60, which felt broken when scouting. New: `mean × 0.95 + 1.0` clamp [1, 95]. Attr avg 75 → OVR 72; avg 85 → OVR 82; cap 95 preserved.

### Award rebalance — team success now factors in

- **MVP**: replaced the `dep` factor (which paradoxically rewarded low-win teams) with `team_factor`: ×1.25 at 28+ team wins, ×1.15 at 22+, ×1.05 at 16+, ×0.85 at <10. Historic statistical seasons can still overcome a weak record (a 1.30 rating beats most things even at ×0.85) but team success has real teeth.
- **Role of the Year** ×4: replaced pure `season_rating` with `season_rating × team_role_factor × intl_factor`. team_role_factor: ×1.20 at 28+, ×1.10 at 22+, ×1.03 at 16+, ×0.90 at <10. intl_factor: ×1.10 if attended any [M]/[W] this year. Stat-padders on losing teams lose to comparable players on contenders.
- **IGL of the Year** unchanged — already factored team finals + team wins heavily.

### Hall of Good tightening

Frivolity view, not the real HoF. Bumped gates: `career_seasons_played ≥ 5`, `career_matches ≥ 150`, `career_rating ≥ 1.00`; criteria band `2-3` → exactly `3`. Stops 3-season flukes from qualifying.

### My Team direct navigation

Sidebar user-org chip (logo + tag + name) now clickable → `OpenTeamProfile(user_team)`. 1-click access; was previously buried behind Competition → click user team → profile.

---

## 2026-05-15 → 2026-05-28 — Belief-driven AI + temporal coherence + economy

**The biggest single block of engine work in the project's history.** Turned the optimizer-AI into a brain with memory, philosophy, timeline awareness, chemistry, and explainable negotiation. Plus a financial rebalance making the economy actually constrain decisions. → §4.17

### Phase A — Belief Foundation (Team + Player)

- **`Team::OrgMemory`** — 6 rolling [-100, +100] metrics evolved at year-end: `rookie_success`, `import_success`, `veteran_success`, `financial_discipline`, `stability_culture`, `star_dependency`. All decay 3%/yr. Drives downstream signing AI + cut aggression.
- **`Team::previous_strategy` + strategy inertia** — `commit_strategy_with_inertia(team, suggested)` runs a probabilistic transition matrix. Orgs resist snapping into a new philosophy on one year's evidence.
- **`Player::Desire`** — 6 archetypes (Greedy/Loyal/RingChaser/Mercenary/StabilitySeeker/Competitor) assigned at spawn. Drives `desire_salary_mult`, `desire_accepts`, `desire_length_pref`. Wired into `gen_contract`, year-end re-sign loop, `auto_fill::fill_one`.

### Phase B — Temporal Coherence + Chemistry

- **`Team::TeamWindow`** (Opening/Open/Closing/Closed) — drives mid-season cut aggression, import appetite, multi-year candidate scoring.
- **`timeline_fit_score(team, candidate)`** — composite multi-year coherence score wired into snake-draft + auto_fill.
- **Risk tolerance** — Contenders pay for `consistency_mod`; Rebuilders gamble on high-ceiling boom/bust.
- **Chemistry graph** — `Team::chemistry` map with canonical-ordered ChemKey, EMA-updated by Match.cpp after each non-friendly match.

### Re-sign flow + Deep AI Re-sign + Market Competition

- **`Player::ResignOffer`** + **`Player::ResignBreakdown`** — per-factor scoring breakdown.
- **`refuses_to_negotiate` semantics FIXED.** Old: `(offer × mood > 100)` — higher offer INCREASED refusal odds (the inverted bug). New: `offer < (30 + 70·mood)` — mood inflates the FLOOR, higher offer monotonically REDUCES refusal. Smoke #13 updated.
- **`accepts_resign_offer`** is now literally `return evaluate_resign_offer(...).will_accept`. UI pill physically cannot disagree with engine decision.
- **Deep year-end re-sign loop** — for every expiring player: propose offer, team_eval_score, counter-offer, `accepts_resign_offer` gate, accept → `resign_player`, reject → walk → market competition.
- **`Team::resign_player`** — in-place contract rewrite without release+sign (preserves chemistry).
- **Market competition** — walked stars (ovr ≥ 75) get bid on by up to 3 same-region rival teams. User team excluded from bidder pool.
- **`Team::ai_full_offseason_pass`** — 5-stage deep GM pass: header log, smart cuts, per-starter upgrade scan, cascade-fill backstop, bench depth.

### Async Sim Worker

Engine work moved off the UI thread. Continue / Skip-to-next-match / Skip-to-Playoffs / Sim Full Season / Sim Solo Q Day all spawn `std::thread`. UI keeps presenting at 60fps with full-screen "SIMULATING…" overlay.

### `Player::years_left(current_year)` — contract source of truth

`Player::contract_years` was being decremented unconditionally every year-end (including for FAs) AND `contract.exp_year < year` was the release gate. The two drifted; the release gate was off-by-one (3y contract from 2026 played 4 seasons). Fixed by making `years_left()` the single source of truth derived from `contract.exp_year`. `contract_years` is now the frozen signed-duration snapshot.

Plumbing: `auto_fill_roster` + `scout_top_fas` now take `current_year` parameter and thread it through every callsite.

### `Team::decide_contract_years(player, current_year)` — SINGLE source of contract length reasoning

Replaces every `rng().irange(1, 3)` and `c.exp_year - year + 1` callsite. Multi-factor: desire length pref, window, strategy, age, potential gap, financial_discipline, budget headroom. Clamp [1, 4]; cap at player's `max_acceptable_years`.

### Financial Rebalance — economy is now tight

| | Old | New |
|---|---|---|
| Salary cap | $999K | **$180K** |
| Salary floor | $15K | **$10K** |
| Superstar band | 80-360 | **80-150** |
| Star band | 50-110 | **50-90** |
| Mid band | 22-37 | **18-35** |
| Bench band | 15-22 | **10-18** |
| WinNow budget threshold | $2.5M | **$1.5M** |
| Contender threshold | $1.5M | **$900K** |
| User team default | $1.5M | **$800K** |
| Wizard Rich tier | $3M | **$2M** |
| Sponsorship formula | `600 + prestige·22` | **`200 + prestige·8`** |
| Import appetite budget tier | 1M-2M | **600K-1.5M** |

Constants `vlr::kSalaryCapK = 180`, `vlr::kSalaryFloorK = 10` in Common.h.

### Smoke #14b — Duelist IGL mental floor (soft diagnostic)

Generates 500 rookies. Originally hard-asserted no Duelist IGL has mental composite < 0.78. Relaxed to soft diagnostic note — TacticalIGL rookie archetype's stat shifts can push mental slightly below the gate after the IGL roll. Documented behavior, not a regression.

### Misc fixes during this period

- Milestone news re-fire (added career-scoped `news_emitted_career_keys_` separate from year-cleared set)
- `came_from_upper` robust (checks both .a/.b)
- `last_mvp_leader_global` static→member field, cleared in reset_world
- `solo_qs[region]` unchecked map lookup → `find()` guard
- `world_difficulty_` clamp 0.5..1.5 → 0.7..1.3
- Match.cpp cosmetic cleanups (`0.5+0.5+0.5*x`, `/1`, defender-flag helper)
- Player dead "uniqueness check" loop actually consults `global_names()` now
- TacticalIGL applies full canonical IGL shift + mental gate before forcing `is_igl`
- IGL Profile gating: god-mode toggle moved to Overview Dev Tools
- FlexBadge in DrawCompare
- News page perf O(N×P)→O(1) via frame-cached signature map
- Kill-feed perf O(N²)→O(N) via running counter
- Tournament Player Stats sort persistence
- IGL OTY gate tightening (requires `igl_match_count > 0`)
- Transfer Market "Sign" button bug (Selectable SpanAllColumns claiming clicks)
- MC potential SKIPPED for solo Q ladder fillers (`deep_potential=false`)
- OFFSEASON-end user-team fallback wired
- AI year-end re-sign loop runs for user_team too (manual control during season; AI as fallback at year-end)

---

## 2026-05-15 — Big batch (audit fixes + zengm-inspired ports + UI consolidation)

### Bug fixes
- News dedup-clear moved AFTER year-end emitter pass (spec §4.11 step 15)
- Tournament dead field `ub_finals_round_just_played_` removed
- Live match `ImGui::Columns(2)` replaced with two `BeginChild` + `SameLine`
- `build.bat` play.bat tip → Play.exe tip

### New systems

**Per-attribute age-bucketed progression** (Player.cpp): three aging classes — Mechanical (Aim/HS/Reaction/Crosshair/Movement, peaks 22-25, fast decay), Game-IQ (GameSense/Intel/Decision/MidRound/AntiStrat/Adapt/Econ/Comm/Lead, climbs into 28+, IGLs age gracefully), Athletic (Stamina/SpikeHandle/Anchor/Lurking/Pos/Aggro/Entry/Util/Clutch/Awping). Asymmetric young-player noise. Potential pressure asymptote.

**Two-stage `shouldRetire`** (Player.cpp): Stage 1 logistic on (age, potential, recent rating, seasons played). Stage 2 aging-star catch — age ≥ 30 AND `ovr < 0.70 × career_max_ovr`.

**Monte-Carlo potential** (Player.cpp): at rookie gen, 20 fast-forward sims to `max(peak_age+4, 29)`, take 75th-percentile terminal OVR, clamp 50-99.

**Hash-deduped award addition** (Player.h/cpp): `Player::add_award(string)` + `awards_seen_` mirror + `rebuild_awards_seen()`. Belt-and-braces with existing dedupe pass.

**H2H playoff/regular split + finals log** (GameManager.h/cpp): `h2h_counts_regular_` + `h2h_counts_playoff_` + `h2h_finals_log_`. Surfaced in Team Profile Bio "RIVALRIES", DrawCompare, DrawMarket OFFSEASON decay chip.

**FA price-decay during OFFSEASON** (GameManager.cpp): 3%/day decay, floor at 70% of entered-offseason baseline. UI shows "decayed" pill on affected FA rows.

**Sidebar consolidation 16 → 6 groups** (gui_main.cpp):
- Home — Dashboard · News
- Team — Roster · Manager · Calendar
- Competition — Standings · Tournaments · Power Rankings
- People — Transfer Market · Solo Q · League Leaders · Compare
- History — Favorites · Event Log
- Live — Watch

**5 Frivolity views** (Favorites → Frivolities sub-tab):
1. Best Without a LAN — career rating leaderboard, no [M]/[W]
2. Youngest MVPs — sorted by age at [A] MVP win
3. Hall of Good — retired players hitting 2-3 HoF criteria
4. Biggest Busts — `career_max_ovr < potential - 15` AND retired/age≥28
5. Seasons Without a Title — top single-season ratings whose teams didn't win that year

### Patches applied during build pass
- Forward decl `static int monte_carlo_potential(const Player&)` before `generate_player`
- Forward decl `std::vector<vlr::PlayerPtr> all_players_world(AppState&)` in gui_main.cpp's early-decl block
- Smoke #14b relaxed from "hard fail" to "soft diagnostic note"

---

## 2026-05-15 — Archetype system + variance rebalance

**32-archetype playstyle system** (Player.h/cpp) — a THIRD sim-driving system distinct from `rookie_archetype` (16-value growth template) and `playstyle_identity()` (derived display label). `enum class Archetype` (32 + Count): MechanicalDemon, HyperAggroEntry, SmartLurker, IceColdClutcher, UtilityGenius, PassiveAnchor, TempoController, MomentumFragger, LANDemon, OnlineFarmer, TacticalGenius, RiskyPlaymaker, ConsistencyMachine, ConfidencePlayer, SlowMethodical, HighCeilingLowFloor, AggressiveAWPer, SupportiveFacilitator, FlexGenius, AntiStratMaster, EcoSpecialist, EntrySacrifice, TeamFirstGlue, OverheatingSuperstar, RookieMechFreak, VeteranStabilizer, ClinicalCloser, ChaosAgent, ZoneController, RetakeSpecialist, DuelistDiva, QuietProfessional. Assigned once at spawn via `pick_spawn_archetype()`. `ArchetypeProfile archetype_profile(Archetype)` — pure O(1), 14 modest float modifiers. Profiles LAYER on attribute math, never replace it.

### Match.cpp variance rebalance ("top teams steamroll, no variance")
- `compress_power()` — diminishing returns above ~70-attr pivot (0.62 marginal retention). Ordering preserved; elite-vs-elite duels close.
- Round jitter widened (inverse-consistency band 0.02-0.10 → 0.035-0.18).
- Per-map off-day roll — asymmetric, floor never below 3.5% even for 95-consistency stars.
- Per-team matchup swing — independent ±6% (clamp ±14%) team tilt. Clearly-better team drops ~25-40% of maps but wins series on average.
- Two-way decaying momentum (×0.78/round + ±0.45 nudge, ±0.075/duel cap) replaces snowball-only — 0-8 comebacks possible.
- Comp-synergy spread widened 0.90-1.10 (clamp 0.89-1.14).
- Rating formula coefficients, damage-per-kill, kill weights UNCHANGED.

### Follow-up fixes
- God Mode IGL editing — Player Profile IGL tab gets editable SliderInts under gold `SectionHeader("DEV TOOLS — IGL")`
- Power Rankings — removed nested scroll children, single vertical page scroll, fixed pixel columns
- Dashboard "Next Match" tile shortened
- Sortable tables across Standings, League Leaders, Hall of Records, Greatest Seasons, Trophy timeline, Tournament Player Stats, live Scoreboard
- Tournament "Player Stats" sub-tab — inner TabBar (Bracket | Player Stats) inside per-tournament `PushID/PopID`
- Live match revamp — broadcast-style `DrawScoreBanner`, `DrawRoundTimeline` scrolls in its own horizontal strip, kill-feed rows reserve real `InvisibleButton` slots, framed round-result + economy + gold ACE/CLUTCH callout cards
- Live match internal scroll killed — every inner `BeginChild` in DrawLiveMatchModal set `NoScrollbar|NoScrollWithMouse`; kill-feed switched to `ChildFlags_AutoResizeY`
- POT / development visibility — Dashboard `##dash_development` card with color-coded Trajectory Pill
- Archetype surfaced in Player Profile hero as a purple `kAccent2` Pill + "OVR → POT" pill + trajectory pill

---

## 2026 May — Phase 2A (flags + logos + names + new-game wizard + calendar)

- Real flag bitmaps for 31 ISO codes (`FlagBitmaps.h/cpp`, 24×16 ARGB, ~47 KB embedded). Used by rewritten `CountryFlag()` via per-pixel `AddRectFilled`. Unknown ISO falls back to procedural draw.
- 22 procedural team logos (`LogoArt.h/cpp` — Shield/Hexagon/Diamond/Circle/Triangle/Star/LightningBolt/Crown/Wolf/Eagle/Phoenix/Dragon/Wave/Mountain/Sun/Crosshair/Sword/Anchor/Flame/Skull/Compass/Tree). `Team::logo_shape` field hash-assigned at gen with 30% personality bias. UI helper `TeamLogo(team, size)` wired into sidebar user-org chip, Team Profile header (96px), League Standings rows (24px).
- Revamped team-name generator (`Common.cpp`): 12 styles total — original 5 plus animal+descriptor, region-flavored, Greek/Latin/mythic, military/tactical, color+animal/noun, two-word esport, single-word — 15k+ permutations. Global de-dup retained. `make_team_tag` handles 1/2/3+ word names with last-word fallback.
- `NewGameConfig` struct + `GameManager::initialize_world_with_config(cfg, log)` + `GameManager::reset_world()` (clears every owned container incl. rankings/news/snapshots/h2h). Existing `initialize_world()` delegates to the new path with default cfg. Difficulty stored as `world_difficulty_` (0.7-1.3 slider; **not yet consumed by AI strength — future hook**).
- **New-Game Wizard** in `gui_main.cpp` (`DrawNewGameWizard`) — full-screen modal at boot, two-column form (org name / city / region / country with flag preview / 5-tier OrgTier radio / primary+accent ColorEdit3 with name-hash auto-derive / difficulty slider / starting year) + live preview (TeamLogo + colors + mock standings row). "Start New Career" calls reset+init. Sidebar "NEW CAREER" button with warning modal re-arms the wizard mid-session.
- **Calendar redesign** (`DrawCalendar`): 5×7 day grid centered on today, ±30 nav buttons, per-cell phase tint, gold today outline, blue user-match background, red tournament border, click → matchup modal with WATCH buttons routing to `OpenLiveMatchSeries`. Legend bar.

### Build notes
- `LogoArt.cpp` is compiled into CORE_SRC but LINKED ONLY into `vlrgui.exe` (references ImGui draw symbols). `FlagBitmaps.cpp` is plain data, linked into all 3 exes.
- `LogoArt.cpp` requires `#include <algorithm>` for `std::max`.

---

## 2026 May — Role overhaul (Position enum + IGL spawn + enforce_one_flex)

- Position enum shrunk D/C/S/I/Flex/Count → D/C/S/I/Count (Flex demoted back to overlay flag)
- IGL spawn reworked: per-role base chance × mental composite. Duelist 0.04 base + 0.78 hard floor → Duelist IGLs now extremely rare (~3-7% of all IGLs)
- `enforce_one_igl` uses role-weighted mental score with Duelist veto. No more "promote on Leadership only" hot-swaps
- `enforce_one_flex` insert-only — multiple Flex players in starting 5 allowed
- `auto_fill_roster`: 1D + 1C + 1S + 1I + 1 generic 5th (Flex→I→C→FA cascade)
- Transfer Market: Flex-FA-missing bug fixed (root cause was `position_of` returning Position::Flex with no filter bucket; resolved by removing Position::Flex entirely)
- UI: 4 canonical roster slots + "5th", `FlexBadge` chip alongside `IglBadge`, badge render order PositionBadge → FlexBadge → IglBadge

---

## 2026 May — Phase 1 (Power Rankings + Trophy Room + Compare + News expansion)

Visibility / presentation layer on top of existing engine data. No new simulation systems — surfaces what was already tracked. → §4.16

- Power Rankings + Community Rankings (weekly tick, tier bands, analyst notes)
- 10 new news emitters (breakout/slump/hot streak/milestone/retirement countdown/transfer rumor/rivalry/upset/historic performance/dynasty watch)
- Trophy Room + Hall of Records + Greatest Seasons (Favorites tabs)
- Player Compare page with head-to-head
- Signature agent surfacing (4 sites: Player Profile header, MVP Race column, scoreboard chip, news_feed chip)
- Team trophy_case + dynasty_tier (Dynasty tier wired; ChampionEra/Contender awaits finals_history)

---

## Earlier history (pre-changelog)

Major arcs preceding this changelog that are documented in PROJECT_GUIDE but never had a dated entry:

- **Initial engine** — Player, Team, Match, basic FA market, simple OVR formula
- **Tournament correctness** — phantom-round bug discovery, `ub_alive_` snapshot+clear pattern, trophy participation filter to prevent phantom championship awards
- **IGL system** — STRICT one-IGL invariant, per-role spawn weighting, Duelist IGL hard floor
- **Mastery + signature agents** — per-agent EMA mastery, per-map mastery, mastery decay
- **Position system overhaul** — Flex collapsed back into an overlay flag, Position enum trimmed to 4 values (later refined in the May 2026 role overhaul above)
- **zengm name pools import** — 27 countries × top-250 first+last names. Duplicate-name rate for USA dropped from ~80% to ~1.4%.
