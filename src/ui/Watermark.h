#pragma once

namespace mxb {
namespace ui {

// gamesense-style indicator bar, reproduced from the reference lua's
// `renderer_shit` helper. Drawn on ImGui's foreground draw list, so it shows
// whether or not the menu is open.
struct WatermarkSettings {
    bool  enabled   = true;
    bool  showName  = true;
    bool  showClock = true;
    bool  showFps   = true;
    float color[4]  = {142.f / 255.f, 165.f / 255.f, 229.f / 255.f, 1.0f};  // gamesense default accent
};

WatermarkSettings& Watermark();

// Call once per frame from inside an ImGui frame.
void RenderWatermark();

// Draws just the bar chrome at (x, y) with width w. Exposed because the same
// shape is the house style for any indicator stacked under the watermark.
void DrawIndicatorBar(float x, float y, float w, unsigned int col, float edgeH = 0.0f);

// Height and vertical pitch the reference uses when stacking indicators.
inline constexpr float kBarHeight = 18.0f;
inline constexpr float kBarPitch = 25.0f;

}  // namespace ui
}  // namespace mxb
