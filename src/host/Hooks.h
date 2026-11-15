#pragma once

namespace mxb {
namespace host {

// Installs three hooks and subclasses the game window.
//
// 1. gdi32!SwapBuffers -- not wglSwapBuffers. mxbikes.exe imports SwapBuffers
//    from GDI32 and the string "wglSwapBuffers" does not appear in the binary at
//    all, so a hook on it would never fire. This is where we draw.
//
// 2/3. Input_CollectKeyboardMsg (0x28DFD0) and Input_CollectMouseMsg (0x28E1F0),
//    the game's UI input collectors. App_WndProc calls both for every message.
//    We make them no-ops while the menu is open, which is how input is taken
//    away from the game.
//
// Why input isolation is done there and not in the window proc: App_WinMainLoop
// only advances a frame when the message queue is empty
// (`while (!PeekMessage) { App_Frame(); }`), and App_WndProc always ends in
// DefWindowProcA. Swallowing messages in the subclass broke both and froze the
// game until the menu was closed. The subclass now always forwards, so the pump
// behaves exactly as it does without us.
//
// Only the swap hook is required; the collector hooks are best-effort, and if
// either fails the overlay still works with the game reacting behind it.
//
// ReShade is installed as opengl32.dll in the game folder. We deliberately do not
// proxy or re-export that DLL; MinHook on gdi32!SwapBuffers sits alongside it.
// Our overlay therefore draws before ReShade's own present path, which means
// ReShade's effects apply to the overlay too. Acceptable, and the alternative
// (hooking after ReShade) would mean drawing after the swap.
bool Install();

// Removes every hook, restores the window proc, and waits until no thread is
// inside our callbacks before returning, so the DLL can be unloaded safely.
void Remove();

bool IsInstalled();

// False when Remove() could not fully detach -- in practice when something else
// subclassed the window on top of us, so restoring the original proc would leave
// that other subclass calling into our code. Freeing the DLL in that state means
// the game eventually executes unmapped pages, which is exactly the
// "mxb_physics.dll_unloaded" access violation seen on an earlier build. The
// caller must keep the module loaded when this is false.
bool SafeToUnload();

// Set once the hook has actually fired, so the status tab can say whether we are
// really in the present path.
bool HasPresented();

struct VerificationStatus {
    bool ready = false;
    unsigned stockReplies = 0;
    unsigned unavailable = 0;
    const char* lastFailure = "none";
    unsigned lastActualChecksum = 0;
    unsigned lastStockChecksum = 0;
};
VerificationStatus GetVerificationStatus();

}  // namespace host
}  // namespace mxb
