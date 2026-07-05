#include "LogoArt.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace vlr {

namespace {

constexpr float kPi  = 3.14159265358979323846f;
constexpr float kTau = 6.28318530717958647692f;

inline ImVec2 polar(ImVec2 c, float r, float angle_rad) {
    return ImVec2(c.x + r * std::cos(angle_rad), c.y + r * std::sin(angle_rad));
}

// Color math — darker shade / lighter tint of a packed RGBA. Drives the
// frame's depth stack (rim → fill → highlight) so every team's badge has
// the same "raised patch" feel regardless of source colors.
inline ImU32 shade(ImU32 col, float factor) {
    int r = (col      ) & 0xFF;
    int g = (col >>  8) & 0xFF;
    int b = (col >> 16) & 0xFF;
    int a = (col >> 24) & 0xFF;
    r = std::max(0, static_cast<int>(r * factor));
    g = std::max(0, static_cast<int>(g * factor));
    b = std::max(0, static_cast<int>(b * factor));
    return IM_COL32(r, g, b, a);
}

inline ImU32 tint(ImU32 col, float factor) {
    int r = (col      ) & 0xFF;
    int g = (col >>  8) & 0xFF;
    int b = (col >> 16) & 0xFF;
    int a = (col >> 24) & 0xFF;
    r = std::min(255, r + static_cast<int>((255 - r) * factor));
    g = std::min(255, g + static_cast<int>((255 - g) * factor));
    b = std::min(255, b + static_cast<int>((255 - b) * factor));
    return IM_COL32(r, g, b, a);
}

inline float luma(ImU32 col) {
    int r = (col      ) & 0xFF;
    int g = (col >>  8) & 0xFF;
    int b = (col >> 16) & 0xFF;
    return (0.299f * r + 0.587f * g + 0.114f * b) / 255.0f;
}

// Rebuild an ImU32 with a different alpha channel (preserving RGB). Used to
// derive the soft inner-glow ring color from the team's hairline accent.
inline ImU32 with_alpha(ImU32 col, int a) {
    return (col & 0x00FFFFFFu) | (static_cast<ImU32>(a & 0xFF) << 24);
}

// Pick a high-contrast monogram color against the primary backplate.
// Falls back to white/black when accent fails the contrast check — same
// logic real esports identities use to keep wordmarks legible.
inline ImU32 pick_monogram_color(ImU32 primary, ImU32 accent) {
    float lp = luma(primary);
    float la = luma(accent);
    // Need at least 0.30 luma delta for the accent to read on the backplate.
    if (std::fabs(lp - la) > 0.30f) return accent;
    return (lp < 0.50f) ? IM_COL32(0xF8, 0xF8, 0xFA, 0xFF)
                        : IM_COL32(0x12, 0x14, 0x1A, 0xFF);
}

// ---------------------------------------------------------------------------
// Frame backplates. Five polished shapes — every team's logo sits inside
// one of these. Each frame fills with the team's primary color, layers a
// darker shade rim + lighter top highlight for depth, and ends with a
// hairline accent stroke. Shape selected per-team via shape_idx % 5.
// ---------------------------------------------------------------------------

// Draws a soft drop shadow disc beneath whatever frame is about to render.
// Shadow tint is a slightly blue-leaning charcoal (matches the deep-charcoal
// surface bg around the badge) — pure black halos read as "stock CG asset"
// against a real broadcast palette.
inline void drop_shadow(ImDrawList* dl, ImVec2 c, float r) {
    // Two offset translucent discs stack into a soft glow.
    dl->AddCircleFilled(ImVec2(c.x + r * 0.04f, c.y + r * 0.08f),
                        r * 1.04f, IM_COL32(0x05, 0x08, 0x10, 60), 36);
    dl->AddCircleFilled(ImVec2(c.x + r * 0.02f, c.y + r * 0.05f),
                        r * 1.02f, IM_COL32(0x05, 0x08, 0x10, 40), 36);
}

void frame_shield(ImDrawList* dl, ImVec2 c, float r,
                  ImU32 primary, ImU32 accent) {
    drop_shadow(dl, c, r);
    ImU32 rim   = shade(primary, 0.42f);
    ImU32 fill  = primary;
    ImU32 hi    = tint(primary, 0.20f);
    // 7-vertex heraldic shield — flat top, narrow shoulders, tapered point.
    auto build = [&](float scale, ImVec2 pts[7]) {
        float sr = r * scale;
        pts[0] = ImVec2(c.x - sr * 0.88f, c.y - sr * 0.78f);
        pts[1] = ImVec2(c.x + sr * 0.88f, c.y - sr * 0.78f);
        pts[2] = ImVec2(c.x + sr * 0.88f, c.y - sr * 0.10f);
        pts[3] = ImVec2(c.x + sr * 0.72f, c.y + sr * 0.55f);
        pts[4] = ImVec2(c.x,              c.y + sr * 0.98f);
        pts[5] = ImVec2(c.x - sr * 0.72f, c.y + sr * 0.55f);
        pts[6] = ImVec2(c.x - sr * 0.88f, c.y - sr * 0.10f);
    };
    ImVec2 rim_pts[7];   build(1.00f, rim_pts);
    ImVec2 fill_pts[7];  build(0.95f, fill_pts);
    dl->AddConvexPolyFilled(rim_pts,  7, rim);
    dl->AddConvexPolyFilled(fill_pts, 7, fill);
    // Top highlight band — slight gradient hint via a brightened crescent.
    ImVec2 hl[5] = {
        ImVec2(c.x - r * 0.76f, c.y - r * 0.66f),
        ImVec2(c.x + r * 0.76f, c.y - r * 0.66f),
        ImVec2(c.x + r * 0.60f, c.y - r * 0.32f),
        ImVec2(c.x,             c.y - r * 0.22f),
        ImVec2(c.x - r * 0.60f, c.y - r * 0.32f),
    };
    dl->AddConvexPolyFilled(hl, 5, IM_COL32(255, 250, 235, 28));
    (void)hi;
    // Soft inner-glow ring (30% accent) inset ~2px from the hairline border.
    // Subtly lifts the badge so it reads as a layered patch rather than flat.
    {
        ImVec2 inner[7];
        build(0.91f, inner);
        dl->AddPolyline(inner, 7, with_alpha(accent, 0x4D),
                        ImDrawFlags_Closed, 1.0f);
    }
    // Hairline accent border.
    dl->AddPolyline(fill_pts, 7, accent, ImDrawFlags_Closed,
                    std::max(1.4f, r * 0.045f));
}

void frame_hex(ImDrawList* dl, ImVec2 c, float r,
               ImU32 primary, ImU32 accent) {
    drop_shadow(dl, c, r);
    ImU32 rim  = shade(primary, 0.42f);
    ImU32 fill = primary;
    // Pointy-top hexagon.
    auto build = [&](float scale, ImVec2 pts[6]) {
        float sr = r * scale;
        for (int i = 0; i < 6; ++i) {
            float a = (kTau * i / 6.0f) - kPi / 2.0f;
            pts[i] = polar(c, sr, a);
        }
    };
    ImVec2 rim_pts[6];  build(0.97f, rim_pts);
    ImVec2 fill_pts[6]; build(0.92f, fill_pts);
    dl->AddConvexPolyFilled(rim_pts,  6, rim);
    dl->AddConvexPolyFilled(fill_pts, 6, fill);
    // Top highlight wedge.
    ImVec2 hl[3] = {fill_pts[5], fill_pts[0], fill_pts[1]};
    dl->AddTriangleFilled(hl[0], hl[1], hl[2], IM_COL32(255, 250, 235, 32));
    // Soft inner-glow ring (30% accent) inset ~2px from the hairline border.
    {
        ImVec2 inner[6];
        build(0.88f, inner);
        dl->AddPolyline(inner, 6, with_alpha(accent, 0x4D),
                        ImDrawFlags_Closed, 1.0f);
    }
    // Hairline accent border.
    dl->AddPolyline(fill_pts, 6, accent, ImDrawFlags_Closed,
                    std::max(1.4f, r * 0.045f));
}

void frame_circle(ImDrawList* dl, ImVec2 c, float r,
                  ImU32 primary, ImU32 accent) {
    drop_shadow(dl, c, r);
    ImU32 rim  = shade(primary, 0.42f);
    ImU32 fill = primary;
    dl->AddCircleFilled(c, r * 0.97f, rim,  40);
    dl->AddCircleFilled(c, r * 0.92f, fill, 40);
    // Top highlight crescent.
    dl->AddCircleFilled(ImVec2(c.x, c.y - r * 0.32f),
                        r * 0.55f, IM_COL32(255, 250, 235, 28), 32);
    dl->AddCircleFilled(ImVec2(c.x, c.y - r * 0.46f),
                        r * 0.32f, IM_COL32(255, 250, 235, 36), 28);
    // Soft inner-glow ring (30% accent) inset ~2px from the hairline border.
    dl->AddCircle(c, r * 0.88f, with_alpha(accent, 0x4D), 40, 1.0f);
    // Hairline accent border.
    dl->AddCircle(c, r * 0.94f, accent, 40, std::max(1.4f, r * 0.05f));
}

void frame_diamond(ImDrawList* dl, ImVec2 c, float r,
                   ImU32 primary, ImU32 accent) {
    drop_shadow(dl, c, r);
    ImU32 rim  = shade(primary, 0.42f);
    ImU32 fill = primary;
    auto build = [&](float scale, ImVec2 pts[4]) {
        float sr = r * scale;
        pts[0] = ImVec2(c.x,            c.y - sr * 0.98f);
        pts[1] = ImVec2(c.x + sr * 0.90f, c.y);
        pts[2] = ImVec2(c.x,            c.y + sr * 0.98f);
        pts[3] = ImVec2(c.x - sr * 0.90f, c.y);
    };
    ImVec2 rim_pts[4];  build(1.00f, rim_pts);
    ImVec2 fill_pts[4]; build(0.94f, fill_pts);
    dl->AddConvexPolyFilled(rim_pts,  4, rim);
    dl->AddConvexPolyFilled(fill_pts, 4, fill);
    // Top quadrant highlight.
    ImVec2 hl[3] = {
        fill_pts[3], fill_pts[0], fill_pts[1],
    };
    dl->AddTriangleFilled(
        ImVec2((hl[0].x + hl[1].x) * 0.5f, (hl[0].y + hl[1].y) * 0.5f),
        fill_pts[0],
        ImVec2((hl[1].x + hl[2].x) * 0.5f, (hl[1].y + hl[2].y) * 0.5f),
        IM_COL32(255, 250, 235, 32));
    // Soft inner-glow ring (30% accent) inset ~2px from the hairline border.
    {
        ImVec2 inner[4];
        build(0.88f, inner);
        dl->AddPolyline(inner, 4, with_alpha(accent, 0x4D),
                        ImDrawFlags_Closed, 1.0f);
    }
    // Hairline accent border.
    dl->AddPolyline(fill_pts, 4, accent, ImDrawFlags_Closed,
                    std::max(1.4f, r * 0.045f));
}

void frame_banner(ImDrawList* dl, ImVec2 c, float r,
                  ImU32 primary, ImU32 accent) {
    drop_shadow(dl, c, r);
    ImU32 rim  = shade(primary, 0.42f);
    ImU32 fill = primary;
    // Slightly wider-than-tall rectangle with chevron-cut bottom edge.
    auto build = [&](float scale, ImVec2 pts[6]) {
        float sr = r * scale;
        pts[0] = ImVec2(c.x - sr * 0.95f, c.y - sr * 0.80f);
        pts[1] = ImVec2(c.x + sr * 0.95f, c.y - sr * 0.80f);
        pts[2] = ImVec2(c.x + sr * 0.95f, c.y + sr * 0.45f);
        pts[3] = ImVec2(c.x + sr * 0.05f, c.y + sr * 0.95f);
        pts[4] = ImVec2(c.x - sr * 0.05f, c.y + sr * 0.95f);
        pts[5] = ImVec2(c.x - sr * 0.95f, c.y + sr * 0.45f);
    };
    ImVec2 rim_pts[6];  build(1.00f, rim_pts);
    ImVec2 fill_pts[6]; build(0.95f, fill_pts);
    dl->AddConvexPolyFilled(rim_pts,  6, rim);
    dl->AddConvexPolyFilled(fill_pts, 6, fill);
    // Top highlight band.
    ImVec2 hl[4] = {
        ImVec2(c.x - r * 0.85f, c.y - r * 0.70f),
        ImVec2(c.x + r * 0.85f, c.y - r * 0.70f),
        ImVec2(c.x + r * 0.85f, c.y - r * 0.30f),
        ImVec2(c.x - r * 0.85f, c.y - r * 0.30f),
    };
    dl->AddConvexPolyFilled(hl, 4, IM_COL32(255, 250, 235, 28));
    // Soft inner-glow ring (30% accent) inset ~2px from the hairline border.
    {
        ImVec2 inner[6];
        build(0.89f, inner);
        dl->AddPolyline(inner, 6, with_alpha(accent, 0x4D),
                        ImDrawFlags_Closed, 1.0f);
    }
    // Hairline accent border.
    dl->AddPolyline(fill_pts, 6, accent, ImDrawFlags_Closed,
                    std::max(1.4f, r * 0.045f));
}

void draw_frame(ImDrawList* dl, ImVec2 c, float r,
                int frame_idx, ImU32 primary, ImU32 accent) {
    switch (frame_idx % 5) {
        case 0: frame_shield (dl, c, r, primary, accent); break;
        case 1: frame_hex    (dl, c, r, primary, accent); break;
        case 2: frame_circle (dl, c, r, primary, accent); break;
        case 3: frame_diamond(dl, c, r, primary, accent); break;
        case 4: frame_banner (dl, c, r, primary, accent); break;
    }
}

// ---------------------------------------------------------------------------
// Monogram — the team's TAG rendered as bold typography in the center.
// Scales with radius and tag length so 1-3 char tags all read cleanly. Uses
// the active ImGui font (assumed to be a strong sans-serif). Drop shadow
// offset for depth.
// ---------------------------------------------------------------------------

void draw_monogram(ImDrawList* dl, ImVec2 c, float r,
                   const char* tag, ImU32 fg) {
    if (!tag || !*tag) return;
    // Clamp the rendered tag length to 4 chars (defensive — make_team_tag
    // returns 3, but god-mode editing or save-data drift could push higher).
    char buf[5];
    int  n = 0;
    for (; n < 4 && tag[n]; ++n) buf[n] = tag[n];
    buf[n] = 0;

    // Defensive font fallback. ImGui::GetFont() should always return the
    // active font once io.Fonts->Build() has run, but if a caller invokes
    // this outside a frame (e.g. wizard preview mid-init), fall back to the
    // first atlas font rather than silently rendering nothing.
    ImFont* font = ImGui::GetFont();
    if (!font) {
        ImFontAtlas* atlas = ImGui::GetIO().Fonts;
        if (atlas && atlas->Fonts.Size > 0) font = atlas->Fonts[0];
    }
    if (!font) return;

    // Font size scales inversely with tag length so the monogram always
    // fills ~62% of the frame width. Tight enough to read as a wordmark,
    // loose enough that the frame still frames it.
    float fs;
    switch (n) {
        case 1: fs = r * 1.20f; break;
        case 2: fs = r * 0.90f; break;
        case 3: fs = r * 0.70f; break;
        default: fs = r * 0.58f; break;
    }
    // Cap font size to a sensible upper bound so the wizard's 120-px preview
    // doesn't ask ImGui to rasterize a 130-px glyph (cache-thrashing).
    fs = std::min(fs, 64.0f);
    // Floor so the bracket-card 14-px logo doesn't render unreadable text.
    fs = std::max(fs, 9.0f);

    ImVec2 sz = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, buf);
    ImVec2 pos(c.x - sz.x * 0.5f, c.y - sz.y * 0.5f);

