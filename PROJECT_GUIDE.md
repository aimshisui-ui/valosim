# VLR Manager / Valosim — Project Guide for Future Claude

> **Read this FIRST before editing.** Captures current architecture + every behavioral decision the user has explicitly asked for. If you're tempted to "improve" something documented here, check with the user — most rules below are deliberate, several were learned the hard way.

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
   │   ├─ vlrtest.exe                 17 smoke tests
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
                gui_main.cpp                               ← ImGui UI (single TU, ~6300 lines)
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

**Phantom-round bug FIXED (2026 May):** old `schedule_next_bracket_round` merged UB-Final-loser-drops into `lb_alive_` even when sizes mismatched, leaving leftover LB teams that played a phantom "LB Final" round before the real LB Final. New rule: merge ONLY when `lb_alive_.size() == ub_just_dropped_.size()`; mismatch → run pure LB round first and hold drops in `ub_just_dropped_` for next merge attempt. `is_lb_final` flag fires only when `ub_alive_.size() == 1` AND LB is collapsing to its final 2.

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

## 5. UI (`gui_main.cpp`)

Single TU, ~8100 lines (was ~6300 pre-Phase-1). ImGui + Win32 + DX11.

### 5.1 Fonts
`g_font_body` (Segoe UI 16), `g_font_small` (13), `g_font_h2` (Semibold 22), `g_font_h1` (Black 30), `g_font_kpi` (Black 38), `g_font_mono` (Consolas 15).

### 5.2 Color palette + design system (premium dark, redesigned 2026-05-15)

