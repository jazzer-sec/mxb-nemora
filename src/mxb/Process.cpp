#include "Process.h"

#include <windows.h>
#include <psapi.h>

namespace mxb {
namespace {

uintptr_t g_base = 0;
bool      g_baseResolved = false;

bool Readable(const void* p, size_t n) {
    if (!p) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    const auto start = reinterpret_cast<uintptr_t>(p);
    const auto end   = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return start + n <= end;
}

RegistryProbe g_probe;

}  // namespace

RegistryProbe LastProbe() { return g_probe; }

uintptr_t ModuleBase() {
    if (!g_baseResolved) {
        g_baseResolved = true;
        HMODULE h = GetModuleHandleA("mxbikes.exe");
        if (!h) h = GetModuleHandleA(nullptr);  // we live inside the game process
        g_base = reinterpret_cast<uintptr_t>(h);
    }
    return g_base;
}

bool IsReadable(const void* p, size_t n) { return Readable(p, n); }

void* LocalBike() {
    const uintptr_t base=ModuleBase();
    if(!base) return nullptr;
    auto pointer=[&](uintptr_t address) -> uint8_t* {
        if(!Readable(reinterpret_cast<const void*>(address),sizeof(void*))) return nullptr;
        return *reinterpret_cast<uint8_t* const*>(address);
    };
    auto byHandle=[&](uint8_t* collection,int32_t handle) -> uint8_t* {
        if(!handle || !Readable(collection,4)) return nullptr;
        const int32_t count=*reinterpret_cast<const int32_t*>(collection);
        if(count<1 || count>512 || !Readable(collection,4+16*static_cast<size_t>(count))) return nullptr;
        for(int32_t i=0;i<count;++i) {
            const auto* entry=collection+4+16*static_cast<size_t>(i);
            if(*reinterpret_cast<const int32_t*>(entry)==handle) return pointer(reinterpret_cast<uintptr_t>(entry+8));
        }
        return nullptr;
    };
    if(!Readable(reinterpret_cast<const void*>(base+off::kLocalPlayerIndex),4)) return nullptr;
    const int32_t index=*reinterpret_cast<const int32_t*>(base+off::kLocalPlayerIndex);
    if(index<1 || index>50) return nullptr;
    const auto idAddress=base+off::kPlayerSlots+off::kPlayerSlotStride*static_cast<size_t>(index-1)+off::kPlayerSlotId;
    if(!Readable(reinterpret_cast<const void*>(idAddress),4)) return nullptr;
    const int32_t id=*reinterpret_cast<const int32_t*>(idAddress);
    const auto* entry=byHandle(pointer(base+0x567770),id);
    if(!Readable(entry,16)) return nullptr;
    const auto* world=pointer(reinterpret_cast<uintptr_t>(entry+8));
    if(!world) return nullptr;
    auto* bike=byHandle(pointer(reinterpret_cast<uintptr_t>(world+off::kWorldBikeColl)),
                        *reinterpret_cast<const int32_t*>(entry));
    return Readable(bike,off::bike::kSize)?bike:nullptr;
}

std::vector<BikeData> LoadedBikes() {
    std::vector<BikeData> out;

    g_probe = RegistryProbe{};

    const uintptr_t worlds = Rva(off::kWorldArray);
    if (!worlds) {
        g_probe.note = "module not resolved";
        return out;
    }

    // Walk every world's bike collection. Physics_StepWorld iterates worlds 1..3
    // and takes the collection at +40 as its bikes, so that is what we read.
    for (int w = 0; w < off::kWorldCount; ++w) {
        const auto* world =
            reinterpret_cast<const uint8_t*>(worlds + off::kWorldStride * static_cast<size_t>(w));
        if (!Readable(world + off::kWorldBikeColl, sizeof(void*))) continue;

        const auto* coll = *reinterpret_cast<const uint8_t* const*>(world + off::kWorldBikeColl);
        if (!coll || !Readable(coll, sizeof(int32_t))) continue;

        const int32_t count = *reinterpret_cast<const int32_t*>(coll);
        // A plausible session holds a handful of bikes. A wild count means the
        // pointer is stale or wrong, so skip this world rather than walk it.
        if (count <= 0 || count > 64) continue;
        if (!Readable(coll, kEntriesOffset + kEntryStride * static_cast<size_t>(count))) continue;

        ++g_probe.worldsSeen;
        if (!g_probe.collection) g_probe.collection = coll;
        g_probe.rawCount += count;

        // Explicit byte offsets, not a C++ struct: entries start 4 bytes in, but
        // any struct holding a pointer is 8-byte aligned, so the obvious
        // `struct { int32_t count; Entry entries[]; }` places them at 8 and every
        // pointer read lands 4 bytes off.
        for (int32_t i = 0; i < count; ++i) {
            const uint8_t* entry = coll + kEntriesOffset + kEntryStride * static_cast<size_t>(i);
            void* p = *reinterpret_cast<void* const*>(entry + kEntryPtrOffset);
            if (Readable(p, off::bike::kSize)) {
                out.emplace_back(p);
                ++g_probe.accepted;
            } else {
                ++g_probe.rejected;
            }
        }
    }

    if (!g_probe.worldsSeen)       g_probe.note = "no world has a bike collection";
    else if (out.empty())          g_probe.note = "all entries rejected";
    else                           g_probe.note = "ok";
    return out;
}

}  // namespace mxb
