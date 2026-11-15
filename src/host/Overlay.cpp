#include "Overlay.h"

#include <windows.h>

#include <objidl.h>
#include <wincodec.h>

#include <atomic>
#include <vector>

#include "Hooks.h"
#include "Log.h"
#include "imgui.h"
#include <GL/gl.h>

#include "imgui_impl_gl2.h"
#include "imgui_impl_win32.h"
#include "mxb/CarMode.h"
#include "mxb/Chams.h"
#include "mxb/Modifiers.h"
#include "mxb/Visuals.h"
#include "ui/ModMenu.h"
#include "ui/Overlays.h"
#include "ui/Watermark.h"

// Font and background blobs from the reference menu.
#include "menu/MenuBackground.h"
#include "menu/MenuFonts.h"

// The ImGui fork's custom widgets reach for these by extern, and ui/ModMenu.cpp
// uses the first two. All five have to exist or imgui.cpp and imgui_widgets.cpp
// do not link:
//   menuFont      regular face, body text
//   boldMenuFont  bold face at body size, group box titles
//   controlFont   regular face, control labels
//   comboFont     the comboarrow icon face, dropdown arrows
//   tabFont       bold face, large, sidebar glyphs
ImFont* menuFont = nullptr;
ImFont* boldMenuFont = nullptr;
ImFont* controlFont = nullptr;
ImFont* comboFont = nullptr;
ImFont* tabFont = nullptr;
void*   menuBgTexture = nullptr;

namespace mxb {
namespace host {
namespace Overlay {
namespace {

std::atomic<bool> g_ready{false};
std::atomic<bool> g_open{false};
std::atomic<bool> g_drew{false};
std::atomic<bool> g_shutdownRequested{false};
std::atomic<bool> g_shutdownComplete{false};

HWND g_hwnd = nullptr;
bool g_initTried = false;
bool g_comInitialised = false;

// Kept so the background can be re-uploaded if the game recreates its GL
// context, which invalidates the texture name.
std::vector<unsigned char> g_bgPixels;
int g_bgWidth = 0, g_bgHeight = 0;

// Decodes an in-memory image to tightly packed RGBA8 using WIC, so we do not
// have to vendor a PNG decoder. Returns false and leaves out* untouched on
// failure; a missing background just means the menu draws without it.
bool DecodeImageRGBA(const void* data, size_t size, std::vector<unsigned char>* out, int* width,
                     int* height) {
    if (!data || size == 0) return false;

    const HRESULT coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (coInit == S_OK || coInit == S_FALSE) g_comInitialised = true;

    bool ok = false;
    IWICImagingFactory* factory = nullptr;
    IStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    HGLOBAL mem = nullptr;

    do {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&factory))))
            break;

        mem = GlobalAlloc(GMEM_MOVEABLE, size);
        if (!mem) break;
        if (void* p = GlobalLock(mem)) {
            memcpy(p, data, size);
            GlobalUnlock(mem);
        } else {
            break;
        }
        // Hands ownership of `mem` to the stream, so we must not free it below.
        if (FAILED(CreateStreamOnHGlobal(mem, TRUE, &stream))) break;
        mem = nullptr;

        if (FAILED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad,
                                                    &decoder)))
            break;
        if (FAILED(decoder->GetFrame(0, &frame))) break;
        if (FAILED(factory->CreateFormatConverter(&converter))) break;
        if (FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
                                         WICBitmapDitherTypeNone, nullptr, 0.0,
                                         WICBitmapPaletteTypeCustom)))
            break;

        UINT w = 0, h = 0;
        if (FAILED(converter->GetSize(&w, &h)) || w == 0 || h == 0) break;

        out->resize(static_cast<size_t>(w) * h * 4);
        if (FAILED(converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(out->size()),
                                         out->data())))
            break;

        *width = static_cast<int>(w);
        *height = static_cast<int>(h);
        ok = true;
    } while (false);

    if (mem) GlobalFree(mem);
    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();
    return ok;
}

void LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig cfg;
    cfg.OversampleH = 1;
    cfg.OversampleV = 1;
    cfg.PixelSnapH = true;

    menuFont = io.Fonts->AddFontFromMemoryCompressedTTF(verdana_compressed_data,
                                                        verdana_compressed_size, 12.0f, &cfg);
    boldMenuFont = io.Fonts->AddFontFromMemoryCompressedTTF(verdanab_compressed_data,
                                                            verdanab_compressed_size, 12.0f, &cfg);
    controlFont = io.Fonts->AddFontFromMemoryCompressedTTF(verdana_compressed_data,
                                                           verdana_compressed_size, 12.0f, &cfg);
    comboFont = io.Fonts->AddFontFromMemoryCompressedTTF(comboarrow_compressed_data,
                                                         comboarrow_compressed_size, 12.0f, &cfg);
    // Sized for the short words on the 75px tab buttons, not for single glyphs.
    tabFont = io.Fonts->AddFontFromMemoryCompressedTTF(verdanab_compressed_data,
                                                       verdanab_compressed_size, 15.0f, &cfg);

    // Any of these can come back null if a blob fails to decompress; the widgets
    // dereference them without checking, so fall back rather than crash.
    if (!menuFont) menuFont = io.Fonts->AddFontDefault();
    if (!boldMenuFont) boldMenuFont = menuFont;
    if (!controlFont) controlFont = menuFont;
    if (!comboFont) comboFont = menuFont;
    if (!tabFont) tabFont = boldMenuFont;
}

bool Init() {
    if (g_initTried) return g_ready.load(std::memory_order_acquire);
    g_initTried = true;
    if (!g_hwnd) return false;

    Logf("overlay init: hwnd=%p", static_cast<void*>(g_hwnd));

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // do not write anything into the game folder
    io.LogFilename = nullptr;

    LoadFonts();

    Logf("fonts loaded (menu=%p bold=%p ctrl=%p combo=%p tab=%p)",
         static_cast<void*>(menuFont), static_cast<void*>(boldMenuFont),
         static_cast<void*>(controlFont), static_cast<void*>(comboFont),
         static_cast<void*>(tabFont));

    if (!ImGui_ImplWin32_Init(g_hwnd)) {
        Logf("ImGui_ImplWin32_Init failed");
        ImGui::DestroyContext();
        return false;
    }
    if (!ImGui_ImplGL2_Init()) {
        Logf("ImGui_ImplGL2_Init failed");
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        return false;
    }

    if (DecodeImageRGBA(menuBackground, sizeof(menuBackground), &g_bgPixels, &g_bgWidth,
                        &g_bgHeight))
        menuBgTexture = ImGui_ImplGL2_CreateTextureRGBA(g_bgPixels.data(), g_bgWidth, g_bgHeight);
    Logf("background %dx%d tex=%p", g_bgWidth, g_bgHeight, menuBgTexture);

    g_ready.store(true, std::memory_order_release);
    Logf("overlay ready");
    return true;
}

}  // namespace

bool Ready() { return g_ready.load(std::memory_order_acquire); }
bool IsOpen() { return g_open.load(std::memory_order_acquire); }

bool IsInteractive() {
    return g_open.load(std::memory_order_acquire) && g_drew.load(std::memory_order_acquire);
}

void AttachWindow(HWND hwnd) { g_hwnd = hwnd; }

void Toggle() {
    const bool next = !g_open.load(std::memory_order_acquire);
    g_open.store(next, std::memory_order_release);
    if (!next) g_drew.store(false, std::memory_order_release);
    ui::IsOpen() = next;
    Logf("menu %s", next ? "opened" : "closed");
}

void RequestShutdown() { g_shutdownRequested.store(true, std::memory_order_release); }
bool ShutdownComplete() { return g_shutdownComplete.load(std::memory_order_acquire); }

