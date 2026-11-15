#pragma once

// Fixed-function OpenGL renderer backend for the ImGui fork in
// "skrt menu clone/imgui" (1.70 WIP).
//
// Why not imgui_impl_opengl3: mxbikes.exe is pure fixed-function GL. Its import
// table is glBegin / glEnd / glVertex3f / glMatrixMode / glLoadIdentity /
// glOrtho / glMaterialfv / glLightfv / glTexGeni / glEnableClientState — no
// shader, program, VAO or VBO entry point anywhere. That means a compatibility
// profile context, which is exactly what the GL3 backend cannot target: it needs
// GL 3.2+ core, a GLSL pipeline and a function loader.
//
// This backend uses only GL 1.1/1.2 calls that opengl32.dll exports directly, so
// there is nothing to load and nothing to vendor, and it leaves the GL state
// exactly as it found it (push/pop attrib plus explicit matrix save) so the
// game's own rendering is unaffected.

// Diagnostic build switch: draws a plain magenta square through this backend's
// own state, with no ImGui geometry involved, to tell "our GL output never
// reaches the screen" apart from "ImGui produced nothing to draw". Off now that
// the overlay renders; kept because it answered exactly that question once.
#ifndef MXB_GL_TEST_QUAD
#define MXB_GL_TEST_QUAD 0
#endif

struct ImDrawData;

bool ImGui_ImplGL2_Init();
void ImGui_ImplGL2_Shutdown();
void ImGui_ImplGL2_NewFrame();
void ImGui_ImplGL2_RenderDrawData(ImDrawData* draw_data);

// Called by NewFrame on demand; exposed so the host can rebuild the atlas after
// adding fonts.
bool ImGui_ImplGL2_CreateDeviceObjects();
void ImGui_ImplGL2_DestroyDeviceObjects();

// True when the current GL context is not the one the device objects were built
// on, i.e. the game recreated its context and anything we uploaded is gone.
bool ImGui_ImplGL2_ContextChanged();

// Why the last RenderDrawData call drew nothing, or null if it drew. Points at
// static storage; for logging only.
const char* ImGui_ImplGL2_LastSkipReason();

// Uploads an RGBA8 image and returns the GL texture name as an ImTextureID.
// Used for the menu background. Returns nullptr on failure.
void* ImGui_ImplGL2_CreateTextureRGBA(const void* pixels, int width, int height);
void  ImGui_ImplGL2_DestroyTexture(void* texture_id);
