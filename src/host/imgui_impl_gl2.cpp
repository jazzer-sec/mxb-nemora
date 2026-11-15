#include "imgui_impl_gl2.h"

#include <windows.h>

#include <GL/gl.h>

#include <cstdio>

#include "imgui.h"

// GL 1.2 token, not always present in the SDK's gl.h.
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

// GL 1.2+ tokens the SDK's gl.h may not carry.
#ifndef GL_SECONDARY_COLOR_ARRAY
#define GL_SECONDARY_COLOR_ARRAY 0x845E
#endif
#ifndef GL_FOG_COORD_ARRAY
#define GL_FOG_COORD_ARRAY 0x8457
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif
#ifndef GL_MAX_TEXTURE_UNITS
#define GL_MAX_TEXTURE_UNITS 0x84E2
#endif
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER 0x8892
#endif
#ifndef GL_ELEMENT_ARRAY_BUFFER
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#endif

namespace {

GLuint g_fontTexture = 0;
const char* g_skipReason = nullptr;

// The context the current device objects belong to. Texture names are per
// context, so if the game tears its context down and makes a new one (it
// imports wglCreateContext / wglDeleteContext / wglShareLists) our font atlas
// name becomes meaningless and has to be re-uploaded.
HGLRC g_objectsContext = nullptr;

// Anything past GL 1.1 has to come from wglGetProcAddress; opengl32.dll only
// exports 1.1. These are all optional -- if the driver does not offer one we
// simply skip that bit of cleanup.
using PFN_glClientActiveTexture = void(APIENTRY*)(GLenum);
using PFN_glActiveTexture       = void(APIENTRY*)(GLenum);
using PFN_glBindBuffer          = void(APIENTRY*)(GLenum, GLuint);
using PFN_glBindVertexArray     = void(APIENTRY*)(GLuint);

struct GlExt {
    PFN_glClientActiveTexture clientActiveTexture = nullptr;
    PFN_glActiveTexture       activeTexture = nullptr;
    PFN_glBindBuffer          bindBuffer = nullptr;
    PFN_glBindVertexArray     bindVertexArray = nullptr;
    HGLRC                     resolvedFor = nullptr;
    int                       major = 1;
    int                       minor = 1;

    bool atLeast(int ma, int mi) const {
        return major > ma || (major == ma && minor >= mi);
    }
};
GlExt g_ext;

template <typename T>
T Resolve(const char* name) {
    return reinterpret_cast<T>(wglGetProcAddress(name));
}

// wglGetProcAddress results are only valid for the context they were queried
// on, so re-resolve whenever the context changes.
void EnsureExtensions() {
    const HGLRC ctx = wglGetCurrentContext();
    if (g_ext.resolvedFor == ctx) return;
    g_ext = GlExt{};
    g_ext.resolvedFor = ctx;
    if (!ctx) return;
    // Token availability follows the context version. Issuing a GL 1.3+ enum on
    // an older context sets GL_INVALID_ENUM, and this game calls glGetError
    // itself, so leaving errors behind is not harmless bookkeeping.
    if (const char* ver = reinterpret_cast<const char*>(glGetString(GL_VERSION))) {
        int ma = 1, mi = 1;
        if (sscanf_s(ver, "%d.%d", &ma, &mi) == 2) {
            g_ext.major = ma;
            g_ext.minor = mi;
        }
    }

    g_ext.clientActiveTexture = Resolve<PFN_glClientActiveTexture>("glClientActiveTexture");
    g_ext.activeTexture       = Resolve<PFN_glActiveTexture>("glActiveTexture");
    g_ext.bindBuffer          = Resolve<PFN_glBindBuffer>("glBindBuffer");
    g_ext.bindVertexArray     = Resolve<PFN_glBindVertexArray>("glBindVertexArray");
}

// With a buffer object bound, the pointer arguments to glVertexPointer and
// friends are reinterpreted as offsets into that buffer, so our client arrays
// would be read from entirely the wrong memory. ReShade lives in this process
// and does use modern GL, so do not assume nothing is bound.
void UnbindBufferObjects() {
    if (g_ext.bindVertexArray) g_ext.bindVertexArray(0);
    if (g_ext.bindBuffer) {
        g_ext.bindBuffer(GL_ARRAY_BUFFER, 0);
        g_ext.bindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    }
}

// Turn off every texture unit above 0 and select unit 0, so our texcoord
// pointer and bound texture land where the draw will actually read them.
void ResetTextureUnits() {
    if (!g_ext.clientActiveTexture || !g_ext.atLeast(1, 3)) return;

    GLint units = 1;
    glGetIntegerv(GL_MAX_TEXTURE_UNITS, &units);
    if (units < 1) units = 1;
    if (units > 32) units = 32;

    for (GLint i = units - 1; i >= 1; --i) {
        g_ext.clientActiveTexture(static_cast<GLenum>(GL_TEXTURE0 + i));
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        if (g_ext.activeTexture) {
            g_ext.activeTexture(static_cast<GLenum>(GL_TEXTURE0 + i));
            glDisable(GL_TEXTURE_2D);
            glDisable(GL_TEXTURE_GEN_S);
            glDisable(GL_TEXTURE_GEN_T);
        }
    }
    g_ext.clientActiveTexture(GL_TEXTURE0);
    if (g_ext.activeTexture) g_ext.activeTexture(GL_TEXTURE0);
}

}  // namespace