void Shutdown() {
    if (!g_ready.load(std::memory_order_acquire)) {
        g_shutdownComplete.store(true, std::memory_order_release);
        return;
    }
    g_ready.store(false, std::memory_order_release);

    // Put every bike back before the overlay goes away, or the last multipliers
    // would stay applied with no UI left to turn them off.
    RestoreAll();
    VisualsRestore();

    if (menuBgTexture) {
        ImGui_ImplGL2_DestroyTexture(menuBgTexture);
        menuBgTexture = nullptr;
    }
    ImGui_ImplGL2_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    menuFont = nullptr;
    boldMenuFont = nullptr;
    controlFont = nullptr;
    comboFont = nullptr;
    tabFont = nullptr;

    if (g_comInitialised) {
        CoUninitialize();
        g_comInitialised = false;
    }

    g_shutdownComplete.store(true, std::memory_order_release);
}

void Frame() {
    if (g_shutdownRequested.load(std::memory_order_acquire)) {
        Shutdown();
        return;
    }
    if (!g_ready.load(std::memory_order_acquire) && !Init()) return;

    // Heartbeat. App_WinMainLoop only advances a frame when the message queue is
    // empty, so if the game ever looks frozen this says whether we are still
    // being presented to (frames climbing) or whether frame advancement itself
    // stopped (frames flat).
    {
        static unsigned long long frames = 0;
        static DWORD lastLog = 0;
        ++frames;
        const DWORD now = GetTickCount();
        if (now - lastLog >= 2000) {
            lastLog = now;
            // Name every modifier that is not at its neutral value. When the
            // game dies, the last heartbeat before it says what was actually
            // active, which beats trying to remember afterwards.
            const Settings& cfg = Get();
            char act[192];
            int n = 0;
            auto add = [&](const char* name, float v) {
                if (v != 1.0f && n < static_cast<int>(sizeof(act)) - 1)
                    n += _snprintf_s(act + n, sizeof(act) - n, _TRUNCATE, "%s=%.2f ", name, v);
            };
            act[0] = 0;
            if (cfg.grip.enabled) {
                add("gripLong", cfg.grip.peakLong); add("gripLat", cfg.grip.peakLat);
                add("gripDry", cfg.grip.surfaceDry); add("gripWet", cfg.grip.surfaceWet);
                add("carcassK", cfg.grip.carcassSpring); add("carcassD", cfg.grip.carcassDamper);
            }
            if (cfg.suspension.enabled) {
                add("fSpring", cfg.suspension.frontSpring);
                add("rSpring", cfg.suspension.rearSpring);
                add("fDamp", cfg.suspension.frontDamping);
                add("rDamp", cfg.suspension.rearDamping);
                add("bumpStop", cfg.suspension.bumpStop); add("topOut", cfg.suspension.topOut);
            }
            if (cfg.engine.enabled) {
                add("torque", cfg.engine.torque); add("engBrake", cfg.engine.engineBraking);
                add("clutchTq", cfg.engine.clutchTorque);
                add("revRange", cfg.engine.revRange);
                add("topSpeed", cfg.engine.topSpeed);
            }
            if (cfg.transmission.enabled) {
                add("gearRatio", cfg.transmission.gearRatio);
                add("finalDrive", cfg.transmission.finalDrive);
                add("shiftSpd", cfg.transmission.shiftSpeed);
                add("clutchSpd", cfg.transmission.clutchSpeed);
                add("drvEff", cfg.transmission.drivelineEff);
                add("drvInertia", cfg.transmission.drivelineInertia);
            }
            if (cfg.crash.enabled) {
                add("crashTol", cfg.crash.tolerance);
                if (cfg.crash.noCrash && n < static_cast<int>(sizeof(act)) - 1)
                    _snprintf_s(act + n, sizeof(act) - n, _TRUNCATE, "NOCRASH ");
            }

            Logf("heartbeat: frames=%llu menu=%d active[ %s]", frames,
                 g_open.load(std::memory_order_acquire) ? 1 : 0, act);
        }
    }

    // A recreated GL context invalidates every texture name we hold. The
    // backend re-uploads the font atlas itself; the background is ours.
    if (ImGui_ImplGL2_ContextChanged() && !g_bgPixels.empty()) {
        menuBgTexture = ImGui_ImplGL2_CreateTextureRGBA(g_bgPixels.data(), g_bgWidth, g_bgHeight);
        Logf("GL context changed: background re-uploaded tex=%p", menuBgTexture);
    }

    ImGui_ImplGL2_NewFrame();
    ImGui_ImplWin32_NewFrame();

    // ImGui culls everything if it does not believe it has a viewport, and
    // io.DisplaySize starts out as (-1,-1). The renderer has its own fallback
    // for the draw, but ImGui needs a sane value BEFORE NewFrame or it builds no
    // geometry at all.
    {
        ImGuiIO& io = ImGui::GetIO();
        if (!(io.DisplaySize.x > 0.0f) || !(io.DisplaySize.y > 0.0f)) {
            RECT rc{};
            if (g_hwnd && GetClientRect(g_hwnd, &rc) && rc.right > rc.left && rc.bottom > rc.top) {
                io.DisplaySize = ImVec2(static_cast<float>(rc.right - rc.left),
                                        static_cast<float>(rc.bottom - rc.top));
            } else {
                GLint vp[4] = {0, 0, 0, 0};
                glGetIntegerv(GL_VIEWPORT, vp);
                if (vp[2] > 0 && vp[3] > 0)
                    io.DisplaySize = ImVec2(static_cast<float>(vp[2]), static_cast<float>(vp[3]));
            }
        }
    }
    ImGui::NewFrame();

    // Reads the session guard and applies the multipliers to every loaded bike.
    // Cheap when nothing is enabled, and idempotent, so running it per frame is
    // intended.
    mxb::Tick();
    mxb::VisualsTick();
    mxb::CarModeTick();
    mxb::ChamsTick();

    // World-space overlays first: the screen filter goes in the background draw
    // list so the menu stays readable on top of it, and the china hat wants the
    // view the frustum hook captured earlier this frame.
    ui::DrawWorldOverlays();

    ui::RenderWatermark();
    ui::Render();

    ImGui::Render();
    ImDrawData* dd = ImGui::GetDrawData();
    ImGui_ImplGL2_RenderDrawData(dd);

    g_drew.store(dd && dd->TotalVtxCount > 0 && ImGui_ImplGL2_LastSkipReason() == nullptr,
                 std::memory_order_release);

    // While the menu is open, report once a second what we actually handed to
    // GL. cmds/vtx at zero means ImGui built nothing; non-zero with a visible
    // test quad but no menu means the geometry is there but off-screen or
    // clipped; a skip reason names the guard that bailed out.
    if (g_open.load(std::memory_order_acquire)) {
        static DWORD lastDraw = 0;
        const DWORD now = GetTickCount();
        if (now - lastDraw >= 1000) {
            lastDraw = now;
            const ImGuiIO& io = ImGui::GetIO();
            const char* skip = ImGui_ImplGL2_LastSkipReason();
            Logf("draw: display=%.0fx%.0f scale=%.2fx%.2f cmds=%d vtx=%d idx=%d ctx=%p%s%s",
                 io.DisplaySize.x, io.DisplaySize.y,
                 dd ? dd->FramebufferScale.x : 0.0f, dd ? dd->FramebufferScale.y : 0.0f,
                 dd ? dd->CmdListsCount : -1, dd ? dd->TotalVtxCount : -1,
                 dd ? dd->TotalIdxCount : -1,
                 static_cast<void*>(wglGetCurrentContext()),
                 skip ? " SKIPPED: " : "", skip ? skip : "");
        }
    }

    // Keep the cursor visible while the menu is up; the game hides it in-race.
    if (g_open.load(std::memory_order_acquire)) ImGui::GetIO().MouseDrawCursor = true;
    else ImGui::GetIO().MouseDrawCursor = false;
}

}  // namespace Overlay
}  // namespace host
}  // namespace mxb