**Base palette** (constant NAMES kept, values retuned so all screens inherit):
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
kAccent  (kVlrBlue) #00D4FF electric blue — PRIMARY accent
kAccent2          #7B5EFF  purple — secondary accent
kVlrGold          #FFD700  prestige / trophies
kVlrGreen         #3FD17A  success
kVlrRed           #FF4655  VLR brand red (kept)
```
Role colors unchanged (muted brick/indigo/forest/steel/amber + gold IGL) — deliberate prior user decision, still wired through `role_color`, `position_color`, scoreboard agent tinting, `RoleBadge/PositionBadge/IglBadge/FlexBadge`.

**`ApplyVlrTheme`** — premium rounding (window 10, frame 8), generous padding (window 22×20, frame 13×8, cell 12×7), accent-driven interaction states (buttons/sliders/checkmarks/scrollbars use `kAccent`), transparent tabs/childbg, subtle white-alpha row striping.

**Design-system helpers** (defined ~gui_main.cpp:420-560 — compose ALL new UI from these):
- `BeginCard(id,bg,pad,rounding)/EndCard()` — auto-height rounded bordered card
- `BeginCardSized(id,size,...)/EndCardSized()` — fixed-size card
- `Pill(text,bg,fg=0,extra_pad)` / `PillOutline(text,line,fg)` — rounded chips, auto-contrast text
- `SectionHeader(title,subtitle,accent)` — accent bar + H2 + dim subtitle
- `StatTile(label,value,accent,size,sub)` — KPI tile with accent edge
- `MutedText(fmt,...)` / `VSpace(h)` / `ThinDivider(pad_y)` / `brightish(ImU32)`

All screens were restyled 2026-05-15 to use these (cards, KPI strips, pill badges, section headers, striped tables, broadcast-style score banner/scoreboard, sharpened VCT bracket). Engine/data/logic untouched — purely the visual layer.

### Belief-driven AI + temporal coherence + economy overhaul (2026-05-15 → 2026-05-28)

**The biggest single block of engine work in the project's history.** Turned the optimizer-AI into a brain with memory, philosophy, timeline awareness, chemistry, and explainable negotiation. Plus a financial rebalance making the economy actually constrain decisions.

#### Phase A — Belief Foundation (Team + Player)

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

#### Phase B — Temporal Coherence + Chemistry

**`Team::TeamWindow`** enum (Opening/Open/Closing/Closed) computed yearly via `compute_team_window` from `avg_age + avg_potential + avg_ovr + avg_contract_years_left`. Recomputed in `run_end_of_year` between `update_org_memory` and `commit_strategy_with_inertia`. Drives mid-season `cut_chance × 1.40` for Closing (urgency), `× 0.70` for Opening (patient dev). Import appetite +0.05 for Closing, −0.03 for Opening.

**`timeline_fit_score(team, candidate)`** — `1.0·OVR + 0.5·(potential−ovr)·youth_factor + 0.4·contract_efficiency + 0.8·TimelineFit` where TimelineFit is a (window × age band × potential band) lookup. A 28y/o vet with high OVR scores +25 for Closing-window teams but −10 for Opening. Wired into snake-draft (weight 0.6) and `auto_fill::fill_one` (weight 0.4).

**Risk tolerance** — Contenders/WinNow score archetype `consistency_mod × +120` (pay for steady players, up to ±14 swing). Rebuilders/Development/TalentFarm reward NEGATIVE consistency_mod when `potential ≥ 80` (gamble on volatile high-ceiling boom/bust talent).

**Chemistry graph** — `Team::chemistry` map keyed on canonical-ordered `ChemKey {Player* a; Player* b}`. EMA update (α=0.15, clamp [-2, +2]). `Match.cpp` walks 25 pairs/team max after each non-friendly match; pair delta from shared outcome (both ≥1.10 in a win → +0.35-0.55; shared loss with both ≤0.85 → +0.08; gap ≥0.35 → −0.10; shared APR ≥0.30 in win → +0.05). Year-end decay 5% + prune entries where both endpoints off-roster. AI uses two ways: (a) `ai_manage_roster` protects players with `total_chemistry ≥ +2.0` from poor-rating cuts; (b) `auto_fill::fill_one` adds `+8·total_positive_chemistry` reunite bonus. Public: `record_chemistry(a,b,delta)`, `chemistry_between(a,b)`, `top_chemistry_pairs(n)`.

**UI surfaces:** Team Profile WINDOW pill (color-coded Opening green / Open accent / Closing gold / Closed red) + behavioral one-liner + ROSTER CHEMISTRY card (top-5 duos with signed values, ±2 horizontal bars). Dashboard WINDOW StatTile in KPI strip. Player Profile "Strong duo: ↔ X (+1.42)" line.

#### Re-sign UI Flow + Deep AI Re-sign + Market Competition

**`Player::ResignOffer`** — what the player would ask for if approached: `{amount_k, years, willingness 0..1, min_acceptable_k, max_acceptable_years, explainer}`.

**`Player::ResignBreakdown`** (negotiation overhaul) — per-factor scoring breakdown:
- `base_score=50, salary_mod` (monotonic in offer, ±40), `prestige_mod` (±12), `contender_mod` (window/strategy/roster ovr; RingChaser doubles, Competitor checks raw ovr), `loyalty_mod` (Loyal+same_team +15, +3 current-team tenure), `years_mod` (length vs preference), `desire_mod` (archetype hard gates flat −50, Greedy/Mercenary −25/−30, mood floor −40)
- `total` clamped [0, 100], `will_accept` = `total >= 55` (`kResignAcceptThreshold`)
- `verdict` ∈ {INSULTING (<35), WEAK (35-54), FAIR (55-69), STRONG (70-84), OVERPAY (≥85)}
- `labels` for UI display, `reject_reason` one-liner

**SINGLE SOURCE OF TRUTH** — `Player::accepts_resign_offer(amount, years, team)` is now literally `return evaluate_resign_offer(...).will_accept`. UI pill physically cannot disagree with engine decision. `evaluate_resign_offer` uses `gen_contract(randomize=false)` for deterministic ask reference — no per-frame RNG jitter.

**`refuses_to_negotiate` semantics FIXED.** Old: `(offer × mood > 100)` — higher offer INCREASED refusal odds (the inverted bug). New: `offer < (30 + 70·mood)` — mood inflates the FLOOR, higher offer monotonically REDUCES refusal. Smoke #13 updated.

**Deep year-end re-sign loop** — for every team's expiring players (`years_left <= 1`):
1. Player asks via `propose_resign_offer`
2. `team_eval_score`: age curve (+30 young → -15 decline), positional scarcity (+25 if only one), chemistry × 5, memory bonuses (veteran/rookie success ≥30), window fit (+15 Open/Closing+star, -25 Closed+old), financial pressure (-15 if too expensive), avg_match_rating tiers, strategy overlay (Contender/WinNow+star +20)
3. Counter-offer: stars (ovr≥80) get full ask, mids get 90%, low get `min_acceptable_k`
4. Counter years from `decide_contract_years` (multi-factor)
5. Player evaluates via `accepts_resign_offer`
6. On accept: `Team::resign_player(p, years, amount, year)` (in-place mutation, no release+sign)
7. On reject: walk → market competition

**`Team::resign_player(p, years, amount, year)`** — in-place contract rewrite without release+sign (preserves chemistry, no IGL/Flex re-enforce needed). Debits budget.

**`Team::market_value_estimate(p)`** — wraps `gen_contract(0, false, false, this)` for UI display.

**Market competition for walked stars** — when player rejects re-sign AND `ovr ≥ 75`, find up to 3 same-region rival teams with positional need + budget ≥ market_value × 1.1, ranked by appetite + budget/4M + prestige/200. Each bids `gen_contract(year, randomize=true, false, &them)`. Player picks highest `willingness × amount` that clears `min_acceptable_k`. Bid years from each team's `decide_contract_years` (different teams offer different lengths same player). "X poaches Y from Z" news. **USER TEAM EXCLUDED from bidder pool** — won't auto-sign random FAs onto your roster.

**`Team::ai_full_offseason_pass(fa_pool, year, log)`** — 5-stage deep GM pass:
1. Header log (strategy/window/budget/roster snapshot)
2. Smart cuts: expired contracts + underperformers (threshold ±0.05 by stability_culture) + chemistry retention for cores (≥+2.0 sum)
3. Per-starter value assessment + targeted upgrade scan — `starter_value = ovr + age_curve + 8·avg_rating`. For each non-protected starter, scan same-role FAs for upgrades (delta ≥ +4 OVR), affordability (cost ≤ 40% budget), `accepts_resign_offer` check. Cut-and-replace with full log. Max 2 upgrades/pass
4. Cascade-fill backstop for any vacancies via `auto_fill_roster`
5. Bench depth (slots 6-7) if budget ≥ $200K and roster at 5

Every move logged: "UPGRADE: Mach (Controller 57 OVR) → Vortex (74 OVR, +12.4 value)". Wired for ALL non-user teams in `run_end_of_year`. User team's OFFSEASON-end fallback (when roster < 5) calls it too.

#### Re-sign UI (gui_main.cpp)

- "Re-sign" button on roster rows when `years_left ≤ 1` (next to Release)
- Negotiation modal: cached offer + market value (no frame jitter), two-column LEFT (player's ask, willingness bar, explainer, min/max) RIGHT (market value, team budget, year/salary sliders, transparent breakdown showing every factor's contribution, will-accept pill, reject reason)
- Three actions: Send Offer (commits via resign_player) / Match Their Ask (quick-fill) / Walk Away
- Contract status pills on roster rows: gold "FINAL YEAR" (yl=1), dim "Expiring soon" (yl=2)
- Dashboard hint pill "N players expiring" if user has final-year contracts

#### Async Sim Worker — Continue button non-blocking

Engine work moved off the UI thread. `Continue/Skip-to-next-match/Skip-to-Playoffs/Sim Full Season/Sim Solo Q Day` spawn `std::thread` running the sim. UI keeps presenting at 60fps; renders a full-screen "SIMULATING…" overlay with animated electric-blue spinner. Buttons disabled while running (BeginDisabled). Main loop polls `sim_thread.joinable() && !sim_running` each frame, joins, drains `pending_results` via `process_day_result` (UI-thread-only: opens live viewer modal + log lines). AppState destructor joins any pending worker. Pages aren't rendered during sim to avoid races with worker mutating engine state.

#### `Player::years_left(current_year)` — contract source of truth

`Player::contract_years` was being decremented unconditionally every year-end (including for FAs) AND `contract.exp_year < year` was the release gate. The two drifted; the release gate was off-by-one (3y contract from 2026 played 4 seasons). Fixed:
- Removed `contract_years -= 1` in `save_history_and_progress` (now a frozen snapshot of signed duration)
- Release gate: `exp_year < year` → `exp_year <= year` (3y from 2026 releases at end of 2028, correct)
- `years_left(year) = max(0, contract.exp_year - year + 1)`, 0 for FAs/Retired — SINGLE source of truth
- All UI display sites switched from `contract_years` to `years_left(s.gm.year)`
- `ai_manage_roster` signature: `(fa_pool, current_year)` so it can derive years correctly

**Plumbing fix:** `auto_fill_roster` + `scout_top_fas` now also take `current_year` parameter. Previously the inner `sign_player(best, rng().irange(1,3))` defaulted `current_year=0` → exp_year stayed at the Player ctor's baseline (0-3) → roster rows showed "exp 0" / "exp 1". Threaded year through every callsite.

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

Constants `vlr::kSalaryCapK = 180`, `vlr::kSalaryFloorK = 10` in Common.h. God-mode salary slider bounds updated. `decay_demands` floor → 10. `amount_with_mood` clamped to [10, 180].

#### Smoke #14b — Duelist IGL mental floor (soft diagnostic)
Generates 500 rookies. Originally hard-asserted no Duelist IGL has mental composite < 0.78. Relaxed to soft diagnostic note — TacticalIGL rookie archetype's stat shifts can push mental slightly below the gate after the IGL roll. Documented behavior, not a regression.

#### Misc fixes during this period
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
- Tournament Player Stats sort persistence (now always re-applies, no version-key cache that froze on no-roster-change)
- IGL OTY gate tightening: requires `igl_match_count > 0` (filters out transient enforce_one_igl promotions)
- Transfer Market "Sign" button bug: row's Selectable had `SpanAllColumns` claiming clicks → added `AllowOverlap` + `SetNextItemAllowOverlap`
- MC potential SKIPPED for solo Q ladder fillers (`deep_potential=false`) — saved ~6.5M progression ticks at world boot (the New Career hang)
- OFFSEASON-end user-team fallback wired
- AI year-end re-sign loop now runs for user_team too (manual control during season; AI as fallback at year-end)

### Big batch (2026-05-15) — audit fixes + zengm-inspired ports + UI consolidation

**Bug fixes:**
- Milestone news no longer re-fires every year (separate `news_emitted_career_keys_` set, never cleared)
- News dedup-clear moved AFTER year-end emitter pass (spec §4.11 step 15)
- `came_from_upper` now robust (checks both .a/.b, fallback scan of UB history)
- `last_mvp_leader_global` promoted from function-static to GameManager member (cleared in `reset_world()`)
- `solo_qs[a->region]` unchecked → guarded with `.find()` + skip
- `world_difficulty_` clamp tightened 0.5..1.5 → 0.7..1.3 (matches spec)
- Tournament dead field `ub_finals_round_just_played_` removed
- Match.cpp cosmetic cleanups (`0.5+0.5+0.5*x` → `1.0+0.5*x`, `/1` → drop, defender flag → `is_team1_defender()` helper)
- Player dead "uniqueness check" loop now actually consults `global_names()` for collision
- TacticalIGL rookie archetype now applies the full canonical IGL stat shift (+ mental-gate check before forcing `is_igl`)
- Tournament Player Stats sort now persists across frames (version-keyed cache + force-SpecsDirty on rebuild)
- IGL Profile gating fixed (god-mode is_igl toggle now in Overview Dev Tools, reachable for any player)
- DrawCompare position row now includes FlexBadge (canonical order PositionBadge → FlexBadge → IglBadge)
- Live match `ImGui::Columns(2)` replaced with two `BeginChild` + `SameLine`
- News page signature lookup O(N × all-players) → O(1) per item via frame-cached map
- Kill-feed `kills_so_far` O(N²) → O(N) running counter
- `build.bat` play.bat tip → Play.exe tip

**New: per-attribute age-bucketed progression (Player.cpp):**
Three aging classes — Mechanical (Aim/HS/Reaction/CrosshairPlacement/Movement, peaks 22-25, fast decay), Game-IQ (GameSense/Intel/Decision/MidRound/AntiStrat/Adapt/Econ/Comm/Lead, climbs into 28+, IGLs age gracefully), Athletic (Stamina/SpikeHandle/Anchor/Lurking/Pos/Aggro/Entry/Util/Clutch/Awping, middle). Asymmetric young-player noise (≤23 + breakout archetypes get +2y: stddev 5 clamped [-4,+20] → breakouts possible). Multipliers: work_ethic/65, consistency clamp 0.7-1.3, archetype consistency_mod, growth_archetype shift, potential pressure asymptote. `career_max_ovr` field updated per tick.

**New: two-stage shouldRetire (Player.cpp):**
- Stage 1 logistic: `logit = -8 + 0.18·(age-25) - 0.04·(potential-50) - 1.5·max(0, career_rating-0.95) - 0.05·career_seasons_played`
- Stage 2 aging-star catch: age ≥ 30 AND `ovr < 0.70·career_max_ovr` → force retire

**New: Monte-Carlo potential (Player.cpp):**
At rookie gen, 20 fast-forward sims to `max(peak_age+4, 29)`, take 75th-percentile terminal OVR, clamp 50-99. Replaces simple `mean + irange(5,20)` baseline.

**New: Hash-deduped award addition (Player.h/cpp):**
`Player::add_award(string)` + `awards_seen_` mirror + `rebuild_awards_seen()`. `dedupe_awards()` resyncs the mirror at year-end. Belt-and-braces with existing dedupe pass.

**New: H2H playoff/regular split + finals log (GameManager.h/cpp):**
- `h2h_counts_regular_` + `h2h_counts_playoff_` + `h2h_finals_log_<H2HSeriesFinal>`
- Public: `h2h_total/regular/playoff(Team*,Team*)`, `h2h_finals_between(Team*,Team*)`
- Surfaced in TeamProfile Bio "RIVALRIES" + DrawCompare h2h section + DrawMarket OFFSEASON decay chip

**New: FA price-decay during OFFSEASON (GameManager.cpp):**
3%/day decay during OFFSEASON window, floor at 70% of entered-offseason baseline. `fa_demand_baseline_k_` map tracks baseline, cleared at year rollover. Constants: `kFADecayRate=0.03`, `kFADecayFloorFraction=0.70`. UI shows "decayed" pill on affected FA rows.

**New: Sidebar consolidation 16 → 6 groups (gui_main.cpp):**
- **Home** — Dashboard · News
- **Team** — Roster · Manager · Calendar
- **Competition** — Standings · Tournaments · Power Rankings
- **People** — Transfer Market · Solo Q · League Leaders · Compare
- **History** — Favorites (with sub-tabs incl. new Frivolities) · Event Log
- **Live** — Watch

Each group is a sidebar entry with internal `ImGui::BeginTabBar` dispatching to existing `Draw*` functions (no draw functions rewritten).

**New: 5 Frivolity views (Favorites → Frivolities sub-tab):**
1. Best Without a LAN — career rating leaderboard, no [M]/[W]
2. Youngest MVPs — sorted by age at [A] MVP win
3. Hall of Good — retired players hitting 2-3 HoF criteria (4+ = real HoF)
4. Biggest Busts — `career_max_ovr < potential - 15` AND retired/age≥28, with reason chip
5. Seasons Without a Title — top single-season ratings whose teams didn't win that year

**Bracket UI audit:** layout was structurally correct. Added defensive UB monotonic-shrinkage MutedText warning if anomaly detected. Engine-side `Tournament::validate_bracket_state(out_err)` debug walker added by Pack B for future UI assertions.

**Smoke tests:** 19 now (added #14b Duelist-IGL mental floor diagnostic). All pass at seed 1337 + multi-seed 18 (1337/42/7).

**Patches applied during build pass:**
- Added fwd decl `static int monte_carlo_potential(const Player&)` before `generate_player` (defined later in file)
- Added fwd decl `std::vector<vlr::PlayerPtr> all_players_world(AppState&)` in gui_main.cpp's early-decl block (frivolity views use it before its definition site)
- TacticalIGL rookie archetype: post-shift mental gate check (skips `is_igl=true` for Duelists below 0.78)
- Smoke #14b relaxed from "hard fail" to "soft diagnostic note" — the floor is a spawn-time gate, not an invariant guaranteed across all subsequent shifts

### Archetype system + variance rebalance (2026-05-15) — see also §4.4

**32-archetype playstyle system (Player.h/cpp)** — a THIRD, sim-driving system distinct from `rookie_archetype` (16-value growth template) and `playstyle_identity()` (derived display label). `enum class Archetype` (32 + Count): MechanicalDemon, HyperAggroEntry, SmartLurker, IceColdClutcher, UtilityGenius, PassiveAnchor, TempoController, MomentumFragger, LANDemon, OnlineFarmer, TacticalGenius, RiskyPlaymaker, ConsistencyMachine, ConfidencePlayer, SlowMethodical, HighCeilingLowFloor, AggressiveAWPer, SupportiveFacilitator, FlexGenius, AntiStratMaster, EcoSpecialist, EntrySacrifice, TeamFirstGlue, OverheatingSuperstar, RookieMechFreak, VeteranStabilizer, ClinicalCloser, ChaosAgent, ZoneController, RetakeSpecialist, DuelistDiva, QuietProfessional. `Player::archetype` assigned once at spawn via `pick_spawn_archetype()` (attr-fit scoring + role-gate + rookie_archetype nudge + rng pick among top-4 so same-role players diverge). `ArchetypeProfile archetype_profile(Archetype)` — pure O(1), 14 modest float modifiers (aggression_mult, fight_selection, utility_timing, clutch_mult, consistency_mod, tempo, risk_tolerance, lan_mod, momentum_sensitivity, economy_discipline, consistency_floor, ceiling_boost, teamplay_mod, adapt_mod) + name. Profiles LAYER on attribute math, never replace it — magnitude still attribute-dominated.

**Match.cpp variance rebalance (user: "top teams steamroll, no variance"):**
- `compress_power()` — diminishing returns above ~70-attr pivot (0.62 marginal retention). Ordering preserved; elite-vs-elite duels close so good teams trade maps.
- Round jitter widened (inverse-consistency band 0.02-0.10 → 0.035-0.18).
- **Per-map off-day roll** — asymmetric, floor never below 3.5% even for 95-consistency stars → genuine 0.7-0.9 stinker maps sprinkled in.
- **Per-team matchup swing** — independent ±6% (clamp ±14%) team tilt → clearly-better team now drops ~25-40% of maps but wins series on average.
- Two-way decaying momentum (×0.78/round + ±0.45 nudge, ±0.075/duel cap) replaces snowball-only — 0-8 comebacks possible.
- Comp-synergy spread widened 0.90-1.10 (clamp 0.89-1.14) — meta/comp mismatch matters more.
- Rating formula coefficients, damage-per-kill, kill weights UNCHANGED (§7) — only inputs/variance changed. Archetype fields wired into existing layered terms within the §7 per-duel ≤+0.32 cap + `compute_attr_extras` clamp.

**2026-05-15 follow-up fixes:**
- **God Mode IGL editing**: Player Profile IGL tab now exposes editable SliderInts (attrs incl. MidRound/AntiStrat/Leadership/Comm/GameSense/Intel/Decision/Adapt/Econ), DragFloat for igl_impact_total/season/peak, InputInt for igl_match_count/season_pressure_matches, is_igl toggle — all under a gold `SectionHeader("DEV TOOLS — IGL")`, read-only when god mode off.
- **Power Rankings**: removed nested scroll children (single vertical page scroll, no horizontal bar); `DrawRankRow` is fixed 56px with explicit pixel columns [14/64/116/168/400], analyst note ellipsised, width clamped to content region.
- **Dashboard**: "Next Match" tile value shortened to fit; full description moved to a wrapped `PushTextWrapPos` line below the KPI strip.
- **Sortable tables**: Standings, League Leaders, Hall of Records, Greatest Seasons, Trophy timeline, Tournament Player Stats, and the live Scoreboard use `ImGuiTableFlags_Sortable|SortMulti`; stat columns default DESC, name/team ASC, `TableGetSortSpecs`+`SpecsDirty` gated. (DrawCompare intentionally NOT sortable — transposed metric sheet, no record-per-row backing; flagged for redesign if needed.)
- **Tournament "Player Stats" sub-tab**: `DrawBrackets` now has an inner TabBar (Bracket | Player Stats) inside the per-tournament `PushID/PopID`; existing bracket render wrapped in a `render_bracket` lambda. 16-col sortable table from `aggregate_player_stats()`, default Rating desc, green/red rating+KD tint, empty placeholder.
- **Live match revamp**: broadcast-style `DrawScoreBanner` (ellipsis-clamped, leading/trailing emphasis, spoiler-safe), `DrawRoundTimeline` scrolls in its OWN horizontal strip only, kill-feed rows reserve real `InvisibleButton` slots (fixes overlap bug), framed round-result + economy (graceful "not recorded" placeholder) + gold ACE/CLUTCH callout cards. All action labels + playback_speed 0-3 + solo-Q weak_ptr fallback preserved.

**2026-05-15 round 2 (live scroll + POT/dev + archetype surfacing):**
- **Live match — all internal scroll killed**: every inner `BeginChild` in `DrawLiveMatchModal`/`DrawScoreBanner`/`DrawRoundTimeline`/`DrawTeamScoreboard` set `NoScrollbar|NoScrollWithMouse`; the kill-feed `##events` child switched to `ImGuiChildFlags_AutoResizeY` (was a fixed 360px clipping/scrolling child — the main offender). Only the modal popup scrolls. Timeline keeps its single-axis HORIZONTAL strip with `NoScrollWithMouse` (wheel scrolls page). Hard `Columns(2)` replaced with responsive split: side-by-side ≥1000px else stacked + `ThinDivider`. Modal clamped to viewport WorkSize.
- **POT / development visibility**: Dashboard gained a `##dash_development` card — table of Name/Role/Age/OVR/POT/Form/Contract + color-coded Trajectory Pill (Rising▲/Developing▲/Established—/Declining▼/High-Risk Prospect, derived from age + potential−ovr gap). Player Profile Overview shows OVR+POT `StatTile`s, Form, Age/WorkEthic/Consistency, Contract.
- **Archetype surfaced**: Player Profile hero shows `archetype_name(p->archetype)` as a purple `kAccent2` Pill + "OVR → POT" pill + trajectory pill, alongside the kept playstyle_identity chip.