bool ImGui_ImplGL2_Init() {
    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = "imgui_impl_gl2 (mxb)";
    return true;
}

void ImGui_ImplGL2_Shutdown() { ImGui_ImplGL2_DestroyDeviceObjects(); }

void ImGui_ImplGL2_NewFrame() {
    EnsureExtensions();

    // Texture names belong to a context. If the game made a new one, whatever
    // g_fontTexture holds refers to nothing and every textured draw would come
    // out blank -- which looks exactly like the overlay not rendering at all,
    // while untextured geometry keeps working.
    const HGLRC ctx = wglGetCurrentContext();
    if (g_fontTexture && ctx != g_objectsContext) {
        g_fontTexture = 0;  // not ours to delete; it died with the old context
        ImGui::GetIO().Fonts->TexID = nullptr;
    }
    if (!g_fontTexture) ImGui_ImplGL2_CreateDeviceObjects();
}

bool ImGui_ImplGL2_ContextChanged() {
    return wglGetCurrentContext() != g_objectsContext;
}

bool ImGui_ImplGL2_CreateDeviceObjects() {
    ImGuiIO& io = ImGui::GetIO();

    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    if (!pixels || width <= 0 || height <= 0) return false;

    GLint lastTexture = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &lastTexture);

    glGenTextures(1, &g_fontTexture);
    glBindTexture(GL_TEXTURE_2D, g_fontTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    io.Fonts->TexID = reinterpret_cast<ImTextureID>(static_cast<intptr_t>(g_fontTexture));
    g_objectsContext = wglGetCurrentContext();

    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(lastTexture));
    return true;
}

void ImGui_ImplGL2_DestroyDeviceObjects() {
    if (!g_fontTexture) return;
    ImGuiIO& io = ImGui::GetIO();
    // Only delete if we are on the context that owns the name.
    if (wglGetCurrentContext() == g_objectsContext) glDeleteTextures(1, &g_fontTexture);
    io.Fonts->TexID = nullptr;
    g_fontTexture = 0;
    g_objectsContext = nullptr;
}

void* ImGui_ImplGL2_CreateTextureRGBA(const void* pixels, int width, int height) {
    if (!pixels || width <= 0 || height <= 0) return nullptr;

    GLint lastTexture = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &lastTexture);

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(lastTexture));

    if (!tex) return nullptr;
    return reinterpret_cast<void*>(static_cast<intptr_t>(tex));
}

void ImGui_ImplGL2_DestroyTexture(void* texture_id) {
    if (!texture_id) return;
    GLuint tex = static_cast<GLuint>(reinterpret_cast<intptr_t>(texture_id));
    glDeleteTextures(1, &tex);
}

const char* ImGui_ImplGL2_LastSkipReason() { return g_skipReason; }

