#include "Hooks.h"

#include <windows.h>

#include <MinHook.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <intrin.h>

#include "Log.h"
#include "Overlay.h"
#include "imgui.h"
#include "mxb/Offsets.h"
#include "mxb/Process.h"
#include "mxb/Diagnostics.h"
#include "mxb/StockBikeView.h"
#include "mxb/Spinny.h"
#include "mxb/NetworkRam.h"
#include "mxb/GuidSpoofer.h"
#include "mxb/Chams.h"
#include "mxb/Visuals.h"
#include "mxb/CleanLens.h"
#include "mxb/World.h"

#include <GL/gl.h>

// From imgui_impl_win32.cpp in the menu's ImGui fork. Its header deliberately
// comments this out to avoid pulling in <windows.h> types, so it is declared here.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam,
                                                             LPARAM lParam);

namespace mxb {
namespace host {
namespace {

using SwapBuffersFn  = BOOL(WINAPI*)(HDC);
using CollectKbdFn   = void(__fastcall*)(int msg, uintptr_t wParam);
using CollectMouseFn = int64_t(__fastcall*)(int msg, int wParam, int lParam);

// View_BuildFrustum(view). Returns a pointer we neither use nor need.
using BuildFrustumFn = void*(__fastcall*)(void* view);
using RenderVisorEffectsFn = int64_t(__fastcall*)(void* view);

// Mat_SetupMaterial(a1, a2, a3, a4, a5) -- five integer arguments, no floats,
// so the convention is unambiguous.
using MaterialSetupFn = void(__fastcall*)(void** a1, void* a2, void* a3, int a4, int a5);
using MaterialfvFn    = void(WINAPI*)(GLenum face, GLenum pname, const GLfloat* params);
using MaterialfFn     = void(WINAPI*)(GLenum face, GLenum pname, GLfloat param);
using BindTextureFn   = void(WINAPI*)(GLenum target, GLuint texture);

// The engine's own world-look setters. Each caches into its GL state block and
// then calls GL, so intercepting here rewrites the value as it is applied.
using SetClearColorFn = int64_t(__fastcall*)(float r, float g, float b);
using SetFogColorFn   = int64_t(__fastcall*)(float* rgb);
using SetFogEnableFn  = int64_t(__fastcall*)(int on);
using FogfFn          = void(WINAPI*)(GLenum pname, GLfloat param);
using LightfvFn       = void(WINAPI*)(GLenum light, GLenum pname, const GLfloat* params);
using ClientSummaryFn = int64_t(__fastcall*)();
using BikeChecksumFn = int64_t(__fastcall*)(void* bike, int useTemplate, uint32_t* checksum, int32_t* flags);
using SerializeStateFn = int64_t(__fastcall*)(void* outgoing,const void* snapshot);
using BeginPlayerConnectFn = int64_t(__fastcall*)(void* connection, const void* payload, int bytes, void* callback);
using PlayerInfoFn = int64_t(__fastcall*)(const void* payload, int bytes);

SwapBuffersFn   g_origSwapBuffers = nullptr;
CollectKbdFn    g_origCollectKbd = nullptr;
CollectMouseFn  g_origCollectMouse = nullptr;
BuildFrustumFn  g_origBuildFrustum = nullptr;
RenderVisorEffectsFn g_origRenderVisorEffects = nullptr;
MaterialSetupFn g_origMaterialSetup = nullptr;
MaterialfvFn    g_origMaterialfv = nullptr;
MaterialfFn     g_origMaterialf = nullptr;
BindTextureFn   g_origBindTexture = nullptr;
SetClearColorFn g_origSetClearColor = nullptr;
SetFogColorFn   g_origSetFogColor = nullptr;
SetFogEnableFn  g_origSetFogEnable = nullptr;
FogfFn          g_origFogf = nullptr;
LightfvFn       g_origLightfv = nullptr;
ClientSummaryFn g_origClientSummary = nullptr;
BikeChecksumFn g_origBikeChecksum = nullptr;
SerializeStateFn g_origSerializeState = nullptr;
BeginPlayerConnectFn g_origBeginPlayerConnect = nullptr;
PlayerInfoFn g_origSetPlayerInfo = nullptr, g_origSendPlayerInfo = nullptr;
thread_local unsigned g_inClientSummary = 0;
std::atomic<bool> g_verificationReady{false};
std::atomic<unsigned> g_stockReplies{0}, g_stockUnavailable{0};
std::atomic<const char*> g_lastStockFailure{"none"};
std::atomic<unsigned> g_lastActualChecksum{0}, g_lastStockChecksum{0};

WNDPROC g_origWndProc = nullptr;
HWND    g_window = nullptr;

std::atomic<bool> g_installed{false};
std::atomic<bool> g_shuttingDown{false};
std::atomic<bool> g_presented{false};
std::atomic<bool> g_safeToUnload{true};
// Counts threads currently inside our callbacks, so Remove() can wait them out
// instead of pulling the DLL out from under a live frame.
std::atomic<int>  g_inCallback{0};

struct Scope {
    Scope() { g_inCallback.fetch_add(1, std::memory_order_acq_rel); }
    ~Scope() { g_inCallback.fetch_sub(1, std::memory_order_acq_rel); }
};

int64_t __fastcall HookedBeginPlayerConnect(void* connection, const void* payload, int bytes, void* callback) {
    Scope scope;
    guid::Payload copy{};
    const auto action = g_shuttingDown.load(std::memory_order_acquire) ? guid::RewriteResult::PassThrough : guid::RewriteCopy(payload, bytes, copy);
    if (action == guid::RewriteResult::Blocked) return 1;
    return g_origBeginPlayerConnect(connection, action == guid::RewriteResult::Replaced ? copy.data() : payload, bytes, callback);
}
int64_t __fastcall HookedSetPlayerInfo(const void* payload, int bytes) {
    Scope scope;
    guid::Payload copy{};
    const auto action = g_shuttingDown.load(std::memory_order_acquire) ? guid::RewriteResult::PassThrough : guid::RewriteCopy(payload, bytes, copy);
    if (action == guid::RewriteResult::Blocked) return 1;
    return g_origSetPlayerInfo(action == guid::RewriteResult::Replaced ? copy.data() : payload, bytes);
}
int64_t __fastcall HookedSendPlayerInfo(const void* payload, int bytes) {
    Scope scope;
    guid::Payload copy{};
    const auto action = g_shuttingDown.load(std::memory_order_acquire) ? guid::RewriteResult::PassThrough : guid::RewriteCopy(payload, bytes, copy);
    if (action == guid::RewriteResult::Blocked) return 1;
    return g_origSendPlayerInfo(action == guid::RewriteResult::Replaced ? copy.data() : payload, bytes);
}

int64_t __fastcall HookedSerializeState(void* outgoing,const void* snapshot) {
    const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    Scope scope;
    const int64_t result=g_origSerializeState(outgoing,snapshot);
    // This serializer can be reused elsewhere. Only the local multiplayer
    // client's verified call site gets the synthetic outgoing pose.
    if(!g_shuttingDown.load(std::memory_order_acquire) && result==0 && caller==Rva(0x71A37)) {
        if (networkram::GetStatus().enabled) networkram::OnSerialized(outgoing,snapshot);
        else spinny::OnSerialized(outgoing,snapshot);
    }
    return result;
}

int64_t __fastcall HookedClientSummary() {
    Scope scope;
    if (g_shuttingDown.load(std::memory_order_acquire)) return g_origClientSummary();
    struct SummaryScope {
        SummaryScope() { ++g_inClientSummary; }
        ~SummaryScope() { --g_inClientSummary; }
    } summary;
    return g_origClientSummary();
}

int64_t __fastcall HookedBikeChecksum(void* bike, int useTemplate, uint32_t* checksum, int32_t* flags) {
    Scope scope;
    if (g_shuttingDown.load(std::memory_order_acquire) || !g_inClientSummary || useTemplate || !checksum)
        return g_origBikeChecksum(bike, useTemplate, checksum, flags);

    StockBikeView stock;
    const char* failure = "none";
    if (!BuildStockBikeView(bike, stock, &failure)) {
        g_lastStockFailure.store(failure, std::memory_order_relaxed);
        const unsigned misses = g_stockUnavailable.fetch_add(1, std::memory_order_relaxed) + 1;
        Logf("verification: stock view unavailable attempt=%u bike=%p reason=%s; using actual values", misses, bike, failure);
        return g_origBikeChecksum(bike, useTemplate, checksum, flags);
    }
    // Preserve the original result and live flag calculation. Only the bike's
    // checksum contribution is replaced, during this response callback alone.
    const int64_t result = g_origBikeChecksum(bike, useTemplate, checksum, flags);
    if (result != 0) return result;
    const uint32_t actual = *checksum;
    uint32_t pristine = 0;
    if (g_origBikeChecksum(stock.data(), 0, &pristine, nullptr) != 0) {
        g_lastStockFailure.store("game_checksum_failed", std::memory_order_relaxed);
        g_stockUnavailable.fetch_add(1, std::memory_order_relaxed);
        return result;
    }
    *checksum = pristine;
    g_lastStockFailure.store("none", std::memory_order_relaxed);
    g_lastActualChecksum.store(actual, std::memory_order_relaxed);
    g_lastStockChecksum.store(pristine, std::memory_order_relaxed);
    const unsigned count = g_stockReplies.fetch_add(1, std::memory_order_relaxed) + 1;
    Logf("verification: stock bike contribution reply=%u bike=%p actual=%u stock=%u", count, bike, actual, pristine);
    return result;
}

// --- input isolation -----------------------------------------------------
//
// App_WndProc feeds every message to these two collectors and then always falls
// through to DefWindowProcA. Starving the collectors takes input away from the
// game's own UI, and unlike swallowing messages in the window proc it leaves the
// message pump completely untouched -- which matters, because App_WinMainLoop
// only advances a frame when the queue is empty.
//
// Gated on Overlay::IsInteractive(), not IsOpen(): if the overlay is not actually
// drawing, blocking input would leave the game unresponsive with nothing on
// screen to explain it, which is indistinguishable from a freeze.

void __fastcall HookedCollectKbd(int msg, uintptr_t wParam) {
    if (!g_shuttingDown.load(std::memory_order_acquire) && Overlay::IsInteractive()) return;
    g_origCollectKbd(msg, wParam);
}

int64_t __fastcall HookedCollectMouse(int msg, int wParam, int lParam) {
    if (!g_shuttingDown.load(std::memory_order_acquire) && Overlay::IsInteractive()) return 0;
    return g_origCollectMouse(msg, wParam, lParam);
}

// FOV override.
//
// View_BuildFrustum is the one place a field of view becomes geometry: from the
// FOV at camera+4 it derives the near-plane extents, the far-plane extents, the
// screen projection matrix, the eight frustum corners and the six CULLING
// planes. Overriding camera+4 around it keeps all of those consistent.
//
// The previous version hooked glFrustum and rescaled its planes instead, which
// widened the projection and nothing else -- so geometry only visible in the new
// edges was still culled against the stock frustum, and two of the three
// main-view glFrustum calls were never touched at all. That is what made the
// periphery break up at high FOV.
//
// All the scoping lives in BeginViewFrustum, which reads the view object; no
// call-site or return-address filtering is needed.
void* __fastcall HookedBuildFrustum(void* view) {
    if (g_shuttingDown.load(std::memory_order_acquire)) return g_origBuildFrustum(view);

    Scope scope;
    const ViewHookState st = BeginViewFrustum(view);
    void* result = g_origBuildFrustum(view);
    EndViewFrustum(view, st);
    return result;
}

int64_t __fastcall HookedRenderVisorEffects(void* view) {
    Scope scope;
    if (g_shuttingDown.load(std::memory_order_acquire)) return g_origRenderVisorEffects(view);
    if (cleanlens::ShouldSkipDraw()) return 0;
    return g_origRenderVisorEffects(view);
}

// Chams.
//
// Mat_SetupMaterial is not a pre-draw state setter, it IS the draw call: it
// carries every glMaterialfv in the renderer and increments per-draw statistics
// as it goes, including a texture-switch counter and a realloc-grown table of
// unique texture ids. An earlier version set the material AFTER calling the
// original and nothing appeared on screen, because by then the mesh was drawn.
//
// So the game's own material calls are rewritten as it makes them: arm the
// substitution, let the original run, disarm. The GL hooks below do the work.
//
// Rider and bike are told apart by return address. Mat_SetupMaterial has only
// two callers, and Render_RiderObject is invoked only for a type-1 entity's
// +6392 object, so no mesh classification is needed.
//
// This runs once per MESH, hundreds of times a frame. An earlier version called
// IsReadable here -- a VirtualQuery -- and the two kernel transitions per mesh
// cost most of the frame rate. Nothing on this path may syscall, allocate or
// lock, and no pointer needs validating: the renderer has already dereferenced
// everything touched here.
void __fastcall HookedMaterialSetup(void** a1, void* a2, void* a3, int a4, int a5) {
    if (g_shuttingDown.load(std::memory_order_acquire) || !a1) {
        g_origMaterialSetup(a1, a2, a3, a4, a5);
        return;
    }

    // Computed once; Rva goes through a cached base but the bounds only need
    // working out on the first mesh.
    static uintptr_t s_lo = 0, s_hi = 0;
    if (!s_lo) {
        s_lo = Rva(off::kRiderRenderBegin);
        s_hi = Rva(off::kRiderRenderEnd);
    }

    const auto ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
    const bool armed = ChamsBegin(*a1, s_lo && ret >= s_lo && ret < s_hi);

    g_origMaterialSetup(a1, a2, a3, a4, a5);

    if (armed) ChamsEnd();
}

void WINAPI HookedMaterialfv(GLenum face, GLenum pname, const GLfloat* params) {
    GLfloat repl[4];
    if (ChamsMaterialfv(pname, repl)) {
        // Both faces: a chammed mesh showing its stock colour from the inside
        // looks like the feature half-working.
        g_origMaterialfv(GL_FRONT_AND_BACK, pname, repl);
        return;
    }
    g_origMaterialfv(face, pname, params);
}

void WINAPI HookedMaterialf(GLenum face, GLenum pname, GLfloat param) {
    GLfloat repl = param;
    if (ChamsMaterialf(pname, &repl)) {
        g_origMaterialf(GL_FRONT_AND_BACK, pname, repl);
        return;
    }
    g_origMaterialf(face, pname, param);
}

// Unbinding the texture is what makes a cham flat rather than a faint tint:
// material alone still loses to a shader that samples the skin and multiplies.
void WINAPI HookedBindTexture(GLenum target, GLuint texture) {
    if (target == GL_TEXTURE_2D && ChamsSuppressTexture()) {
        g_origBindTexture(target, 0);
        return;
    }
    g_origBindTexture(target, texture);
}

// --- world look ----------------------------------------------------------
//
// Four interceptors on applied values. Nothing is saved or restored: the engine
// still holds the stock value, we only change what reaches GL, so switching the
// modifier off restores the stock look on the very next frame.

int64_t __fastcall HookedSetClearColor(float r, float g, float b) {
    if (!g_shuttingDown.load(std::memory_order_acquire)) ModulateClearColor(&r, &g, &b);
    return g_origSetClearColor(r, g, b);
}

int64_t __fastcall HookedSetFogColor(float* rgb) {
    if (g_shuttingDown.load(std::memory_order_acquire) || !rgb)
        return g_origSetFogColor(rgb);

    // Copied out first: the array belongs to the caller's stack frame and the
    // engine reads it again after this returns.
    float local[3] = {rgb[0], rgb[1], rgb[2]};
    ModulateFogColor(local);
    return g_origSetFogColor(local);
}

int64_t __fastcall HookedSetFogEnable(int on) {
    if (!g_shuttingDown.load(std::memory_order_acquire)) ModulateFogEnable(&on);
    return g_origSetFogEnable(on);
}

void WINAPI HookedFogf(GLenum pname, GLfloat param) {
    if (!g_shuttingDown.load(std::memory_order_acquire) && pname == GL_FOG_DENSITY)
        ModulateFogDensity(&param);
    g_origFogf(pname, param);
}

// The engine drives GL_LIGHT0's AMBIENT term (glLightfv(0x4000, 0x1200, ...)),
// not its diffuse -- 0x1200 is GL_AMBIENT, and GL_LIGHT_MODEL_AMBIENT is left at
// {0,0,0,1}, so LIGHT0's ambient is where the world's light level actually
// lives. Diffuse is handled too, since modulating both is what "sun brightness"
// means and it costs nothing if the engine never sets it.
void WINAPI HookedLightfv(GLenum light, GLenum pname, const GLfloat* params) {
    if (!g_shuttingDown.load(std::memory_order_acquire) && params && light == GL_LIGHT0 &&
        (pname == GL_AMBIENT || pname == GL_DIFFUSE)) {
        float local[4] = {params[0], params[1], params[2], params[3]};
        ModulateWorldLight(local);
        g_origLightfv(light, pname, local);
        return;
    }
    g_origLightfv(light, pname, params);
}

LRESULT CALLBACK HookedWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_shuttingDown.load(std::memory_order_acquire))
        return CallWindowProcA(g_origWndProc, hWnd, msg, wParam, lParam);