    // Drop shadow — vertical-dominant offset (light-from-above), low alpha,
    // slight blue-leaning charcoal tint to match the deep-charcoal surface
    // bg. A tiny 2% horizontal offset adds just enough lateral depth without
    // reading as italic-skew.
    ImU32 shadow = IM_COL32(0x05, 0x08, 0x10, 110);
    dl->AddText(font, fs, ImVec2(pos.x + fs * 0.02f, pos.y + fs * 0.08f),
                shadow, buf);
    // Foreground tag.
    dl->AddText(font, fs, pos, fg, buf);
}

// ---------------------------------------------------------------------------
// Accent flourishes. One of four small decorative elements rendered on top
// of the frame to give each team's badge a unique signature. Selected per-
// team via `(shape_idx / 5) % 4` so a team's frame and accent are stable
// across save/load.
// ---------------------------------------------------------------------------

void accent_top_stripe(ImDrawList* dl, ImVec2 c, float r, ImU32 accent) {
    // Thin horizontal accent bar across the top inner quarter of the frame.
    // Rounded ends read as a finished bar rather than a hard-cut sliver.
    float y0 = c.y - r * 0.72f;
    float y1 = c.y - r * 0.62f;
    float x0 = c.x - r * 0.62f;
    float x1 = c.x + r * 0.62f;
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), accent, 1.5f);
}

