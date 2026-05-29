#include "LogoArt.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace vlr {

namespace {

constexpr float kPi  = 3.14159265358979323846f;
constexpr float kTau = 6.28318530717958647692f;

inline ImVec2 polar(ImVec2 c, float r, float angle_rad) {
    return ImVec2(c.x + r * std::cos(angle_rad), c.y + r * std::sin(angle_rad));
}

inline ImVec2 v_add(ImVec2 a, ImVec2 b) { return ImVec2(a.x + b.x, a.y + b.y); }
inline ImVec2 v_mul(ImVec2 a, float s)  { return ImVec2(a.x * s, a.y * s); }

// ---------------------------------------------------------------------------
// Individual shape draw helpers.
//
// All helpers accept the center, radius, and the two style colors. They
// render into the provided ImDrawList. Geometry uses normalized offsets
// (× radius) so each shape scales cleanly with the caller's size request.
// ---------------------------------------------------------------------------

void draw_shield(ImDrawList* dl, ImVec2 c, float r, ImU32 primary, ImU32 accent) {
    // Pentagonal-ish shield: flat top with two small notches in the
    // shoulders, tapering to a point at the bottom.
    ImVec2 pts[6] = {
        ImVec2(c.x - r * 0.85f, c.y - r * 0.80f),  // top-left
        ImVec2(c.x + r * 0.85f, c.y - r * 0.80f),  // top-right
        ImVec2(c.x + r * 0.85f, c.y + r * 0.10f),  // mid-right
        ImVec2(c.x + r * 0.20f, c.y + r * 0.95f),  // bottom-right curve
        ImVec2(c.x - r * 0.20f, c.y + r * 0.95f),  // bottom-left curve
        ImVec2(c.x - r * 0.85f, c.y + r * 0.10f),  // mid-left
    };
    dl->AddConvexPolyFilled(pts, 6, primary);
    // Accent border via polyline (closed).
    dl->AddPolyline(pts, 6, accent, ImDrawFlags_Closed, std::max(1.0f, r * 0.10f));
    // Inner accent chevron — a single thick line down the middle.
    dl->AddLine(ImVec2(c.x, c.y - r * 0.50f),
                ImVec2(c.x, c.y + r * 0.65f),
                accent, std::max(1.0f, r * 0.15f));
}

void draw_hexagon(ImDrawList* dl, ImVec2 c, float r, ImU32 primary, ImU32 accent) {
    ImVec2 pts[6];
    for (int i = 0; i < 6; ++i) {
        float a = (kTau * i / 6.0f) - kPi / 2.0f;  // start with point up
        pts[i] = polar(c, r * 0.95f, a);
    }
    dl->AddConvexPolyFilled(pts, 6, primary);
    dl->AddPolyline(pts, 6, accent, ImDrawFlags_Closed, std::max(1.0f, r * 0.10f));
}

void draw_diamond_logo(ImDrawList* dl, ImVec2 c, float r, ImU32 primary, ImU32 accent) {
    ImVec2 pts[4] = {
        ImVec2(c.x,          c.y - r * 0.95f),
        ImVec2(c.x + r * 0.80f, c.y),
        ImVec2(c.x,          c.y + r * 0.95f),
        ImVec2(c.x - r * 0.80f, c.y),
    };
    dl->AddConvexPolyFilled(pts, 4, primary);
    // Inset accent diamond.
    ImVec2 in[4] = {
        ImVec2(c.x,          c.y - r * 0.45f),
        ImVec2(c.x + r * 0.38f, c.y),
        ImVec2(c.x,          c.y + r * 0.45f),
        ImVec2(c.x - r * 0.38f, c.y),
    };
    dl->AddConvexPolyFilled(in, 4, accent);
}

void draw_circle_logo(ImDrawList* dl, ImVec2 c, float r, ImU32 primary, ImU32 accent) {
    dl->AddCircleFilled(c, r * 0.92f, primary, 32);
    dl->AddCircle(c, r * 0.92f, accent, 32, std::max(1.0f, r * 0.14f));
    // Inner accent ring (smaller).
    dl->AddCircle(c, r * 0.50f, accent, 24, std::max(1.0f, r * 0.10f));
}

void draw_triangle_logo(ImDrawList* dl, ImVec2 c, float r, ImU32 primary, ImU32 accent) {
    // Equilateral, point up.
    ImVec2 p1(c.x,            c.y - r * 0.85f);
    ImVec2 p2(c.x + r * 0.85f, c.y + r * 0.55f);
    ImVec2 p3(c.x - r * 0.85f, c.y + r * 0.55f);
    dl->AddTriangleFilled(p1, p2, p3, primary);
    ImVec2 pts[3] = {p1, p2, p3};
    dl->AddPolyline(pts, 3, accent, ImDrawFlags_Closed, std::max(1.0f, r * 0.12f));
}

void draw_star(ImDrawList* dl, ImVec2 c, float r, ImU32 primary, ImU32 accent) {
    // 5-point star using 10 alternating outer/inner vertices.
    ImVec2 pts[10];
    float outer = r * 0.95f;
    float inner = r * 0.42f;
    for (int i = 0; i < 10; ++i) {
        float a = (kTau * i / 10.0f) - kPi / 2.0f;
        float rad = (i % 2 == 0) ? outer : inner;
        pts[i] = polar(c, rad, a);
    }
    // Star is concave — split into triangles fanned from center for fill.
    for (int i = 0; i < 10; ++i) {
        int j = (i + 1) % 10;
        dl->AddTriangleFilled(c, pts[i], pts[j], primary);
    }
    dl->AddPolyline(pts, 10, accent, ImDrawFlags_Closed, std::max(1.0f, r * 0.08f));
}

void draw_lightning(ImDrawList* dl, ImVec2 c, float r, ImU32 primary, ImU32 accent) {
    // 7-vertex Z-bolt. Concave overall but we draw it as 2 convex halves.
    // Top half: from upper-right peak down-left to the middle waist.
    ImVec2 a(c.x + r * 0.45f, c.y - r * 0.85f);
    ImVec2 b(c.x - r * 0.20f, c.y - r * 0.10f);
    ImVec2 cc(c.x + r * 0.10f, c.y - r * 0.10f);
    ImVec2 d(c.x + r * 0.55f, c.y - r * 0.55f);
    ImVec2 top[4] = {a, d, cc, b};
    dl->AddConvexPolyFilled(top, 4, primary);
    // Bottom half: from middle waist down-right then back left.
    ImVec2 e(c.x - r * 0.45f, c.y + r * 0.85f);
    ImVec2 f(c.x + r * 0.20f, c.y + r * 0.10f);
    ImVec2 g(c.x - r * 0.10f, c.y + r * 0.10f);
    ImVec2 h(c.x - r * 0.55f, c.y + r * 0.55f);
    ImVec2 bot[4] = {e, h, g, f};
    dl->AddConvexPolyFilled(bot, 4, primary);
    // Accent outline strokes.
    dl->AddLine(a, d, accent, std::max(1.0f, r * 0.10f));
    dl->AddLine(b, e, accent, std::max(1.0f, r * 0.10f));
}

void draw_crown(ImDrawList* dl, ImVec2 c, float r, ImU32 primary, ImU32 accent) {
    // 3-point crown: base bar + three triangular peaks.
    // Base.
    ImVec2 base_tl(c.x - r * 0.85f, c.y + r * 0.30f);
    ImVec2 base_tr(c.x + r * 0.85f, c.y + r * 0.30f);
    ImVec2 base_br(c.x + r * 0.85f, c.y + r * 0.75f);
    ImVec2 base_bl(c.x - r * 0.85f, c.y + r * 0.75f);
    dl->AddQuadFilled(base_tl, base_tr, base_br, base_bl, primary);
    // 3 peaks (center taller).
    dl->AddTriangleFilled(ImVec2(c.x - r * 0.85f, c.y + r * 0.30f),
                          ImVec2(c.x - r * 0.40f, c.y + r * 0.30f),
                          ImVec2(c.x - r * 0.62f, c.y - r * 0.45f), primary);
    dl->AddTriangleFilled(ImVec2(c.x - r * 0.20f, c.y + r * 0.30f),
                          ImVec2(c.x + r * 0.20f, c.y + r * 0.30f),
                          ImVec2(c.x,             c.y - r * 0.85f), primary);
    dl->AddTriangleFilled(ImVec2(c.x + r * 0.40f, c.y + r * 0.30f),
                          ImVec2(c.x + r * 0.85f, c.y + r * 0.30f),
                          ImVec2(c.x + r * 0.62f, c.y - r * 0.45f), primary);
    // Accent jewel dots on the peaks.
    dl->AddCircleFilled(ImVec2(c.x, c.y - r * 0.60f), r * 0.12f, accent, 12);
    dl->AddCircleFilled(ImVec2(c.x - r * 0.62f, c.y - r * 0.20f), r * 0.10f, accent, 10);
    dl->AddCircleFilled(ImVec2(c.x + r * 0.62f, c.y - r * 0.20f), r * 0.10f, accent, 10);
}

void draw_wolf(ImDrawList* dl, ImVec2 c, float r, ImU32 primary, ImU32 accent) {
    // Stylized wolf head silhouette: 2 triangle ears + circle face + triangle snout.
    // Ears (set wide).
    dl->AddTriangleFilled(ImVec2(c.x - r * 0.85f, c.y - r * 0.25f),
                          ImVec2(c.x - r * 0.45f, c.y - r * 0.35f),
                          ImVec2(c.x - r * 0.55f, c.y - r * 0.95f), primary);
    dl->AddTriangleFilled(ImVec2(c.x + r * 0.85f, c.y - r * 0.25f),
                          ImVec2(c.x + r * 0.45f, c.y - r * 0.35f),
                          ImVec2(c.x + r * 0.55f, c.y - r * 0.95f), primary);
    // Face (circle).
    dl->AddCircleFilled(c, r * 0.65f, primary, 24);
    // Snout (downward triangle).
    dl->AddTriangleFilled(ImVec2(c.x - r * 0.30f, c.y + r * 0.20f),
                          ImVec2(c.x + r * 0.30f, c.y + r * 0.20f),
                          ImVec2(c.x,             c.y + r * 0.90f), primary);
    // Eyes — small accent dots.
    dl->AddCircleFilled(ImVec2(c.x - r * 0.25f, c.y - r * 0.05f), r * 0.10f, accent, 8);
    dl->AddCircleFilled(ImVec2(c.x + r * 0.25f, c.y - r * 0.05f), r * 0.10f, accent, 8);
}

void draw_eagle(ImDrawList* dl, ImVec2 c, float r, ImU32 primary, ImU32 accent) {
    // Spread-wing silhouette: a broad V with a center body.
    // Left wing (triangle).
    dl->AddTriangleFilled(ImVec2(c.x - r * 0.95f, c.y - r * 0.10f),
                          ImVec2(c.x,             c.y + r * 0.10f),
                          ImVec2(c.x - r * 0.30f, c.y + r * 0.50f), primary);
    // Right wing.
    dl->AddTriangleFilled(ImVec2(c.x + r * 0.95f, c.y - r * 0.10f),
                          ImVec2(c.x,             c.y + r * 0.10f),
                          ImVec2(c.x + r * 0.30f, c.y + r * 0.50f), primary);
    // Body (vertical quad).
    ImVec2 b1(c.x - r * 0.15f, c.y - r * 0.30f);
    ImVec2 b2(c.x + r * 0.15f, c.y - r * 0.30f);
    ImVec2 b3(c.x + r * 0.10f, c.y + r * 0.70f);
    ImVec2 b4(c.x - r * 0.10f, c.y + r * 0.70f);
    dl->AddQuadFilled(b1, b2, b3, b4, primary);
    // Head circle.
    dl->AddCircleFilled(ImVec2(c.x, c.y - r * 0.50f), r * 0.20f, primary, 16);
    // Accent beak.
    dl->AddTriangleFilled(ImVec2(c.x - r * 0.05f, c.y - r * 0.40f),
                          ImVec2(c.x + r * 0.05f, c.y - r * 0.40f),
                          ImVec2(c.x,             c.y - r * 0.20f), accent);
}

void draw_phoenix(ImDrawList* dl, ImVec2 c, float r, ImU32 primary, ImU32 accent) {
    // Flame-bird: two wing-flame triangles sweeping up, with center body
    // and a small head circle. Accent line down the body for plumage.
    // Left wing (curling up).
    dl->AddTriangleFilled(ImVec2(c.x - r * 0.90f, c.y + r * 0.10f),
                          ImVec2(c.x - r * 0.10f, c.y - r * 0.20f),
                          ImVec2(c.x - r * 0.50f, c.y - r * 0.90f), primary);
    // Right wing.
    dl->AddTriangleFilled(ImVec2(c.x + r * 0.90f, c.y + r * 0.10f),
                          ImVec2(c.x + r * 0.10f, c.y - r * 0.20f),
                          ImVec2(c.x + r * 0.50f, c.y - r * 0.90f), primary);
    // Body.
    ImVec2 body[4] = {
        ImVec2(c.x - r * 0.15f, c.y - r * 0.10f),
        ImVec2(c.x + r * 0.15f, c.y - r * 0.10f),
        ImVec2(c.x,             c.y + r * 0.90f),
        ImVec2(c.x,             c.y + r * 0.90f),
    };
    dl->AddConvexPolyFilled(body, 4, primary);
    // Head + accent plume.
    dl->AddCircleFilled(ImVec2(c.x, c.y - r * 0.25f), r * 0.15f, primary, 14);
    dl->AddLine(ImVec2(c.x, c.y - r * 0.10f),
                ImVec2(c.x, c.y + r * 0.85f),
                accent, std::max(1.0f, r * 0.08f));
}

void draw_dragon(ImDrawList* dl, ImVec2 c, float r, ImU32 primary, ImU32 accent) {
    // Serpentine S-shape: 3 stacked half-curves approximated as triangles.
    // Top curl.
    dl->AddTriangleFilled(ImVec2(c.x - r * 0.30f, c.y - r * 0.85f),
                          ImVec2(c.x + r * 0.70f, c.y - r * 0.85f),
                          ImVec2(c.x + r * 0.10f, c.y - r * 0.30f), primary);
    dl->AddTriangleFilled(ImVec2(c.x - r * 0.30f, c.y - r * 0.85f),
                          ImVec2(c.x - r * 0.60f, c.y - r * 0.30f),
                          ImVec2(c.x + r * 0.10f, c.y - r * 0.30f), primary);
    // Middle bar (the S-waist).
    ImVec2 wb[4] = {
        ImVec2(c.x - r * 0.50f, c.y - r * 0.20f),
        ImVec2(c.x + r * 0.50f, c.y - r * 0.20f),
        ImVec2(c.x + r * 0.50f, c.y + r * 0.20f),
        ImVec2(c.x - r * 0.50f, c.y + r * 0.20f),
    };
    dl->AddConvexPolyFilled(wb, 4, primary);
    // Bottom curl (mirror of top).
    dl->AddTriangleFilled(ImVec2(c.x - r * 0.70f, c.y + r * 0.85f),
                          ImVec2(c.x + r * 0.30f, c.y + r * 0.85f),
                          ImVec2(c.x - r * 0.10f, c.y + r * 0.30f), primary);
    dl->AddTriangleFilled(ImVec2(c.x + r * 0.30f, c.y + r * 0.85f),
                          ImVec2(c.x + r * 0.60f, c.y + r * 0.30f),
                          ImVec2(c.x - r * 0.10f, c.y + r * 0.30f), primary);
    // Accent eye dot.
    dl->AddCircleFilled(ImVec2(c.x + r * 0.40f, c.y - r * 0.55f), r * 0.10f, accent, 10);
}

void draw_wave(ImDrawList* dl, ImVec2 c, float r, ImU32 primary, ImU32 accent) {
    // 3 stacked horizontal sine-wave curves, approximated by polylines.
    const int kSegments = 16;
    for (int row = 0; row < 3; ++row) {
        float y0 = c.y - r * 0.50f + row * (r * 0.50f);
        ImVec2 pts[kSegments + 1];
        for (int i = 0; i <= kSegments; ++i) {
            float t = (float)i / (float)kSegments;
            float x = c.x - r * 0.90f + t * (r * 1.80f);
            float y = y0 + std::sin(t * kTau) * (r * 0.15f);
            pts[i] = ImVec2(x, y);
        }
        ImU32 col = (row == 1) ? accent : primary;
        dl->AddPolyline(pts, kSegments + 1, col, 0, std::max(1.0f, r * 0.16f));
    }
}

void draw_mountain(ImDrawList* dl, ImVec2 c, float r, ImU32 primary, ImU32 accent) {
    // 3 triangular peaks. Center taller, sides shorter and overlapping.
    // Left peak.
    dl->AddTriangleFilled(ImVec2(c.x - r * 0.95f, c.y + r * 0.85f),
                          ImVec2(c.x - r * 0.15f, c.y + r * 0.85f),
                          ImVec2(c.x - r * 0.55f, c.y - r * 0.20f), primary);
    // Right peak.
    dl->AddTriangleFilled(ImVec2(c.x + r * 0.15f, c.y + r * 0.85f),
                          ImVec2(c.x + r * 0.95f, c.y + r * 0.85f),
                          ImVec2(c.x + r * 0.55f, c.y - r * 0.20f), primary);
    // Center peak (tallest, drawn last so its snowcap accent sits on top).
    ImVec2 p1(c.x - r * 0.50f, c.y + r * 0.85f);
    ImVec2 p2(c.x + r * 0.50f, c.y + r * 0.85f);
    ImVec2 p3(c.x,             c.y - r * 0.85f);
    dl->AddTriangleFilled(p1, p2, p3, primary);
    // Snowcap (accent) at the tip of the center peak — small triangle.
    dl->AddTriangleFilled(ImVec2(c.x - r * 0.15f, c.y - r * 0.55f),
                          ImVec2(c.x + r * 0.15f, c.y - r * 0.55f),
                          p3, accent);
}

void draw_sun(ImDrawList* dl, ImVec2 c, float r, ImU32 primary, ImU32 accent) {
    // Center disc + 8 short radial rays.
    dl->AddCircleFilled(c, r * 0.45f, primary, 24);
    for (int i = 0; i < 8; ++i) {
        float a = (kTau * i / 8.0f);
        ImVec2 inner = polar(c, r * 0.55f, a);
        ImVec2 outer = polar(c, r * 0.92f, a);
        dl->AddLine(inner, outer, accent, std::max(1.5f, r * 0.14f));
    }
}

void draw_crosshair(ImDrawList* dl, ImVec2 c, float r, ImU32 primary, ImU32 accent) {
    // Sniper reticle: thin outer circle + cross lines + center dot.
    dl->AddCircle(c, r * 0.85f, primary, 24, std::max(1.5f, r * 0.10f));
    dl->AddCircle(c, r * 0.45f, primary, 18, std::max(1.0f, r * 0.06f));
    dl->AddLine(ImVec2(c.x - r * 0.95f, c.y), ImVec2(c.x - r * 0.20f, c.y),
                primary, std::max(1.0f, r * 0.10f));
    dl->AddLine(ImVec2(c.x + r * 0.20f, c.y), ImVec2(c.x + r * 0.95f, c.y),
                primary, std::max(1.0f, r * 0.10f));
    dl->AddLine(ImVec2(c.x, c.y - r * 0.95f), ImVec2(c.x, c.y - r * 0.20f),
                primary, std::max(1.0f, r * 0.10f));
    dl->AddLine(ImVec2(c.x, c.y + r * 0.20f), ImVec2(c.x, c.y + r * 0.95f),
                primary, std::max(1.0f, r * 0.10f));
    // Center accent dot.
    dl->AddCircleFilled(c, r * 0.12f, accent, 10);
}

void draw_sword(ImDrawList* dl, ImVec2 c, float r, ImU32 primary, ImU32 accent) {
    // Vertical sword: thin blade rect + crossguard + pommel.
    // Blade.
    ImVec2 b1(c.x - r * 0.10f, c.y - r * 0.90f);
    ImVec2 b2(c.x + r * 0.10f, c.y - r * 0.90f);
    ImVec2 b3(c.x + r * 0.10f, c.y + r * 0.40f);
    ImVec2 b4(c.x - r * 0.10f, c.y + r * 0.40f);
    dl->AddQuadFilled(b1, b2, b3, b4, primary);
    // Crossguard.
    ImVec2 g1(c.x - r * 0.60f, c.y + r * 0.35f);
    ImVec2 g2(c.x + r * 0.60f, c.y + r * 0.35f);
    ImVec2 g3(c.x + r * 0.60f, c.y + r * 0.55f);
    ImVec2 g4(c.x - r * 0.60f, c.y + r * 0.55f);
    dl->AddQuadFilled(g1, g2, g3, g4, accent);
    // Grip + pommel.
    ImVec2 h1(c.x - r * 0.08f, c.y + r * 0.55f);
    ImVec2 h2(c.x + r * 0.08f, c.y + r * 0.55f);
    ImVec2 h3(c.x + r * 0.08f, c.y + r * 0.80f);
    ImVec2 h4(c.x - r * 0.08f, c.y + r * 0.80f);
    dl->AddQuadFilled(h1, h2, h3, h4, accent);
    dl->AddCircleFilled(ImVec2(c.x, c.y + r * 0.90f), r * 0.12f, accent, 12);
}

void draw_anchor(ImDrawList* dl, ImVec2 c, float r, ImU32 primary, ImU32 accent) {
    // Stylized anchor: ring at top + vertical shaft + crescent base.
    // Top ring.
    dl->AddCircle(ImVec2(c.x, c.y - r * 0.65f), r * 0.18f, primary, 16,
                  std::max(1.5f, r * 0.12f));
    // Shaft.
    ImVec2 s1(c.x - r * 0.08f, c.y - r * 0.45f);
    ImVec2 s2(c.x + r * 0.08f, c.y - r * 0.45f);
    ImVec2 s3(c.x + r * 0.08f, c.y + r * 0.55f);
    ImVec2 s4(c.x - r * 0.08f, c.y + r * 0.55f);
    dl->AddQuadFilled(s1, s2, s3, s4, primary);
    // Crossbar near the top of the shaft.
    ImVec2 c1(c.x - r * 0.45f, c.y - r * 0.30f);
    ImVec2 c2(c.x + r * 0.45f, c.y - r * 0.30f);
    ImVec2 c3(c.x + r * 0.45f, c.y - r * 0.18f);
    ImVec2 c4(c.x - r * 0.45f, c.y - r * 0.18f);
    dl->AddQuadFilled(c1, c2, c3, c4, primary);
    // Crescent base — approximated by 2 triangles flaring outward.
    dl->AddTriangleFilled(ImVec2(c.x - r * 0.10f, c.y + r * 0.40f),
                          ImVec2(c.x - r * 0.70f, c.y + r * 0.40f),
                          ImVec2(c.x - r * 0.45f, c.y + r * 0.90f), primary);
    dl->AddTriangleFilled(ImVec2(c.x + r * 0.10f, c.y + r * 0.40f),
                          ImVec2(c.x + r * 0.70f, c.y + r * 0.40f),
                          ImVec2(c.x + r * 0.45f, c.y + r * 0.90f), primary);
    // Accent inner ring detail.
    dl->AddCircleFilled(ImVec2(c.x, c.y - r * 0.65f), r * 0.08f, accent, 10);
}

void draw_flame(ImDrawList* dl, ImVec2 c, float r, ImU32 primary, ImU32 accent) {
    // Teardrop flame: a stretched triangle with a rounded bottom.
    ImVec2 pts[6] = {
        ImVec2(c.x,             c.y - r * 0.95f),
        ImVec2(c.x + r * 0.45f, c.y - r * 0.30f),
        ImVec2(c.x + r * 0.55f, c.y + r * 0.35f),
        ImVec2(c.x + r * 0.20f, c.y + r * 0.85f),
        ImVec2(c.x - r * 0.20f, c.y + r * 0.85f),
        ImVec2(c.x - r * 0.55f, c.y + r * 0.35f),
    };
    dl->AddConvexPolyFilled(pts, 6, primary);
    // Inner accent flame (smaller, brighter).
    ImVec2 pts2[6] = {
        ImVec2(c.x,             c.y - r * 0.40f),
        ImVec2(c.x + r * 0.22f, c.y - r * 0.10f),
        ImVec2(c.x + r * 0.25f, c.y + r * 0.25f),
        ImVec2(c.x + r * 0.10f, c.y + r * 0.55f),
        ImVec2(c.x - r * 0.10f, c.y + r * 0.55f),
        ImVec2(c.x - r * 0.25f, c.y + r * 0.25f),
    };
    dl->AddConvexPolyFilled(pts2, 6, accent);
}

void draw_skull(ImDrawList* dl, ImVec2 c, float r, ImU32 primary, ImU32 accent) {
    // Stylized skull: oval cranium + small rect jaw + 2 eye holes + nose.
    // Cranium (large oval — approximate with circle).
    dl->AddCircleFilled(ImVec2(c.x, c.y - r * 0.15f), r * 0.72f, primary, 28);
    // Jaw — rectangle below.
    ImVec2 j1(c.x - r * 0.45f, c.y + r * 0.40f);
    ImVec2 j2(c.x + r * 0.45f, c.y + r * 0.40f);
    ImVec2 j3(c.x + r * 0.35f, c.y + r * 0.85f);
    ImVec2 j4(c.x - r * 0.35f, c.y + r * 0.85f);
    dl->AddQuadFilled(j1, j2, j3, j4, primary);
    // Eye holes (accent — dark).
    dl->AddCircleFilled(ImVec2(c.x - r * 0.28f, c.y - r * 0.15f), r * 0.18f, accent, 14);
    dl->AddCircleFilled(ImVec2(c.x + r * 0.28f, c.y - r * 0.15f), r * 0.18f, accent, 14);
    // Nose triangle.
    dl->AddTriangleFilled(ImVec2(c.x,             c.y + r * 0.05f),
                          ImVec2(c.x - r * 0.10f, c.y + r * 0.30f),
                          ImVec2(c.x + r * 0.10f, c.y + r * 0.30f), accent);
}

void draw_compass(ImDrawList* dl, ImVec2 c, float r, ImU32 primary, ImU32 accent) {
    // Compass rose: 4 long cardinal rays + 4 short intercardinal rays.
    // Long rays — diamonds along N/S/E/W.
    auto ray = [&](float angle, float length, ImU32 col) {
        ImVec2 tip = polar(c, length, angle);
        ImVec2 left  = polar(c, r * 0.15f, angle - kPi / 2.0f);
        ImVec2 right = polar(c, r * 0.15f, angle + kPi / 2.0f);
        dl->AddTriangleFilled(c, tip, left, col);
        dl->AddTriangleFilled(c, tip, right, col);
    };
    for (int i = 0; i < 4; ++i) {
        float a = (kTau * i / 4.0f) - kPi / 2.0f;  // N, E, S, W
        ray(a, r * 0.95f, primary);
    }
    for (int i = 0; i < 4; ++i) {
        float a = (kTau * i / 4.0f) - kPi / 4.0f;  // NE, SE, SW, NW
        ray(a, r * 0.55f, accent);
    }
    // Center hub.
    dl->AddCircleFilled(c, r * 0.12f, accent, 12);
}

void draw_tree(ImDrawList* dl, ImVec2 c, float r, ImU32 primary, ImU32 accent) {
    // Pine tree — 3 stacked triangles + a trunk rect at the bottom.
    // Top (smallest).
    dl->AddTriangleFilled(ImVec2(c.x - r * 0.40f, c.y - r * 0.20f),
                          ImVec2(c.x + r * 0.40f, c.y - r * 0.20f),
                          ImVec2(c.x,             c.y - r * 0.90f), primary);
    // Middle.
    dl->AddTriangleFilled(ImVec2(c.x - r * 0.60f, c.y + r * 0.15f),
                          ImVec2(c.x + r * 0.60f, c.y + r * 0.15f),
                          ImVec2(c.x,             c.y - r * 0.55f), primary);
    // Bottom (largest).
    dl->AddTriangleFilled(ImVec2(c.x - r * 0.80f, c.y + r * 0.55f),
                          ImVec2(c.x + r * 0.80f, c.y + r * 0.55f),
                          ImVec2(c.x,             c.y - r * 0.20f), primary);
    // Trunk.
    ImVec2 t1(c.x - r * 0.12f, c.y + r * 0.55f);
    ImVec2 t2(c.x + r * 0.12f, c.y + r * 0.55f);
    ImVec2 t3(c.x + r * 0.12f, c.y + r * 0.95f);
    ImVec2 t4(c.x - r * 0.12f, c.y + r * 0.95f);
    dl->AddQuadFilled(t1, t2, t3, t4, accent);
}

}  // namespace

