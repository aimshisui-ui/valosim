# Valosim — VLR Manager

> An esports management sim for Valorant. C++17 engine, Dear ImGui DX11 UI. Built to feel like a living competitive ecosystem — players, orgs, contracts, scouting, regional metas, year-over-year coherence.

This isn't a demo project. It's a deep simulation with belief-driven AI, organizational memory, multi-year team windows, chemistry graphs, transparent contract negotiation, and broadcast-style match presentation. Every system is wired into every other — sign a player and you'll see them in the news, the rankings, the scoreboards, the awards race, the hall of fame.

---

## Table of contents

1. [What this is](#what-this-is)
2. [Screenshots](#screenshots)
3. [Quick start](#quick-start)
4. [Build from source](#build-from-source)
5. [Repository layout](#repository-layout)
6. [Architecture at a glance](#architecture-at-a-glance)
7. [Engine systems in depth](#engine-systems-in-depth)
8. [UI systems in depth](#ui-systems-in-depth)
9. [Design philosophy](#design-philosophy)
10. [Tests](#tests)
11. [Project history](#project-history)
12. [PROJECT_GUIDE.md](#project_guidemd)

---

## What this is

You run a Valorant esports organization in one of three regions (Americas, EMEA, Pacific). The calendar advances day-by-day. Across each season:

- 3 stages of league play with regional playoffs between
- 2 Masters international events
- A year-end Champions tournament
- A 14-day OFFSEASON during which you manage roster, contracts, scouting, and finances

Behind the scenes, 900-player ranked ladders per region run nightly, feeding the FA pool. Players age, peak, decline, retire. Orgs accumulate organizational memory — "we tried that rookie pipeline; it didn't work." Coaches age out. Sponsors come and go.

The simulation engine is deterministic given a seed — every smoke test runs the same way every time, which makes debugging tractable on a system this interconnected.

---

## Screenshots

> Screenshots will land in a `docs/screenshots/` folder once captured. The current build renders a full ImGui DX11 interface with broadcast-style scoreboards, VCT-style bracket cards, sortable league standings, calendar grids, and per-player profile modals with God-mode editing.

---

## Quick start

If you just want to play:

1. Clone the repo
2. Run `cpp\build.bat release` (requires MSVC Build Tools 17+, see below)
3. Double-click `cpp\build\vlrgui.exe`, or use `Play.exe` as a one-click rebuild-and-launch wrapper

The launcher (`Play.exe`) walks up the directory tree, mtime-compares against `build\vlrgui.exe`, runs `build.bat` if anything changed, then launches the game. So the typical day-to-day workflow is "edit code, double-click Play."

---

## Build from source

### Requirements

- **Windows 10/11** (DX11 backend; cross-platform port is not yet planned)
- **MSVC Build Tools 17.x** with the Windows 10 SDK
  - The build script hard-codes `vcvars64.bat` at `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat`. Edit `cpp\build.bat` if your install path differs.
- No external library dependencies — Dear ImGui v1.91.5 is vendored at `cpp/third_party/imgui/`.
- `comdlg32.lib` (ships with the Windows SDK) — used by the JSON export's native Save dialog.

### Build commands

```
cd cpp
build.bat release   # default; /O2 optimized build
build.bat debug     # /Zi /RTC1 debug build
build.bat clean     # wipe build/
build.bat test      # build then run vlrtest.exe (19 smoke tests, ~1-3 min)
build.bat run       # build then launch vlrgui.exe
```

### What gets built

The build produces four binaries in `cpp\build\`:

| Binary | Purpose |
|---|---|
| `vlrgui.exe` | The actual game (ImGui DX11 UI, ~2.1 MB) |
| `vlrtest.exe` | 19 smoke tests covering engine invariants |
| `vlrmanager.exe` | Console fallback (no graphics) |
| `Play.exe` | Auto-build launcher — checks mtimes, rebuilds if needed, launches the game |

### Workflow guardrails

- **Kill running exes before rebuild.** The MSVC linker silently fails when an exe is held open.
  ```powershell
  Get-Process vlrgui,vlrtest,Play -ErrorAction SilentlyContinue | Stop-Process -Force
  ```
- **PowerShell `*>` buffers output.** For live build logs, use:
  ```
  cmd /c "build.bat release < nul > log.txt 2>&1"
  ```
- **Build times.** Clean ~30-60s. Incremental ~10s. Smoke test pass ~1-3 minutes (full-season simulation).

---

## Repository layout

```
Valosim/
├─ Play.exe                          One-click rebuild-and-launch wrapper
├─ PROJECT_GUIDE.md                  ~1300-line living architecture document
├─ README.md                         This file
├─ .gitignore                        Excludes build/, *.exe, *.obj, etc.
└─ cpp/
   ├─ build.bat                      MSVC build script (vcvars64 + cl + link)
   ├─ include/                       Public headers (Player.h, Team.h, etc.)
   ├─ src/                           Implementation files
   ├─ tools/                         Build-time generators
   │  ├─ gen_names.js                Node script: imports zengm names → NamesData.cpp
   │  └─ dup_probe.cpp               Name-uniqueness diagnostic
   └─ third_party/
      └─ imgui/                      Vendored Dear ImGui v1.91.5
```

`cpp/build/` (gitignored) is where compiled binaries land.

### Source-file inventory

| File | Lines | Role |
|---|---:|---|
| `gui_main.cpp` | ~12,200 | Single-TU ImGui DX11 UI (sidebar, modals, tables, broadcast scoreboards, brackets, dashboards, all in one file by design) |
| `NamesData.cpp` | ~9,700 | Generated from zengm name pools (27 countries × top-250 first + top-250 last names) |
| `GameManager.cpp` | ~4,100 | World orchestrator: phase calendar, year-end loop, awards, news, rankings |
| `Player.cpp` | ~3,200 | Player simulation: progression, archetypes, contracts, role-fit, mastery, retirement |
| `Match.cpp` | ~2,300 | Per-duel simulation engine: phase-aware power calc, assist gating, IGL impact, archetype layering |
| `Team.cpp` | ~2,300 | Team-level: roster invariants, comp resolution, AI front-office logic, chemistry, org memory |
| `smoke_test.cpp` | ~970 | 19 smoke tests |
| `Tournament.cpp` | ~960 | Double-elim brackets, groups, trophy integrity, phantom-round detection |

Everything else is either small data tables (Country, Agent, Coach), I/O (MatchExport JSON), or vendored.

---

## Architecture at a glance

```
Common  Country  Names+NamesData  Agent  Coach           ← primitives + data
   │       │           │            │      │
   └───────┴───────────┴────────────┴──────┘
                       │
                   Player                                  ← individual entity
                       │
            Team    League    Match    Series              ← team-level
                       │
                 Tournament   MatchExport                  ← bracket + JSON export
                       │
                 SoloQ   GameManager                       ← world orchestration
                       │
                     Goat                                  ← derived rankings
                       │
                gui_main.cpp                               ← ImGui UI (single TU)
```

Two important invariants in the dependency graph:

- **`RecordedMatch` lives in `Player.h`** even though it logically belongs to Match. This breaks a Match → Team → Player → RecordedMatch → Match header cycle.
- **`RecordedMatch::team1/team2` are `weak_ptr<Team>`**, not `shared_ptr`. This breaks a Match → Team → Player → pro_match_history → RecordedMatch → Team ownership cycle. Every call site must `.lock()` and nullptr-guard.

---

## Engine systems in depth

This is a survey. The full reference lives in [PROJECT_GUIDE.md](PROJECT_GUIDE.md).

### Player

Every Player is held by `shared_ptr<Player>`. Identity stays separate from gameplay-driving state:

- **Identity**: gamertag (`name`), legal first/last, country + ISO code, region, birth year, birth city
- **Hidden development**: `growth_archetype` (EarlyPeaker/Standard/LateBloomer), `peak_age`, `work_ethic`, `consistency`, `potential` — invisible without God-mode
- **24 attributes**: 13 core (Aim, Headshot, Entry, Utility, GameSense, Clutch, DecisionMaking, Intelligence, Aggressiveness, Positioning, Communication, Adaptability, Leadership) + 3 micro (Reaction, SpikeHandle, Anchor) + 8 round-2 attributes (Movement, CrosshairPlacement, Lurking, Awping, Stamina, MidRoundCalling, EconomyMgmt, AntiStrat)
- **Four playstyle layers — coexisting, not merged**:
  1. `rookie_archetype` — 16-value spawn growth template (e.g. MechanicalProdigy, TacticalIGL)
  2. `Archetype` — 32 sim-driving playstyle tags (e.g. MechanicalDemon, IceColdClutcher) read by Match.cpp
  3. `Desire` — 6 negotiation temperaments (Greedy, Loyal, RingChaser, Mercenary, StabilitySeeker, Competitor)
  4. `playstyle_identity()` — derived display label
- **IGL designation** — `is_igl` flag, not a role. Layers on top of any of the 4 core gameplay roles via a stat shift (-Aim/HS/Entry, +Lead/Intel/Comm/GS/MidRound/Econ/AntiStrat) plus a per-duel 0.95 focus tax.
- **Mastery**: per-agent and per-map `AgentMastery` / `MapMastery` structs. EMA-updated by Match.cpp on every completed pro match. Decay 15%/yr matches, 3%/yr avg-rating. Drives `signature_agent()` and per-map agent selection.
- **Contracts**: `contract.amount_k` (in thousands), `contract.exp_year` (inclusive last season), `contract_years` (frozen signed duration snapshot), `contract_role` (the role hired into — 2026-05 addition for role-lock semantics)
- **Career counters**: kills/deaths/assists/first-bloods/first-deaths/survivals/trades/rounds/matches plus per-season counterparts. Hall-of-Fame milestones tracked separately (`career_max_match_kills`, `career_max_match_kd_x100`, `career_grand_final_clutches`, `ever_top20_solo`).

### Team

5-7 player roster (5 starters at indices 0..4, 0-2 bench at 5..6). Critical invariants:

- **`primary_role` is write-once at spawn.** Nothing in the engine reassigns roles dynamically — that's a deliberate stability guarantee. The `contract_role` field stamps what role the player was hired for.
- **Roster has no sort/swap reorderings.** `sign_player` appends; `release_player` preserves order. A bench player can never silently become a starter without explicit user action.
- **One IGL invariant.** `enforce_one_igl()` runs after every roster mutation. Exactly one starter carries `is_igl`. Duelist IGLs require an elite mental composite (≥ 0.80) — that's intentional, the user wants Duelist IGLs to be rare.
- **At-least-one-Flex invariant.** `enforce_one_flex()` is insert-only — multiple Flex players in the starting 5 are legal.

#### Team-level intelligence systems

- **`Strategy`** — 7 values (Contender, Rebuilding, Bridge, BudgetRoster, DevelopmentFocus, WinNow, TalentFarm). Recomputed yearly. Transition is gated by `commit_strategy_with_inertia()` — orgs resist snapping into a new philosophy on one year's evidence (probabilistic transition matrix).
- **`TeamWindow`** — Opening / Open / Closing / Closed. Computed from avg age + avg potential + avg OVR + avg years-left. Drives mid-season cut aggression, import appetite, multi-year candidate scoring.
- **`OrgMemory`** — 6 rolling metrics in [-100, +100] decayed 3%/year toward zero. Tracks rookie_success, import_success, veteran_success, financial_discipline, stability_culture, star_dependency. Walked at year-end inside `update_org_memory()`. Drives downstream signing AI.
- **`CompIdentity`** — 6-value comp identity (Balanced, AggressiveDive, UtilityHeavy, DoubleDuelistTeam, StructuredMacro, FlexExperimental) biasing per-map comp selection independently of strategy.
- **Chemistry graph** — per-team `unordered_map<ChemKey, double>` with canonical-ordered Player\* pairs. EMA-updated (α=0.15, clamp ±2) by Match.cpp from shared-game outcomes. Year-end 5% decay, off-roster prune.

### Match engine

`Match::play()` is round-by-round. The per-duel power calc:

```
base = aim*1.2 + headshot*0.8 + entry*0.5 + decision*0.6
       × consistency_jitter × eco_advantage × coordination
       × positioning × utility × comp_synergy_bonus
```

Then ~30 layered multipliers stack on top:

- **Phase-aware** (Opening / MidRound / SiteAction / PostPlant / Late) — Lurking only matters on MidRound, Anchor only on PostPlant defender, etc.
- **Per-duel context** — first-bullet bonus, spray follow-up, headshot clustering, team-execute quality, eco-upset, retake quality, pressure (match-point/OT)
- **Identity channels** — agent synergy (a1/a2/a3 fit), agent identity, import chemistry penalty, IGL focus tax, designated lurker isolation, LAN nerves
- **Confidence/composure cascades** — within-3-ordinals teammate-death confidence, down-2-players composure
- **All clamped** — `compute_attr_extras` keeps the identity-channel total in [−0.09, +0.12]; per-duel cap is +0.32 total

The VLR-style rating formula:

```
rating = 0.95 × weighted_KPR + 0.30 × APR − 0.38 × weighted_DPR
       + 0.003 × ADRa + 0.45 × SR + 0.34 × KAST + 0.235
```

Damage per kill: 130-180. Kill weights: clutch 1.20×, advantage 0.88×, eco mismatch 1.40×, OP'd-an-eco 0.65×, trade 0.70×, post-plant 0.92×, post-round 0.20×.

Assist attribution is **agent-role-gated, not primary_role-gated**: Initiator 1.00×, Smoke Controller (Brimstone/Omen/Clove) 0.85×, other Controller 0.30×, Sentinel 0.45×, Duelist 0.12×. The 0.12 Duelist multiplier is deliberate — high Duelist assists are a "missed kill" signal, not a virtue.

### GameManager — the world orchestrator

Phase calendar:

```
STAGE 1 → REGIONALS 1 → MASTERS 1 →
STAGE 2 → REGIONALS 2 → MASTERS 2 →
STAGE 3 → REGIONALS 3 → CHAMPIONS →
AWARDS → OFFSEASON
```

Total: 185 days of action in a 200-day cycle. `advance_day()` is the FM-style "Continue" button — every click advances exactly one day on a worker thread (async), with the UI rendering a full-screen "SIMULATING…" overlay at 60fps.

`run_end_of_year` is the most-orchestrated chunk in the codebase. Order matters:

1. `compute_season_awards` — MVP (multi-factor), Role-of-the-Year ×4, IGL of the Year. Team success now factors into all of them (2026-05 rebalance).
2. Per-team payroll, coach progression, coach contract decrement
3. Stat sanity sweep (caps career_matches at 5000, kills at 250000 for overflow guard)
4. Per-player `save_history_and_progress` (which dedupe_awards() first), retirement check
5. Top-20 solo Q tracking → `ever_top20_solo` for HoF
6. Hall of Fame qualification check (4-of-10 strict criteria)
7. Roster cleanup pass (release expired/retired)
8. AI team strategy re-classify (through Strategy Inertia)
9. Year-end re-sign loop (with market poach for walked stars)
10. `ai_full_offseason_pass` for every non-user team — 5-stage deep GM pass
11. `sync_all_ranked_regions` + MMR + W/L reset
12. Rookie generation (55 baseline + 1 per retired)
13. `year += 1`

### Contract negotiation — transparent

The full re-sign flow:

- `Player::propose_resign_offer(team, year)` returns `ResignOffer { amount_k, years, willingness, min_acceptable_k, max_acceptable_years, explainer }`
- `Player::evaluate_resign_offer(amount, years, team, offered_role)` returns `ResignBreakdown` — a per-factor scoring breakdown (salary, prestige, contender fit, loyalty/tenure, contract length, personality archetype gates, role fit). Same function the UI displays AND the AI uses to decide acceptance — they cannot disagree.
- `Player::accepts_resign_offer(...)` is literally `return evaluate_resign_offer(...).will_accept`. Single source of truth.

Role-fit penalty (added 2026-05): if you offer a Duelist a Controller deal, `role_fit_score(Controller)` returns a 0..1 compatibility. Mismatches dock the score; overpays can rescue mid-tier fits; hard mismatches reject.

### Smoke tests

19 deterministic tests pinning engine invariants:

1. Attr table round-trip
2. Player generation + sanity bounds
3. Team auto-fill from FA pool
4. Match runs to a valid score
5. Solo Q ranked day moves MMR
6. Full world boots and a season runs
7. Year-end progression updates ages
8. Solo Q replay storage stays bounded
9. Only 4 legal comps exist (1/2/1/1, 2/1/1/1, 1/1/2/1, 1/1/1/2)
10. Duelists are the leading first-blood role
11. Coaches generated and tied to teams
12. Every signed player has a real contract
13. FA mood adjusts demands and refusals
14. STRICT one-IGL-per-team after a full season
14b. Duelist IGL mental floor (soft diagnostic)
15. Tournament bracket plays through all rounds
16. JSON export of completed series produces valid output
17. Trophy integrity — no phantom championships
18. Multi-seed tournament structure audit (3 seeds × full bracket)

Run: `cpp\build\vlrtest.exe --seed 1337`. All 19 pass deterministically.

---

## UI systems in depth

The entire UI is a single ~12k-line translation unit (`gui_main.cpp`). This is deliberate — ImGui IDs are label-based, so cross-file UI helpers are more fragile than the single-file approach. Compilation cost is mitigated by `/MP` and unity-style headers in the engine.

### Sidebar navigation

Six grouped nav routes (collapsed from 16 flat screens):

- **Home** — Dashboard + News
- **Team** — Roster + Manager + Calendar
- **Competition** — Standings + Tournaments + Power Rankings
- **People** — Transfer Market + Solo Q + League Leaders + Compare
- **History** — Favorites (Trophy Room / Hall of Records / Greatest Seasons / Hall of Good / Biggest Busts / Seasons Without a Title / MVP Race / Awards / Formula Lab) + Event Log
- **Live** — Watch (any match between any two teams, friendly-flagged so stats don't pollute career)

The user-org chip at the top of the sidebar (logo + tag + name) is clickable — 1-click route to My Team profile.

### Modals

- **Player Profile** — Overview / Detail / Match History / Map Mastery / Agents & Mastery / IGL Profile (when applicable). God-mode editing of every numeric and categorical field.
- **Re-sign / Extend modal** — Two-column layout: player's ask + willingness bar on the left; market value + salary slider + years slider + role offer combo + transparent breakdown + accept/reject pill on the right.
- **FA negotiation modal** (2026-05 addition) — Mirrors the Re-sign modal. Replaces the old "direct sign" button so FA signings feel as deliberate as extensions.
- **Live Match Viewer** — Broadcast-style score banner + per-team scoreboards (9 columns incl. HS%) + round timeline + kill feed + per-round economy + ACE/CLUTCH callout cards. Speed controls (Slow/Normal/Fast/Round-skip). Spoiler-safe (no map count, no round-winner during in-progress play-by-play).

### Live match presentation

- **Score banner** — Two team panels with primary/accent colors. Tag + name + per-team comp tag subtitle.
- **Series comp evolution** — Strip showing comp_tag chosen per map (only revealed maps, no future spoilers).
- **Round timeline** — Single horizontal strip, fixed 24px slot width per round. Only revealed rounds + 1 current shown.
- **Scoreboards** — Player rows with agent name colored by the AGENT's role (not the player's primary_role). A Controller main flexed onto Killjoy renders in Sentinel green.
- **JSON export** — Schema v1, hand-rolled writer (no library), available on the final map of any series. Includes round-level kill data, economy log hooks, ability usage hooks (v2 reserved fields).

### God Mode

Sidebar toggle. When on:

- Player Profile becomes editable: attribute sliders, IGL toggle, rookie archetype combo, badge editor, agent/map mastery editor, force-retire, MMR reset, contract editor (signed years snapshot + exp year)
- Manager → Player Development reveals HIDDEN PEAK ARCHETYPES (growth_archetype, peak_age, work_ethic, consistency, potential)
- Manager → Head Coach exposes coach attribute editing + contract slider
- Manager → Commissioner shows per-region team table (Budget / Prestige / Sponsorship / Strategy)

---

## Design philosophy

The user has been explicit about several non-obvious design choices over the project's lifetime. Some highlights — see PROJECT_GUIDE.md §12 for the full list:

- **Star players should not artificially carry.** High attrs naturally produce good stats. Don't force "50-kill carry games" with explicit modifiers. Let the simulation emerge.
- **Duelists should have FEW assists.** High Duelist assists indicate missed kills, not virtue. The 0.12 assist multiplier is intentional.
- **Awards only for actual wins.** No Finalist or Semifinalist participation badges. Champion only. `[T]` regional, `[M]` Masters, `[W]` World/Champions.
- **OFFSEASON is MANUAL for the user.** The AI does not touch user_team's roster during OFFSEASON — the user gets a 14-day window to manage uninterrupted. The OFFSEASON-end fallback runs only if the user ignored it and ended the window with < 5 starters.
- **Live viewer must not spoil.** Hide map count, hide round winner during in-progress play, hide best_of in spoiler-sensitive contexts.
- **IGL is NOT a position.** It's a leadership designation overlaid on a player's actual gameplay role. Position enum is strictly D / C / S / I.
- **Flex IS a real role-overlay** but not a Position either. Multiple Flex players in the starting 5 are legal — Flex is insert-only, never demoted.
- **Imports should feel exceptional.** Only 2-3 teams per region should have active imports at any given time. The `team_import_appetite` function is heavy-tailed by design — most teams land < 0.10; only contender + WinNow rosters with the right coach + budget clear 0.25.
- **Memory ceiling is 1 GB.** Solo Q ladder + match history + chemistry graph all bounded so a 30-year career run doesn't OOM.

---

## Tests

19 smoke tests live in [`cpp/src/smoke_test.cpp`](cpp/src/smoke_test.cpp). They're deterministic given a seed. The suite takes ~1-3 minutes because it actually simulates full seasons.

```
cpp\build\vlrtest.exe --seed 1337
```

Multi-seed audit (test #18) re-runs the entire tournament-structure pipeline at 3 seeds (1337 / 42 / 7) to catch edge cases that any single seed might miss. Every test must pass at every seed before a change ships.

---

## Project history

The project has accumulated thousands of small refinements over its lifecycle. The headline arcs:

- **Initial engine** — Player, Team, Match, basic FA market, simple OVR formula
- **Tournament correctness** — phantom-round bug discovery, ub_alive_ snapshot+clear pattern, trophy participation filter to prevent phantom championship awards
- **IGL system** — STRICT one-IGL invariant, per-role spawn weighting, Duelist IGL hard floor
- **Mastery + signature agents** — per-agent EMA mastery, per-map mastery, mastery decay
- **Position system overhaul** — Flex collapsed back into an overlay flag, Position enum trimmed to 4 values
- **Belief-driven AI** (2026-05) — OrgMemory, Strategy Inertia, Player Desire, TeamWindow, timeline_fit_score, chemistry graph
- **Transparent negotiation overhaul** — ResignBreakdown, single-source-of-truth evaluator, monotonic salary acceptance, archetype hard-gates with explicit reject reasons
- **Re-sign flow** — propose_resign_offer + cached offer modal, FA price decay during OFFSEASON, deep year-end re-sign loop with market poach
- **Financial rebalance** — salary cap $999K → $180K, budget tiers tightened, sponsorship formula reduced
- **Async sim worker** — engine work off the UI thread, "SIMULATING…" overlay
- **Contract role lock + role-fit** (2026-05) — `contract_role` field, `role_fit_score()` evaluator, FA negotiation modal mirroring re-sign UX, role offer combo in both modals, bench-player re-sign gate opened
- **Year-end contract math fix** (2026-05) — fixed off-by-one in year-end re-sign + market poach + AI offseason pass (was crediting the just-played season as year 1 of the new deal)

Each arc is documented in [`PROJECT_GUIDE.md`](PROJECT_GUIDE.md) with dated comments at every behavioral decision.

---

## PROJECT_GUIDE.md

The single most important file in the repo. ~1300 lines covering every system in depth, every magic number with its origin, every common pitfall, every architectural decision the user has explicitly made.

**If you're modifying this codebase, read PROJECT_GUIDE.md first.** It is the authoritative reference — the README is just an orientation.

---

## License

Not yet declared. Default copyright reserved. If you want to use this code outside of the repository owner's explicit permission, open an issue first.
