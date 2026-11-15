#pragma once

// Contract between the physics module and whatever renders it.
//
// The module itself is renderer-agnostic: it needs an ImGui frame to draw into
// and one call per frame on the game's main thread. The host supplies these
// three symbols (ModMenu.cpp declares them extern):
//
//     ImFont* menuFont;        // the regular face from MenuFonts.h
//     ImFont* tabFont;         // the bold face used for the sidebar glyphs
//     void*   menuBgTexture;   // ImTextureID for MenuBackground.h, or nullptr
//
// Per-frame order, inside the swap-buffers hook:
//
//     ImGui_ImplOpenGL3_NewFrame();
//     ImGui_ImplWin32_NewFrame();
//     ImGui::NewFrame();
//
//     mxb::Tick();              // reads the session guard, patches loaded bikes
//     mxb::ui::Render();        // draws the overlay when it is open
//
//     ImGui::Render();
//     ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
//
// mxb::Tick() is cheap when nothing is enabled and is safe to call every frame:
// it always writes stock * multiplier from its own snapshot, never current *
// multiplier, so repeated calls do not compound.
//
// Threading: both calls touch live game memory and the engine dispatcher, so
// they must run on the thread that owns the GL context (the game's main thread).
// The swap hook already runs there.
//
// Renderer note: MX Bikes is OpenGL, and ReShade ships as opengl32.dll in the
// game folder. Hook wglSwapBuffers or gdi32!SwapBuffers through MinHook rather
// than proxying opengl32.dll, so ReShade keeps working. The reference menu under
// "skrt menu clone" is wired for DX9; its imgui fork and fonts carry over, but
// imgui_impl_dx9 has to be swapped for imgui_impl_opengl3.

namespace mxb {
void Tick();
void RestoreAll();
namespace ui {
void Render();
bool& IsOpen();
}  // namespace ui
}  // namespace mxb
