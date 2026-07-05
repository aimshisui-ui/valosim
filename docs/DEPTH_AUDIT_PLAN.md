# Depth Audit & Implementation Plan (match engine / contracts / menus)

Source: 10-agent adversarially-verified audit, 2026-06-20. Evidence cited as file:line.

## Match engine — duel resolution & combat realism
Verdict: architecturally sound & emergent (kill-by-kill, first-blood, trades, multikills, clutch,
economy, phases all real, 24 live attributes) but **mathematically muted** — linear win-prob flattens
skill gaps; damage/assists are kill-derived fictions; rounds end on wipe only (no spike/defuse).

1. **[S/high] Duel skill-gap exponent.** Match.cpp:1586 — replace `P=p1/(p1+p2)` with
   `p1^k/(p1^k+p2^k)`, k≈1.8–2.5 in SimConfig (Common.h ~107). Amplifies every attribute. SMOKE-FLAG: tests [21], bands.
2. **[M/high] Per-duel damage independent of kill (real ADR).** Match.cpp ~1581-1598, dmg write 1806-1833.
   loser_dmg = clamp(150*(loser_pwr/total)*rng,0,149); cap 150/victim. SMOKE-FLAG: ADR bands, rating blend 1960-1987.
3. **[S/high] Relative gun-tier economy term.** Match.cpp:1108-1109 — gun_gap=(t1_tier−t2_tier);
   p1_pwr*=1+gun_advantage*gun_gap (~0.06/tier, clamped). Full-vs-eco ~+24%, full-vs-full ~0. SMOKE-FLAG.
4. **[M/med] Scale up/consolidate phase/identity bonuses.** Match.cpp:1126 + clamps 676/1561. Do AFTER #1.
5. **[L/high] Post-plant resolution (spike timer, detonation/defuse win).** Match.cpp:884 round loop, 1607-1619, 1821.
   Highest structural risk; biggest realism gap. Land #1-3 first. Watch alive-vector erase (UAF).
6. **[M/high] Asymmetric peek-vs-hold first-shot advantage.** Match.cpp:956-957, defender flag 1122-1125.
7. **[M/med] Event-driven assists (chip-damage or util cast).** Match.cpp:1697-1785; depends on #2.
8. **[M/med] Activate near-dead calling attrs** (EconomyMgmt 786-799, AntiStrat ~763, MidRoundCalling 1192, GameSense ~1173/1648).
9. **[S/low] Gate clutch on genuine outnumber; track shots for real HS%.** Match.cpp:1842-1853, 1809-1814/2044.

## Contracts — re-sign + free agency
Verdict: realistic finances around a non-negotiation — one frozen ask, yes/no only, zero-cost reject
retry (slider-solver), no live AI competition for the user's FAs.

1. **[L/high] Live AI bidding for FAs during OFFSEASON.** GameManager.cpp ~888-906, reuse B8 bid logic 1987-2099.
   "Other interest: N teams" + deadline; rival can sign the FA first. Route via sign_player (enforce_one_igl/flex).
2. **[M/high] Player counter-offer layer.** new Player::make_counter_offer near Player.cpp:2807; modal 5046-5062.
   Close-but-short (score 40-54) → returns concrete "4y / $95K" counter + Accept button.
3. **[S/med] Cost to rejected/insulting offers.** gui_main 4988-4994 & 5018-5020; bump_mood ~473.
   Lowball → mood penalty + raised min ask; show "Your offer vs market: −23%". Kills reject-retry exploit.
4. **[S/med] Unify willingness into the accept decision.** evaluate_resign_offer (Player.cpp:2807) consumes offer.willingness.
5. **[M/med] Multi-year structure: signing bonus + role/starter promise + release cost.** Contract POD Player.h:246-249;
   sign/resign Team.cpp:414/2335. Use defaulted fields/args to keep call sites compiling.
6. **[M/low] Positive role-change branch + FA urgency signals.** Player.cpp:2988-3004; gui_main 4861-4864.

## Menus — Dear ImGui front-end
Verdict: mostly solid but a cluster of cheap player-visible bugs + 2 latent crash/nav defects. All S, test-neutral.

1. **[S/high] Guard 3 unguarded BeginTabBar sites (latent crash).** gui_main.cpp:9662, 9675, 10860.
2. **[M/high] One unit-aware money formatter** (fix $1.50M-next-to-$1500K + negative-budget raw render).
   fmt_money 1201-1205; call sites 11187-11202, 11234, 11244, 5473, 5758, 8509. Scale on llabs(), prepend sign; add fmt_money_k.
3. **[S/med] Day off-by-one in GOAT finals table.** gui_main.cpp:13130 → +1 (matches fixtures/calendar).
4. **[S/med] HS% sort key matches displayed value.** gui_main.cpp:3760 vs 3836 — sort on .hs %, not hs_hits count.
5. **[M/high] Navigation stack for Back (fix stranding).** OpenTeamProfile 1458-1463, Back 8324-8329, deep-links 6943/6998/8659.
6. **[M/med] Ellipsis-clamp round header + kill feed.** lambda 3246-3263 → ##rd_hdr 4490, kill feed 4637-4665.
7. **[M/med] Team Profile "Recent Matches" aggregate across roster** (not roster.front() only). 8582-8623.
8. **[M/low] Multi-region standings scroll/wrap + label cleanups.** DrawStandings 6203-6290; misc 2294/11216/11184.

## Sequencing (this engagement)
- Batch 1 (test-neutral polish): menu fixes #1-5 → build, expect 26/26 still pass.
- Batch 2 (highest match leverage): duel exponent #1 + relative economy #3 → build, MEASURE, re-tune k, re-baseline bands.
- Batch 3: per-duel damage #2 + event assists #7 + dead-attr activation #8 → build, re-baseline stat bands.
- Batch 4 (contracts): reject cost #3 + counter-offer #2 + willingness unify #4 → build (mostly test-neutral).
- Batch 5 (larger): live AI bidding #1, contract structure #5; post-plant resolution match #5.
Always grep the build log for `ALL TESTS PASSED` (build.bat masks the test exit code).