void accent_corner_dots(ImDrawList* dl, ImVec2 c, float r, ImU32 accent) {
    // 4 small dots at the inner-frame "corners" (offset toward the center
    // a bit so they sit inside any of the 5 frame shapes). Bumped dot
    // radius so the dots still read at 24-px standings thumbnails.
    float off = r * 0.66f;
    float dr  = std::max(1.5f, r * 0.10f);
    dl->AddCircleFilled(ImVec2(c.x - off, c.y - off), dr, accent, 12);
    dl->AddCircleFilled(ImVec2(c.x + off, c.y - off), dr, accent, 12);
    dl->AddCircleFilled(ImVec2(c.x - off, c.y + off), dr, accent, 12);
    dl->AddCircleFilled(ImVec2(c.x + off, c.y + off), dr, accent, 12);
}

void accent_bottom_chevron(ImDrawList* dl, ImVec2 c, float r, ImU32 accent) {
    // V-shape under the monogram.
    float yc = c.y + r * 0.55f;
    float xL = c.x - r * 0.42f;
    float xR = c.x + r * 0.42f;
    float xM = c.x;
    float yT = yc - r * 0.10f;
    float yB = yc + r * 0.10f;
    auto thick = std::max(1.8f, r * 0.08f);
    dl->AddLine(ImVec2(xL, yT), ImVec2(xM, yB), accent, thick);
    dl->AddLine(ImVec2(xM, yB), ImVec2(xR, yT), accent, thick);
}

