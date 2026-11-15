#include <cstdio>
#include <stdexcept>
#include "mxb/CleanLens.h"

namespace {
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
}
int main() {
    using namespace mxb::cleanlens;
    try {
        Check(!GetStatus().ready && !GetStatus().enabled, "Must start disabled");
        Check(!SetEnabled(true) && !ShouldSkipDraw(), "Enabled without hook");
        SetHookReady(true);
        Check(!ShouldSkipDraw(), "Hook installation enabled feature");
        Check(SetEnabled(true), "Enable failed");
        Check(ShouldSkipDraw() && ShouldSkipDraw(), "Dirt/rain renderer was not skipped");
        Check(GetStatus().hidden == 2, "Skipped draw count wrong");
        SetEnabled(false);
        Check(!ShouldSkipDraw() && GetStatus().hidden == 2, "Disable did not restore drawing");
        SetEnabled(true);
        Check(ShouldSkipDraw(), "Re-enable failed");
        SetHookReady(false);
        Check(!GetStatus().ready && !GetStatus().enabled && !ShouldSkipDraw(), "Shutdown left enabled");
        Check(!SetEnabled(true), "Enabled after shutdown");
        SetHookReady(true);
        Check(!ShouldSkipDraw(), "Reinstallation enabled feature");
        std::puts("Clean lens smoke passed: default, hook guard, draw suppression, toggle, shutdown.");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Clean lens smoke failed: %s\n", error.what());
        return 1;
    }
}
