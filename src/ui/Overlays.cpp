#include "Overlays.h"

#include <cmath>

#include "imgui.h"
#include "mxb/BikeData.h"
#include "mxb/Process.h"
#include "mxb/World.h"

namespace mxb {
namespace ui {
namespace {

ChinaHatSettings g_hat;

// Enough that the cone reads as round at close range without spending vertices
// on something that is usually a few dozen pixels across.
constexpr int kSegments = 20;

ImU32 Hsv(float h, float s, float v, float a) {
    float r = 0.0f, g = 0.0f, b = 0.0f;
    ImGui::ColorConvertHSVtoRGB(h - std::floor(h), s, v, r, g, b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, a));
}

// The colour of one cone face, given where it sits around the hat.
ImU32 FaceColor(float t, float around) {
    const float a = g_hat.color[3];
    switch (g_hat.style) {
        case 1:  // Rainbow: the whole hat cycles together
            return Hsv(t * 0.25f, 0.70f, 1.0f, a);
        case 2: {  // Pulse: brightness breathes
            const float k = 0.55f + 0.45f * std::sin(t * 3.0f);
            return ImGui::ColorConvertFloat4ToU32(
                ImVec4(g_hat.color[0] * k, g_hat.color[1] * k, g_hat.color[2] * k, a));
        }
        case 3:  // Hue sweep: hue runs around the cone, so it looks like it spins
            return Hsv(t * 0.35f + around, 0.75f, 1.0f, a);
        default:
            return ImGui::ColorConvertFloat4ToU32(
                ImVec4(g_hat.color[0], g_hat.color[1], g_hat.color[2], a));
    }
}

void DrawChinaHats() {
    if (!g_hat.enabled || !HaveView()) return;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (!dl) return;

    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    if (!(disp.x > 0.0f) || !(disp.y > 0.0f)) return;

    // The foreground list takes its flags from the shared data at NewFrame,
    // which leaves them off; without this the cone edges are visibly stepped.
    const ImDrawListFlags saved = dl->Flags;
    dl->Flags |= ImDrawListFlags_AntiAliasedLines | ImDrawListFlags_AntiAliasedFill;

    const float t = static_cast<float>(ImGui::GetTime());

    for (BikeData& bike : LoadedBikes()) {
        float pos[3];
        bike.worldPos(pos);
        if (!std::isfinite(pos[0]) || !std::isfinite(pos[1]) || !std::isfinite(pos[2])) continue;

        // World up is +Y. The anchor is the wheel midpoint, so the height is
        // measured from roughly axle level rather than from the bike's origin.
        const float base[3] = {pos[0], pos[1] + g_hat.height, pos[2]};
        const float tip[3]  = {base[0], base[1] + g_hat.coneHeight, base[2]};

        float apex[2];
        float apexDepth = 0.0f;
        if (!ProjectWorldToScreen(tip, disp.x, disp.y, apex, &apexDepth)) continue;

        ImVec2 ring[kSegments];
        bool   ok = true;
        for (int i = 0; i < kSegments && ok; ++i) {
            const float a = 6.28318531f * static_cast<float>(i) / static_cast<float>(kSegments);
            const float p[3] = {base[0] + std::cos(a) * g_hat.radius, base[1],
                                base[2] + std::sin(a) * g_hat.radius};
            float s[2];
            // Rejecting the whole hat if any point falls behind the near plane
            // is cheaper and steadier than clipping it: at that distance the
            // rider fills the screen and a half-clipped cone just flickers.
            ok = ProjectWorldToScreen(p, disp.x, disp.y, s, nullptr);
            ring[i] = ImVec2(s[0], s[1]);
        }
        if (!ok) continue;

        // Degenerate or absurd sizes mean the projection is not telling us
        // anything useful; skip rather than smear a triangle across the screen.
        float minX = ring[0].x, maxX = ring[0].x;
        for (int i = 1; i < kSegments; ++i) {
            if (ring[i].x < minX) minX = ring[i].x;
            if (ring[i].x > maxX) maxX = ring[i].x;
        }
        const float span = maxX - minX;
        if (!(span > 1.5f) || span > disp.x * 4.0f) continue;

        const ImVec2 apexPt(apex[0], apex[1]);
        for (int i = 0; i < kSegments; ++i) {
            const ImVec2& a = ring[i];
            const ImVec2& b = ring[(i + 1) % kSegments];
            const float around = static_cast<float>(i) / static_cast<float>(kSegments);
            dl->AddTriangleFilled(apexPt, a, b, FaceColor(t, around));
        }

        if (g_hat.outline) {
            const ImU32 edge = FaceColor(t, 0.0f) | 0xFF000000u;
            dl->AddPolyline(ring, kSegments, edge, true, 1.0f);
        }
    }

    dl->Flags = saved;
}

// A flat veil over the finished frame. The lighting interceptors only reach what
// the sun lights, so this is what catches the sky texture and anything unlit --
// the difference between "the world is darker" and "it is night". Background
// draw list, so the menu still sits on top of it.
void DrawScreenFilter() {
    float rgb[3] = {0.0f, 0.0f, 0.0f};
    const float strength = ScreenFilter(rgb);
    if (!(strength > 0.0f)) return;

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    if (!dl) return;

    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    if (!(disp.x > 0.0f) || !(disp.y > 0.0f)) return;

    dl->AddRectFilled(ImVec2(0.0f, 0.0f), disp,
                      ImGui::ColorConvertFloat4ToU32(ImVec4(rgb[0], rgb[1], rgb[2], strength)));
}

}  // namespace

ChinaHatSettings& ChinaHat() { return g_hat; }

void DrawWorldOverlays() {
    DrawScreenFilter();
    DrawChinaHats();
}

}  // namespace ui
}  // namespace mxb