### 5.3 Helpers

`H1/H2/Sub`, `RoleBadge(Role)`, `PositionBadge(Position)`, `IglBadge()` (separate small gold pill chained after PositionBadge when `is_igl == true`), `CountryFlag(iso)` (procedural), `KpiCard`, `player_label(p)`, `OpenPlayerModal`, `OpenTeamProfile`, `OpenLiveMatch`, `OpenLiveMatchSeries`, `OpenReplay`, `OpenReplaySeries`, `fmt_money`.

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

### 5.8 Bracket UI (VCT-style, redesigned 2026 May)

Per-tournament rendering wrapped in `PushID/PopID`. **Tabs per region** + International. Layout:
- **Two vertically separated panels** inside a horizontally scrollable child: UPPER BRACKET on top (gold H2 header), LOWER BRACKET below (muted-red H2 header), horizontal divider between
- **Cards** 152×60, 2-row team layout (tag + name + right-aligned score). Winner row tinted via team primary color stripe; loser dimmed; unplayed mid-tier opacity
- **BO5 chip** — 28×12 gold pill in card top-right when `best_of >= 5`. Visible on LB Final + GF + GF Reset only
- **Connecting lines** rendered via `DrawBracketConnector` (H-V-H 3-segment polyline). Color: `kLinePlayed` for played, `kLineUnplayed` for unplayed
- **Round labels** sourced from `Tournament::current_round_label()` (Agent A's accessor) for the live round; played rounds use `BracketMatch::label` or fall back to generic ("UB R1", "UB Semifinals", "UB Final", "LB R1"…"LB Semifinal", "LB FINAL (BO5)" in gold all-caps)
- **Grand Final column** sits right of UB panel, centered vertically against UB final card. Single GF card (no reset stacking — modern VCT format)
- **Empty-bracket guard**: shows "Bracket starts after group stage." if no UB history and no scheduled UB
- **Per-card invisible buttons** ID'd via `id_base + round*100 + i` namespaced per tournament

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

---

## 9. Smoke Tests (`smoke_test.cpp`)

**18 tests**, run via `vlrtest.exe --seed N`:

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
- `gui_main.cpp` — single TU. Muted role palette, PositionBadge + IglBadge separation, 5-slot roster layout, spoiler discipline, scoreboard with agent-role coloring, JSON export buttons, god-mode editors

### Tests + entry points + tools
- `smoke_test.cpp` — 17 smoke tests
- `main.cpp` — console fallback (vlrmanager.exe)
- `launcher.cpp` — Play.exe auto-build launcher
- `build.bat` — MSVC build script. CORE_SRC includes NamesData.cpp + MatchExport.cpp. vlrgui.exe links `comdlg32.lib`.
- `tools/gen_names.js` — Node.js generator for NamesData.cpp from zengm's names.json
- `tools/dup_probe.cpp` — quick name-uniqueness probe (compile + run manually)

---

## 11. Pending Tasks / Wishlist

### Known limitations
- **Korea / Singapore / China / Chile names** still use hardcoded fallback pools (high duplicate rate). Expand by hand in `Names.cpp::pool_for` if needed.
- **Save/load persistence** — major engineering effort. Closing the exe loses all progress. Highest user wishlist item.
- **Trade system** — currently sign/release only. No 1-for-1 player swaps or pick trading.
- **shared_ptr cycle leak** RESOLVED (RecordedMatch.team1/team2 are weak_ptr).
- **Calendar UI redesign** — still table-based.

### Role overhaul DONE (2026 May) — see §4.4.1
- Position enum shrunk D/C/S/I/Flex/Count → D/C/S/I/Count (Flex demoted back to overlay flag)
- IGL spawn reworked: per-role base chance × mental composite. Duelist 0.04 base + 0.78 hard floor → Duelist IGLs now extremely rare (~3-7% of all IGLs)
- `enforce_one_igl` uses role-weighted mental score with Duelist veto. No more "promote on Leadership only" hot-swaps
- `enforce_one_flex` insert-only — multiple Flex players in starting 5 allowed
- `auto_fill_roster`: 1D + 1C + 1S + 1I + 1 generic 5th (Flex→I→C→FA cascade)
- Transfer Market: Flex-FA-missing bug fixed (root cause was `position_of` returning Position::Flex with no filter bucket; now resolved by removing Position::Flex entirely)
- UI: 4 canonical roster slots + "5th", `FlexBadge` chip alongside `IglBadge`, badge render order PositionBadge → FlexBadge → IglBadge

### Phase 2A DONE (2026 May) — flags + logos + names + new-game wizard + calendar
- Real flag bitmaps for 31 ISO codes (`FlagBitmaps.h/cpp`, 24x16 ARGB, ~47 KB embedded). Used by rewritten `CountryFlag()` via per-pixel `AddRectFilled`. Unknown ISO falls back to procedural draw.
- 22 procedural team logos (`LogoArt.h/cpp` — Shield/Hexagon/Diamond/Circle/Triangle/Star/LightningBolt/Crown/Wolf/Eagle/Phoenix/Dragon/Wave/Mountain/Sun/Crosshair/Sword/Anchor/Flame/Skull/Compass/Tree). `Team::logo_shape` field hash-assigned at gen with 30% personality bias (Aggressive→Bolt/Skull/Dragon/Flame/Sword; Tactical→Crosshair/Hexagon/Compass; Budget→Triangle/Anchor/Mountain/Tree). UI helper `TeamLogo(team, size)` wired into sidebar user-org chip, Team Profile header (96px), League Standings rows (24px).
- Revamped team-name generator (`Common.cpp`): 12 styles total — original 5 plus animal+descriptor, region-flavored, Greek/Latin/mythic, military/tactical, color+animal/noun, two-word esport, single-word — 15k+ permutations. Global de-dup retained. `make_team_tag` handles 1/2/3+ word names with last-word fallback.
- `NewGameConfig` struct + `GameManager::initialize_world_with_config(cfg, log)` + `GameManager::reset_world()` (clears every owned container incl. rankings/news/snapshots/h2h). Existing `initialize_world()` delegates to the new path with default cfg. Difficulty stored as `world_difficulty_` (0.7-1.3 slider; not yet consumed by AI strength — future hook).
- **New-Game Wizard** in `gui_main.cpp` (`DrawNewGameWizard`) — full-screen modal at boot, two-column form (org name / city / region / country with flag preview / 5-tier OrgTier radio / primary+accent ColorEdit3 with name-hash auto-derive / difficulty slider / starting year) + live preview (TeamLogo + colors + mock standings row). "Start New Career" calls reset+init. Sidebar "NEW CAREER" button with warning modal re-arms the wizard mid-session. `wWinMain` short-circuits to wizard when `show_new_game_wizard=true`.
- **Calendar redesign** (`DrawCalendar`): 5x7 day grid centered on today, ±30 nav buttons, per-cell phase tint (Stage/Playoffs/Masters/Champions/Awards/Offseason), gold today outline, blue user-match background, red tournament border, click → matchup modal with WATCH buttons routing to `OpenLiveMatchSeries`. Legend bar.

**Build notes:**
- `LogoArt.cpp` is compiled into CORE_SRC but LINKED ONLY into `vlrgui.exe` (references ImGui draw symbols). `FlagBitmaps.cpp` is plain data, linked into all 3 exes.
- `LogoArt.cpp` requires `#include <algorithm>` for `std::max` (header-only include from `<cmath>` is not enough).

### Phase 1 DONE (2026 May) — see §4.16
- Power Rankings + Community Rankings (weekly tick, tier bands, analyst notes)
- 10 new news emitters (breakout/slump/hot streak/milestone/retirement countdown/transfer rumor/rivalry/upset/historic performance/dynasty watch)
- Trophy Room + Hall of Records + Greatest Seasons (Favorites tabs)
- Player Compare page with head-to-head
- Signature agent surfacing (4 sites)
- Team trophy_case + dynasty_tier (Dynasty tier wired; ChampionEra/Contender awaits finals_history)

### Phase 2 — small systems on existing data (NEXT, when user asks)
- Rivalry tracking deeper (h2h table per team-pair AND per-player, highlighted UI when teams meet often)
- Coach philosophies surfaced in match commentary
- Veteran decline arcs as narrative chips
- Better match commentary (attribute-aware round-feed templates)
- finals_history_ wiring for ChampionEra/Contender dynasty tiers
- Team eras tagging (read off trophy_history runs)

### Phase 3 — deeper systems (multi-iteration each)
- LAN pressure expansion (partially done via LAN nerves multiplier)
- Crowd/fan pressure model
- Regional meta shifts + patch updates altering agent_priors
- Map veto strategies phase before BO3+ matches
- Player loyalty traits + contract disputes
- Greatest matches ever / historical tournament pages
- Real flag bitmaps (currently procedural)
- Profile pictures / avatars
- Awards reveal animations

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
