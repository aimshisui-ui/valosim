# VLR Manager / Valosim — Project Guide for Future Claude

> **Read this FIRST before editing.** Captures current architecture + every behavioral decision the user has explicitly asked for. If you're tempted to "improve" something documented here, check with the user — most rules below are deliberate, several were learned the hard way.
>
> **What this file is:** a pure reference manual — current systems, magic numbers, pitfalls, file layout. **What this file is NOT:** a history of changes. For dated history of when things were added or changed, see [CHANGELOG.md](CHANGELOG.md).

---

## 0. Project Overview

C++17 Valorant esports manager. Dear ImGui DX11 UI on Windows. The user wants it to feel like a polished esports management sim — believable living ecosystem, broadcast-quality match presentation, long-term career sandbox.

### Core gameplay loop
1. User runs an org in the **Americas** region by default.
2. Calendar advances day-by-day (FM-style "Continue").
3. 3 regions (Americas / EMEA / Pacific) play 3 stages each per season, interleaved with regional playoffs, 2 international Masters events, and year-end Champions.
4. Between matches the user manages roster, coach, finances, contracts.
5. Solo Q simulates 900-player ranked ladders per region in the background, feeding the FA pool.
6. Year-end → AWARDS day → OFFSEASON (14 days, manual roster moves) → next season.

---

## 1. Layout

```
C:\Users\fulls\Desktop\Valosim\
├─ Play.exe                           Auto-build launcher (project root)
├─ Play VLR Manager.exe (Desktop)     Same launcher, on Desktop
├─ PROJECT_GUIDE.md                   THIS FILE
└─ cpp\
   ├─ build.bat                       MSVC build script (vcvars64 + cl + link)
   ├─ build\                          Output objs + 4 exes
   │   ├─ vlrgui.exe                  THE GAME (ImGui DX11, ~2.1 MB)
   │   ├─ vlrtest.exe                 19 smoke tests
   │   ├─ vlrmanager.exe              Console fallback
   │   └─ Play.exe                    Launcher binary
   ├─ include\                        Public headers
   ├─ src\                            Implementations
   ├─ tools\                          Build-time generators (gen_names.js, dup_probe.cpp)
   └─ third_party\imgui\              Vendored Dear ImGui v1.91.5

Source backup: C:\Users\fulls\Documents\valosim_backup_LATEST.zip (~330 KB)
```

`Play.exe` walks up the dir tree, mtime-compares vs `build\vlrgui.exe`, runs `build.bat` if anything changed, then launches the game. **The user clicks Play.exe — never `build.bat` directly.**

---

## 2. Build

```
cd cpp
build.bat release    # default — release with /O2
build.bat debug      # /Zi /RTC1 debug
build.bat clean      # wipe build/
build.bat test       # build then run vlrtest.exe
build.bat run        # build then launch vlrgui.exe
```

`vcvars64.bat` hard-coded to `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat`.

### Workflow guardrails

- **Kill running exes before rebuild**: PowerShell `Get-Process vlrgui,vlrtest,Play,cmd -ErrorAction SilentlyContinue | Stop-Process -Force`. Link can fail silently if any exe is held open.
- **PowerShell `*>` buffering**: output may not appear until the process exits. Use `cmd /c "build.bat release < nul > log.txt 2>&1"` for live logging + suppress the trailing `pause`.
- **Verify exe mtimes** after rebuild — incremental builds can no-op.
- **`vlr::clamp_v` namespace**: must use `vlr::` prefix from gui_main.cpp's anonymous namespace.
- **`comdlg32.lib` links** with vlrgui.exe (required by GetSaveFileNameA for the JSON export dialog).

Builds are ~30-60s clean, ~10s incremental. Smoke tests are ~1-3 min per seed because of full-season simulation work.

---

## 3. Module Layout

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
                gui_main.cpp                               ← ImGui UI (single TU, ~12.5k lines)
```

`Match.h` forward-declares `RecordedMatch` which lives in `Player.h` to break the Match → Team → Player header cycle.

---

## 4. Current Systems

### 4.1 Country / Region (`Country.h/cpp`, `Names.h/cpp`, `NamesData.h/cpp`)

`enum class Region { Americas, EMEA, Pacific }`. Each country has `name`, ISO 2-letter code, region, weight.

- **Americas**: USA=80, Canada=35, Brazil=22, Mexico=10, Chile=6, Argentina=6 (NA dominates).
- **EMEA main**: Germany, Sweden, UK, Ukraine, France, Turkey, Finland, Scotland, Ireland (14-22). Side: Spain, Russia, Poland, Netherlands, Italy, Denmark, Norway (4-8).
- **Pacific main**: South Korea (35), Australia (20), Japan (20), Philippines (18), Thailand (14), Singapore (12), Vietnam (12). Side: China (8), Indonesia (8).

**Name pools** — `Names.cpp::make_identity(country)` picks first + last via weighted sampling:
- **Primary source**: `NamesData.cpp::generated_name_pools()` — zengm's real-world frequency-weighted pools (27 countries × top-250 first + top-250 last names = ~4.6k first + ~5k last total). Common names dominate but rare ones still appear via cumulative-weight sampling.
- **Fallback**: hardcoded `NamePool` in Names.cpp for ISOs zengm doesn't ship deep data for: `kr` (Korea, only 6/5 in zengm), `sg` (Singapore), `cn` (China), `cl` (Chile).
- **Duplicate-name rate** dropped from ~80% (old 10-name pool) to ~1.4% for USA after the zengm import. Korea/Singapore still ~70-80% — expand by hand in Names.cpp's `pool_for` if needed.
- **Regenerate** with `node cpp/tools/gen_names.js`. Path to zengm hard-coded at the top.

**Procedural org names** (`Common.cpp::take_team_name`) — 5 styles, never produces "Team_NNNN". Global de-dup. `make_team_tag(name)` derives 3-letter ALL-CAPS abbreviation.

**Gamertag** (`Names.cpp::make_handle`) — 5-style generator (bare root, root+suffix, root+number, leet root, two-word composite).

### 4.2 Agents, Maps, Comps (`Agent.h/cpp`)

**29 agents** (7 Controllers + 8 Duelists + 7 Initiators + 7 Sentinels), 11 maps. Each agent has `Role` (gameplay archetype) + 3 mapped `Attr`s.

| Role | Agents |
|---|---|
| Controller | Astra, Brimstone, Clove, Viper, Harbor, Miks, Omen |
| Duelist | Iso, Jett, Neon, Phoenix, Raze, Reyna, Waylay, Yoru |
| Initiator | Breach, Sova, Fade, Kayo, Skye, Tejo, Gekko |
| Sentinel | Chamber, Cypher, Deadlock, Killjoy, Sage, Vyse, Veto |

`find_agent_by_name(string_view)` accessor returns `const Agent*` or nullptr.

**Smoke Controllers** (Brimstone / Omen / Clove) — full assist weight; other Controllers reduced.

**Map preferences** (`map_pref(name)` returns `MapCompPreference{primary, secondary, favors_lurk, favors_dive, favors_anchor}`):

| Map | Primary | Secondary | Lurk | Dive | Anchor |
|---|---|---|---|---|---|
| Ascent | DoubleInitiator | DoubleController | | | ✓ |
| Bind | DoubleController | DoubleInitiator | ✓ | | |
| Haven | DoubleSentinel | DoubleInitiator | | | ✓ |
| Split | DoubleSentinel | DoubleInitiator | ✓ | | ✓ |
| Icebox | DoubleDuelist | DoubleInitiator | | ✓ | |
| Breeze | DoubleInitiator | DoubleController | ✓ | | |
| Fracture | DoubleInitiator | DoubleDuelist | | ✓ | |
| Pearl | DoubleController | DoubleInitiator | ✓ | | |
| Lotus | DoubleSentinel | DoubleInitiator | | | ✓ |
| Sunset | DoubleDuelist | DoubleInitiator | | ✓ | |
| Abyss | DoubleInitiator | DoubleSentinel | ✓ | | ✓ |

Lurk agents = {Viper, Cypher, Yoru, Chamber}; Dive = {Neon, Raze, Phoenix, Jett}; Anchor = {Killjoy, Cypher, Sage, Sentinels generally, Viper}. `agent_flavor_fit(agent, pref)` returns 0..3.

**4 legal comps** (CompPlan need totals 5): DoubleInitiator (1D 2I 1C 1S), DoubleDuelist (2D 1I 1C 1S), DoubleController (1D 1I 2C 1S), DoubleSentinel (1D 1I 1C 2S).

**Option A interpretation** — CompTag now classifies which role the Flex player extends per map. `DoubleInitiator` = "Flex plays Initiator this map". This preserves all comp_synergy / per-map adaptation plumbing.

### 4.3 Attributes (`Common.h::Attr`)

24 attrs, all 1-99. Base 13: Aim, Headshot, Entry, Utility, GameSense, Clutch, DecisionMaking, Intelligence, Aggressiveness, Positioning, Communication, Adaptability, Leadership. Micro: Reaction, SpikeHandle, Anchor. Round-2: Movement, CrosshairPlacement, Lurking, Awping, Stamina, MidRoundCalling, EconomyMgmt, AntiStrat. The last 3 are IGL-flavored — boosted +12/+12/+10 when `is_igl == true`.

### 4.4 Player (`Player.h/cpp`)

Held by `shared_ptr<Player>` everywhere — no raw new/delete.

**Identity:** name (gamertag), first/last (legal), country/country_iso/region/birth_year/birth_city.

**IGL fields:** `is_igl` flag + 4 tendency sliders (`tend_play_aggressive`, `tend_lurk_vs_execute`, `tend_vocal`, `tend_adaptive`). ~25% spawn as IGLs. **IGL stat shift (NERFED for gunfighting):** -12 Aim, -10 HS, -8 Entry, -4 Aggro → +14 Lead, +10 Intel, +10 Comm, +8 GS, +8 Decision, +12 MidRound, +12 Econ, +10 AntiStrat. Plus per-duel **× 0.95 IGL focus tax**. Promotion via `Team::enforce_one_igl()`.

**`is_flex` flag** — independent of `is_igl`. Set by `Team::enforce_one_flex()`. NO attribute shift (positional designation only).

**Hidden development:** `growth_archetype` ∈ {Standard, EarlyPeaker, LateBloomer}, `peak_age`, `work_ethic`, `consistency`, `potential`. God-mode reveals.

**16 rookie archetypes:** MechanicalProdigy, RawAimer, TacticalIGL, ClutchSpecialist, AggressiveEntry, SupportMastermind, FlexibleUtility (boosts agent_pool_size ≥5), SoloQueueDemon, HighPotentialProject, InconsistentSuperstar, VeteranStyleRookie, DefensiveAnchor, LANPerformer, FragileConfidence, HighIQStrategist, TeamChemistryPlayer. Set once at spawn, persists.

**`agent_pool_size`** — distribution: ~0.1% have 6-7, ~9% 5, ~25% 4, ~55% 3, ~10% 1-2.

**Career counters:** career_kills/deaths/assists/fb/fd/survivals/trades/rounds/matches/rating_total/damage/hs_hits/rounds_with_kast/mvps. HoF milestones: career_max_match_kills, career_max_match_kd_x100, career_grand_final_clutches, career_seasons_played, ever_top20_solo.

**`adaptation_months_remaining` + `last_seen_team_region`** — import chemistry tracker. Reset to 24 when a player signs with a non-native-region team; decays 12/yr at year-end. Drives match coordination + duel power penalties.

**`agent_mastery`** — `unordered_map<string, AgentMastery>` per-player. `AgentMastery { matches, avg_rating (EMA α=0.15), peak_rating, last_played_year }`. Updated each pro match by `Match.cpp` calling `Player::record_agent_performance(name, rating, year)`. Decay at year-end: 15%/yr matches, 3%/yr avg, prune at 4y stale. Adds up to +12 to pool scoring. `signature_agent()` returns highest-mastery name (≥5 matches required).

**`map_mastery`** — mirrors agent_mastery for maps. `record_map_performance(map, rating, year)` called from Match.cpp. `map_mastery_bonus(map)` and `comfort_map()` accessors.

**`igl_impact_total` + `igl_impact_season` + `igl_impact_season_peak` + `igl_match_count`** — accumulated strategic value from `compute_igl_impact()` in Match.cpp (free helper). Drives IGL of the Year, MVP, HoF, GOAT for IGLs.

**`season_pressure_matches`** + **`season_clutch_pts`** — counters that drive the new multi-factor MVP scoring.

**Match histories:**
- `pro_match_history` — vector of `RecordedMatchPtr`, capped at 10 per player
- `solo_match_history` — same, for ranked
- `history` — vector of `MatchHistoryEntry`. NEVER cleared (permanent record for GOAT Season).

**Contract** `{amount_k, exp_year}`. Salary 15-999 K/yr. Role mults: D 1.30, C 1.20, I 1.05, S 0.85.

**`gen_contract`** — rich dynamic formula factoring `value_p = 0.55 × ovr() + 0.45 × potential`, career-phase U-curve, star status, IGL premium, upside premium, risk discount, last-season multiplier, trophy multiplier, veteran discount, ±10% market noise.

**Awards integrity:**
- `dedupe_awards()` — runs at year-end inside `save_history_and_progress`. Removes duplicate award strings.
- `award_count_by_prefix(prefix)` / `has_award_with_prefix(prefix)` — robust prefix-anchored queries.
- `ever_won_international()` / `ever_won_role_award()` / `ever_won_mvp()` — HoF criterion accessors.

**Form / risk accessors (UI ↔ engine bridge):**
- `is_form_at_risk()` — true if `season_matches >= 4 AND season_rating < 0.95`. Single source of truth for the "AT RISK" form chip on the Manager tracker; AI mid-season replacement logic should also gate on this so engine + UI cannot diverge. Was previously inlined in UI as a hardcoded threshold — never reintroduce.

**`TournamentPlayerStat` lifetime snapshot (2026-06-11):**
- `Tournament::aggregate_player_stats()` returns rows that hold a raw `Player*` (identity handle) PLUS snapshot fields `display_name`, `is_igl`, `signature_agent` captured at accumulation time inside `absorb_series_participants`. UI MUST read display data from the snapshots — NEVER dereference `r.player`. The raw pointer is only safe to use as a pointer-identity key for click-resolver lookups (which now also fall back to gamertag match across both `leagues` and `solo_qs` ladders so retired/cut participants still open). Was the root cause of the Tournament → Player Stats hover crash. → CHANGELOG 2026-06-11.

### 4.4.1 Position system (1D + 1C + 1S + 1I + 1 flexible 5th + IGL/Flex overlays)

**Every starting 5 has**: 1 Duelist + 1 Controller + 1 Sentinel + 1 Initiator + 1 flexible 5th slot. The 5th can be a Flex player (any role with `is_flex=true`), a secondary Initiator, a secondary Controller, or a utility-heavy role player. The IGL flag is overlaid on whichever of the 5 has the strongest mental composite + role fit.

**`Position` enum** (Player.h): `Duelist, Controller, Sentinel, Initiator, Count`. **No Flex value.** Derived live via `position_of(player)` purely from `primary_role`. Neither `is_flex` nor `is_igl` is consulted — both are overlay flags orthogonal to position.

Derivation:
- `primary_role == Duelist` → `Position::Duelist`
- `primary_role == Controller` → `Position::Controller`
- `primary_role == Sentinel` → `Position::Sentinel`
- `primary_role == Initiator` → `Position::Initiator`
- fallback → `Position::Initiator`

**`is_igl` and `is_flex` are INDEPENDENT overlay flags.** A player can have any combination: plain Controller, Controller • IGL, Initiator • Flex, Sentinel • IGL • Flex, etc. No mutual exclusion.

**IGL spawn weighting (Player.cpp)** — per-role base chance × mental composite multiplier:
- Per-role base: Controller 0.32, Initiator 0.30, Sentinel 0.18, Duelist 0.04
- Mental composite (0..1): `(0.20·GameSense + 0.18·Communication + 0.18·MidRoundCalling + 0.15·Intelligence + 0.12·Adaptability + 0.12·Leadership + 0.05·DecisionMaking) / 99`
- General gate: mental ≥ 0.55
- **Duelist hard floor:** mental ≥ 0.78 (elite-mind only)
- Multiplier: `0.5 + (mental − 0.55) × 2.0`
- Result: Duelist IGLs ~3-7% of all IGLs; Controllers + Initiators ~70%; Sentinels ~20%

**`enforce_one_igl` (Team.cpp)** — `igl_candidate_score = mental × role_mult` with role_mult Controller 1.20 / Initiator 1.15 / Sentinel 1.00 / Duelist 0.35. **Duelist veto**: if top-scoring candidate is a Duelist with mental < 0.80, fall through to the next-best non-Duelist (only fall back to Duelist if all 5 starters are Duelists). 0 IGLs → promote; 2+ → keep highest, demote rest (revert stat shift); 1 → no-op (no annual stat-thrash).

**`enforce_one_flex` (Team.cpp)** — **insert-only**. Ensures ≥1 `is_flex` starter; never demotes (multiple Flex players in starting 5 are now allowed). Promotion score: `agent_pool_size·8 + Adaptability·2 + GameSense + Communication·0.5`, prefers non-IGL, falls back to allow Flex-IGL only when all 5 are IGL. NO `primary_role` mutation, NO stat shift.

**`auto_fill_roster` slot order** (Team.cpp): Duelist → Controller → Initiator → Sentinel → 5th cascade (`try_fill_with_flex_player` → secondary Initiator → secondary Controller → best remaining FA). Final calls: `enforce_one_igl() → enforce_one_flex()`.

**`Team::sign_player`** runs `enforce_one_igl()` then `enforce_one_flex()` after every successful add.

**UI implications (gui_main.cpp):**
- Roster bucketing: 4 canonical slots (D/C/S/I) + 1 "5th" slot. Empty 5th → `[empty 5th slot]`.
- Badges render in canonical order: `PositionBadge → FlexBadge → IglBadge`. Renders examples: "Controller", "Controller • IGL", "Initiator • Flex", "Sentinel • Flex • IGL".
- `FlexBadge()` uses muted amber `kRoleFlex` (same color reused — Flex is no longer a Position but still has its color).
- Transfer Market: FA listing has NO Position filter; Flex players appear in their primary_role's category with a Flex chip.

### 4.5 Team (`Team.h/cpp`)

5-7 player roster (5 starters + 0-2 bench). `name`, `tag` (3-letter), `home_country/iso/city`, `color_primary/accent` (hash-seeded HSV→RGB), `personality` ∈ {Aggressive, Tactical, Budget, Balanced}, `target_comp`, `head_coach`, `prestige` (0-99), `sponsorship_k`, `strategy` ∈ {Contender, Rebuilding, Bridge, BudgetRoster, DevelopmentFocus, WinNow, TalentFarm}.

**`CompIdentity` enum** — strategic identity that biases per-map comp selection (distinct from `personality` / `strategy`):
- **Balanced** (default ~40%) — picks meta comp for the map
- **AggressiveDive** (~15%) — duelist-heavy fast tempo
- **UtilityHeavy** (~15%) — initiator/controller-heavy methodical
- **DoubleDuelistTeam** (~10%) — almost always DoubleDuelist
- **StructuredMacro** (~10%) — sentinel + slow, biases DoubleSentinel
- **FlexExperimental** (~10%) — off-meta, weights mastery > map fit

Per-personality weights: Aggressive 25/30/5/25/5/10, Tactical 25/5/30/5/25/10, Budget 30/10/10/5/10/35, Balanced 40/15/15/10/10/10.

**`Team::auto_fill_roster`** scoring: 50% comp_fit + 35% strategy_fit + 15% personality_pref. Calls `enforce_one_igl()` + `enforce_one_flex()` after every add.

**`enforce_one_igl` (STRICT INVARIANT):** counts IGLs in starting 5. 0 → promote highest-Leadership starter (apply IGL stat shift). 2+ → keep best, demote others (REVERT shift). 1 → no-op. **Called from `Team::sign_player` — single point of enforcement.**

**Imports:** hard cap 2/roster (`config().max_imports`), OVR ≥ 60 floor.

**`team_import_appetite(team, target_role)` (v2 — tightened)** — heavy-tailed 0..0.28 score driving ALL import-decision callsites:
- Budget floor $1.0M / ceiling $2.0M for full +0.10
- Prestige floor 50 / ceiling 80 for full +0.08
- Strategy Contender/WinNow: +0.06; Rebuilding-with-trophy: +0.05
- Recent international award: +0.05; domestic role scarcity: +0.06
- Coach TacticalGenius/Innovator/AnalyticsHeavy: +0.02
- Region multiplier: Americas 1.05, EMEA 0.90, Pacific 0.75

Consumption sites:
- `auto_fill_roster`: `chance(appetite*0.45)` + 0.20 floor
- `run_initial_snake_draft` per-pick: `chance(max(0.03, appetite*0.55))`
- `run_mid_season_replacements`: filters foreign FAs out below 0.22

Result: ~2-3 contender teams per region have imports. Snake-draft logs `[Imports] season-start summary` showing teams-with-imports per region.

### 4.6 Coach (`Coach.h/cpp`)

**16 personality archetypes:** Disciplinarian, PlayerFocused, TacticalGenius, DevelopmentCoach, AggressiveRiskTaker, StructuredMacro, EmotionDriven, AnalyticsHeavy, VeteranMentor, HarshRebuilder, Motivator, Pragmatist, Innovator, DefensiveAnchor, EntryLover, BudgetBalancer.

`personality_replacement_aggressiveness(p)` returns 0.10-0.95, used by `run_mid_season_replacements`.

**Coach contracts:** `contract_years` + `contract_exp_year` fields. Decremented at year-end. God-mode editor in Manager → Head Coach.

`generate_coach(region)`, `generate_coach_market(per_region)`.

### 4.7 Match (`Match.h/cpp`)

Core simulation. `Match::play()` is the round-by-round engine.

**`friendly` flag:** when true, stats compute locally for the live viewer but **NEVER** transfer to player career counters.

**Round phases:** Opening / MidRound / SiteAction / PostPlant / Late. `compute_phase` decides per duel from `first_engagement`, `spike_planted`, alive counts, map_control.

**Per-round side-aware map_control init:** Round 1-12 t1 attacks, 13-24 t2 attacks, OT alternates. Defenders 65, attackers 35. Pistol rounds 50/50.

**Per-duel power calc:**

```
base = aim*1.2 + headshot*0.8 + entry*0.5 + decision*0.6
       × consistency_jitter × eco_advantage × coordination × positioning × utility × comp_synergy_bonus
