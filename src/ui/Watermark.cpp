#include "Watermark.h"

#include <ctime>
#include <cstdio>

#include "imgui.h"
#include "mxb/Session.h"

namespace mxb {
namespace ui {
namespace {

WatermarkSettings g_settings;

constexpr const char* kBrand = "nemora";

// Degrees in the reference API run clockwise from "east", matching screen space
// with y downwards, so 180->270 is the top-left quarter and 270->360 the
// top-right. ImGui's PathArcTo takes radians on the same convention.
constexpr float kPi = 3.14159265358979323846f;

float Rad(float degrees) { return degrees * (kPi / 180.0f); }

}  // namespace

WatermarkSettings& Watermark() { return g_settings; }

void DrawIndicatorBar(float x, float y, float w, unsigned int col, float edgeH) {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (!dl) return;

    // The foreground draw list inherits its flags from the shared data at
    // NewFrame, which leaves the arcs aliased. Set them explicitly so the
    // rounded corners get ImGui's edge fringe.
    const ImDrawListFlags savedFlags = dl->Flags;
    dl->Flags |= ImDrawListFlags_AntiAliasedLines | ImDrawListFlags_AntiAliasedFill;

    // Reproduces the reference lua exactly:
    //   circle_outline(x+5,   y+8, r,g,b,a, 8, 180, 0.25, 2)
    //   circle_outline(x+w-5, y+8, r,g,b,a, 8, 270, 0.25, 2)
    //   rectangle(x+5, y, w-10, 2, r,g,b,a)
    //   gradient(x-3,   y+8, 2, 4+edge, r,g,b,a -> a=0, vertical)
    //   gradient(x+w+1, y+8, 2, 4+edge, r,g,b,a -> a=0, vertical)
    //
    // Radius 8 centred at y+8 puts both arc crowns exactly on y, which is why
    // the 2px top rail lines up with them.
    constexpr float kRadius = 8.0f;
    constexpr float kThickness = 2.0f;

    dl->PathArcTo(ImVec2(x + 5.0f, y + 8.0f), kRadius, Rad(180.0f), Rad(270.0f), 24);
    dl->PathStroke(col, false, kThickness);

    dl->PathArcTo(ImVec2(x + w - 5.0f, y + 8.0f), kRadius, Rad(270.0f), Rad(360.0f), 24);
    dl->PathStroke(col, false, kThickness);

    dl->AddRectFilled(ImVec2(x + 5.0f, y), ImVec2(x + w - 5.0f, y + kThickness), col);

    const ImU32 clear = col & 0x00FFFFFFu;  // same rgb, zero alpha
    const float tail = 4.0f + edgeH;
    dl->AddRectFilledMultiColor(ImVec2(x - 3.0f, y + 8.0f), ImVec2(x - 1.0f, y + 8.0f + tail), col,
                                col, clear, clear);
    dl->AddRectFilledMultiColor(ImVec2(x + w + 1.0f, y + 8.0f),
                                ImVec2(x + w + 3.0f, y + 8.0f + tail), col, col, clear, clear);

    dl->Flags = savedFlags;
}

void RenderWatermark() {
    if (!g_settings.enabled) return;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (!dl) return;

    // Text layout follows the reference: "<brand> | <name> | [delay: Nms | ]HH:MM:SS",
    // with the delay segment present only while connected.
    char text[192];
    int n = std::snprintf(text, sizeof(text), "%s", kBrand);

    if (g_settings.showName)
        n += std::snprintf(text + n, sizeof(text) - n, " | rider");

    if (g_settings.showFps) {
        const float fps = ImGui::GetIO().Framerate;
        n += std::snprintf(text + n, sizeof(text) - n, " | %d fps", static_cast<int>(fps + 0.5f));
    }

    if (QueryOnline() == Online::Connected)
        n += std::snprintf(text + n, sizeof(text) - n, " | online");

    if (g_settings.showClock) {
        std::time_t t = std::time(nullptr);
        std::tm tm{};
        if (localtime_s(&tm, &t) == 0)
            n += std::snprintf(text + n, sizeof(text) - n, " | %02d:%02d:%02d", tm.tm_hour,
                               tm.tm_min, tm.tm_sec);
    }

    const ImVec2 size = ImGui::CalcTextSize(text);
    const float w = size.x + 8.0f;
    const float x = ImGui::GetIO().DisplaySize.x - w - 10.0f;
    const float y = 10.0f;  // + kBarPitch * index, once anything stacks under it

    const ImU32 accent = ImGui::ColorConvertFloat4ToU32(
        ImVec4(g_settings.color[0], g_settings.color[1], g_settings.color[2], 1.0f));

    dl->AddText(ImVec2(x + 4.0f, y + 4.0f), IM_COL32(255, 255, 255, 255), text);
    DrawIndicatorBar(x, y, w, accent, 2.0f);
}

}  // namespace ui
}  // namespace mxb
