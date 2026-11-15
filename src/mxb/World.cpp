#include "World.h"

#include <cmath>
#include <cstring>

#include "Offsets.h"
#include "Process.h"
#include "Visuals.h"
#include "host/Log.h"

namespace mxb {
namespace {

WorldSettings g_world;

// What night looks like. The world light drops to a tenth and turns
// moonlight-blue; the clear colour and fog go nearly black. Picked to read as
// night without going so dark that the track stops being rideable.
//
// Note the sky dome itself is NOT reached by any of this -- only the light on
// the world, the fog and the clear colour behind the sky. The screen filter is
// what covers the sky, which is why night mode drives it on its own.
constexpr float kNightLightScale   = 0.10f;
constexpr float kNightLightTint[3] = {0.45f, 0.55f, 0.95f};
constexpr float kNightClear[3]     = {0.015f, 0.020f, 0.045f};
constexpr float kNightFog[3]     = {0.020f, 0.030f, 0.070f};

// The main view's projection, captured once per frame by EndViewFrustum. The
// hook and the overlay both run on the render thread, inside the same frame, so
// this needs no synchronisation.
struct CapturedView {
    bool  valid = false;
    float screenMatrix[16] = {};
    float nearPlane = 0.0f;
};
CapturedView g_view;
float g_stockFov = 0.0f;

float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
float Lerp(float a, float b, float t) { return a + (b - a) * t; }

float NightT() {
    return g_world.night ? Clamp01(g_world.nightAmount) : 0.0f;
}

int32_t ReadI32(const void* base, size_t off) {
    return *reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(base) + off);
}

}  // namespace

WorldSettings& World() { return g_world; }

// --- world look ------------------------------------------------------------

void ModulateWorldLight(float rgba[4]) {
    if (!g_world.enabled || !rgba) return;

    const float t = NightT();
    float scale = g_world.lightLevel * Lerp(1.0f, kNightLightScale, t);

    for (int i = 0; i < 3; ++i) {
        const float tint = g_world.tint[i] * Lerp(1.0f, kNightLightTint[i], t);
        rgba[i] = Clamp01(rgba[i] * scale * tint);
    }
    // Alpha is left alone: a light colour's alpha is not a brightness.
}

void ModulateClearColor(float* r, float* g, float* b) {
    if (!g_world.enabled || !r || !g || !b) return;

    float c[3] = {*r, *g, *b};
    if (g_world.overrideClear)
        std::memcpy(c, g_world.clearColor, sizeof(c));

    const float t = NightT();
    for (int i = 0; i < 3; ++i)
        c[i] = Clamp01(Lerp(c[i], kNightClear[i], t) * g_world.tint[i]);

    *r = c[0];
    *g = c[1];
    *b = c[2];
}

void ModulateFogColor(float rgb[3]) {
    if (!g_world.enabled || !rgb) return;

    float c[3] = {rgb[0], rgb[1], rgb[2]};
    if (g_world.overrideFogColor)
        std::memcpy(c, g_world.fogColor, sizeof(c));

    const float t = NightT();
    for (int i = 0; i < 3; ++i)
        rgb[i] = Clamp01(Lerp(c[i], kNightFog[i], t) * g_world.tint[i]);
}

void ModulateFogEnable(int* on) {
    if (!g_world.enabled || !on) return;
    if (g_world.fogMode == 1) *on = 1;
    else if (g_world.fogMode == 2) *on = 0;
}

void ModulateFogDensity(float* density) {
    if (!g_world.enabled || !density) return;
    const float v = *density * g_world.fogDensity;
    if (std::isfinite(v) && v >= 0.0f) *density = v;
}

float ScreenFilter(float rgb[3]) {
    if (!g_world.enabled || !rgb) return 0.0f;

    const float t = NightT();

    // Night mode brings its own veil, so turning it on is enough on its own.
    // Whichever asks for more wins, rather than the two stacking into mud.
    const float manual = Clamp01(g_world.screenTint);
    const float fromNight = 0.35f * t;
    const float strength = manual > fromNight ? manual : fromNight;
    if (!(strength > 0.001f)) return 0.0f;

    for (int i = 0; i < 3; ++i)
        rgb[i] = Clamp01(Lerp(g_world.filterColor[i], kNightClear[i], t));

    return strength;
}

// --- view / FOV ------------------------------------------------------------