```

Plus layered multipliers:

| Multiplier | Weight | Trigger |
|---|---|---|
| Phase bonus | × 0.12 | Per phase (Lurking on MidRound, Anchor on PostPlant defender, etc.) |
| Reaction | up to +0.10 | × map_contest |
| Movement | up to +0.08 | non-pistol rounds |
| CrosshairPlacement | up to +0.09 | always |
| Lurking | up to +0.10 | MidRound phase only |
| Crossfire (Positioning) | up to +0.05 | when 3+ teammates alive |
| Entry pathing (Entry+Movement) | up to +0.18 | SiteAction attacker only |
| Pistol round (Aim+Reaction) | up to +0.12 | rounds 1/13/25 |
| Eco upset (SpikeHandle+Clutch+Aggro) | up to +0.12 | tier-1 vs tier-3+ |
| Retake (Decision+Adaptability) | up to +0.10 | PostPlant retaker side |
| Pressure (Stamina+Clutch+Decision) | up to +0.06 | match point / OT |
| Team Communication (avg) | up to +0.04 | always |
| Awping | up to +0.15 | invest ≥ 18000 |
| Stamina | 0.92-1.02 | round > 30 |
| Anchor (last alive) | up to +0.22 + SpikeHandle | self only |
| Clutch (1vN) | (0.28×Clutch + 0.05×Decision) per body disadvantage | self only |
| Adaptability comeback | up to +0.05 | down 3+ rounds |
| **Agent synergy** | −0.04 to +0.10 | self, based on agent's a1/a2/a3 fit |
| **Agent identity** | up to +0.20 (capped) | per-agent flavor branches |
| **Team info bonus** | up to +0.03 coordination | Cypher/Sova/Fade alive |
| **Import chemistry penalty** | up to −0.05 coord, −0.015 power | unintegrated imports (adapt > 0) |
| **IGL focus tax** | × 0.95 power | self if `is_igl` true |
| **Opening duel selection** | 0.70..1.45× peek weight | `aim·0.4 + mov·0.3 + agg·0.3 − dec·0.1` |
| **Fight IQ avoidance** | skip up to 18% non-forced duels + +0.04 re-pick bump | Pos+GS+Decision > 80 |
| **Round-level consistency jitter** | ±0.02..±0.10 c_mod, inverse-consistency | per round |
| **First-bullet duel** | up to +0.05 (Crosshair × Reaction) | kill_ordinal=0, defender hasn't shot |
| **Spray follow-up** | up to +0.04 (Aim·0.5 + Sta·0.25 + Adapt·0.25, /Crosshair) | mid-firefight, no round kills yet |
| **Designated lurker** | +0.06 isolated, −0.04 exposed | Yoru/Viper/Cypher OR (Lurking>80 + Pos>75) |
| **Headshot clustering** | −0.025..+0.045 (HS−50)·0.005 | when round_kills_p ≥ 1 |
| **Team execute quality** | ±0.05 attacker multiplier | SiteAction; (Util+Comm+Intel) team avg |
| **Confidence** (Aggro × Decision / 99) | −0.02..+0.04 | within 3 ordinals of teammate death |
| **Composure** (Clutch × Decision / 99) | 0..+0.05 | down 2+ players |
| **LAN nerves** (Adapt × Consistency / 99) | −0.05..+0.02 | playoff/finals events |

`compute_attr_extras` clamps the new identity-channel additions to [−0.09, +0.12]. Total per-duel cap ≤ +0.32.

**Trade detection (REWORKED):** Looks back 2 ordinals for a teammate-killer = current victim. If found, trade chance = 0.55 + 0.10·Pos + 0.10·Reaction + 0.08·Crosshair + 0.07·Adaptability, capped 0.40-0.92.

**Assist attribution (role + agent gated, KEYED OFF AGENT ROLE NOT primary_role):** Per-kill weighted single-assist roll using `agent_role_of(player) = chosen_agents_[player]->role`:
- **Initiator: 1.00×** (full)
- **Smoke Controller (Brimstone/Omen/Clove): 0.85×**
- **Other Controller: 0.30×**
- **Sentinel: 0.45×**
- **Duelist: 0.12×** (high duelist assists = missed-kill signal)

`any_assist_chance = clamp(sum_w × 0.18, 0.06, 0.45)`. Max 1 assist per kill. Realistic targets: 13-15 assists = high-impact, 20+ vanishingly rare.

**VLR rating formula:**
```
rating = 0.95 × weighted_KPR + 0.30 × APR − 0.38 × weighted_DPR
       + 0.003 × ADRa + 0.45 × SR + 0.34 × KAST + 0.235
