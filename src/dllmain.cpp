#include <windows.h>

#include "host/Hooks.h"
#include "host/Log.h"
#include "host/Overlay.h"

// Internal, injected DLL. Load it into mxbikes.exe with any injector; there is
// no loader-specific entry point and nothing has to be renamed or placed in the
// game folder.
//
// INSERT toggles the menu. END unloads cleanly: the overlay tears itself down on
// the render thread, the hook comes off, then the DLL frees itself.

namespace {

HMODULE g_self = nullptr;
HANDLE  g_thread = nullptr;

// Unload has to be driven from our own thread, not from DllMain and not from the
// render thread: FreeLibrary cannot be called from inside the code being freed,
// and the GL teardown has to happen where the context lives.
DWORD WINAPI Watchdog(LPVOID) {
    mxb::host::LogInit();
    mxb::host::Logf("watchdog up, installing hook");

    if (!mxb::host::Install()) {
        mxb::host::Logf("Install() failed - nothing hooked, unloading");
        // Nothing hooked, so nothing to clean up. Leave quietly rather than
        // sitting in memory doing nothing.
        FreeLibraryAndExitThread(g_self, 1);
        return 1;
    }

    mxb::host::Logf("hook installed on gdi32!SwapBuffers; INSERT = menu, F7 = ban diagnostic, END = unload");

    while (true) {
        if (GetAsyncKeyState(VK_END) & 0x8000) break;
        Sleep(50);
    }

    // Ask the render thread to tear the overlay down, and give it a second to do
    // it. If the game stopped presenting we fall through anyway, because the
    // hook removal below drains in-flight callbacks on its own.
    mxb::host::Logf("END pressed, shutting down");
    mxb::host::Overlay::RequestShutdown();
    for (int spins = 0; spins < 1000 && !mxb::host::Overlay::ShutdownComplete(); ++spins)
        Sleep(1);
    mxb::host::Logf("overlay down (complete=%d), removing hook",
                    mxb::host::Overlay::ShutdownComplete() ? 1 : 0);

    mxb::host::Remove();

    if (!mxb::host::SafeToUnload()) {
        // Detaching did not fully succeed. Unmapping now would leave the game
        // calling into freed pages, so the module stays resident and inert --
        // every hook is already off and every callback is a pass-through.
        mxb::host::Logf("staying resident: detach incomplete, module left loaded but inert");
        return 0;
    }

    mxb::host::Logf("hooks removed, freeing module");

    // Let the last callbacks unwind out of our code before the pages go away.
    Sleep(250);

    FreeLibraryAndExitThread(g_self, 0);
    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            g_self = module;
            DisableThreadLibraryCalls(module);
            // Keep DllMain to the bare minimum: everything real happens on the
            // watchdog thread, outside the loader lock.
            g_thread = CreateThread(nullptr, 0, &Watchdog, nullptr, 0, nullptr);
            if (!g_thread) return FALSE;
            CloseHandle(g_thread);
            g_thread = nullptr;
            break;

        case DLL_PROCESS_DETACH:
            // Process teardown (reserved != nullptr) means no other thread is
            // running and the loader is about to drop everything, so touching GL
            // or waiting on anything here would be unsafe. Only clean up on an
            // explicit FreeLibrary that did not come from our own watchdog.
            if (!reserved && mxb::host::IsInstalled()) mxb::host::Remove();
            break;

        default:
            break;
    }
    return TRUE;
}