void ImGui_ImplGL2_RenderDrawData(ImDrawData* draw_data) {
    g_skipReason = nullptr;

    if (!draw_data || !draw_data->Valid) {
        g_skipReason = "draw data not valid";
        return;
    }

    // Everything below needs the game's GL context current on this thread. It
    // normally is, since we run from inside its swap call, but if some other
    // layer (ReShade owns opengl32.dll here) has made its own context current
    // our draws would land somewhere invisible instead.
    if (!wglGetCurrentContext()) {
        g_skipReason = "no GL context current";
        return;
    }

    // Framebuffer size in pixels; ImGui works in points, which can differ on a
    // scaled display. A zero or negative scale would silently cancel the whole
    // frame, so treat anything odd as 1:1.
    float scaleX = draw_data->FramebufferScale.x;
    float scaleY = draw_data->FramebufferScale.y;
    if (!(scaleX > 0.0f)) scaleX = 1.0f;
    if (!(scaleY > 0.0f)) scaleY = 1.0f;

    int fbWidth  = static_cast<int>(draw_data->DisplaySize.x * scaleX);
    int fbHeight = static_cast<int>(draw_data->DisplaySize.y * scaleY);

    // io.DisplaySize starts life as (-1,-1) and only becomes real once the
    // platform backend has a usable window. Fall back to the viewport the game
    // already set rather than drawing nothing.
    if (fbWidth <= 0 || fbHeight <= 0) {
        GLint vp[4] = {0, 0, 0, 0};
        glGetIntegerv(GL_VIEWPORT, vp);
        if (vp[2] > 0 && vp[3] > 0) {
            fbWidth = vp[2];
            fbHeight = vp[3];
            draw_data->DisplaySize = ImVec2(static_cast<float>(vp[2]), static_cast<float>(vp[3]));
            draw_data->DisplayPos = ImVec2(0.0f, 0.0f);
            scaleX = scaleY = 1.0f;
        }
    }
    if (fbWidth <= 0 || fbHeight <= 0) {
        g_skipReason = "framebuffer size unknown";
        return;
    }

    // Clear whatever the game may have left queued, so the error we report at the
    // end is unambiguously ours.
    for (int drain = 0; drain < 32 && glGetError() != GL_NO_ERROR; ++drain) {
    }

    // The game draws with its own GL state and does not expect us to disturb it,
    // so save everything we are about to touch.
    glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_TRANSFORM_BIT | GL_VIEWPORT_BIT |
                 GL_SCISSOR_BIT | GL_TEXTURE_BIT | GL_CURRENT_BIT | GL_POLYGON_BIT);
    glPushClientAttrib(GL_CLIENT_VERTEX_ARRAY_BIT);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    glDisable(GL_COLOR_MATERIAL);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_FOG);
    glDisable(GL_STENCIL_TEST);
    // An sRGB framebuffer would gamma-convert our colours on write, and
    // multisample resolve softens 1px UI edges. Both are newer than GL 1.1, so
    // only touch them when the context actually knows the token.
    if (g_ext.atLeast(3, 0)) glDisable(0x8DB9 /* GL_FRAMEBUFFER_SRGB */);
    if (g_ext.atLeast(1, 3)) glDisable(0x809D /* GL_MULTISAMPLE */);
    glEnable(GL_SCISSOR_TEST);
    // Client-array hygiene. This matters more than it looks: glDrawElements
    // sources EVERY enabled client array using our index range, including arrays
    // we never set up. mxbikes.exe enables its own vertex/normal/texcoord/colour
    // arrays and uses multitexturing, so anything it left enabled would be read
    // through ITS pointer with OUR indices -- which is an out-of-bounds read
    // inside the driver, and is what crashed nvoglv64.dll on track load.
    //
    // The same applies to the active client texture unit: glTexCoordPointer only
    // touches the selected unit, so if the game left unit 1 selected our UVs
    // would go there while unit 0 kept the game's stale pointer.
    UnbindBufferObjects();
    ResetTextureUnits();

    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    glDisableClientState(GL_INDEX_ARRAY);
    glDisableClientState(GL_EDGE_FLAG_ARRAY);
    // GL 1.4 arrays; the enums do not exist before that.
    if (g_ext.atLeast(1, 4)) {
        glDisableClientState(GL_SECONDARY_COLOR_ARRAY);
        glDisableClientState(GL_FOG_COORD_ARRAY);
    }
    glEnable(GL_TEXTURE_2D);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glShadeModel(GL_SMOOTH);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

    // The game leaves texture generation on for its own passes; it would warp our
    // UVs if it stayed enabled.
    glDisable(GL_TEXTURE_GEN_S);
    glDisable(GL_TEXTURE_GEN_T);

    glViewport(0, 0, static_cast<GLsizei>(fbWidth), static_cast<GLsizei>(fbHeight));

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(draw_data->DisplayPos.x, draw_data->DisplayPos.x + draw_data->DisplaySize.x,
            draw_data->DisplayPos.y + draw_data->DisplaySize.y, draw_data->DisplayPos.y,
            -1.0, 1.0);

    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    // GL_TEXTURE is not covered by GL_TRANSFORM_BIT's matrix stacks on every
    // driver, so reset it explicitly.
    glMatrixMode(GL_TEXTURE);
    glPushMatrix();
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);

