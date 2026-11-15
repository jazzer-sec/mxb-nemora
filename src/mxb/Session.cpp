#include "Session.h"

#include <cstdint>

#include "Modifiers.h"
#include "Offsets.h"
#include "Process.h"

namespace mxb {

Online QueryOnline() {
    const uintptr_t addr = Rva(off::kNetConnectionState);
    if (!IsReadable(reinterpret_cast<void*>(addr), sizeof(int32_t))) return Online::Unknown;

    switch (*reinterpret_cast<const int32_t*>(addr)) {
        case 0:  // client never brought up
        case 1:  // disconnected / idle
            return Online::Offline;
        case 2:
            return Online::Connecting;
        case 3:
        case 4:
            return Online::Connected;
        default:
            return Online::Unknown;
    }
}

bool WritesBlocked() {
#if MXB_TESTING_NO_GUARDS
    // Testing build: write regardless of session state. See MXB_TESTING_NO_GUARDS.
    return false;
#else
    return QueryOnline() != Online::Offline;
#endif
}

}  // namespace mxb