void accent_inner_outline(ImDrawList* dl, ImVec2 c, float r,
                          int frame_idx, ImU32 accent) {
    // A second smaller outline of the same frame shape, painted in accent.
    // Gives a "framed wordmark" feel.
    auto thick = std::max(1.4f, r * 0.045f);
    switch (frame_idx % 5) {
        case 0: {  // shield
            float sr = r * 0.72f;
            ImVec2 pts[7] = {
                ImVec2(c.x - sr * 0.88f, c.y - sr * 0.78f),
                ImVec2(c.x + sr * 0.88f, c.y - sr * 0.78f),
                ImVec2(c.x + sr * 0.88f, c.y - sr * 0.10f),
                ImVec2(c.x + sr * 0.72f, c.y + sr * 0.55f),
                ImVec2(c.x,              c.y + sr * 0.98f),
                ImVec2(c.x - sr * 0.72f, c.y + sr * 0.55f),
                ImVec2(c.x - sr * 0.88f, c.y - sr * 0.10f),
            };
            dl->AddPolyline(pts, 7, accent, ImDrawFlags_Closed, thick);
            break;
        }
        case 1: {  // hex
            ImVec2 pts[6];
            for (int i = 0; i < 6; ++i) {
                float a = (kTau * i / 6.0f) - kPi / 2.0f;
                pts[i] = polar(c, r * 0.70f, a);
            }
            dl->AddPolyline(pts, 6, accent, ImDrawFlags_Closed, thick);
            break;
        }
        case 2: {  // circle
            dl->AddCircle(c, r * 0.72f, accent, 36, thick);
            break;
        }
        case 3: {  // diamond
            ImVec2 pts[4] = {
                ImVec2(c.x,            c.y - r * 0.72f),
                ImVec2(c.x + r * 0.66f, c.y),
                ImVec2(c.x,            c.y + r * 0.72f),
                ImVec2(c.x - r * 0.66f, c.y),
            };
            dl->AddPolyline(pts, 4, accent, ImDrawFlags_Closed, thick);
            break;
        }
        case 4: {  // banner
            ImVec2 pts[6] = {
                ImVec2(c.x - r * 0.72f, c.y - r * 0.62f),
                ImVec2(c.x + r * 0.72f, c.y - r * 0.62f),
                ImVec2(c.x + r * 0.72f, c.y + r * 0.34f),
                ImVec2(c.x + r * 0.04f, c.y + r * 0.72f),
                ImVec2(c.x - r * 0.04f, c.y + r * 0.72f),
                ImVec2(c.x - r * 0.72f, c.y + r * 0.34f),
            };
            dl->AddPolyline(pts, 6, accent, ImDrawFlags_Closed, thick);
            break;
        }
    }
}

