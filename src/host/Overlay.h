#pragma once

#include <windows.h>

namespace mxb {
namespace host {
namespace Overlay {

// Created lazily on the first frame, from inside the swap hook, so ImGui is
// always initialised on the thread that owns the GL context.
bool Ready();

void AttachWindow(HWND hwnd);

// One frame: build the ImGui frame, run the physics tick, draw, submit.
void Frame();

void Toggle();
bool IsOpen();

// True only while the menu is open AND the last frame actually put geometry on
// screen. Input is taken away from the game on this, not on IsOpen(), so an
// overlay that is failing to draw can never leave the game unresponsive with
// nothing visible to explain why.
bool IsInteractive();

// Tears down ImGui and releases GL objects. Must run on the render thread, so
// the swap hook calls this when a shutdown has been requested.
void Shutdown();

// Asks the next frame to shut the overlay down and then signals completion.
void RequestShutdown();
bool ShutdownComplete();

}  // namespace Overlay
}  // namespace host
}  // namespace mxb