#if MXB_GL_TEST_QUAD
    // Diagnostic: a magenta square at the top-left, drawn through the state we
    // just configured but without any ImGui geometry. If this shows up and the
    // menu does not, the GL path reaches the screen and the problem is upstream
    // in ImGui; if neither shows, the problem is this path or the present.
    {
        glBindTexture(GL_TEXTURE_2D, 0);
        glDisable(GL_TEXTURE_2D);
        glScissor(0, 0, fbWidth, fbHeight);
        const float x0 = draw_data->DisplayPos.x + 20.0f;
        const float y0 = draw_data->DisplayPos.y + 20.0f;
        glBegin(GL_TRIANGLE_STRIP);
        glColor4f(1.0f, 0.0f, 1.0f, 1.0f);
        glVertex2f(x0, y0);
        glVertex2f(x0 + 200.0f, y0);
        glVertex2f(x0, y0 + 200.0f);
        glVertex2f(x0 + 200.0f, y0 + 200.0f);
        glEnd();
        glEnable(GL_TEXTURE_2D);
    }
#endif

    const ImVec2 clipOff = draw_data->DisplayPos;
    const ImVec2 clipScale = ImVec2(scaleX, scaleY);

    for (int n = 0; n < draw_data->CmdListsCount; ++n) {
        const ImDrawList* cmdList = draw_data->CmdLists[n];
        const ImDrawVert* vtxBuffer = cmdList->VtxBuffer.Data;
        const ImDrawIdx*  idxBuffer = cmdList->IdxBuffer.Data;

        glVertexPointer(2, GL_FLOAT, sizeof(ImDrawVert),
                        reinterpret_cast<const char*>(vtxBuffer) + IM_OFFSETOF(ImDrawVert, pos));
        glTexCoordPointer(2, GL_FLOAT, sizeof(ImDrawVert),
                          reinterpret_cast<const char*>(vtxBuffer) + IM_OFFSETOF(ImDrawVert, uv));
        glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(ImDrawVert),
                       reinterpret_cast<const char*>(vtxBuffer) + IM_OFFSETOF(ImDrawVert, col));

        for (int i = 0; i < cmdList->CmdBuffer.Size; ++i) {
            const ImDrawCmd* cmd = &cmdList->CmdBuffer[i];
            if (cmd->UserCallback) {
                cmd->UserCallback(cmdList, cmd);
                idxBuffer += cmd->ElemCount;
                continue;
            }

            // Clip rect from ImGui space into framebuffer pixels, y flipped.
            ImVec4 clip;
            clip.x = (cmd->ClipRect.x - clipOff.x) * clipScale.x;
            clip.y = (cmd->ClipRect.y - clipOff.y) * clipScale.y;
            clip.z = (cmd->ClipRect.z - clipOff.x) * clipScale.x;
            clip.w = (cmd->ClipRect.w - clipOff.y) * clipScale.y;

            if (clip.x >= static_cast<float>(fbWidth) || clip.y >= static_cast<float>(fbHeight) ||
                clip.z < 0.0f || clip.w < 0.0f) {
                idxBuffer += cmd->ElemCount;
                continue;
            }

            // A negative width or height here is GL_INVALID_VALUE, which is one
            // of the two errors this backend was leaving behind.
            const float sw = clip.z - clip.x;
            const float sh = clip.w - clip.y;
            if (sw <= 0.0f || sh <= 0.0f) {
                idxBuffer += cmd->ElemCount;
                continue;
            }
            glScissor(static_cast<GLint>(clip.x),
                      static_cast<GLint>(static_cast<float>(fbHeight) - clip.w),
                      static_cast<GLsizei>(sw), static_cast<GLsizei>(sh));

            glBindTexture(GL_TEXTURE_2D,
                          static_cast<GLuint>(reinterpret_cast<intptr_t>(cmd->TextureId)));
            glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(cmd->ElemCount),
                           sizeof(ImDrawIdx) == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT,
                           idxBuffer);
            idxBuffer += cmd->ElemCount;
        }
    }

    glMatrixMode(GL_TEXTURE);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);

    glPopClientAttrib();
    glPopAttrib();

    // Record the first error, then drain the queue. The game imports glGetError
    // and checks it itself, so leaving ours queued would hand it a failure that
    // has nothing to do with its own rendering.
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        static char buf[64];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "glGetError 0x%04X after draw", err);
        g_skipReason = buf;
        for (int drain = 0; drain < 32 && glGetError() != GL_NO_ERROR; ++drain) {
        }
    }
}
