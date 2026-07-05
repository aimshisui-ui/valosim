#pragma once
#include <cstdint>
#include "imgui.h"

namespace vlr {

// ---------------------------------------------------------------------------
// Procedural team-logo rendering.
//
// Agent A is introducing a `LogoShape` enum on Team. To avoid coupling this
// header to Team.h (and to let other agents iterate on the enum layout
// without forcing a rebuild of every caller), we accept the shape as a raw
// `std::uint8_t`. Callers pass `static_cast<std::uint8_t>(team.logo_shape)`.
//
// `shape_idx` is expected to be in [0, 22). Out-of-range values fall back to
// a Shield (shape 0).
//
// Logos are drawn into the supplied `ImDrawList` centered on `center`,
// roughly inscribed in a circle of `radius` pixels. Use `color_primary` for
// the main fill and `color_accent` for inner detail / outline. Both colors
// are packed `ImU32` (RGBA).
//
// Rendering uses convex polygon fills + circles + triangles only — cheap
// vector primitives that scale cleanly from 24 px roster thumbnails up to
// 128 px team-profile heroes.
// ---------------------------------------------------------------------------

// As of 2026-06-11 the logo system is a TYPOGRAPHIC MONOGRAM BADGE rather
// than a procedural emblem. Every team's logo is composed of three pieces,
// all driven from `shape_idx` and the optional `tag`:
//
//   1. Frame backplate — one of 5 polished shapes (Shield, Hexagon, Circle,
//      Diamond, Banner) selected via `shape_idx % 5`. Each shape renders
//      with a depth stack (drop shadow → darker rim → primary fill → top
//      highlight → hairline accent border).
//   2. Monogram — the team's `tag` rendered as bold typography in the
//      center using the active ImGui font. Auto-sized for tag length and
//      drop-shadowed for depth. Color picked for luma contrast against
//      the primary backplate (accent → white → black fallback chain).
//      If `tag` is null/empty, a per-shape default monogram is used so
//      legacy callers still render a brand-feel badge.
//   3. Accent flourish — one of 4 small decorations (top stripe, corner
//      dots, bottom chevron, or inner outline of the same frame shape)
//      selected via `(shape_idx / 5) % 4`.
//
// 5 frames × 4 accents = 20 distinct badge templates, multiplied by the
// team-color palette and unique 3-letter tag — so the league reads as a
// coherent set of professional wordmarks rather than 30 disconnected
// procedural silhouettes.
//
// `shape_idx` is expected to be in [0, kLogoShapeCount). Out-of-range
// values are wrapped via modulo.
void draw_team_logo(ImDrawList* dl, ImVec2 center, float radius,
                    std::uint8_t shape_idx,
                    ImU32 color_primary, ImU32 color_accent,
                    const char* tag = nullptr);

// Number of distinct shape-idx values consumed by the assignment hash on
// the Team side. The badge composition (frame × accent) only uses 20
// combinations, but we keep a wider hash range so the personality-bias
// pools on Team::Team (Aggressive/Tactical/Budget) still spread teams
// across the badge templates evenly. Must stay in lockstep with
// `LogoShape::Count` in Team.h.
constexpr std::uint8_t kLogoShapeCount = 30;

}  // namespace vlr
