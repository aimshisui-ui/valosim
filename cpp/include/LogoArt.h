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

void draw_team_logo(ImDrawList* dl, ImVec2 center, float radius,
                    std::uint8_t shape_idx,
                    ImU32 color_primary, ImU32 color_accent);

// Number of shape variants supported. Kept in lockstep with Agent A's
// LogoShape enum count. If the enum grows beyond 22 entries, update this
// value AND add new draw branches to LogoArt.cpp.
constexpr std::uint8_t kLogoShapeCount = 22;

}  // namespace vlr