```
**Damage per kill:** 130-180. **Kill weights:** clutch 1.20×, advantage 0.88×, eco mismatch 1.40×, OP'd-an-eco 0.65×, trade 0.70×, post-plant 0.92×, post-round 0.20×.

**HoF milestones tracked per pro match:** `career_max_match_kills`, `career_max_match_kd_x100`, `career_grand_final_clutches`.

**Per-map agent selection** — `Team::build_round_selection(map, is_team1, prior_results, ...)` replaces the old deterministic single-agent picker. Per-player score:
```
0.65 × get_rating(agent, map) + 0.10 × agent_pool_priority
+ 0.10 × agent_flavor_fit + 0.10 × map_mastery_bonus + 0.05 × jitter
```
Team-level `desired_comp_tag`: start with map's `primary`, apply CompIdentity override, in-series adaptation (bias away from losing comps; force switch after 2 same-tag uses), 10-15% creative pick (FlexExperimental: 30%). Jitter ensures rotation between maps.

**IGL Impact** — `compute_igl_impact(p, is_team1, team_won, team_strength, opp_strength, t1_score, t2_score, event_name)`:
```
strategic = 0.30·MidRound + 0.20·AntiStrat + 0.15·Intel + 0.15·Adapt + 0.10·Lead + 0.10·Comm    (all /99)
upset_delta = clamp((opp_str − team_str) / 30, −1, +1)
outcome = team_won ? clamp(1 + 0.5·upset_delta, 0, 2) : clamp(0 + 0.5·upset_delta, 0, 2)
comeback = 0.20 if (team_won + close result + 13+ rounds)
pressure = 1.40 GF / 1.20 Final / 1.18 Champions / 1.15 Masters / 1.08 Regionals|Playoffs / 1.00
impact = clamp((strategic·0.7 + outcome·0.3 + comeback) × pressure, 0, 2.5)
```
Accumulated into `igl_impact_total` + season counters.

### 4.8 Series (`Series.h/cpp`)

Best-of 1/2/3/5. **`is_over()`** special-cases BO2: returns true after exactly 2 maps (1-1 ties allowed).

**`add_match_data(m)`** — increments wins, accumulates aggregate_stats (lazy-inserts unknown Player keys to catch substitutions), creates `RecordedMatchPtr` via `make_recorded_match`, stores in `last_recording_`, appends to `all_recordings_`, pins to all 10 players' pro_match_history (capped at 10) **but only when `!m.is_friendly()`** (friendly Watch series don't pollute career history). Also populates `map_history_` with CompTag derived from `chosen_agents()` for in-series adaptation.

**`finalize_stats(is_league_play)`** sets winner_/loser_ ONLY when there's a clear winner (BO2 ties leave both null).

**`map_history()`** (`vector<MapResultEntry>`) — populated as a side-effect of `add_match_data`. Each entry: `{map_name, t1_comp, t2_comp, t1_score, t2_score, t1_won}`. Consumed by next map's `build_round_selection`.

### 4.9 Tournament (`Tournament.h/cpp`)

`enum class TournamentFormat`: SingleElim (fallback <4 teams), DoubleElim (default), GroupThenPlayoffs (Masters/Champions).

`enum class Phase`: Groups, Bracket, GrandFinal, Done. **No `GrandFinalReset`** — modern VCT format (single BO5 GF decides champion, no bracket reset regardless of UB/LB origin).

**Double-elim state:** `ub_alive_`, `lb_alive_`, `ub_just_dropped_`. (`gf_played_once_` + `bracket_reset_needed_` removed 2026-05-14.)

**`bracket_bo_for(is_lower, teams_in_round, is_lb_final)`:** returns 5 ONLY when `is_lower && is_lb_final`; everything else 3. GF hardcoded BO5. **Result: only LB Final + GF are BO5; all other bracket matches are BO3 (including UB Final).** Every `BracketMatch` carries a stamped `best_of` field for UI and smoke-test asserts.

**CRITICAL: `ub_alive_` snapshot+clear pattern** in `schedule_next_bracket_round` — snapshot the vector, CLEAR it, then `pair_matchups(snapshot, ...)`. Without the clear, play_round appends winners onto the existing pool → bracket size doubles every round.

**Bracket history:** `ub_history_`, `lb_history_`, `gf_history_` — `vector<vector<BracketMatch>>` snapshots populated each play_round. Used by NBA-style bracket UI.

**Group stage seeding fix** (2-group case): `{p[0], p[2], p[1], p[3]}`. 4-group case: `{p[0], p[2], p[6], p[4], p[7], p[5], p[1], p[3]}` — guarantees each group's #1 and #2 are in opposite halves.

**Group standings sort:** `stable_sort` by `(wins desc, map_diff desc, round_diff desc, name asc)`. Deterministic across `std::sort` impl changes.

### 4.9.1 Trophy integrity (CRITICAL — championship awards only go to actual participants)

**`participated_players_`** (`unordered_set<Player*>` on Tournament) — every player who appeared as a starter in any map. Populated incrementally via `absorb_series_participants(series)` from each Series' `history_record->blue_team/red_team`. Bench/sub players who never started don't count.

**`pinned_titles_already_`** — one-shot fuse on Tournament. `award_event_titles` short-circuits if already run, prevents double-firing from natural `play_round` Done transition + `force_finish_stale_tournaments`. Plus existing per-award `if (!exists)` dedup.

**Award recipient = `champion->roster ∩ participated_players_`** — phantom roster members (signed mid-tournament but never played) are silently excluded.

**`winning_roster_snapshot_`** (`vector<Player*>`) — captures exact recipients at pin time. Exposed via accessor for audit/UI.

**`award_event_titles`** early-returns on unrecognized name (no silent `[*]` ghost awards). Champion-only awards: `[T] Regional Champ YYYY`, `[M] Masters Champ YYYY`, `[W] World Champion YYYY`. Finalist/Semifinalist tags NEVER pinned.

**Tournament queries:** `current_matchups()`, `lower_matchups()`, `eliminated()`, `groups()`, `current_round_label()`, `in_grand_final()`, `group_stage_complete()`, `initial_seeding()`, `ub_history()`, `lb_history()`, `gf_history()`, `runner_up()`, `semifinalists()`, `participated_players()`, `winning_roster_snapshot()`, `titles_already_pinned()`, `upper_bracket_round_count()`, `lower_bracket_round_count()`, `current_upper_best_of()`, `current_lower_best_of()`, `aggregate_player_stats()`. (`has_gf_reset_played()` removed 2026-05-14 with the reset mechanic.)

**`aggregate_player_stats()` (added 2026-05-15):** returns `vector<TournamentPlayerStat>` (player, team_name, maps, rounds, k/d/a, fk/fd, clutches, rating, adr, hs_pct, kast, top_agent). Incrementally accumulated inside `absorb_series_participants` keyed off each `RecordedMatch*` via a `stat_absorbed_recs_` dedup fuse (prevents the natural-Done + force-finish double-count). Counting stats summed; rating/adr/hs/kast are maps-weighted means. Accumulator reset in the Tournament ctor only (survives group→playoff `start_bracket` re-entry). Powers the Tournament "Player Stats" sub-tab.

**Phantom-round merge rule:** `schedule_next_bracket_round` merges UB-Final-loser-drops into `lb_alive_` ONLY when `lb_alive_.size() == ub_just_dropped_.size()`. On mismatch → run a pure LB round first and hold drops in `ub_just_dropped_` for the next merge attempt. `is_lb_final` fires only when `ub_alive_.size() == 1` AND LB is collapsing to its final 2. This avoids the phantom "LB Final" round that an earlier unconditional merge produced.

**8-team structure (post-fix, locked by smoke 15 + 18):**
- Upper bracket: 3 rounds (R1=4 matches, SF=2, Final=1) — all BO3
- Lower bracket: 4 rounds (R1=2, R2=2, SF=1, **Final=1 BO5**) — all BO3 except Final
- Grand Final: 1 BO5 — winner is champion immediately, NO bracket reset
- **Total: exactly 14 matches always** (modern VCT format)

### 4.10 SoloQ (`SoloQ.h/cpp`)

**Initial ladder: 900 players/region.** Targets ~15 Radiants/region/season.

`simulate_ranked_day(loops)` per region. **Tuned: 3 loops on matchday, 6 on off-day**. Activity probability: pro 0.30, FA 0.32. **Per-player daily cap: 4 matches per `simulate_ranked_day` call.** Result: typical pro ~270 ranked matches/season, FA ~290, Ranked Junkie pros peak ~400.

**`balance_lobby` STRICT one-IGL rule:** snake-draft by `player_skill_score` (50% MMR-norm + 40% OVR-norm + 10% consistency + ±5% agg). Up to 6 swap passes if team gap > 5%. Role diversity correction. IGL guarantee: swap to ensure exactly 1 IGL per team.

**MMR floor 1100, peak_mmr tracked.** Win = +20-50 (rating-scaled), loss = -5-25.

**Year-end MMR + W/L reset:** all non-retired players get `solo_mmr=1100`, `solo_wins=0`, `solo_losses=0`. `peak_mmr` preserved.

### 4.11 GameManager (`GameManager.h/cpp`)

World orchestrator.

**Phases (kPhases — 11 entries):**
```
STAGE 1 → REGIONALS 1 → MASTERS 1 →
STAGE 2 → REGIONALS 2 → MASTERS 2 →
STAGE 3 → REGIONALS 3 → CHAMPIONS →
AWARDS → OFFSEASON
```

**Pacing (`current_phase_pacing()`):**
- STAGE: (3, 11) → 33 days
- REGIONALS: (1, 9) → 9 days
- MASTERS: (1, 14) → 14 days
- CHAMPIONS: (1, 16) → 16 days
- AWARDS: (1, 1)
- OFFSEASON: (1, 14)

Total per season: **185 days** of action in a 200-day cycle.

**`advance_day(log)`** = FM "Continue" button. AWARDS day → `run_end_of_year` then `phase_idx++` → OFFSEASON. After 14 OFFSEASON days → `phase_idx = 0` → STAGE 1 of new year. Solo Q tick (3 matchday / 6 off-day loops).

**`set_current_world_year(year)`** called at top of `advance_day` so Match.cpp can stamp mastery records via `current_world_year()`.

**Phase end:** `force_finish_stale_tournaments(log)` drains unfinished brackets then `active_tournaments.clear()`.

**`active_tournaments` lifecycle (CRITICAL):** `force_finish_stale_tournaments` is the SOLE clear path. `generate_tournaments_if_needed` and `play_tournament_round` do NOT auto-clear finished tournaments mid-phase. Previous auto-clear caused brackets to respawn on the next matchday, silently doubling `[T]/[M]/[W]` awards.

**Skip helpers:**
- `advance_to_next_user_match` — pops live viewer at user match.
- `advance_to_next_phase` — plows silently. Clears `user_match_recording/series` from final result.
- `advance_to_playoffs` — no-op if already in playoffs phase. Every early-return clears `user_match_recording` + `user_match_series`.
- `simulate_full_season` — loops until phase becomes AWARDS or OFFSEASON. Stops in offseason BEFORE consuming AWARDS day.

**Year-end (`run_end_of_year`):**
1. `compute_season_awards` (FIRST, before counters reset)
2. Per team: payroll, coach dev/aging/retirement, coach contract decrement
3. Stat sanity sweep (caps career_matches > 5000 / kills > 250000)
4. Per player: `save_history_and_progress` (which runs `dedupe_awards()` first), retirement check
5. Top-20 solo Q tracking → `ever_top20_solo`
6. Hall of Fame check (4-of-10 criteria, see §4.15)
7. Roster cleanup pass (release expired contracts / retired)
8. Skip user_team in ai_manage_roster
9. Re-classify strategies, prestige, sponsorship
10. Re-sign loop
11. `sync_all_ranked_regions`
12. MMR + W/L reset
13. Rookie generation: 55 baseline + 1 per retired
14. `year++`. **`phase_idx` NOT reset here** (advance_day handles AWARDS → OFFSEASON → STAGE 1).
15. `news_pushed_events_.clear()` — auto-news dedup resets for the new season

**Mid-season replacements** — `run_mid_season_replacements`:
- Threshold: 0.90
- 0.89-0.90 band: cut chance × 0.30; 0.85-0.89 × 0.65; < 0.85 full coach-aggression
- Young prospects (≤21 + potential ≥75): 85% protected
- Contract ≤1yr left: cut chance × 0.55
- **Foreign FA scan gated by `team_import_appetite >= 0.22`**

**`update_mvp_race`** — monthly tick. 6 leaderboards (MVP, Duelist/Initiator/Controller/Sentinel Race, IGL Race) with hybrid scoring + delta tracking.

### 4.12 News Feed + MVP Race

`news_feed` — vector of `NewsItem { year, day_in_year, category, headline, body, team_name, player_name }`. Capped at 200, most-recent first. Categories: "Roster Move", "Awards", "Tournament", "MVP Race", "Coaching", "Retirement".

**`push_tournament_news(tour, year, gf_series)`** — auto-emits 1-3 news items when a Tournament transitions to `Phase::Done`:
1. Champion announcement (mandatory)
2. Finals MVP (skipped if `gf_series == null` — derived from highest-aggregate-rating winner-side player)
3. Champion run summary (UB undefeated / bracket reset / lower-bracket)

Idempotent dedup via `news_pushed_events_` keyed by `tour.name() + "/" + year`. Cleared at year rollover. Hooked from `play_tournament_round` (primary, has GF series) + `force_finish_stale_tournaments` (defensive, MVP skipped).

### 4.13 Goat (`Goat.h/cpp`)

`GoatWeights` editable in Favorites → Formula Lab.

**Robust prefix-anchored matching:** Goat.cpp uses `award_count_by_prefix("[T] ")` / `("[M] ")` / `("[W] ")` / `("[A] ")` instead of raw substring searches. New weight `igl_impact_per_career = 0.25` × `igl_impact_total` factors into `goat_career_score`.

**`goat_season_score`** preserves the year filter (`a.find(yr_str) != npos`) verbatim — `MatchHistoryEntry.awards` is a CUMULATIVE snapshot so the year filter prevents old championships being re-credited every subsequent season.

### 4.13.1 Playstyle Identity

`Player::playstyle_identity()` — pure read-only computation. Cascade priority:
1. Ice-cold Closer (Clutch≥90, consistency≥80, GF clutches≥2)
2. Mechanical Demon (Aim≥88, HS≥85, Reaction≥80)
3. Smart Lurker (Lurking≥80, Pos≥80, GS≥75)
4. Passive Anchor (Anchor≥85, Pos≥80, SpikeHandle≥75)
5. Aggressive Entry (Entry≥85, Aggro≥80, Movement≥75)
6. Utility Support (Utility≥85, Comm≥80, Aim<75)
7. Clutch Specialist (Clutch≥88, Anchor≥75, GF clutches≥1)
8. Hyper-consistent Veteran (consistency≥85, age≥25, career_matches≥100)
9. Momentum Player (consistency<65, Aim≥80 OR HS≥80)
10. LAN Choker (Adapt<50, GF clutches=0, career_matches>50)
11. Generalist (fallback)

Surfaced in Player Profile header as a chip below Signature Agent.

### 4.14 Awards (`SeasonAward`)

`compute_season_awards()` at year-end (BEFORE counters reset):

1. **MVP (multi-factor):** `score = season_rating × clutch_factor × igl_factor × sos × playoff_factor × dep_factor`. Floor 0.95 rating. Gate: `attended_intl OR rating ≥ 1.20`. `clutch = 1 + min(0.30, season_clutch_pts/30)`. `igl_factor = 1 + min(0.35, igl_impact_season/40)` for IGLs. `sos = 1.15× intl × 1.10× finals`. `playoff = 1 + min(0.20, season_pressure_matches/10)`. `dep = 1.10×` if team_wins < 18 AND rating ≥ 1.05.
2. **Duelist / Initiator / Controller / Sentinel of the Year** — top by rating, role-gated.
3. **IGL of the Year:** `0.35 × rating + 0.25 × per_match_igl_impact + 0.20 × team_finals_factor + 0.20 × team_wins/40`. Gate: `is_igl && !is_retired && team_name not Free Agent/Retired`.

8-match minimum. Top 3 finalists. Pinned as `[A] {Category} {year}`.

**Tournament awards (only champion):** `[T] Regional Champ YYYY` / `[M] Masters Champ YYYY` / `[W] World Champion YYYY`. NO Finalist or Semifinalist awards.

### 4.15 Hall of Fame

**Players must be RETIRED + 3+ seasons played** + 4 of these 10 criteria:
1. Won an international event (`[M]` or `[W]`)
2. Won a role award (Duelist/Initiator/Controller/Sentinel of the Year)
3. Won an MVP
4. Maintained 1.20+ rating across an entire season
5. Reached top 20 in solo Q (`ever_top20_solo`)
6. Earned over $1,000,000 career (sum of salary_log ≥ 1000 K)
7. Dropped 30+ kills in a pro match (`career_max_match_kills`)
8. Achieved 2.0+ KD in a pro match (`career_max_match_kd_x100`)
9. Clutched the final round of a Grand Final (`career_grand_final_clutches`)
10. Career IGL impact > 50 (if IGL — caller-route HoF)

UI safeguard: `DrawFavorites` HoF tab filters through local hof vector dropping anyone where `!is_retired`.

### 4.16 Phase 1 — Power Rankings + Trophy Room + Compare + News Expansion

Visibility / presentation layer added on top of existing engine data. No new simulation systems — surfaces what was already tracked.

**Player aggregators (`Player.h/cpp`):**
- `Player::TrophySummary trophy_summary() const` — counts of `[T]/[M]/[W]/[A]` awards (regional/masters/worlds/role_awards/mvps/igl_oty + all_titles list)
- `std::vector<Player::CareerEvent> career_timeline()` — chronological event list parsed from awards + salary_log + history. Categories: trophy / award / transfer / milestone / debut / retirement
- `Player::SeasonHighlight best_season()` and `top_n_seasons(n)` — sorted by rating desc, matches ≥ 8 gate. MatchHistoryEntry extended with `matches/kills/deaths/assists` (populated in `save_history_and_progress` BEFORE counter reset)
- `Player::HeadToHeadRecord head_to_head(const Player&)` — walks `pro_match_history`, identifies sides via `history_record->blue_team/red_team` raw pointer comparison, reads K + rating from `match_stats`

**Team aggregators (`Team.h/cpp`):**
- `Team::TrophyCase trophy_case() const` — fields: `regional_titles, masters_titles, world_titles, total_finals, ordered (vector<pair<int,string>>)`
- `Team::record_trophy(year, event_name)` — idempotent appender. Called by GameManager in `play_tournament_round` + `force_finish_stale_tournaments` (belt-and-braces) when a tour finishes
- `Team::dynasty_tier(current_year)` enum {None, Contender, ChampionEra, Dynasty}. Dynasty = 3+ titles in last 5y; ChampionEra/Contender require `finals_history_` which is not yet wired (stays None until a finals-detection hook is added)
- Private: `trophy_history_` (populated) + `finals_history_` (empty stub)

**Power Rankings + Community Rankings (`GameManager.h/cpp`):**
- `struct PowerRanking { Team* team; int rank; int prev_rank; double score; string tier; string analyst_note; vector<int> rank_history; }`
- `struct CommunityRanking : PowerRanking { double popularity_score; double narrative_boost; }`
- Weekly tick in `advance_day` every 7 days; reset on year rollover. Scope: each region + "International" (top teams across regions).
- Power score: `0.40 × win_rate_last5 + 0.20 × prestige_norm + 0.15 × map_diff_season + 0.15 × tournament_finish_factor + 0.10 × strength_of_schedule`
- Community score: `0.50 × prev_community_rank_lag + 0.25 × star_presence + 0.15 × flashy_moments_last30d + 0.10 × narrative_buzz_last30d` (intentionally lagged + biased — diverges from power)
- Tier bands: S (1-2) / A (3-4) / B (5-6) / C (7-8) / Bubble (rest). International: S(1-3)/A(4-6)/B(7-9)/C(10-12).
- Analyst notes: deterministic template lookup keyed on (tier, delta vs prev_rank)
- Accessors: `power_rankings_for(region)`, `community_rankings_for(region)`, `power_rank_of(team)`, `next_power_tick_in_days()`

**Ten new news emitters (`GameManager.cpp`):**
| # | Emitter | Hook | Trigger |
|---|---|---|---|
| 1 | `emit_breakout_news` | advance_day, STAGE 1 past day 50 | YoY rating delta ≥ +0.15 AND season_rating ≥ 1.15 AND seasons_played ≥ 2 |
| 2 | `emit_slump_news` | advance_day weekly | career_rating ≥ 1.10 but season_rating < 0.90 over 8+ matches |
| 3 | `emit_hot_streak_news` | play_stage_round + play_tournament_round | consecutive_series_wins ∈ {5, 8, 12} thresholds |
| 4 | `emit_milestone_news` | run_end_of_year (before reset) | career_kills crosses [5k, 10k, 20k, 30k], matches [500, 1k, 1.5k], trophies [3, 5, 10] |
| 5 | `emit_retirement_countdown` | run_end_of_year | age ≥ 30 + season_rating < career-0.10 for 2 consecutive years |
| 6 | `emit_transfer_rumor` | advance_day, STAGE 1/3 | contract ≤ 1y left + target team has appetite ≥ 0.18 + role fit (rate-limited 3/pass) |
| 7 | `emit_rivalry_match_night` | advance_day on matchdays | two teams scheduled today with h2h_count ≥ 4 |
| 8 | `emit_massive_upset` | play_tournament_round | winner seed ≥ 5 vs loser seed 1-3 (uses `tour.initial_seeding()`) |
| 9 | `emit_historic_performance` | run_end_of_year | new career_max_match_kills ≥ 30, KDx100 ≥ 200, or GF clutch this year (year-start snapshot diff) |
| 10 | `emit_dynasty_watch` | run_end_of_year | team has 3+ titles in last 5 years (reads `trophy_case().ordered`) |

Dedup: `news_emitted_keys_` (cleared at year rollover). State tracking: `consecutive_series_wins_` per Team, `h2h_counts_` keyed by team pairs (PairHash/PairEq), `max_kills_snapshot_at_year_start_` etc. for milestone diffs. Year 1 skips historic-performance emission until first snapshot exists.

**UI surfaces (`gui_main.cpp`):**
- **Favorites tabs (3 new)**: Trophy Room (KpiCards for W/M/T/MVPs/Role/IGL + all_titles list + color-coded career timeline), Hall of Records (2×3 grid of top-10 leaderboards: kills/MVPs/titles/rating/GF clutches/matches), Greatest Seasons (top-25 single-season table)
- **`Screen::PowerRankings`** new sidebar entry between League Standings and Tournament — H1 "Rankings" + top tabs Power/Community + region tabs inside. Row layout: rank + arrow (▲▼— or NEW for prev_rank=-1) + tier badge (S=gold, A=silver, B=bronze, C=gray, Bubble=dim) + tag + score + italic analyst_note. Community mode adds POP chip + DIVERGENCE badge when |community_rank − power_rank| ≥ 2.
- **`Screen::Compare`** new sidebar entry — two-column search picker, 13-row metric table (career stats, peak rating, trophies, MVPs, signature agent, earnings), head-to-head section using `head_to_head()`
- **Signature agent surfacing (4 sites)**: Player Profile header sub-line ("Best/Top/Notable {agent} player in {region}" computed locally by ranking same-signature peers via `agent_mastery.avg`), MVP Race new SIG column, scoreboard `[Sig]` chip beside gamertag, news_feed gold `[*]` chip + "(Signature X main)" italic subline when item.player_name has signature_agent
- Helper `find_team_by_name(AppState&, name)` shim resolves `Team*` from rankings back to `TeamPtr`

---

### 4.17 Belief-driven AI + temporal coherence + economy

> Cross-cutting reference section covering the intelligence layer that sits on top of Player / Team / GameManager: organizational memory, strategy inertia, championship windows, chemistry graph, transparent negotiation, deep AI re-sign loop, financial bands. The dated history of when these were introduced lives in [CHANGELOG.md](CHANGELOG.md).

#### Organizational memory (Team)

**`Team::OrgMemory`** — 6 rolling [-100, +100] metrics evolved at year-end:
- `rookie_success` — youth picks panned out? Drives snake-draft scoring (+25 if ≥30, -15 if ≤-30)
- `import_success` — feeds `team_import_appetite` (±0.04 swing)
- `veteran_success` — old-vet picks (+20 in snake-draft if ≥30)
- `financial_discipline` — payroll vs strategy tier_baseline; subtracts extra weight on expensive FAs
- `stability_culture` — cuts_this_year counter; modulates year-end cut threshold (0.85↔0.90) AND mid-season cut_chance (×0.70/×1.30)
- `star_dependency` — top1/avg ratio; tracked for UI

All decay 3%/yr. Updated in `Team::update_org_memory` called from `run_end_of_year` AFTER awards but BEFORE roster cleanup (so per-player stats from THIS season are observable).

**`Team::previous_strategy` + strategy inertia** — `commit_strategy_with_inertia(team, suggested)` runs a probabilistic transition matrix (e.g. TalentFarm→Contender 35%, WinNow→Bridge 50%). Orgs RESIST change; year-1 fast-path on equality means initial world behavior is identical. Replaces direct `t->strategy = classify_team_strategy(*t)` write.

**`Player::Desire`** — 6 archetypes (Greedy/Loyal/RingChaser/Mercenary/StabilitySeeker/Competitor) assigned at spawn via `pick_spawn_desire` (attribute + age + archetype scoring, weighted top-4 pick). Distinct from `rookie_archetype` (16 spawn template), `Archetype` (32 sim-driver), `playstyle_identity()` (derived label). Public accessors:
- `desire_salary_mult(team)` — ~[0.80, 1.30] (Loyal +5% from current team, RingChaser −15% to contenders, Mercenary +18%, etc.)
- `desire_accepts(offer_k, team)` — hard archetype gates (StabilitySeeker rejects Aggressive personality, RingChaser at 28+ rejects Rebuilders, Competitor in prime rejects weak teams)
- `desire_length_pref(team)` — 0..1 short→long bias

Wired into:
- `gen_contract` (Team-context overload applies salary_mult)
- Year-end re-sign loop (desire_accepts gate)
- `auto_fill::fill_one` (skips candidates whose desire_mult × cost > 40% budget; skips if desire_accepts==false)

#### Championship windows + chemistry (Team)

**`Team::TeamWindow`** enum (Opening/Open/Closing/Closed) computed yearly via `compute_team_window` from `avg_age + avg_potential + avg_ovr + avg_contract_years_left`. Recomputed in `run_end_of_year` between `update_org_memory` and `commit_strategy_with_inertia`. Drives mid-season `cut_chance × 1.40` for Closing (urgency), `× 0.70` for Opening (patient dev). Import appetite +0.05 for Closing, −0.03 for Opening.

**`timeline_fit_score(team, candidate)`** — `1.0·OVR + 0.5·(potential−ovr)·youth_factor + 0.4·contract_efficiency + 0.8·TimelineFit` where TimelineFit is a (window × age band × potential band) lookup. A 28y/o vet with high OVR scores +25 for Closing-window teams but −10 for Opening. Wired into snake-draft (weight 0.6) and `auto_fill::fill_one` (weight 0.4).

**Risk tolerance** — Contenders/WinNow score archetype `consistency_mod × +120` (pay for steady players, up to ±14 swing). Rebuilders/Development/TalentFarm reward NEGATIVE consistency_mod when `potential ≥ 80` (gamble on volatile high-ceiling boom/bust talent).

**Chemistry graph** — `Team::chemistry` map keyed on canonical-ordered `ChemKey {Player* a; Player* b}`. EMA update (α=0.15, clamp [-2, +2]). `Match.cpp` walks 25 pairs/team max after each non-friendly match; pair delta from shared outcome (both ≥1.10 in a win → +0.35-0.55; shared loss with both ≤0.85 → +0.08; gap ≥0.35 → −0.10; shared APR ≥0.30 in win → +0.05). Year-end decay 5% + prune entries where both endpoints off-roster. AI uses two ways: (a) `ai_manage_roster` protects players with `total_chemistry ≥ +2.0` from poor-rating cuts; (b) `auto_fill::fill_one` adds `+8·total_positive_chemistry` reunite bonus. Public: `record_chemistry(a,b,delta)`, `chemistry_between(a,b)`, `top_chemistry_pairs(n)`.

**UI surfaces:** Team Profile WINDOW pill (color-coded Opening green / Open accent / Closing gold / Closed red) + behavioral one-liner + ROSTER CHEMISTRY card (top-5 duos with signed values, ±2 horizontal bars). Dashboard WINDOW StatTile in KPI strip. Player Profile "Strong duo: ↔ X (+1.42)" line.

#### Re-sign Flow + Deep AI Re-sign + Market Competition

**`Player::ResignOffer`** — what the player would ask for if approached: `{amount_k, years, willingness 0..1, min_acceptable_k, max_acceptable_years, explainer}`.

**`Player::ResignBreakdown`** — per-factor scoring breakdown returned to UI and consumed by the engine acceptance check:
- `base_score=50, salary_mod` (monotonic in offer, capped ±55), `prestige_mod` (±14), `contender_mod` (window/strategy/roster ovr; RingChaser doubles, Competitor checks raw ovr), `loyalty_mod` (Loyal+same_team +22, +5 current-team tenure, +2/season ramp cap +10), `years_mod` (length vs preference), `desire_mod` (archetype gates −22..−25, Greedy/Mercenary −15/−18, mood floor −22, role-fit penalty −6..−45 by verdict band)
- `total` clamped [0, 100], `will_accept` = `total >= 55` (`kResignAcceptThreshold`)
- `verdict` ∈ {INSULTING (<35), WEAK (35-54), FAIR (55-69), STRONG (70-84), OVERPAY (≥85)}
- `labels` for UI display, `reject_reason` one-liner

**SINGLE SOURCE OF TRUTH** — `Player::accepts_resign_offer(amount, years, team)` is now literally `return evaluate_resign_offer(...).will_accept`. UI pill physically cannot disagree with engine decision. `evaluate_resign_offer` uses `gen_contract(randomize=false)` for deterministic ask reference — no per-frame RNG jitter.

**`refuses_to_negotiate`** — mood-inflated dignity floor: `offer < (30 + 70·mood)` returns true. Mood inflates the floor; higher offer monotonically reduces refusal. Smoke #13 pins the semantics.

**Deep year-end re-sign loop** — for every team's expiring players (`years_left <= 1`):
1. Player asks via `propose_resign_offer`
2. `team_eval_score`: age curve (+30 young → -15 decline), positional scarcity (+25 if only one), chemistry × 5, memory bonuses (veteran/rookie success ≥30), window fit (+15 Open/Closing+star, -25 Closed+old), financial pressure (-15 if too expensive), avg_match_rating tiers, strategy overlay (Contender/WinNow+star +20)
3. Counter-offer: stars (ovr≥80) get full ask, mids get 90%, low get `min_acceptable_k`
4. Counter years from `decide_contract_years` (multi-factor)
5. Player evaluates via `accepts_resign_offer`
6. On accept: `Team::resign_player(p, years, amount, year + 1)` (in-place mutation, no release+sign). **`year + 1`** because this loop runs at year-end inside `run_end_of_year` BEFORE `year += 1` — the new contract starts in the upcoming season, not the just-played one.
7. On reject: walk → market competition

**`Team::resign_player(p, years, amount, year)`** — in-place contract rewrite without release+sign (preserves chemistry, no IGL/Flex re-enforce needed). Debits budget. 5-arg overload also stamps `contract_role`.

**`Team::market_value_estimate(p)`** — wraps `gen_contract(0, false, false, this)` for UI display.

**Market competition for walked stars** — when player rejects re-sign AND `ovr ≥ 75`, find up to 3 same-region rival teams with positional need + budget ≥ market_value × 1.1, ranked by appetite + budget/4M + prestige/200. Each bids `gen_contract(year, randomize=true, false, &them)`. Player picks highest `willingness × amount` that clears `min_acceptable_k`. Bid years from each team's `decide_contract_years` (different teams offer different lengths same player). Signs at `year + 1` so contracts start the upcoming season. "X poaches Y from Z" news. **USER TEAM EXCLUDED from bidder pool** — won't auto-sign random FAs onto your roster.

**`Team::ai_full_offseason_pass(fa_pool, year, log)`** — 5-stage deep GM pass:
1. Header log (strategy/window/budget/roster snapshot)
2. Smart cuts: expired contracts + underperformers (threshold ±0.05 by stability_culture) + chemistry retention for cores (≥+2.0 sum)
3. Per-starter value assessment + targeted upgrade scan — `starter_value = ovr + age_curve + 8·avg_rating`. For each non-protected starter, scan same-role FAs for upgrades (delta ≥ +4 OVR), affordability (cost ≤ 40% budget), `accepts_resign_offer` check. Cut-and-replace with full log. Max **1 upgrade per pass**.
4. Cascade-fill backstop for any vacancies via `auto_fill_roster`
5. Bench depth (single slot, max roster 6) if budget ≥ $400K AND no positional duplicates at that role.

Every move logged: `"UPGRADE: Mach (Controller 57 OVR) → Vortex (74 OVR, +12.4 value)"`. Wired for all non-user teams in `run_end_of_year` (called with `year + 1`). User team's OFFSEASON-end fallback (when roster < 5) calls it too, with the already-incremented OFFSEASON `year`.

#### Async Sim Worker — Continue button non-blocking

Engine work moved off the UI thread. `Continue/Skip-to-next-match/Skip-to-Playoffs/Sim Full Season/Sim Solo Q Day` spawn `std::thread` running the sim. UI keeps presenting at 60fps; renders a full-screen "SIMULATING…" overlay with animated electric-blue spinner. Buttons disabled while running (BeginDisabled). Main loop polls `sim_thread.joinable() && !sim_running` each frame, joins, drains `pending_results` via `process_day_result` (UI-thread-only: opens live viewer modal + log lines). AppState destructor joins any pending worker. Pages aren't rendered during sim to avoid races with worker mutating engine state.

#### `Player::years_left(current_year)` — contract source of truth

`Player::contract_years` was being decremented unconditionally every year-end (including for FAs) AND `contract.exp_year < year` was the release gate. The two drifted; the release gate was off-by-one (3y contract from 2026 played 4 seasons). Fixed:
- Removed `contract_years -= 1` in `save_history_and_progress` (now a frozen snapshot of signed duration)
- Release gate: `exp_year < year` → `exp_year <= year` (3y from 2026 releases at end of 2028, correct)
- `years_left(year) = max(0, contract.exp_year - year + 1)`, 0 for FAs/Retired — SINGLE source of truth
- All UI display sites switched from `contract_years` to `years_left(s.gm.year)`
- `ai_manage_roster` signature: `(fa_pool, current_year)` so it can derive years correctly

**Plumbing:** `auto_fill_roster` + `scout_top_fas` take a `current_year` parameter and thread it through every callsite. Callers in `run_end_of_year` (re-sign loop, market poach, `ai_full_offseason_pass`) pass `year + 1` because they run BEFORE the `year += 1` step at the bottom of the function — new contracts there are for the upcoming season, not the just-played one. Mid-season replacement signings and user OFFSEASON paths pass the active year directly.

#### `Team::decide_contract_years(player, current_year)` — SINGLE source of contract length reasoning

Replaces every `rng().irange(1, 3)` and `c.exp_year - year + 1` callsite. Multi-factor decision:
1. Base: `desire_length_pref(team) × 4`
2. Window: Closing −1.0, Closed −1.5, Opening +0.8, Open +0.3
3. Strategy: WinNow −0.5, Rebuilding +0.7, DevelopmentFocus +0.9, TalentFarm +0.4, Contender +0.2, BudgetRoster −0.8, Bridge −0.2
4. Age: ≤20 +1.2, ≤23 +0.6, ≤27 +0, ≤30 −0.6, 31+ −1.6
5. Potential gap: ≥12 + age≤24 → +0.7; ≥8 + age≤26 → +0.4; gap<0 → −0.3
6. `memory.financial_discipline`: ≥30 −0.4, ≤−30 +0.3
7. Budget headroom = `budget / (50·1000·5)` years; ≤1 → −0.8, ≤2 → −0.3; rich+Open/Closing → +0.3
8. Clamp [1, 4]; cap at player's `max_acceptable_years`

Used by: snake draft, auto_fill, scout_top_fas, mid-season replacements, year-end re-sign counter, market competition (different teams offer different lengths for same player!), `ai_full_offseason_pass` (upgrade + bench), USER's Transfer Market Sign button.

#### Role lock + role-fit evaluator

- **`Player::contract_role`** — field stamped at every signing path (`sign_player` + `resign_player` both have Role-aware overloads). `Role::Count` means "use natural" (delegates to `primary_role`). Lets the UI show "hired as X" and the negotiation evaluator apply role-fit penalties consistently.
- **`Player::role_fit_score(Role)`** — pure 0..1 function. Weighted attribute composites per role (Duelist: Aim+Headshot+Entry+Aggro+Movement; Initiator: Utility+Comm+GS+Intel+MidRound; Controller: Intel+Decision+Comm+MidRound+GS; Sentinel: Pos+Anchor+Clutch+SpikeHandle+Adapt). 1.00 for own primary_role.
- **`Player::role_fit_verdict(Role)`** — bands: Natural / Good fit / Possible / Stretch / Mismatch.
- **Role-fit penalty in `evaluate_resign_offer`**: 0 for natural, −6 good, −14 possible, −28 stretch, −45 mismatch. Overpay (+55 salary cap) can rescue a "stretch"; mismatches near-reject.
- **`primary_role` is write-once at spawn.** No engine code mutates it dynamically. `contract_role` is belt-and-braces protection against future drift.

#### Financial Rebalance — economy is now tight

| | Old | New |
|---|---|---|
| Salary cap | $999K | **$180K** (`kSalaryCapK`) |
| Salary floor | $15K | **$10K** (`kSalaryFloorK`) |
| Superstar band | 80-360 | **80-150** |
| Star band | 50-110 | **50-90** |
| Strong band | 30-60 | **30-60** |
| Mid band | 22-37 | **18-35** |
| Bench band | 15-22 | **10-18** |
| WinNow budget threshold | $2.5M | **$1.5M** |
| Contender threshold | $1.5M | **$900K** |
| BudgetRoster threshold | <$800K | **<$400K** |
| User team default | $1.5M | **$800K** |
| Wizard Rich tier | $3M | **$2M** |
| Wizard Budget tier | $800K | **$400K** |
| Sponsorship formula | `600 + prestige·22` | **`200 + prestige·8`** |
| Import appetite budget tier | 1M-2M floor/ceiling | **600K-1.5M** |
| Org memory tier_baseline | Contender 1.5M / WinNow 2.5M | **900K / 1.5M** |

Constants `vlr::kSalaryCapK = 180`, `vlr::kSalaryFloorK = 10` in Common.h. God-mode salary slider bounds honor these. `decay_demands` floor → 10. `amount_with_mood` clamped to [10, 180].

> Current MVP / Role-of-the-Year / IGL-of-the-Year scoring formulas (which fold team success into all three) live in §4.14. Current Hall of Good gates live in the Frivolities sub-section of §4.16. Current smoke-test list lives in §9.

---

## 5. UI (`gui_main.cpp`)

Single TU, ~12.5k lines (grew from ~6.3k pre-Phase-1 → ~8.1k post-Phase-1 → current after the Phase-2A flags/logos/wizard, the belief-AI re-sign + FA negotiation modals, the role-fit combo, and the clickable My Team chip). ImGui + Win32 + DX11.

### 5.1 Fonts
`g_font_body` (Segoe UI 16), `g_font_small` (13), `g_font_h2` (Semibold 22), `g_font_h1` (Black 30), `g_font_kpi` (Black 38), `g_font_mono` (Consolas 15).

### 5.2 Color palette + design system (premium dark)

**Base palette** (constant NAMES kept, values retuned so all screens inherit). 2026-06-11 premium pass swapped the primary `kAccent` from electric cyan to a warm amber; cyan demoted to `kInfo` for data lines / info chips; shadows now tinted.
```cpp
kVlrNavy/kBgBase  #0F1117  deep charcoal window bg
kBgDeep           #0B0D12  deepest (gutters / headers)
kSurface          #171A24  card bg
kSurfaceAlt       #1C202C  alt row / hover
kSurfaceHi        #242938  raised / selected
kBorder           #2A2F3D  hairline border
kVlrText          #E9ECF1  near-white text
kVlrSub           #8B93A3  muted secondary text
kTextFaint        #5E6674  captions / labels
kAccent           #D4A14C  warm amber — PRIMARY interaction accent
kAccentDim        #9D7635  darker amber — pressed / focus depth
kAccentSoft       #D4A14C 13%  for hover overlays / soft glows
kAccent2          #7B5EFF  purple — secondary (archetype chips / histo)
kInfo             #5BA8B0  muted teal — info chips / PlotLines (former kAccent)
kInfoDim          #3F787F  darker teal
kVlrBlue          #00D4FF  legacy cyan (still aliased — use kInfo in new code)
kVlrGold          #FFD700  prestige / trophies ONLY — brighter than kAccent
kVlrGreen         #3FD17A  success
kVlrRed           #FF4655  VLR brand red — danger
kShadow           #05070B 56%  surface-hued drop shadow (depth)
kShadowSoft       #05070B 31%  softer shadow halo
```
**Color discipline rules (2026-06-11):**
- Trophy / prestige moments → `kVlrGold`. Never use for routine UI interaction.
- User-agency interaction (buttons, sliders, focus rings, sidebar active, hover lifts) → `kAccent`.
- Data lines, info chips, stable-state indicators → `kInfo`. Don't mix into interaction surfaces.
- Drop shadows → `kShadow` / `kShadowSoft`. Never `IM_COL32(0,0,0, alpha)`.
Role colors unchanged (muted brick/indigo/forest/steel/amber + gold IGL) — deliberate prior user decision, still wired through `role_color`, `position_color`, scoreboard agent tinting, `RoleBadge/PositionBadge/IglBadge/FlexBadge`.

**`ApplyVlrTheme`** (2026-06-11 premium pass) — varied radius scale: Window 14 (softest outer chrome), Child 10 (panels/cards), Frame 6 (instrumented inputs/sliders), Tab 8, Scrollbar 12, Grab 6, Popup 12. Uniform rounding everywhere was the prior generic-AI default — never reintroduce. Generous padding: WindowPadding (24, 22), FramePadding (13, 8), CellPadding (12, 7); ItemSpacing (12, 10) for breathing room. Accent-driven interaction states (buttons/sliders/checkmarks/scrollbars use `kAccent` — warm amber); `ImGuiCol_PlotLines` uses `kInfo` (muted teal) so data lines don't compete with the gold interaction primary; `ImGuiCol_TextSelectedBg` uses 25%-alpha amber. Transparent tabs/childbg, subtle white-alpha row striping. Border kept hairline at 1px via `ChildBorderSize`/`PopupBorderSize` only.

**`DrawSidebar` premium polish (2026-06-11):** active nav gets a 4px full-height `kAccent` "you are here" bar + `kSurfaceHi` raised bg + a 6% amber tint overlay; active text uses a brighter amber `IM_COL32(0xE8, 0xB8, 0x60, 0xFF)` for max contrast on the raised bg. Inactive items hovered get a 2px half-accent left bar painted on top of the framework hover bg. `navgroup` headers (Home/Team/Competition/People/History/Live) have a 1px `kBorder` hairline beneath the title so the six buckets read as distinct sections. User-org chip: hover paints a top-edge `kAccent` stripe mirroring active-nav language; tag font color is `kAccent` (not `kVlrGold`) for interaction-consistency. God Mode OFF uses a `kSurfaceAlt` fill (`kSurfaceHi` hover) so it reads as a real toggle row.

**Design-system helpers** (defined ~gui_main.cpp:420-580 — compose ALL new UI from these):
- `BeginCard(id, bg, pad, rounding, hover_lift=false)/EndCard()` — auto-height rounded bordered card. As of 2026-06-11 every card paints a **tinted stacked drop shadow** via parent-draw-list channel-split + a 1px top inner highlight. `hover_lift=true` adds a 13% `kAccentSoft` tint overlay when the card is hovered. Backward-compat default keeps existing callers identical.
- `BeginCardSized(id, size, bg, pad, rounding, hover_lift=false)/EndCardSized()` — fixed-size card with the same depth stack.
- `TableRowSelectable(label, selected=false, flags=0, size={0,0})` — drop-in wrapper around `ImGui::Selectable()` for table rows. On hover paints a 3px `kAccent` left bar + 13% `kAccentSoft` row overlay. Apply to row Selectables everywhere except sidebar nav, bracket cards, and combo/dropdown items.
- `Pill(text,bg,fg=0,extra_pad)` / `PillOutline(text,line,fg)` — rounded chips, auto-contrast text
- `SectionHeader(title,subtitle,accent)` — accent bar + H2 + dim subtitle
- `StatTile(label,value,accent,size,sub)` — KPI tile with accent edge
- `MutedText(fmt,...)` / `VSpace(h)` / `ThinDivider(pad_y)` / `brightish(ImU32)`
- `SigBadge(Player&)` / `FlexBadge()` / `IglBadge()` / `PositionBadge(Position)` — inline player-side chips.

**Internal card helpers (2026-06-11):** `CardFrame_` + `CardStack_()` thread the parent draw list + `ImDrawListSplitter` from each Begin to its matching End so the tinted drop shadow can be back-painted into channel 0 with the child's resolved final rect (works for AutoResizeY and fixed-size). `CardBegin_` / `CardEnd_` / `CardPaintInner_` / `PushCardStyle_` / `PopCardStyle_` are the implementation seams — do NOT call these directly from screen code; always go through `BeginCard` / `EndCard`.

**Tabular figures rule (2026-06-11):** every numeric cell in a multi-row table wraps `ImGui::Text("%d|%.2f|$%dK|%.0f%%", ...)` with `ImGui::PushFont(g_font_mono) / ImGui::PopFont()`. Text columns (names, regions, agent names) STAY in body font. Already done across DrawRoster, DrawStandings, DrawMarket, DrawSoloQ, DrawCompare, DrawLeagueLeaders, DrawHallOfFame, Awards History, Favorited, GOAT Career / GOAT Season / Hall of Records / Team Profile roster / Player Profile Match History. New tables MUST follow the rule.

All screens use these helpers (cards, KPI strips, pill badges, section headers, striped tables, broadcast-style score banner/scoreboard, VCT bracket). Engine/data/logic flow through here untouched — this is purely the visual layer.

### 5.3 Helpers

`H1/H2/Sub`, `RoleBadge(Role)`, `PositionBadge(Position)`, `IglBadge()` (small gold pill chained after PositionBadge when `is_igl == true`), `FlexBadge()` (muted-amber companion), `SigBadge(Player&)` (canonical muted-gold inline signature-agent pill — empty `signature_agent()` => no-op; used everywhere a player name appears in a list/row), `CountryFlag(iso)` (procedural), `KpiCard`, `StatTile`, `Pill`, `PillOutline`, `SectionHeader`, `MutedText`, `BeginCard/EndCard`, `BeginCardSized/EndCardSized`, `player_label(p)`, `fmt_money`, `OpenPlayerModal`, `OpenTeamProfile`, `OpenLiveMatch`, `OpenLiveMatchSeries`, `OpenReplay`, `OpenReplaySeries`.

**Negotiation modal:** `DrawNegotiationModal(state, mode, popup_id, target, team, years, amount, role, cached_offer, cached_mkt_k, cached_for, show_modal)` is the **single** entry point for both Re-sign (Roster → Re-sign button) and FA Sign (Market → Negotiate button). Caller picks `NegotiationMode::Resign` or `NegotiationMode::FreeAgentSign`. The helper handles the offer cache, role combo, transparent breakdown, action buttons, AND the commit (Resign → `Team::resign_player` / FA → pre-flight gates + `Team::sign_player` + budget debit + `sync_player_ranked_region`). Don't reimplement either flow inline — extend the helper.

### 5.4 Sidebar screens
1. Dashboard
2. News
3. Calendar
4. My Roster
5. Manager (subtabs: Player Development / Head Coach / Finance / Commissioner)
6. League Standings
7. Tournament — **TABS PER REGION** (Americas / EMEA / Pacific / International)
8. League Leaders
9. Transfer Market
10. Solo Q Ladder
11. Favorites (MVP Race / Awards Recap / Awards History / Hall of Fame / Favorited / GOAT Career / GOAT Season / Formula Lab)
12. Watch Match
13. Event Log

Sidebar bottom: CONTINUE, Skip to next match, Skip to Playoffs (no-op if already in playoffs), Sim Full Season, Sim Solo Q Day, GOD MODE.

### 5.5 Modals — Player Profile tabs

- **Overview** — sliders + god-mode IGL toggle, agent pool, per-team morale, force-retire, MMR reset. Includes Rookie Archetype combo + Playstyle Identity chip + Position chip + Signature Agent strip.
- **Detail** — info, agent pool, AWARDS (color-coded by `[W]/[M]/[T]/[A]`, Total trophies count + red anomaly tint if >10 under age 25), SEASON HISTORY, god-mode badge editor.
- **Match History** — Pro / Solo Q tabs. EVENT / MAP / MATCHUP / SCORE / **W/L** / K-D-A / **HS%** (per-match from `history_record.stats[p].hs`) / RATING / MVP.
- **Map Mastery** — comfort map gold strip, sorted per-map table, god-mode editor.
- **Agents & Mastery** — sorted by `update_agent_pool` SCORE. Columns: # / AGENT / ROLE / KEY ATTRS / FIT / MAT / AVG / LAST / SCORE. God-mode editor.
- **IGL Profile** (only when `is_igl`) — leadership stats, tendency sliders, IGL IMPACT section.

**Header strip**: gold `[*] Signature {agent}` strip when `signature_agent()` non-empty. Below that: Playstyle Identity chip. Below that: Position chip (with `• IGL` suffix when is_igl).

### 5.6 Live Match Viewer (`DrawLiveMatchModal`)

Two open paths: `OpenLiveMatch` (single map, friendly), `OpenLiveMatchSeries` (full series, friendly), `OpenReplay` (single recorded map), `OpenReplaySeries` (series of recordings).

**Layout:**
- Score banner with team colors + tags + per-team comp tag subtitle
- **SERIES COMP EVOLUTION** panel (only if `series.size() > 1`) — rows up to `current_map_idx + 1` (no future-map spoilers)
- Round timeline (only renders revealed rounds + 1 current; FIXED 24px slot width)
- Auto Play / Next Round / Skip 5 / Skip to End controls + Speed (Slow 1.20s / Normal 0.60s / Fast 0.25s / Round-skip 0.30s)
- **Prev Map / Map N / Next Map** (validates index + round_history)
- Side-by-side scoreboards (DrawTeamScoreboard) — 9 columns including HS%
- ROUND FEED — kill cards, event_cursor-aware
- **Export to JSON** buttons on map-nav row + scoreboard footer (visible only on final map of series)

**Solo Q scoreboard fallback**: `RecordedMatch.team1/team2` are weak_ptr; for solo Q the synthetic Teams die when `force_solo_match` returns. UI consumers fall back to `rec->history_record->blue_team/red_team` (vector<Player*>) when `lock()` fails.

**Spoiler discipline (strict):** UI shows `Map N` not `Map N / total`. Round timeline only renders played rounds. DEF/OT labels gated to `n_visible > 12 / > 24`. "in progress (3 kills so far)" not "(3/8 kills)". `live.series.size()` never exposed to user.

### 5.7 Scoreboard agent-name color

`DrawTeamScoreboard` colors the agent name by the AGENT's role (via `chosen_agents_[player]->role` looked up via `find_agent_by_name(agname)`), NOT by `p->primary_role`. A Controller main flexed onto Killjoy renders in Sentinel green, not Controller indigo.

### 5.8 Bracket UI (VCT-style)

Per-tournament rendering wrapped in `PushID/PopID`. **Tabs per region** + International. Layout:
- **Two vertically separated panels** inside a horizontally scrollable child: UPPER BRACKET on top (gold H2 header), LOWER BRACKET below (muted-red H2 header), horizontal divider between
- **Cards** 152×60, 2-row team layout (tag + name + right-aligned score). Winner row tinted via team primary color stripe; loser dimmed; unplayed mid-tier opacity
- **BO5 chip** — 28×12 gold pill in card top-right when `best_of >= 5`. Visible on LB Final + GF + GF Reset only
- **Connecting lines** rendered via `DrawBracketConnector` (H-V-H 3-segment polyline). Color: `kLinePlayed` for played, `kLineUnplayed` for unplayed
- **Round labels** sourced from `Tournament::current_round_label()` for the live round; played rounds use `BracketMatch::label` or fall back to a count-based generic that takes `(round_idx, total_rounds, is_lower)` so 1-card LB rounds NOT at the end label as "LB Semifinal" while only the actual last LB round becomes "LB FINAL".
- **Grand Final column** sits right of UB panel, centered vertically against UB final card. Single GF card (no reset stacking — modern VCT format)
- **Empty-bracket guard**: shows "Bracket starts after group stage." if no UB history and no scheduled UB
- **Per-card invisible buttons** ID'd via `id_base + round*100 + i` namespaced per tournament

**Feeder math — size-aware (CRITICAL, 2026-06-11):**
- Each round's card has 1 or 2 feeders from the previous round. The rule:
  - `prev_n == cur_n` → **straight pair**: card `i` feeder is prev card `i` (used by LB R1→R2 and LB SF→Final, where the "other feeder" is a UB drop-in that isn't drawn in the LB panel).
  - `prev_n == cur_n * 2` → **halving**: card `i` feeders are prev cards `i*2` and `i*2+1` (used by every UB round and by LB pure-halve rounds).
  - Otherwise (degenerate): clamp like halving with `std::min(prev_n - 1, ...)`.
- Same rule applies to vertical Y placement (midpoint-of-feeders) AND to connector drawing.
- The OLD bug: halving math everywhere clamped LB R2's two cards both onto R1's last card, collapsing the LB panel. Never reintroduce the unconditional `i*2 / i*2+1` form.

**LB padding (CRITICAL, 2026-06-11):**
- `build_round_grid` for `is_lower=true` pads future rounds with TBD placeholders matching the canonical LB shape — odd next-round indices MERGE (`size = prev_size`), even next-round indices HALVE (`size = (prev_size + 1) / 2`). For 8 teams that yields [2, 2, 1, 1]; for 16 teams [4, 4, 2, 2, 1, 1]; for 4 teams [1, 1].
- `lb_target` comes from `2 * log2(initial_seeding().size()) - 2`, NOT from `lower_bracket_round_count()` (that accessor returns the count of *played* LB rounds — useless for padding).
- Last padded round stamps `best_of = 5` so the gold "BO5" badge renders correctly on the LB Final placeholder.

Click handler: played → `OpenTeamProfile(winner)`. Unplayed → `OpenLiveMatchSeries(s, a, b, event_name, best_of)` using each match's stamped `best_of` (BO3 default, BO5 for LB Final + GF + Reset).

Placement strip below bracket — only rendered when `tour->finished()` (label: "FINAL PLACEMENT:").

`tour_tab_for(t)` uses explicit substring matching (`"Americas"` / `"EMEA"` / `"Pacific"`), MASTERS/CHAMPIONS to International, unknown names fall through to International.

### 5.9 Roster pages (1D + 1C + 1S + 1I + 1Flex layout)

`DrawRoster` (My Roster) + Team Profile Roster tab — bucketed 5-slot layout in canonical order: Duelist / Controller / Sentinel / Initiator / Flex. Empty slots show `[empty Position]` in red; duplicates show `[duplicate]` chip. Bench rows below, sorted by OVR desc.

Each row renders `PositionBadge(position_of(*p))` followed by `IglBadge()` when `p->is_igl == true` — produces "Controller • IGL", "Initiator • IGL", "Flex • IGL", etc.

### 5.10 Team Profile

Bio tab shows:
- `Comp Identity: {identity_name}` (Balanced / AggressiveDive / etc.)
- `Comp shape: 1D + 1C + 1S + 1I + 1Flex (+ 1 IGL flag on any starter)`
- `Flex tendency: {Flex player} rotates between {Role A / Role B / Role C}` — derived from Flex player's agent_pool distinct roles. 1 role = "locked to {Role}" + "(low rotation player)".

### 5.11 Transfer Market — Sign button

Pre-flight gates: refusal / budget / import-cap (`current_imports >= max_imports`) / roster-full. Each gate emits specific reject reason via `s.log`. Post-condition check: `roster.size() == before+1 && p->team_name == team->name`. Failure refunds budget. Success logs full summary. FA list is recomputed each frame from `solo_qs[*]->global_ladder()` filtered by `team_name == "Free Agent"`.

`Team::sign_player` returns void — UI uses post-conditions to detect failure. Could be changed to `bool` for cleaner detection.

### 5.12 God Mode

Sidebar GOD MODE toggle (gold when on):
- Player Profile: editable sliders, IGL toggle, rookie archetype combo, badge add/remove, agent mastery editor (matches/avg/peak sliders per agent), map mastery editor
- Manager → Player Development: HIDDEN PEAK ARCHETYPES reveal
- Manager → Head Coach: editable name, stats, age, salary, contract length slider
- Manager → Commissioner: per-region team table (Budget/Prestige/Sponsorship/Strategy)

---

## 6. JSON Export (`MatchExport.h/cpp`)

`vlr::export_series_to_json(recordings, event, best_of, year, day_in_year)` produces pretty-printed JSON. Hand-rolled writer, no library dependency. C++17 stdlib only.

**Schema v1** — top-level: `schema_version`, `exporter`, `exporter_notes`, `match_id` (deterministic hash), `event`, `series_type`, `best_of`, `exported`, `teams`, `final_score`, `winner`, `maps[]`, `series_totals`.

Per map: `map_number, map_name, score, winner, rounds_played, overtime, mvp, players: {team1: [...], team2: [...]}`.

Per player: `name, agent, role` (from agent's actual role, NOT primary_role), `is_igl, k, d, a, rating, acs` (estimated, flag), `adr, hs_pct, fb, fd, fk_fd_diff, fb_pct, kast_pct, clutches, damage, max_round_dmg, multi_kills {2k/3k/4k/5k}, rounds_played`.

Reserved for v2: round_history, economy_log, utility_impact, heatmap_kills, ability_usage, timeline_events.

**Defensive:** empty recordings → `""`. Missing history_record/match_stats → empty player arrays. Weak_ptr team `.lock()` falls back to recorded name.

**UI buttons** in `DrawLiveMatchModal` (2 slots: map-nav row + scoreboard footer). Only visible on final map of multi-map series. Native Win32 `GetSaveFileNameA` dialog. Default filename `{event}_{team1}_vs_{team2}_{year}-{day}.json` sanitized to `[A-Za-z0-9_-]`, capped 80 chars. Requires `comdlg32.lib` linked.

---

## 7. Magic Numbers — Cheat Sheet

| Number | Where | Meaning |
|---|---|---|
| 200 | `kDaysPerYear` | Season cycle |
| 30 | `days_since_progression` | Monthly progression interval |
| 25% | `is_igl` spawn rate | Fraction of IGL spawns |
| 60 | OVR floor for imports | |
| 2 | `config().max_imports` | Max imports per roster |
| 3 / 11 | STAGE pacing | (days/round, total rounds) |
| 1 / 9 | REGIONALS pacing | |
| 1 / 14 | MASTERS pacing | |
| 1 / 16 | CHAMPIONS pacing | |
| 1 / 14 | OFFSEASON pacing | |
| 2 | bracket_bo_for threshold | Only ≤2 teams = BO5 |
| 0.95/0.30/-0.38/0.003/0.45/0.34/0.235 | VLR rating coefficients | KPR/APR/DPR/ADRa/SR/KAST/intercept |
| 130-180 | damage per kill range | |
| -12/-10/-8/-4 +14/+10/+10/+8/+8/+12/+12/+10 | IGL stat shift | |
| 7 / 5 | Roster cap / starters | |
| 4 | years_unsigned mandatory retire | |
| 3 | career_seasons_played minimum | HoF gate |
| 4 of 10 | HoF criteria (was 4 of 9 — added career IGL impact >50) | |
| 1000 | $K (=$1M) | HoF earnings criterion |
| 30 | Pro match kills | HoF criterion 7 |
| 200 | KD x100 (=2.0) | HoF criterion 8 |
| 50 | career igl_impact_total | HoF criterion 10 |
| 0.90 | Mid-season cut threshold | |
| 900 | Initial solo Q ladder size | |
| 3 / 6 | Solo Q loops/day (matchday/off) | |
| 0.30 / 0.32 | Solo Q activity per loop (pro / FA) | |
| 4 | Solo Q per-player daily cap | |
| 55 | Rookies per region per year | |
| **95** | OVR hard cap | |
| **0.92×, -9** | OVR transform | `mean × 0.92 - 9` |
| 8 | season_matches | Awards qualification |
| 5000 / 250000 | Stat sanity caps | year-end overflow guard |
| 0.00045 | Spawn-time badge chance | |
| 4 | Total badge cap | |
| 0.28 | team_import_appetite ceiling | |
| 1.05 / 0.90 / 0.75 | Region multipliers | Americas / EMEA / Pacific |
| 24 / 12 | Import adaptation_months | initial / yearly decay |
| 0.95 | IGL focus tax (per duel) | |
| 0.32 | Per-duel power multiplier cap | |
| ±0.09 / ±0.12 | `compute_attr_extras` clamp | identity-channel total |
| 2.5 | `compute_igl_impact` per-match cap | |
| 0.35 / 0.25 / 0.20 / 0.20 | IGL of the Year coefficients | rating / per-match impact / team finals / wins/40 |
| 250 | NamesData top-N per pool | |
| 27 | Countries in zengm-sourced names | |

### Role assist multipliers
- Initiator: 1.00
- Smoke Controller (Brimstone/Omen/Clove): 0.85
- Other Controller: 0.30
- Sentinel: 0.45
- Duelist: 0.12

### Live match playback speeds (sec/kill)
- Slow: 1.20 / Normal: 0.60 / Fast: 0.25 / Round-skip: 0.30

---

## 8. Common Pitfalls

1. **Player::name shadowing** — `Player::name` shadows local `name` in lambdas. C4458 warning, harmless.
2. **`vlr::badges()` vs `Player::badges`** — use `::vlr::badges()` to disambiguate.
3. **`vlr::clamp_v` namespace** — use the `vlr::` prefix.
4. **`Player::is_igl` shifts attributes ONCE** — never assign directly. Only via `Team::enforce_one_igl()` or god-mode toggle.
5. **`Team::sign_player` is the SOLE roster mutation entry point** — calls `enforce_one_igl()` + `enforce_one_flex()`. Direct `roster.push_back()` breaks invariants.
6. **`is_friendly_` matches** — Watch screen passes friendly=true. Don't change without breaking demo viewing.
7. **`pro_match_history` cap = 10** — used to silently lose maps when user had 10+ entries. NEVER diff `hist.size()` to detect new matches; use `Series.all_recordings()`.
8. **`schedule_next_bracket_round` snapshot+clear** — snapshot `ub_alive_`, CLEAR it, then `pair_matchups(snapshot, ...)`. Without the clear, play_round appends winners onto the existing pool → bracket size doubles every round.
9. **Tournament BO levels** — `bracket_bo_for(2) = 5`, all else 3. GF + GF Reset hardcoded BO5.
10. **Series BO2** — special-cased in `is_over()`: always 2 maps, ties allowed, no W/L credited.
11. **Live viewer state leakage** — `OpenLiveMatch` / `OpenReplay` / `OpenReplaySeries` MUST clear `live.series.clear()` + `live.current_map_idx = 0` + `live.event_cursor = 0`.
12. **Map nav buttons** — validate `live.series[new_idx]->round_history` is non-null before assigning.
13. **Skip-button auto-pop bug** — every early-return path in skip handlers MUST clear `last.user_match_recording` + `user_match_series`.
14. **`history_record.blue_team` / `red_team`** — `vector<Player*>`, NOT `vector<PlayerPtr>`. Use raw pointer comparison.
15. **`MatchHistoryEntry.awards` is a CUMULATIVE snapshot** — filter by `to_string(h.year)` substring (see goat_season_score).
16. **PowerShell `*>` redirect buffers** — use `cmd /c "build.bat release < nul > log 2>&1"` for live output + suppress pause.
17. **Build link silently fails when exe locked** — always `Stop-Process` running exes before rebuild.
18. **Tournament page tabs** — each tournament wrapped in `PushID/PopID`. Without this, hovering bracket cards crashed with "2 visible items with conflicting ID".
19. **OFFSEASON phase advancement** — AWARDS → `phase_idx++` → OFFSEASON. After 14 days → STAGE 1. `run_end_of_year` does NOT reset phase_idx.
20. **OVR-driven salary** — `value_p = 0.55 × ovr + 0.45 × potential`. Lower OVR → lower salary tier.
21. **`active_tournaments` lifecycle** — `force_finish_stale_tournaments` is the SOLE clear path. Auto-clearing mid-phase silently double-spawns brackets.
22. **`Series::add_match_data` pin step honors `Match::is_friendly()`** — friendly matches skip pin block.
23. **`RecordedMatch.team1/team2` are weak_ptr** — every callsite must `.lock()` to a local TeamPtr at scope top + nullptr-guard. Solo Q synthetic Teams die when force_solo_match returns; UI consumers MUST fall back to `rec->history_record->blue_team/red_team`.
24. **Group standings sort is `stable_sort` + alphabetical fallback** — `(wins, map_diff, round_diff, name)`. Non-stable sort + 3-way ties → non-deterministic playoff seeding.
25. **`award_event_titles` early-returns on unrecognized name** — no silent `[*]` ghost awards.
26. **8-team / 4-group bracket seeding** — `{p[0], p[2], p[6], p[4], p[7], p[5], p[1], p[3]}` guarantees each group's #1 and #2 are in opposite halves.
27. **`Series::add_match_data` lazy-inserts unseeded players** — catches mid-series substitutions.
28. **Assist gating uses the AGENT's role, not `Player::primary_role`** — Match.cpp keys `is_smoke_controller` + `role_assist_mult` off `chosen_agents_[player]->role`. Was the cause of "Astra acting like a Duelist".
29. **Trophy participation filter** — `award_event_titles` awards `champion->roster ∩ participated_players_`. Phantom roster members (signed mid-tournament but never played) don't get the trophy. `participated_players_` populated incrementally via `absorb_series_participants`.
30. **`pinned_titles_already_` fuse** — one-shot guard on Tournament preventing double-firing of `award_event_titles`.
31. **Spoiler discipline** — NEVER expose `live.series.size()`, total round count, or `best_of` to the user in the live viewer. Round timeline only renders revealed rounds + 1. "Map N", not "Map N / total".
32. **Scoreboard agent name color uses AGENT role** — `find_agent_by_name(agname)->role`, not `p->primary_role`. Otherwise flex picks show in the wrong color.
33. **`is_igl` and `is_flex` are INDEPENDENT** — a player CAN be both (rare "Flex IGL"). Don't add mutual exclusion enforcement.
34. **Position::Initiator is the fallback** — `position_of()` returns Initiator if no other role matches. Don't change without auditing the 5-slot bucketing.
35. **Position enum has 4 values only** (D/C/S/I + Count) — NO Flex. Flex is an overlay flag, not a position. Reintroducing Position::Flex breaks `auto_fill_roster` slot logic, roster bucketing, and assist attribution.
36. **Duelist IGLs require elite mental composite** — both `roll_is_igl_at_spawn` (gate ≥0.78) and `enforce_one_igl` (veto if score < 0.80) check this. Don't lower the threshold without user sign-off — they explicitly want Duelist IGLs to be "extremely rare".
37. **`enforce_one_flex` is insert-only** — never demotes. Multiple Flex players in starting 5 are legal. Adding demotion would revert intentional user design.
38. **Transfer Market has NO Position filter on FA list** — `DrawMarket` iterates `solo_qs[*]->global_ladder()` filtered ONLY by `team_name == "Free Agent"`. Adding any Position-keyed filter would re-introduce the Flex-players-missing bug (previously caused when `position_of` returned `Position::Flex` and no filter bucket existed for it).
39. **Tabular figures rule (2026-06-11)** — every numeric body cell in a multi-row table MUST be wrapped with `ImGui::PushFont(g_font_mono) / ImGui::PopFont()` so digits align vertically across rows. Text columns (names, regions, agent names) stay in body font. Skipping this on a new table produces visible "wobble" in stat columns that violates the project's design-system rule. See §5.2 for the full call-site list.
40. **`BeginCard` shadow is back-painted at `EndCard` time** — the helper splits the parent draw list into two channels (0=shadow underlay, 1=card+content), captures the child's resolved final rect right before `EndChild`, then paints `kShadow`+`kShadowSoft` onto channel 0 and merges. Implications: (a) every `BeginCard(...)` MUST have a matching `EndCard()` or the splitter never merges and the draw list is left in a broken state; (b) the per-card `CardFrame_` is held in a file-scope `std::vector<CardFrame_>` stack — nested cards work, but `BeginCard` followed by `BeginChild` (without `EndCard`) does not.
41. **`TableRowSelectable` scope (2026-06-11)** — wrapper that adds the warm-gold hover-lift (3px left bar + 13% kAccentSoft overlay). Use for row Selectables in tables (player rows, team rows, FA rows, tournament participants, leaderboards). DO NOT use for sidebar nav (own bespoke paint), bracket cards (own custom hover tooltip), combo/dropdown items. Using it on those surfaces double-paints the hover affordance and fights with the bespoke art.
42. **Pure-black drop shadows are a regression** — every new shadow MUST use `kShadow` or `kShadowSoft` (surface-hued black) per §5.2 color discipline rules. `IM_COL32(0, 0, 0, alpha)` shadows read as a generic AI fingerprint against the real broadcast palette. The only legitimate pure-black `IM_COL32(0,0,0,...)` uses still in the file are flag outlines (alpha 0xC0) and transparent-button fills (alpha 0).
43. **Color discipline: kVlrGold vs kAccent vs kInfo** — trophy/prestige only ever uses `kVlrGold` (bright `#FFD700`). User-agency interaction (buttons, sliders, focus rings, sidebar active, hover lifts) uses `kAccent` (warm amber `#D4A14C`). Data lines / info chips / stable-state indicators use `kInfo` (muted teal `#5BA8B0`). Mixing these breaks the visual hierarchy — e.g., painting a button in `kVlrGold` falsely signals "this is a trophy moment", and painting a plot line in `kAccent` competes with the gold interaction primary.
44. **`ImDrawListSplitter` MUST NOT be held by value in any owned container (2026-06-16)** — `ImDrawListSplitter` contains `ImVector<ImDrawChannel> _Channels;`, and `ImVector<T>::operator=` does a **shallow `memcpy`** (imgui.h:2061). Copying a struct that owns a splitter aliases its channel `_CmdBuffer.Data` / `_IdxBuffer.Data` pointers between source and copy; the next destructor to run frees memory the other copy still references → `STATUS_HEAP_CORRUPTION` flagged by ntdll on the next allocation. That was the root cause of the 2026-06-16 Start-a-New-Career crash. **Rule**: any struct that owns an `ImDrawListSplitter` and ends up in a `std::vector` / `std::optional` / similar value-storing container MUST hold the splitter via `std::unique_ptr<ImDrawListSplitter>`. The unique_ptr deletes the implicit copy ctor, forcing move semantics and turning any future regression into a compile error. See `CardFrame_` in `gui_main.cpp:483` for the canonical pattern, and `CardBegin_` / `CardEnd_` for the move-on-push / reference-on-pop access pattern.

