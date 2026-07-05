# VCT Realism Tuning Spec (research-derived, 2024–2025 VCT)

Source: multi-agent web research (vlr.gg, liquipedia, valorantesports.com), synthesized 2026-06-20.
This is the reference the sim tunes toward. Numbers are real-VCT targets, not arbitrary.

## IGL role distribution (player-facing / team-level target)
The IGL is a utility/flex job, NOT a duelist job — the shot-caller plays back, watches the
minimap, survives into mid-round, runs comms instead of entry-fragging.

USER OVERRIDE (2026-06-20): initiators are the MOST common IGL role in real VCT — the info
role (Sova/KAY/O/Breach/Fade/Gekko) directly feeds the IGL's mid-round read. Real examples:
FNS, Saadhak, ANGE1, Redgar, Boaster (initiator/flex). The auto-research over-weighted
controllers; the user's correction is authoritative for this sim.

| Role        | Target % of IGLs | Achieved (player-facing, [14c]) | Why |
|-------------|------------------|----------------------------------|-----|
| Initiator   | **~43%** | **42%** | recon/info feeds the IGL's mid-round calls (FNS, Saadhak, ANGE1, Redgar) |
| Controller  | **~28%** | 36% | top-down map view + sets execute tempo (Boaster, stanmth, MaKo) |
| Sentinel    | **~25%** | 22% | calm survivable anchor/lurk vantage (nAts, Less) |
| Duelist     | **~7%**  | 0% | rare; decentralized/no-IGL teams (Paper Rex) or rare legacy experiments |

Implementation: `Player.cpp base_igl_chance_for_role` (Init 0.24 / Ctrl 0.27 / Sent 0.27 / Duel 0.03)
+ `Team.cpp igl_candidate_score` flat role_mult (1.13 for Init/Ctrl/Sent, 0.30 Duel) + the duelist
mental<0.80 veto. Initiator's base is set BELOW ctrl/sent on purpose — initiators carry a natural
IGL-mental edge (GameSense/Communication/MidRoundCalling) and win more team-level elections, so they
lead at ~42% even from a lower spawn rate. Duelist 0% because the veto strips sub-elite-mind duelists.

IGL should also skew to LOWER frag / HIGHER APR profiles (the team's utility player, not its top fragger).
Flex IGLs (FNS, Saadhak) bucket into whichever utility role they most occupy.

## Player stat targets (VLR.gg-style, full-event/season bands)
| Metric | Replacement | Solid starter | MVP/star |
|--------|-------------|---------------|----------|
| Rating 2.0 (mean 1.00) | 0.85–0.95 | 1.00–1.10 | 1.15–1.25+ (event peak ~1.28–1.30) |
| ACS | 175–200 | 200–225 | 235–260 |
| K/D | 0.85–0.98 | 1.00–1.15 | 1.18–1.33 |
| KAST% | 67–71% | 71–75% | 72–77% (band 62–78%) |
| ADR | 115–135 | 135–150 | 150–170+ |
| KPR | 0.62–0.74 | 0.75–0.85 | 0.85–0.92 |
| APR (role tag: <0.22 entry/duelist, >0.30 init/ctrl/IGL) | duel 0.13–0.20 / util 0.30–0.45 | — | star util 0.35–0.44 |
| FKPR/FDPR | non-entry 0.04–0.09 | duel FK 0.15–0.20 | elite duel FK 0.18–0.23, small +FK−FD |
| Clutch / CL% | 0–4 / 8–15% | 3–8 / 12–19% | 6–13 / 20–27% (cap ~27%, don't over-weight) |
| HS% (flavor, not tier) | 20–30% | 25–32% | 28–38% |

## Awards (exactly two layers)
1. **Per-event MVP** at each international event (both Masters + Champions).
   - HARD GATE: MVP comes from the tournament-WINNING roster (16/16 real MVPs 2021–2025 were champions).
   - Within the winner: the fragging core / star carry (usually duelist or star flex), weighted to
     grand-final + deep-playoff performance. NOT the IGL, NOT a pure support.
   - Surface the "snubbed best statistical player" on a losing team in the explanation.
2. **End-of-season regional VCT Awards** — 9 categories per region: Player of the Year, Rookie of the
   Year, Most Improved, Coach of the Year, + role awards (Duelist/Initiator/Controller/Sentinel/Flex).
   - Panel-voted WITH NOISE (not a deterministic top-rating sort). Anti-self-vote.
   - Team success is a MULTIPLIER (dominant rosters sweep). Recency/title-run weighting (deep intl runs).
   - Role awards judge role-relevant impact (a controller/IGL can win their role award without POTY).
   - A breakout rookie can sweep Rookie + role + Most Improved + POTY.
   - Do NOT generate per-regular-season-stage MVPs or an official "All-Pro team" (neither exists in VCT).
   - Composite scoring (kills+deaths weighted most), not any single stat. Cite three legs: headline
     stat + trophy/standing + clutch/signature moment.

## Structure (for future feature work — not all implemented)
- 4 partnered leagues: Americas (NA+SA), EMEA, Pacific, China. 12 teams each in 2025 (10 partners + 2 Ascension) = 48.
- Yearly: Kickoff -> Masters #1 -> Stage 1 -> Masters #2 -> Stage 2 -> Champions. Masters/Champions are intl LANs.
- Slots: Masters #1 = 8 (top 2/region from Kickoff); Masters #2 = 12 (top 3/region from Stage 1); Champions = 16 (4/region).
- Championship Points ledger across the season decides non-playoff Champions berths.
- Ascension/relegation: only Ascension slots are performance-relegated; partner slots permanent.
- Prize pools (2025): Masters Bangkok $500K, Masters Toronto $1M, Champions Paris $2.25M.

## Open realism gaps (logged, larger features)
- Persist a Championship Points ledger + event history (no save/load persistence exists yet).
- Event-MVP picker should select ONLY from the winning roster.
- Panel-vote noise + recency/title-run weighting + team-placement multiplier on season awards.
- Kickoff double-elim format with prior-year byes.