    Scope scope;

    if (Overlay::Ready()) {
        // Bit 30 of lParam is the previous key state; ignore auto-repeat so
        // holding the key does not toggle over and over.
        if (msg == WM_KEYDOWN && wParam == VK_INSERT && (lParam & (1 << 30)) == 0) {
            Overlay::Toggle();
            // Still forwarded below. We deliberately do not return early: the
            // game's loop gates frame advancement on an empty message queue and
            // its window proc always ends in DefWindowProcA, so short-circuiting
            // here is what froze the game until the menu was closed again.
        }
        if (msg == WM_KEYDOWN && wParam == VK_F7 && (lParam & (1 << 30)) == 0)
            diagnostics::Capture();
        ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
    }

    // Always forward. Input isolation happens in the collector hooks instead.
    return CallWindowProcA(g_origWndProc, hWnd, msg, wParam, lParam);
}

BOOL WINAPI HookedSwapBuffers(HDC hdc) {
    if (g_shuttingDown.load(std::memory_order_acquire)) return g_origSwapBuffers(hdc);

    Scope scope;
    g_presented.store(true, std::memory_order_release);

    // Bind to the window the game itself recorded in App_CreateWindow rather
    // than to whatever WindowFromDC reports: the engine also makes a throwaway
    // window while probing pixel formats, and latching onto that one would leave
    // us subclassing a dead window and skipping every real present.
    HWND fromDc = WindowFromDC(hdc);
    HWND owned = nullptr;
    if (const uintptr_t slot = Rva(off::kWindowHandle);
        IsReadable(reinterpret_cast<void*>(slot), sizeof(HWND)))
        owned = *reinterpret_cast<HWND*>(slot);

    HWND wnd = (owned && IsWindow(owned)) ? owned : fromDc;

    if (wnd && !g_window) {
        g_window = wnd;
        g_origWndProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrA(wnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HookedWndProc)));
        RECT rc{};
        GetClientRect(wnd, &rc);
        Logf("first present: bound hwnd=%p (game=%p fromDC=%p) client=%ldx%ld, subclassed",
             static_cast<void*>(wnd), static_cast<void*>(owned), static_cast<void*>(fromDc),
             rc.right - rc.left, rc.bottom - rc.top);
        Overlay::AttachWindow(wnd);
    }

    Overlay::Frame();

    return g_origSwapBuffers(hdc);
}

