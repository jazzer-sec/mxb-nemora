#pragma once

namespace mxb {
namespace diagnostics {

struct Status {
    unsigned captures = 0;
    bool saved = false;
    char message[128] = "F7 saves the current ban diagnostic.";
};

// Read and log only. Call from the window/render thread, including after a kick.
void Capture();
Status GetStatus();

}  // namespace diagnostics
}  // namespace mxb
