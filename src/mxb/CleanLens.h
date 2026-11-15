#pragma once
#include <cstdint>

namespace mxb::cleanlens {

struct Status {
    bool ready;
    bool enabled;
    uint64_t hidden;
};
Status GetStatus();
void SetHookReady(bool ready);
bool SetEnabled(bool enabled);

// The visor renderer draws only dirt and rain. Skip the complete call while
// enabled, without changing the view or any persistent effect state.
bool ShouldSkipDraw();

}  // namespace mxb::cleanlens