void draw_team_logo(ImDrawList* dl, ImVec2 center, float radius,
                    std::uint8_t shape_idx,
                    ImU32 color_primary, ImU32 color_accent) {
    if (dl == nullptr || radius <= 0.0f) return;

    // Defensive: anything out-of-range falls back to a Shield. Keeps logo
    // rendering safe across save-file mismatches or enum drift.
    if (shape_idx >= kLogoShapeCount) {
        draw_shield(dl, center, radius, color_primary, color_accent);
        return;
    }

    switch (shape_idx) {
        case  0: draw_shield   (dl, center, radius, color_primary, color_accent); break;
        case  1: draw_hexagon  (dl, center, radius, color_primary, color_accent); break;
        case  2: draw_diamond_logo(dl, center, radius, color_primary, color_accent); break;
        case  3: draw_circle_logo (dl, center, radius, color_primary, color_accent); break;
        case  4: draw_triangle_logo(dl, center, radius, color_primary, color_accent); break;
        case  5: draw_star     (dl, center, radius, color_primary, color_accent); break;
        case  6: draw_lightning(dl, center, radius, color_primary, color_accent); break;
        case  7: draw_crown    (dl, center, radius, color_primary, color_accent); break;
        case  8: draw_wolf     (dl, center, radius, color_primary, color_accent); break;
        case  9: draw_eagle    (dl, center, radius, color_primary, color_accent); break;
        case 10: draw_phoenix  (dl, center, radius, color_primary, color_accent); break;
        case 11: draw_dragon   (dl, center, radius, color_primary, color_accent); break;
        case 12: draw_wave     (dl, center, radius, color_primary, color_accent); break;
        case 13: draw_mountain (dl, center, radius, color_primary, color_accent); break;
        case 14: draw_sun      (dl, center, radius, color_primary, color_accent); break;
        case 15: draw_crosshair(dl, center, radius, color_primary, color_accent); break;
        case 16: draw_sword    (dl, center, radius, color_primary, color_accent); break;
        case 17: draw_anchor   (dl, center, radius, color_primary, color_accent); break;
        case 18: draw_flame    (dl, center, radius, color_primary, color_accent); break;
        case 19: draw_skull    (dl, center, radius, color_primary, color_accent); break;
        case 20: draw_compass  (dl, center, radius, color_primary, color_accent); break;
        case 21: draw_tree     (dl, center, radius, color_primary, color_accent); break;
        default: draw_shield   (dl, center, radius, color_primary, color_accent); break;
    }
}

}  // namespace vlr
