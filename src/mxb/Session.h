#pragma once

namespace mxb {

// Offline-only guard.
//
// This reads the engine's raw network client state directly:
// g_NetConnectionState, RVA 0x3D7900. Net_ClientUpdate (0x1402A6890) and
// Net_BeginConnect (0x1402A7860) are the only writers:
//
//     0        client never brought up
//     1        disconnected / idle
//     2        connecting
//     3, 4     connected
//
// Net_QueryConnectionState (0x1402A79A0) collapses that to the 0/1/2 the UI
// sees, and the mapping below mirrors it.
//
// Earlier revisions called into the engine's dispatcher to ask for this instead.
// That crashed the game on inject: the handler takes THREE out-params, the third
// being a string buffer it strcpy's into, and we passed two, so it wrote through
// whatever happened to be in r8. A plain read has no calling-convention risk and
// no side effects, so that is what we do now.
enum class Online {
    Unknown,     // state outside the known range -> treated as online
    Offline,
    Connecting,
    Connected,
};

Online QueryOnline();

// Convenience: true unless we positively know the session is offline.
// Fails closed, so an unreadable or unexpected state never lets a write through.
bool WritesBlocked();

}  // namespace mxb