void draw_accent(ImDrawList* dl, ImVec2 c, float r,
                 int accent_idx, int frame_idx, ImU32 accent) {
    switch (accent_idx % 4) {
        case 0: accent_top_stripe    (dl, c, r, accent); break;
        case 1: accent_corner_dots   (dl, c, r, accent); break;
        case 2: accent_bottom_chevron(dl, c, r, accent); break;
        case 3: accent_inner_outline (dl, c, r, frame_idx, accent); break;
    }
}

// ---------------------------------------------------------------------------
// Tag derivation fallback. If a caller doesn't pass a tag (legacy code path
// or wizard preview before world init), derive a 1-3 char monogram from the
// shape index alone so the badge still has a wordmark to render. Uses a
// curated brand-modern set so it never produces awkward letter combos.
// ---------------------------------------------------------------------------

const char* default_monogram_for_shape(std::uint8_t shape_idx) {
    // 30 entries — one per shape — so callers that pass only shape_idx still
    // see a stable per-shape monogram. These read like generic VCT-style
    // org abbreviations rather than placeholder text.
    static const char* const kDefaults[30] = {
        "VLR", "APX", "NRG", "BST", "FOX", "ARC", "ZEN", "RIX",
        "OBS", "ECL", "STX", "TMP", "OPS", "DRX", "FNX", "JET",
        "NOX", "VYR", "EMR", "FRY", "PRT", "ELM", "FNG", "TRI",
        "XSW", "GRX", "TLN", "EYE", "CBR", "SUN"
    };
    return kDefaults[shape_idx % 30];
}

}  // namespace