ViewHookState BeginViewFrustum(void* view) {
    ViewHookState st;
    if (!view || !IsReadable(view, off::view::kProbe + sizeof(int32_t))) return st;

    // The dirty flag short-circuits the whole function, so a write past it does
    // nothing. Views are stack-allocated per frame, so this is normally zero.
    if (ReadI32(view, off::view::kBuilt) != 0) return st;

    // Everything that is not the player looking at the world excludes itself
    // here. These are the same conditions that decide which branch of
    // View_BuildFrustum runs, so a view that fails any of them would never have
    // consulted camera+4 in the first place -- see NOTES.md.
    if (ReadI32(view, off::view::kMode) != 0) return st;    // 1 reflection, 2 explicit bounds
    if (ReadI32(view, off::view::kOrtho) != 0) return st;
    if (ReadI32(view, off::view::kMirror) != 0) return st;  // mirrored passes and cube faces
    if (ReadI32(view, off::view::kProbe) != 0) return st;   // cube probe

    void* camera = *reinterpret_cast<void* const*>(static_cast<uint8_t*>(view) + off::view::kCamera);
    if (!IsReadable(camera, off::cam::kMatrix + 64)) return st;

    float* fov = reinterpret_cast<float*>(static_cast<uint8_t*>(camera) + off::cam::kFov);
    const float stock = *fov;

    // A plausible FOV is also the check that this really is a camera object. If
    // the field holds anything else, leave the view entirely alone.
    if (!std::isfinite(stock) || stock <= 1.0f || stock >= 179.0f) return st;

    st.main = true;
    g_stockFov = stock;

    const VisualSettings& v = Visuals();
    if (!v.fovOverride) return st;

    const float want = v.fov;
    if (!std::isfinite(want) || want <= 1.0f || want >= 179.0f) return st;

    *fov = want;
    st.camera = camera;
    st.stockFov = stock;
    return st;
}

void EndViewFrustum(void* view, const ViewHookState& state) {
    if (state.camera) {
        *reinterpret_cast<float*>(static_cast<uint8_t*>(state.camera) + off::cam::kFov) =
            state.stockFov;
    }

    if (!state.main || !view) return;
    if (!IsReadable(view, off::view::kScreenMatrix + 64)) return;

    const uint8_t* v = static_cast<const uint8_t*>(view);
    std::memcpy(g_view.screenMatrix, v + off::view::kScreenMatrix, sizeof(g_view.screenMatrix));
    g_view.nearPlane = *reinterpret_cast<const float*>(v + off::view::kNear);

    g_view.valid = std::isfinite(g_view.nearPlane) && g_view.nearPlane > 0.0f &&
                   std::isfinite(g_view.screenMatrix[0]) && g_view.screenMatrix[0] != 0.0f;

    static bool logged = false;
    if (g_view.valid && !logged) {
        logged = true;
        host::Logf("view captured: near=%.4f stockFov=%.1f screenMatrix[0,2,5,6]=%.3f %.3f %.3f %.3f",
             g_view.nearPlane, g_stockFov, g_view.screenMatrix[0], g_view.screenMatrix[2],
             g_view.screenMatrix[5], g_view.screenMatrix[6]);
    }
}

bool HaveView() { return g_view.valid; }
float StockCameraFov() { return g_stockFov; }

bool ProjectWorldToScreen(const float world[3], float screenW, float screenH, float out[2],
                          float* depth) {
    if (!g_view.valid || !world || !out) return false;
    if (!(screenW > 0.0f) || !(screenH > 0.0f)) return false;

    // view+116 is the engine's own world -> window matrix, so this is the same
    // transform the game uses, with the Mat_TransformPoint convention:
    // row-major, translation at float index 3/7/11.
    const float* m = g_view.screenMatrix;
    const float x = world[0], y = world[1], z = world[2];

    const float px = x * m[0] + y * m[1] + z * m[2]  + m[3];
    const float py = x * m[4] + y * m[5] + z * m[6]  + m[7];
    const float pz = x * m[8] + y * m[9] + z * m[10] + m[11];

    if (!std::isfinite(pz) || pz <= g_view.nearPlane) return false;

    // Both come out as fractions of the render target: x left to right, y
    // BOTTOM to top, which is why y is flipped for ImGui.
    const float sx = px / pz;
    const float sy = py / pz;
    if (!std::isfinite(sx) || !std::isfinite(sy)) return false;

    out[0] = sx * screenW;
    out[1] = (1.0f - sy) * screenH;
    if (depth) *depth = pz;
    return true;
}

}  // namespace mxb