---

## 9. Smoke Tests (`smoke_test.cpp`)

**19 tests** (1–18 plus #14b), run via `vlrtest.exe --seed N`:

1. attr table round-trip
2. player generation + sanity bounds
3. team auto-fill from FA pool
4. match runs to a valid score
5. solo q ranked day moves MMR
6. full world boots + season runs
7. year-end progression updates ages
8. solo q replay storage stays bounded
9. only 4 legal comps exist
10. duelists are leading first-blood role
11. coaches generated and tied to teams
12. every signed player has a real contract
13. FA mood adjusts demands and refusals
14. STRICT one-IGL-per-team after a full season
14b. Duelist IGL mental floor + scarcity (soft diagnostic) — generates 500 rookies, asserts a sane spread of IGL roles (D/I/C/S) and notes if Duelist IGLs end below the 0.78 mental composite floor after archetype stat shifts. Soft note rather than hard fail because TacticalIGL rookies legitimately land just under the gate post-shift.
15. tournament bracket plays through ALL rounds
16. JSON export of completed series produces valid, accurate output
17. Trophy integrity (no phantoms) — full season, walks every rostered player + FA. Checks: no duplicate `[T]/[M]/[W]` awards; trophy-holding rostered players have `career_matches > 0`; helper accessor consistency; year suffix within `[start_year-1, +1]`; snapshot alignment; no FA phantoms.
18. Multi-seed tournament structure audit — 3 seeds (1337/42/7), 8-team bracket each. Asserts: `ub_rounds==3`, `lb_rounds==4`, every UB+LB-non-Final match `best_of==3`, LB Final + GF `best_of==5`, `total_matches==14` exact, `gf_history().size()==1` exact (no reset). Locks down the BO structure + phantom-round fix + GF-Reset removal.

**Tests are slow** — full season simulation × 4 = ~1-3 min per seed. Peak memory ~120 MB after cycle leak fix.

---

## 10. Important Files Quick Reference

### Engine core
- `Common.h/cpp` — Attr enum, Rng, SimConfig, take_team_name, kRegions
- `Country.h/cpp` — country pool with weights
- `Names.h/cpp` — `make_identity()` weighted picks from generated_name_pools() with fallback to hardcoded NamePool
- **`NamesData.h/cpp`** (GENERATED) — zengm-sourced weighted pools, 27 countries × top-250 first+last
- `Agent.h/cpp` — 29 agents + 11 maps + map preferences + badges + CompPlan/CompTag
- `Coach.h/cpp` — 16 personalities, contract fields, generate_coach
- `Player.h/cpp` — Position enum, position_of, is_flex, agent_mastery, map_mastery, igl_impact, playstyle_identity, dedupe_awards
- `Team.h/cpp` — CompIdentity, enforce_one_igl, enforce_one_flex, team_import_appetite, build_round_selection (per-map selection with jitter), classify_team_strategy
- `Match.h/cpp` — Match::play, compute_attr_extras, agent_role_of, compute_igl_impact, friendly_ flag, role-gated assists, HoF milestones
- `Series.h/cpp` — `all_recordings()`, `map_history()`, BO2 tie handling, friendly pin gate
- `Tournament.h/cpp` — Phase state machine, ub_alive_ snapshot+clear, participated_players_, winning_roster_snapshot_, pinned_titles_already_ fuse
- `League.h/cpp` — round-robin schedule
- `SoloQ.h/cpp` — populate_initial_ladder, simulate_ranked_day, balance_lobby (strict IGL), force_solo_match, per-player daily cap
- `GameManager.h/cpp` — Phases (kPhases), advance_day with OFFSEASON, advance_to_playoffs, run_end_of_year, compute_season_awards (multi-factor MVP + IGL impact), push_tournament_news, set_current_world_year, news_pushed_events_
- `Goat.h/cpp` — prefix-anchored award counting, igl_impact_per_career weight
- `MatchExport.h/cpp` — `export_series_to_json` (hand-rolled writer, schema v1)

### UI
- `gui_main.cpp` — single TU (~12.5k lines). Muted role palette, PositionBadge + IglBadge separation, 5-slot roster layout, spoiler discipline, scoreboard with agent-role coloring, JSON export buttons, god-mode editors. **Module split planned — see §11.**
- `LogoArt.h/cpp` — typographic monogram badge system (2026-06-11). Every team logo composed of three layers driven from `shape_idx` + the team's `tag`:
  1. **Frame backplate** — 5 polished shapes (Shield/Hex/Circle/Diamond/Banner) selected via `shape_idx % 5`. Drop-shadow → darker rim → primary fill → top highlight → 1px inner-glow ring at 30% accent alpha (inset ~2px from the hairline border, lifts the badge) → hairline accent border. The shadow is a slight blue-leaning charcoal `IM_COL32(0x05, 0x08, 0x10, ...)` not pure black; the top highlight is warmed off-white `(255, 250, 235)` not pure white — both tweaks make the badge harmonize with the warm-amber UI accent.
  2. **Monogram** — team's `tag` rendered as bold typography centered in the frame, auto-sized for tag length (1ch=1.20r, 2ch=0.90r, 3ch=0.70r, capped 9..64px). Drop-shadowed with a vertical-dominant offset (2%/8%) for light-from-above. Color picked for luma contrast against the primary backplate (accent → white → black fallback). Legacy callers passing no `tag` get a stable per-shape default monogram (`kDefaults[]` table). Defensive font fallback: if `ImGui::GetFont()` returns null (e.g. wizard preview mid-init), uses the first font in the atlas instead of silently rendering nothing.
  3. **Accent flourish** — 4 small decorations (top stripe with rounded ends / corner dots at 0.10r so they read at 24px thumbnails / bottom chevron / inner outline of the frame) selected via `(shape_idx / 5) % 4`.
  `draw_team_logo(...)` takes an optional `const char* tag = nullptr`. `TeamLogo()` in gui_main.cpp passes `team.tag.c_str()`; the new-game wizard derives a tag from the user's org name. `kLogoShapeCount` stays at 30 so the personality-bias pools on `Team` keep spreading teams across templates evenly even though only 20 distinct frame+accent combinations exist.
- `FlagBitmaps.h/cpp` — 31 ISO-coded country flag bitmaps embedded as 24×16 ARGB. Used by `CountryFlag()`.

### Tests + entry points + tools
- `smoke_test.cpp` — 19 smoke tests
- `main.cpp` — console fallback (vlrmanager.exe)
- `launcher.cpp` — Play.exe auto-build launcher
- `build.bat` — MSVC build script. CORE_SRC includes NamesData.cpp + MatchExport.cpp. vlrgui.exe links `comdlg32.lib`.
- `tools/gen_names.js` — Node.js generator for NamesData.cpp from zengm's names.json
- `tools/dup_probe.cpp` — quick name-uniqueness probe (compile + run manually)

---

## 11. Pending Tasks / Wishlist

> Historical "DONE" entries (Phase 1, Phase 2A, role overhaul, belief-AI overhaul, etc.) live in [CHANGELOG.md](CHANGELOG.md). This section is pending/roadmap only.

### Known limitations
- **Save/load persistence** — major engineering effort. Closing the exe loses all progress. Highest user wishlist item.
- **Trade system** — currently sign/release only. No 1-for-1 player swaps or pick trading.
- **Player injuries** — not implemented at all (no injury state on Player).
- **Korea / Singapore / China / Chile names** still use hardcoded fallback pools (high duplicate rate). Expand by hand in `Names.cpp::pool_for` if needed.

### Half-built hooks (engine wires exist, nothing fills them)
- **`world_difficulty_`** — stored, persisted, clamped 0.7-1.3. Never consumed by AI strength scaling. New Game Wizard surfaces it as a slider that currently has zero gameplay effect.
- **Dynasty tiers — ChampionEra / Contender** — `Team::dynasty_tier()` checks `finals_history_`, but `finals_history_` is never populated by GameManager. Only the **Dynasty** tier (titles-only) is reachable today.
- **Flex of the Year award** — UI placeholder card in Awards Recap. `compute_season_awards` never produces it.
- **JSON export v2 fields** — schema v1 ships. `round_history`, `economy_log`, `utility_impact`, `heatmap_kills`, `ability_usage`, `timeline_events` are reserved but unwritten.
- **Tournament "today's matches" hook** — `active_tournaments` doesn't expose a today-filtered list, so calendar previews fall back to week-of-the-season approximation.

### Recent specs shipped partially
- **Role lock** — `contract_role` field exists and is stamped at every sign path, but no explicit "lineup lock" UI toggle (audit said no engine code currently shuffles roles, so a toggle wouldn't gate anything real).
- **Role-fit ongoing effects** — applied at signing time only. User spec also asked for "reduce morale" and "increase future departure risk" from bad-fit signings — not implemented.
- **Secondary comfort roles in Player Profile** — `role_fit_verdict()` accessor exists but no dedicated UI tab listing the 4 verdicts per player. Only surfaces inside the negotiation combo.
- **Bench extension semantic** — Re-sign button is open on bench players, but "Extension" (additive years) vs "Re-sign" (replacement contract) is still the same operation under the hood — just relabeled.
- **Legacy negotiation modal bodies** — `DrawRoster` and `DrawMarket` each still hold the pre-extraction Re-sign / FA modal body under `#if 0` as a fallback during rollout of `DrawNegotiationModal`. Delete in a follow-up after a full session of confirmed parity.

### Audit gaps — next-slice candidates (2026-06-11)
Captured from a full-UI integration audit on 2026-06-11. Highest user-visible impact first:
- **Bracket card seed numbers** — `Tournament::initial_seeding()` is populated; bracket cards don't display the team's initial seed. Add "#3" prefix or sublabel via index lookup. UB R1 is the highest-value spot.
- **`Tournament::validate_bracket_state` debug overlay** — accessor exists, never called. Wire as optional gold "[debug]" line under bracket panel when validation fails.
- **OrgMemory god-mode display** — 6 rolling metrics tracked, never exposed even in god mode. Add read-only card to Commissioner table per team.
- **`Player::Desire` editor** — god-mode reveals Desire + blurb but never lets user edit it. Add Desire combo to Player Profile god-mode dev-tools, mirroring the rookie_archetype combo pattern.
- **`Team::TeamWindow` override** — Strategy is editable in god mode but Window (Opening/Open/Closing/Closed) isn't. Add combo in Commissioner table for forcing window state.
- **Map mastery god-mode editor** — Agent Mastery tab has matches/avg/peak sliders; Map Mastery doesn't. Mirror the pattern.
- **HoF milestone helper** — UI hardcodes `>= 30 kills`, `>= 200 KDx100` etc. as HoF criterion display literals. Engine should expose `Player::hof_milestone_list()` returning the satisfied criteria as a vector so threshold tweaks don't require GUI edits.
- **Player row rendering duplication** — DrawRoster / DrawMarket / DrawTeamProfile / DrawLeagueLeaders independently build similar row patterns. Strong candidate for an extracted helper, ideally landed during the planned gui_main.cpp split (see §5 below).
- **KPI strip rendering duplication** — Dashboard / Team Profile / Manager Finance hand-build StatTile sequences with subtle inconsistencies. Extract `render_team_kpi_strip(team, flags)` during the split.

### Planned — `gui_main.cpp` module split
~12,500-line single TU is the biggest architectural drag in the project. Documented permission from the user (2026-06-11) to land a multi-file split when context allows. Proposed shape:
```
cpp/src/ui/
├─ ui_common.h/cpp       Theme, fonts, design-system helpers (Pill,
│                        PillOutline, SigBadge, FlexBadge, IglBadge,
│                        StatTile, SectionHeader, BeginCard, MutedText,
│                        fmt_money, OpenPlayerModal, OpenTeamProfile, etc.)
├─ ui_modals.cpp         Player Profile + Live Match + DrawNegotiationModal
├─ ui_dashboard.cpp      Dashboard
├─ ui_team.cpp           Roster + Team Profile + Manager
├─ ui_competition.cpp    Standings + Tournament + Power Rankings + League Leaders
├─ ui_people.cpp         Transfer Market + Solo Q + Compare
├─ ui_history.cpp        Favorites + Event Log
├─ ui_live.cpp           Watch Match + scoreboard
├─ ui_calendar.cpp       Calendar + News
└─ gui_main.cpp          Entry point, AppState, sidebar, main loop
```
Constraints when landing this:
- Pure mechanical move — do NOT change function logic inside the move. Refactor passes happen AFTER the split lands clean.
- ImGui IDs depend on label strings — moving code between TUs is safe; renaming `##` labels is not.
- Each new .cpp gets the same anonymous-namespace pattern. Helpers shared across files move to `ui_common.h`.
- Keep `#if 0` legacy modal bodies in `DrawRoster` / `DrawMarket` UNTIL parity on `DrawNegotiationModal` is confirmed across a full play session — see "Recent specs shipped partially" above.
- Build by extracting `ui_common.{h,cpp}` first, building, smoke-testing. Then move screens one at a time, building between each.

### Phase 2 (small, polish-tier — when user asks)
- Rivalry tracking deeper (h2h table per team-pair AND per-player, highlighted UI when teams meet often)
- Coach philosophies surfaced in match commentary
- Veteran decline arcs as narrative chips
- Better match commentary (attribute-aware round-feed templates)
- `finals_history_` wiring for ChampionEra/Contender dynasty tiers
- Team eras tagging (read off trophy_history runs)

### Phase 3 (deeper systems, multi-iteration each)
- LAN pressure expansion (partially done via `lan_mod` multiplier)
- Crowd/fan pressure model
- Regional meta shifts + patch updates altering `agent_priors`
- Map veto strategies phase before BO3+ matches
- Player loyalty traits + contract disputes (beyond `Desire`)
- Greatest matches ever / historical tournament pages
- Profile pictures / avatars
- Awards reveal animations
- Coach trades / market movement during the season

---

## 12. Development Notes

### Assumptions the codebase makes
- Single user team (`gm.user_team`). Multi-user save isn't supported.
- Three regions, fixed: `kRegions = {"Americas", "EMEA", "Pacific"}`.
- 12 teams per region, fixed (in `initialize_world`).
- 5 starters per team, max 7 roster. Most logic assumes starting 5 is `roster[0..4]`.
- Year is just an int, season cycle 200 days.
- All shared_ptr ownership. RecordedMatch.team1/team2 are weak_ptr.
- ImGui is single-threaded.
- `Player::region` is the player's CURRENT solo Q region. `country_iso` is the unchanging nationality.

### Fragile systems
- **`enforce_one_igl()`** depends on `apply_attribute_delta` being symmetric. Demote branch must mirror promote.
- **`Tournament::schedule_next_bracket_round` ub_alive_ snapshot+clear pattern** — any new alive accumulator needs the same pattern.
- **`run_end_of_year` ordering** — awards BEFORE counter reset; roster cleanup BEFORE resigning; sync AFTER roster shuffle; MMR reset AFTER sync. DO NOT reset phase_idx here.
- **`balance_lobby` swap heuristics** — works in practice but no formal correctness proof.
- **`simulate_full_season` stop condition** — exact match `current_phase() == "AWARDS"` OR `"OFFSEASON"`.
- **Bracket UI tab matching** — `tour_tab_for(t)` uses explicit substring matching for region names.
- **`active_tournaments` lifecycle** — sole clear path is `force_finish_stale_tournaments`.

### Areas likely to break
- Adding new attributes — must update `Attr` enum, `kAttrNames` in Common.cpp, AND every `fill()` call in `generate_player`. Smoke 2 catches missed fills.
- Adding new awards — must update `compute_season_awards`, `update_mvp_race`, Goat formula, HoF criterion check.
- Adding new tournament formats — extend `Phase` enum + `play_round` switch + `schedule_next_bracket_round` logic. Test with smoke 15.
- Refactoring gui_main.cpp — ImGui IDs depend on label strings.
- Changing Position enum order — `position_of()` derivation + roster bucketing iterates by Position::Count.

### Process discipline the user values
- Always run smoke tests after engine changes (long timeout — up to 5 min)
- Always smoke-launch GUI after UI changes
- Always refresh backup after meaningful work
- Always be honest about what's deferred vs done — "honest accounting" sections
- When user reports a bug, audit actual cause before fixing — don't just patch symptoms
- Use `cmd /c build.bat ... > log` (not PowerShell `*>`) for build logging
- Always kill running exes before rebuild (`Stop-Process -Force`)
- Verify exes' mtimes match edit time after rebuild

### CRITICAL clarifications from the user
- **Star players should NOT artificially carry** — high attrs naturally produce good stats. Don't force "50-kill carry games" with explicit modifiers. Let the sim emerge.
- **Duelists should have FEW assists** — high duelist assists are a "missed kill" signal.
- **Awards only for actual wins** — no Finalist/Semifinalist participation badges. Champion only.
- **Skip to Playoffs should NEVER force-open user match** — clear recording on EVERY return path.
- **Live viewer should NOT spoil** — hide map count, hide round winner during in-progress play-by-play.
- **OFFSEASON should be MANUAL** — AI does not touch user_team's roster.
- **Memory at GBs is a problem** — under 1GB acceptable.
- **IGL is NOT a position** — it's a leadership designation that overlays the player's actual gameplay role.
- **Flex IS a real position** — wildcard player rotating between 2-3 role categories.
- **Imports should feel exceptional** — only 2-3 teams per region with active imports.

---

## 13. Asset / Data Backup

```powershell
$dst = "C:\Users\fulls\Documents\valosim_backup_LATEST.zip"
Compress-Archive -Path "C:\Users\fulls\Desktop\Valosim\cpp\include",
                       "C:\Users\fulls\Desktop\Valosim\cpp\src",
                       "C:\Users\fulls\Desktop\Valosim\cpp\tools",
                       "C:\Users\fulls\Desktop\Valosim\cpp\build.bat",
                       "C:\Users\fulls\Desktop\Valosim\PROJECT_GUIDE.md" `
                  -DestinationPath $dst -Force
```

Refresh after every meaningful change. Zip is ~330 KB.

Refresh launcher copies on Desktop after build:
```powershell
Copy-Item "C:\Users\fulls\Desktop\Valosim\cpp\build\Play.exe" "C:\Users\fulls\Desktop\Valosim\Play.exe" -Force
Copy-Item "C:\Users\fulls\Desktop\Valosim\cpp\build\Play.exe" "C:\Users\fulls\Desktop\Play VLR Manager.exe" -Force
```

---

## 14. Typical Session

1. **Read this guide first.** Especially §4 (current systems), §7 (magic numbers), §8 (pitfalls), §12 (user clarifications).
2. Run `cpp\Play.exe` to see the current state in the GUI.
3. Run `vlrtest.exe --seed 1337` to sanity-check engine (slow — 1-3 min).
4. Check the user's most recent message for the current ask.
5. For non-trivial changes: dispatch 2-3 parallel agents with strict file-ownership scoping. Edit-only mode (no build). Main session does the unified build + multi-seed smoke pass after all return.
6. **Don't break invariants** — IGL, contract expiration, RecordedMatch weak_ptr usage, friendly-flag stat isolation, ub_alive_ snapshot+clear, BO2 tie handling, Skip-button recording clear, OFFSEASON manual flow, trophy participation filter, position/IGL independence.
7. After a meaningful round of work: kill exes, rebuild, refresh launcher copies on Desktop, refresh backup zip, **update this guide if you added/removed systems**.
8. If a directory wipes: backup zip is at `C:\Users\fulls\Documents\valosim_backup_LATEST.zip`. Restore from there.
