#include "Chams.h"

#include <windows.h>

#include <cmath>
#include <cstdint>

#include <GL/gl.h>

#include "Offsets.h"
#include "Process.h"
#include "host/Log.h"

namespace mxb {
namespace {

ChamsSettings g_settings;
ChamsStatus   g_status;    // published, read by the menu
ChamsStatus   g_building;  // accumulated during the frame

float g_time = 0.0f;
bool  g_hookedSetup = false;
bool  g_hookedGl = false;

// The vehicles in the scene, refreshed once per frame from the same collection
// the render loops walk.
//
// This replaces testing entity+268 per mesh, which coloured the track: the
// pointer Mat_SetupMaterial hands us is evidently not the scene entity that
// field belongs to, or type 1 does not mean there what it means in the render
// loop. Membership of a list built from the renderer's own source of truth
// cannot make that mistake -- and when the list is empty, or the pointer is
// something else entirely, nothing is chammed rather than everything.
constexpr int kMaxVehicles = 16;
const void* g_vehicles[kMaxVehicles] = {};
int         g_vehicleCount = 0;
int         g_entityCount = 0;   // everything in the collection
int         g_typeOneCount = 0;  // how many of those are type 1

// Armed by ChamsBegin for the duration of one mesh. The colours are resolved
// once here rather than per glMaterialfv call, because the renderer issues
// several of those per draw.
bool  g_active = false;
float g_base[4] = {0, 0, 0, 1};
float g_emission[4] = {0, 0, 0, 1};
float g_specular[4] = {0, 0, 0, 1};
float g_shininess = 0.0f;

float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

void HsvToRgb(float h, float s, float v, float out[3]) {
    h -= std::floor(h);
    const float i = std::floor(h * 6.0f);
    const float f = h * 6.0f - i;
    const float p = v * (1.0f - s);
    const float q = v * (1.0f - f * s);
    const float t = v * (1.0f - (1.0f - f) * s);
    switch (static_cast<int>(i) % 6) {
        case 0: out[0] = v; out[1] = t; out[2] = p; break;
        case 1: out[0] = q; out[1] = v; out[2] = p; break;
        case 2: out[0] = p; out[1] = v; out[2] = t; break;
        case 3: out[0] = p; out[1] = q; out[2] = v; break;
        case 4: out[0] = t; out[1] = p; out[2] = v; break;
        default: out[0] = v; out[1] = p; out[2] = q; break;
    }
}

// Once per frame, off the hot path, so IsReadable is affordable here.
void RefreshVehicles() {
    g_vehicleCount = 0;
    g_entityCount = 0;
    g_typeOneCount = 0;

    const uintptr_t slot = Rva(off::kEntityCollection);
    if (!slot || !IsReadable(reinterpret_cast<void*>(slot), sizeof(void*))) return;

    const auto* coll = *reinterpret_cast<const uint8_t* const*>(slot);
    if (!IsReadable(coll, sizeof(int32_t))) return;

    const int32_t count = *reinterpret_cast<const int32_t*>(coll);
    if (count < 1 || count > 512) return;
    if (!IsReadable(coll, kEntriesOffset + kEntryStride * static_cast<size_t>(count))) return;
    g_entityCount = count;

    for (int32_t i = 0; i < count && g_vehicleCount < kMaxVehicles; ++i) {
        const auto* entry = coll + kEntriesOffset + kEntryStride * static_cast<size_t>(i);
        const auto* ent = *reinterpret_cast<const uint8_t* const*>(entry + kEntryPtrOffset);
        if (!IsReadable(ent, off::entity::kRider + sizeof(void*))) continue;

        // The game's OWN vehicle predicate, copied whole this time. From
        // Render_ViewPasses:
        //
        //   if (*(_DWORD *)(v8 + 268) == 1 && *(_QWORD *)(v8 + 6392)
        //       && !_bittest((v8 + 272), 8u))
        //
        // An earlier version took only the first third of that and coloured the
        // track and the props, because type 1 does NOT mean "vehicle" by
        // itself -- scenery is type 1 too. What separates a vehicle is owning a
        // rider object at +6392, which scenery leaves null. Bit 8 of the flags
        // is the hidden bit.
        if (*reinterpret_cast<const int32_t*>(ent + off::entity::kType) != 1) continue;
        ++g_typeOneCount;

        if (!*reinterpret_cast<const void* const*>(ent + off::entity::kRider)) continue;
        if (*reinterpret_cast<const int32_t*>(ent + off::entity::kFlags) & (1 << 8)) continue;

        g_vehicles[g_vehicleCount++] = ent;
    }
}

bool IsVehicle(const void* p) {
    for (int i = 0; i < g_vehicleCount; ++i)
        if (g_vehicles[i] == p) return true;
    return false;
}

void Resolve(const ChamsPart& part) {
    float base[3] = {part.color[0], part.color[1], part.color[2]};
    float emis[3] = {0.0f, 0.0f, 0.0f};
    float spec[3] = {0.0f, 0.0f, 0.0f};
    g_shininess = 0.0f;

    switch (part.style) {
        case kChamGlow: {
            const float k = 0.55f + 0.45f * std::sin(g_time * 3.0f);
            for (int i = 0; i < 3; ++i) emis[i] = base[i] * k;
            break;
        }
        case kChamRainbow: {
            float rgb[3];
            HsvToRgb(g_time * 0.20f, 0.85f, 1.0f, rgb);
            for (int i = 0; i < 3; ++i) {
                base[i] = rgb[i];
                emis[i] = rgb[i] * 0.85f;
            }
            break;
        }
        case kChamMetallic:
            for (int i = 0; i < 3; ++i) {
                base[i] *= 0.25f;
                spec[i] = 1.0f;
            }
            g_shininess = 80.0f;
            break;
        case kChamPearl:
            for (int i = 0; i < 3; ++i) {
                spec[i] = base[i] * 0.85f;
                emis[i] = base[i] * 0.15f;
            }
            g_shininess = 24.0f;
            break;
        case kChamFlat:
        default:
            // Unlit: the colour is entirely emission, so it does not shift with
            // the sun or with night mode.
            for (int i = 0; i < 3; ++i) emis[i] = base[i];
            break;
    }

    for (int i = 0; i < 3; ++i) {
        g_base[i] = Clamp01(base[i]);
        g_emission[i] = Clamp01(emis[i]);
        g_specular[i] = Clamp01(spec[i]);
    }
    g_base[3] = Clamp01(part.color[3]);
    g_emission[3] = 1.0f;
    g_specular[3] = 1.0f;
}

}  // namespace

ChamsSettings& Chams() { return g_settings; }
ChamsStatus ChamsState() { return g_status; }

void ChamsSetHooked(bool matSetup, bool glMaterial) {
    g_hookedSetup = matSetup;
    g_hookedGl = glMaterial;
}

void ChamsTick() {
    g_time = static_cast<float>(GetTickCount64() % 100000u) * 0.001f;
    RefreshVehicles();

    g_status = g_building;
    g_status.hooked = g_hookedSetup;
    g_status.glHooked = g_hookedGl;
    g_status.sceneEntities = g_entityCount;
    g_status.typeOneEntities = g_typeOneCount;
    g_status.vehicleEntities = g_vehicleCount;
    g_building = ChamsStatus{};

    // Once a second while enabled, so a report can be read out of the log
    // instead of off the menu. Cheap: this is per frame, not per mesh.
    if (g_settings.rider.enabled || g_settings.bike.enabled) {
        static DWORD lastLog = 0;
        const DWORD now = GetTickCount();
        if (now - lastLog >= 1000) {
            lastLog = now;
            host::Logf("chams: entities=%d type1=%d vehicles=%d | meshes=%d rider=%d/%d "
                       "bike=%d/%d matWrites=%d hooks=%d/%d  v0=%p v1=%p",
                       g_entityCount, g_typeOneCount, g_status.vehicleEntities,
                       g_status.totalMeshes, g_status.riderDraws, g_status.riderMeshes,
                       g_status.bikeDraws, g_status.vehicleMeshes, g_status.materialCalls,
                       g_hookedSetup ? 1 : 0, g_hookedGl ? 1 : 0,
                       g_vehicleCount > 0 ? g_vehicles[0] : nullptr,
                       g_vehicleCount > 1 ? g_vehicles[1] : nullptr);
        }
    }
}

bool ChamsBegin(const void* entity, bool fromRiderRenderer) {
    g_active = false;
    if (!g_settings.rider.enabled && !g_settings.bike.enabled) return false;

    ++g_building.totalMeshes;

    // EVERY path checks the vehicle list, including the rider.
    //
    // Both callers really do pass the entity -- Render_SceneModels loads it with
    // `mov rbx, [rsi]` and then reads light colour from [rbx+27Ch], the same
    // field Render_RiderObject reads off its entity argument -- so the check is
    // meaningful on both. An earlier version exempted the rider on the grounds
    // that its return address was identification enough, which left one path
    // able to colour anything if that range were ever wrong. Colouring the
    // world is the failure that keeps happening, so it is worth making it
    // impossible by construction rather than by argument.
    if (!entity || !IsVehicle(entity)) return false;

    const bool isRider = g_settings.swapParts ? !fromRiderRenderer : fromRiderRenderer;
    if (fromRiderRenderer) ++g_building.riderMeshes;
    else                   ++g_building.vehicleMeshes;

    const ChamsPart& part = isRider ? g_settings.rider : g_settings.bike;
    if (!part.enabled) return false;

    Resolve(part);
    g_active = true;

    if (isRider) ++g_building.riderDraws;
    else ++g_building.bikeDraws;
    return true;
}

void ChamsEnd() { g_active = false; }

bool ChamsMaterialfv(unsigned pname, float out[4]) {
    if (!g_active) return false;
    const float* src = nullptr;
    switch (pname) {
        case GL_AMBIENT:
        case GL_DIFFUSE:
        case GL_AMBIENT_AND_DIFFUSE: src = g_base; break;
        case GL_EMISSION:            src = g_emission; break;
        case GL_SPECULAR:            src = g_specular; break;
        default: return false;
    }
    for (int i = 0; i < 4; ++i) out[i] = src[i];
    ++g_building.materialCalls;
    return true;
}

bool ChamsMaterialf(unsigned pname, float* out) {
    if (!g_active || pname != GL_SHININESS || !out) return false;
    *out = g_shininess;
    return true;
}

bool ChamsSuppressTexture() { return g_active && g_settings.forceFlat; }

}  // namespace mxb