// Creates and enables one hook, logging what happened.
bool Hook(void* target, void* detour, void** original, const char* what) {
    if (!target) {
        Logf("hook %s: target not resolved", what);
        return false;
    }
    if (MH_CreateHook(target, detour, original) != MH_OK) {
        Logf("hook %s: MH_CreateHook failed", what);
        return false;
    }
    if (MH_EnableHook(target) != MH_OK) {
        Logf("hook %s: MH_EnableHook failed", what);
        MH_RemoveHook(target);
        return false;
    }
    Logf("hook %s: ok (%p)", what, target);
    return true;
}

}  // namespace

bool IsInstalled() { return g_installed.load(std::memory_order_acquire); }
bool HasPresented() { return g_presented.load(std::memory_order_acquire); }
VerificationStatus GetVerificationStatus() {
    return {g_verificationReady.load(std::memory_order_acquire),
            g_stockReplies.load(std::memory_order_relaxed), g_stockUnavailable.load(std::memory_order_relaxed),
            g_lastStockFailure.load(std::memory_order_relaxed), g_lastActualChecksum.load(std::memory_order_relaxed),
            g_lastStockChecksum.load(std::memory_order_relaxed)};
}

bool Install() {
    if (g_installed.load(std::memory_order_acquire)) return true;

    if (MH_Initialize() != MH_OK) {
        Logf("MH_Initialize failed");
        return false;
    }

    HMODULE gdi = GetModuleHandleA("gdi32.dll");
    void* swap = gdi ? reinterpret_cast<void*>(GetProcAddress(gdi, "SwapBuffers")) : nullptr;
    if (!Hook(swap, reinterpret_cast<void*>(&HookedSwapBuffers),
              reinterpret_cast<void**>(&g_origSwapBuffers), "gdi32!SwapBuffers")) {
        MH_Uninitialize();
        return false;
    }

    // FOV override. Best-effort: without it the view is simply stock.
    Hook(reinterpret_cast<void*>(Rva(off::kViewBuildFrustum)),
         reinterpret_cast<void*>(&HookedBuildFrustum),
         reinterpret_cast<void**>(&g_origBuildFrustum), "View_BuildFrustum");

    // Chams. Best-effort: without it the models just draw normally.
    const bool matSetup = Hook(reinterpret_cast<void*>(Rva(off::kMaterialSetup)),
                               reinterpret_cast<void*>(&HookedMaterialSetup),
                               reinterpret_cast<void**>(&g_origMaterialSetup), "Mat_SetupMaterial");

    // The substitution itself. Without these the setup hook arms something that
    // never fires, so the two are reported separately in the menu.
    bool glMat = false;
    if (HMODULE glm = GetModuleHandleA("opengl32.dll")) {
        glMat = Hook(reinterpret_cast<void*>(GetProcAddress(glm, "glMaterialfv")),
                     reinterpret_cast<void*>(&HookedMaterialfv),
                     reinterpret_cast<void**>(&g_origMaterialfv), "opengl32!glMaterialfv");
        Hook(reinterpret_cast<void*>(GetProcAddress(glm, "glMaterialf")),
             reinterpret_cast<void*>(&HookedMaterialf),
             reinterpret_cast<void**>(&g_origMaterialf), "opengl32!glMaterialf");
        Hook(reinterpret_cast<void*>(GetProcAddress(glm, "glBindTexture")),
             reinterpret_cast<void*>(&HookedBindTexture),
             reinterpret_cast<void**>(&g_origBindTexture), "opengl32!glBindTexture");
    }
    ChamsSetHooked(matSetup, glMat);

    // World look.
    Hook(reinterpret_cast<void*>(Rva(off::kGlSetClearColor)),
         reinterpret_cast<void*>(&HookedSetClearColor),
         reinterpret_cast<void**>(&g_origSetClearColor), "GL_SetClearColor");
    Hook(reinterpret_cast<void*>(Rva(off::kGlSetFogColor)),
         reinterpret_cast<void*>(&HookedSetFogColor),
         reinterpret_cast<void**>(&g_origSetFogColor), "GL_SetFogColor");
    Hook(reinterpret_cast<void*>(Rva(off::kGlSetFogEnable)),
         reinterpret_cast<void*>(&HookedSetFogEnable),
         reinterpret_cast<void**>(&g_origSetFogEnable), "GL_SetFogEnable");

    // ReShade is installed as opengl32.dll in the game folder, so these resolve
    // to its wrappers rather than the system DLL. That is fine -- we sit above
    // it either way -- but it is why a missing export has to be survivable.
    if (HMODULE gl = GetModuleHandleA("opengl32.dll")) {
        Hook(reinterpret_cast<void*>(GetProcAddress(gl, "glFogf")),
             reinterpret_cast<void*>(&HookedFogf),
             reinterpret_cast<void**>(&g_origFogf), "opengl32!glFogf");
        Hook(reinterpret_cast<void*>(GetProcAddress(gl, "glLightfv")),
             reinterpret_cast<void*>(&HookedLightfv),
             reinterpret_cast<void**>(&g_origLightfv), "opengl32!glLightfv");
    }

    // Input isolation is best-effort: if either collector hook fails the overlay
    // still works, the game just keeps reacting to input behind it.
    Hook(reinterpret_cast<void*>(Rva(off::kInputCollectKeyboard)),
         reinterpret_cast<void*>(&HookedCollectKbd),
         reinterpret_cast<void**>(&g_origCollectKbd), "Input_CollectKeyboardMsg");
    Hook(reinterpret_cast<void*>(Rva(off::kInputCollectMouse)),
         reinterpret_cast<void*>(&HookedCollectMouse),
         reinterpret_cast<void**>(&g_origCollectMouse), "Input_CollectMouseMsg");

    // These two entry points were verified in the beta21 database. The TLS
    // scope restricts the stock view to the local client's verification reply;
    // setup validation and every other checksum caller remain pass-through.
    constexpr uintptr_t summaryRva = 0x6C690, checksumRva = 0x1B5B30;
    constexpr uint8_t summaryPrefix[] = {0x48,0x89,0x5C,0x24,0x20,0x57,0x48,0x83,0xEC,0x50,0xBF,0x01,0,0,0};
    constexpr uint8_t checksumPrefix[] = {0x48,0x8B,0xC4,0x4C,0x89,0x48,0x20,0x4C,0x89,0x40,0x18,0x89,0x50,0x10};
    auto summary = reinterpret_cast<void*>(Rva(summaryRva));
    auto checksum = reinterpret_cast<void*>(Rva(checksumRva));
    if (IsReadable(summary, sizeof(summaryPrefix)) && IsReadable(checksum, sizeof(checksumPrefix)) &&
        std::memcmp(summary, summaryPrefix, sizeof(summaryPrefix)) == 0 &&
        std::memcmp(checksum, checksumPrefix, sizeof(checksumPrefix)) == 0) {
        if (Hook(checksum, reinterpret_cast<void*>(&HookedBikeChecksum),
                 reinterpret_cast<void**>(&g_origBikeChecksum), "Bike_VerificationChecksum")) {
            if (Hook(summary, reinterpret_cast<void*>(&HookedClientSummary),
                     reinterpret_cast<void**>(&g_origClientSummary), "Client_VerificationSummary")) {
                g_verificationReady.store(true, std::memory_order_release);
            } else {
                MH_DisableHook(checksum);
                MH_RemoveHook(checksum);
                g_origBikeChecksum = nullptr;
            }
        }
    } else Logf("verification: build entry-point check failed; leaving responses unchanged");

    constexpr uint8_t clientCall[] = {0xE8,0x29,0x41,0xFA,0xFF};
    constexpr uint8_t serializerPrefix[] = {0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x81,0xEC,0x90,0,0,0};
    auto call=reinterpret_cast<void*>(Rva(0x71A32));
    auto serializer=reinterpret_cast<void*>(Rva(0x15B60));
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(ModuleBase());
    const IMAGE_NT_HEADERS64* nt=nullptr;
    if(IsReadable(dos,sizeof(*dos)) && dos->e_magic==IMAGE_DOS_SIGNATURE && dos->e_lfanew>0 && dos->e_lfanew<0x100000)
        nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(ModuleBase()+dos->e_lfanew);
    const bool knownBuild=IsReadable(nt,sizeof(*nt)) && nt->Signature==IMAGE_NT_SIGNATURE &&
                          nt->FileHeader.Machine==IMAGE_FILE_MACHINE_AMD64 &&
                          nt->FileHeader.TimeDateStamp==0x6A21833D && nt->OptionalHeader.SizeOfImage==0x10FD000;

    // Skip both visor effect slots (dirt and rain). Check both
    // the function entry and the two field reads used to gate each effect.
    constexpr uint8_t visorPrefix[] = {0x48,0x8B,0xC4,0x48,0x89,0x58,0x10,0x48,0x89,0x68,0x18,0x48,0x89,0x70,0x20};
    constexpr uint8_t visorGate[] = {0x8B,0x83,0x48,0xFF,0xFF,0xFF,0x85,0xC0,
        0x0F,0x84,0x70,0x05,0,0,0x83,0xBB,0x5C,0xFF,0xFF,0xFF,0};
    auto visor = reinterpret_cast<void*>(Rva(off::kRenderVisorEffects));
    auto visorGateAt = reinterpret_cast<void*>(Rva(off::kRenderVisorEffects + 0xA0));
    bool visorReady = false;
    if (knownBuild && IsReadable(visor, sizeof(visorPrefix)) && IsReadable(visorGateAt, sizeof(visorGate)) &&
        std::memcmp(visor, visorPrefix, sizeof(visorPrefix)) == 0 &&
        std::memcmp(visorGateAt, visorGate, sizeof(visorGate)) == 0) {
        visorReady = Hook(visor, reinterpret_cast<void*>(&HookedRenderVisorEffects),
                          reinterpret_cast<void**>(&g_origRenderVisorEffects), "Render_VisorEffects");
    } else Logf("clean lens: build/visor entry-point check failed; hook skipped");
    cleanlens::SetHookReady(visorReady);

    if(GetModuleHandleW(L"spinny_hook.dll")) {
        spinny::SetHookReady(false,"restart without standalone Spinny");
        Logf("spinny: standalone DLL already loaded; integrated hook skipped");
    } else if(knownBuild && IsReadable(call,sizeof(clientCall)) && IsReadable(serializer,sizeof(serializerPrefix)) &&
              std::memcmp(call,clientCall,sizeof(clientCall))==0 &&
              std::memcmp(serializer,serializerPrefix,sizeof(serializerPrefix))==0) {
        const bool ready=Hook(serializer,reinterpret_cast<void*>(&HookedSerializeState),
                              reinterpret_cast<void**>(&g_origSerializeState),"Spinny_ClientStateSerializer");
        spinny::SetHookReady(ready,ready?"ready":"network hook unavailable");
    } else {
        spinny::SetHookReady(false,"network serializer differs");
        Logf("spinny: serializer/call-site check failed; integrated hook skipped");
    }

    const auto serializerStatus = spinny::GetStatus();
    networkram::SetHookReady(serializerStatus.ready,
        serializerStatus.ready ? "ready" : serializerStatus.note);

    // Player-info consumers synchronously copy/serialize the complete 684-byte
    // argument. Clone there, after the game's own local GUID check, so no local
    // GUID, license state, or caller-owned payload needs to be written.
    constexpr uint8_t connectPrefix[] = {0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57};
    constexpr uint8_t setPrefix[] = {0x48,0x83,0xEC,0x28,0x89,0x15,0xFA,0x42,0x2F,0x00,0x4C,0x63,0xC2,0x48,0x8B,0xD1};
    constexpr uint8_t sendPrefix[] = {0x48,0x89,0x5C,0x24,0x18,0x57,0x48,0x81,0xEC,0xD0,0x05,0x00,0x00};
    auto connectInfo = reinterpret_cast<void*>(Rva(0x2A21A0));
    auto setInfo = reinterpret_cast<void*>(Rva(0x2A47A0));
    auto sendInfo = reinterpret_cast<void*>(Rva(0x2A4640));
    const bool guidBuild = knownBuild && IsReadable(connectInfo,sizeof(connectPrefix)) &&
        IsReadable(setInfo,sizeof(setPrefix)) && IsReadable(sendInfo,sizeof(sendPrefix)) &&
        std::memcmp(connectInfo,connectPrefix,sizeof(connectPrefix)) == 0 &&
        std::memcmp(setInfo,setPrefix,sizeof(setPrefix)) == 0 &&
        std::memcmp(sendInfo,sendPrefix,sizeof(sendPrefix)) == 0;
    if (guidBuild) {
        const bool connected = Hook(connectInfo,reinterpret_cast<void*>(&HookedBeginPlayerConnect),
            reinterpret_cast<void**>(&g_origBeginPlayerConnect),"GUID_PlayerConnect");
        const bool set = connected && Hook(setInfo,reinterpret_cast<void*>(&HookedSetPlayerInfo),
            reinterpret_cast<void**>(&g_origSetPlayerInfo),"GUID_SetPlayerInfo");
        const bool sent = set && Hook(sendInfo,reinterpret_cast<void*>(&HookedSendPlayerInfo),
            reinterpret_cast<void**>(&g_origSendPlayerInfo),"GUID_SendPlayerInfo");
        if (!sent) {
            if (set) { MH_DisableHook(setInfo); MH_RemoveHook(setInfo); }
            if (connected) { MH_DisableHook(connectInfo); MH_RemoveHook(connectInfo); }
        }
        guid::SetHookReady(sent,sent ? "ready; select a GUID and enable before joining" : "player-info hook unavailable");
        if (sent) guid::Generate();
    } else {
        guid::SetHookReady(false,"player-info layout differs from beta21");
        Logf("guid: build/entry-point check failed; hooks skipped");
    }

    g_installed.store(true, std::memory_order_release);
    return true;
}

