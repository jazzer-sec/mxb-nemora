#pragma once

// The ImGui fork under "skrt menu clone/imgui" has a leftover
// `#include "Globals.h"` in imgui.cpp (just above ImGui::Begin). The original
// header belonged to the cheat the menu was lifted from and was not shipped with
// it. Nothing in imgui.cpp or imgui_widgets.cpp actually references a symbol
// from it, so this empty stub satisfies the include without us editing the
// vendored fork.