void draw_team_logo(ImDrawList* dl, ImVec2 center, float radius,
                    std::uint8_t shape_idx,
                    ImU32 color_primary, ImU32 color_accent,
                    const char* tag) {
    if (dl == nullptr || radius <= 0.0f) return;

    // 1. Frame backplate (depth stack: shadow → rim → fill → highlight → border).
    //    Five distinct frame shapes, selected per-team via shape_idx % 5.
    int frame_idx = static_cast<int>(shape_idx) % 5;
    draw_frame(dl, center, radius, frame_idx, color_primary, color_accent);

    // 2. Monogram — bold typographic tag, centered, drop-shadowed. Color
    //    auto-picked for contrast against the primary backplate. If the
    //    caller passed an empty tag (legacy emblem-only paths), fall back
    //    to a stable per-shape default so the badge still reads as a brand.
    const char* monogram = (tag && *tag) ? tag
                                          : default_monogram_for_shape(shape_idx);
    ImU32 mono_col = pick_monogram_color(color_primary, color_accent);
    draw_monogram(dl, center, radius, monogram, mono_col);

    // 3. Accent flourish — one of four (top stripe / corner dots / bottom
    //    chevron / inner outline). Selected per-team via (shape_idx / 5) %
    //    4 so a team's frame + accent are jointly deterministic.
    int accent_idx = (static_cast<int>(shape_idx) / 5) % 4;
    draw_accent(dl, center, radius, accent_idx, frame_idx, color_accent);
}

}  // namespace vlr