void Remove() {
    if (!g_installed.load(std::memory_order_acquire)) return;

    // Make every callback behave as a pass-through first, so anything that is
    // still routed to us does no work while we take the hooks out.
    g_shuttingDown.store(true, std::memory_order_release);
    g_verificationReady.store(false, std::memory_order_release);
    spinny::SetHookReady(false,"unloaded");
    networkram::SetHookReady(false,"unloaded");
    guid::SetHookReady(false,"unloaded");
    cleanlens::SetHookReady(false);

    // Detach the window subclass BEFORE the hooks, so no new messages enter our
    // proc while we are dismantling.
    if (g_window && g_origWndProc) {
        if (!IsWindow(g_window)) {
            // Window already gone; nothing points at us through it.
            Logf("unload: window %p no longer exists, nothing to restore",
                 static_cast<void*>(g_window));
        } else {
            const LONG_PTR current = GetWindowLongPtrA(g_window, GWLP_WNDPROC);
            if (current == reinterpret_cast<LONG_PTR>(&HookedWndProc)) {
                SetWindowLongPtrA(g_window, GWLP_WNDPROC,
                                  reinterpret_cast<LONG_PTR>(g_origWndProc));
            } else {
                // Someone subclassed on top of us. Restoring the original proc
                // would not remove us from the chain -- that other subclass
                // still holds our address and would call into freed pages.
                g_safeToUnload.store(false, std::memory_order_release);
                Logf("unload: window proc is %p, not ours (%p) - another subclass is on top, "
                     "staying loaded",
                     reinterpret_cast<void*>(current),
                     reinterpret_cast<void*>(&HookedWndProc));
            }
        }
        g_origWndProc = nullptr;
    }

    MH_DisableHook(MH_ALL_HOOKS);
    MH_RemoveHook(MH_ALL_HOOKS);
    MH_Uninitialize();

    // Wait for any thread still inside a callback to leave. The hooks' original
    // bytes are back and the subclass is detached, so no new entries are
    // possible; this only drains the ones in flight.
    int spins = 0;
    for (; spins < 4000 && g_inCallback.load(std::memory_order_acquire) > 0; ++spins) Sleep(1);
    if (g_inCallback.load(std::memory_order_acquire) > 0) {
        g_safeToUnload.store(false, std::memory_order_release);
        Logf("unload: %d thread(s) still inside our callbacks after %d ms - staying loaded",
             g_inCallback.load(std::memory_order_acquire), spins);
    }

    g_window = nullptr;
    g_installed.store(false, std::memory_order_release);
    Logf("unload: hooks removed, safe=%d", g_safeToUnload.load(std::memory_order_acquire) ? 1 : 0);
}

bool SafeToUnload() { return g_safeToUnload.load(std::memory_order_acquire); }

}  // namespace host
}  // namespace mxb
