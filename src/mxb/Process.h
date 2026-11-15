#pragma once
#include <cstdint>
#include <vector>

#include "BikeData.h"
#include "Offsets.h"

namespace mxb {

// Module base of mxbikes.exe, resolved once. Zero if we are not inside the game.
uintptr_t ModuleBase();

// Translates an RVA from Offsets.h into a live address.
inline uintptr_t Rva(uintptr_t rva) {
    const uintptr_t b = ModuleBase();
    return b ? b + rva : 0;
}

// The engine's generic collection, used both by the handle registry
// (HandleRegistry_Alloc, 0x14015C040) and by the per-world entity lists that
// Collection_Count (0x14015C240) and Collection_At (0x14015C350) read:
//
//   +0            int32   count
//   +4 + 16*i     int32   id          (1-based handle)
//   +8 + 16*i     int32   padding
//   +12 + 16*i    void*   ptr
//
// Deliberately expressed as offsets, not as a struct. Entries begin at +4, but
// any struct containing a pointer is 8-byte aligned, so the obvious
// `struct { int32_t count; RegEntry entries[]; }` puts them at +8 and every
// pointer read lands 4 bytes off.
inline constexpr size_t kEntriesOffset  = 4;
inline constexpr size_t kEntryStride    = 16;
inline constexpr size_t kEntryIdOffset  = 0;
inline constexpr size_t kEntryPtrOffset = 8;

// What the last LoadedBikes() call actually saw. Surfaced in the menu so a zero
// result can be told apart from a registry that is empty, missing or bogus.
struct RegistryProbe {
    const void* collection = nullptr;  // first non-empty bike collection found
    int32_t     worldsSeen = 0;        // worlds with a readable bike collection
    int32_t     rawCount = 0;          // total entries across all worlds
    int32_t     accepted = 0;          // entries with a usable pointer
    int32_t     rejected = 0;          // entries whose pointer failed validation
    const char* note = "not probed";
};
RegistryProbe LastProbe();

// Every bike physics object currently loaded. Usually one in singleplayer, but a
// replay or a second rider can add more, so callers should handle any count.
std::vector<BikeData> LoadedBikes();

// Resolve the local player's bike via the same two handle lookups as
// 0x1401BF1A0, without calling game code or assuming the first loaded bike is local.
void* LocalBike();

// Committed-and-readable check, used before touching any game pointer.
bool IsReadable(const void* p, size_t n);

}  // namespace mxb
