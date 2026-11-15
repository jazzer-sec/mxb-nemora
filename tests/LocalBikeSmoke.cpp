#include <windows.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>
// Same translation unit lets the fixture substitute the cached module base
// without adding a production API for injecting test addresses.
#include "../src/mxb/Process.cpp"

namespace {
void Check(bool okay,const char* message) { if(!okay) throw std::runtime_error(message); }
template<class T> void Store(void* target,const T& value) { std::memcpy(target,&value,sizeof(value)); }
}
int main() {
    void* allocation=nullptr;
    try {
        constexpr size_t bytes=0x1100000;
        allocation=VirtualAlloc(nullptr,bytes,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
        Check(allocation!=nullptr,"Cannot allocate synthetic image");
        auto* image=static_cast<uint8_t*>(allocation);
        mxb::g_base=reinterpret_cast<uintptr_t>(image); mxb::g_baseResolved=true;
        std::array<uint8_t,36> registry{},collection{};
        std::array<uint8_t,16> localEntry{},remoteEntry{};
        std::array<uint8_t,48> world{};
        std::vector<uint8_t> localBike(mxb::off::bike::kSize),remoteBike(mxb::off::bike::kSize);
        Store<int32_t>(image+0xE62710,1); Store<int32_t>(image+0xF4EE24,11);
        Store<uint8_t*>(image+0x567770,registry.data());
        Store<int32_t>(registry.data(),2);
        Store<int32_t>(registry.data()+4,5); Store<uint8_t*>(registry.data()+12,remoteEntry.data());
        Store<int32_t>(registry.data()+20,11); Store<uint8_t*>(registry.data()+28,localEntry.data());
        Store<int32_t>(localEntry.data(),1002); Store<uint8_t*>(localEntry.data()+8,world.data());
        Store<uint8_t*>(world.data()+40,collection.data());
        Store<int32_t>(collection.data(),2);
        Store<int32_t>(collection.data()+4,1001); Store<uint8_t*>(collection.data()+12,remoteBike.data());
        Store<int32_t>(collection.data()+20,1002); Store<uint8_t*>(collection.data()+28,localBike.data());
        const std::vector<uint8_t> before(image,image+bytes);
        const auto registryBefore=registry,collectionBefore=collection;
        Check(mxb::LocalBike()==localBike.data(),"Local bike did not resolve through both handles");
        Check(std::memcmp(before.data(),image,bytes)==0 && registry==registryBefore && collection==collectionBefore,
              "Lookup wrote game data");
        Store<int32_t>(image+0xE62710,0); Check(!mxb::LocalBike(),"Zero player index accepted");
        Store<int32_t>(image+0xE62710,51); Check(!mxb::LocalBike(),"Out of bounds player index accepted");
        Store<int32_t>(image+0xE62710,1); Store<int32_t>(image+0xF4EE24,9);
        Check(!mxb::LocalBike(),"Unknown player handle accepted"); Store<int32_t>(image+0xF4EE24,11);
        Store<int32_t>(registry.data(),513); Check(!mxb::LocalBike(),"Invalid registry count accepted");
        Store<int32_t>(registry.data(),2); Store<uint8_t*>(localEntry.data()+8,nullptr);
        Check(!mxb::LocalBike(),"Null world accepted"); Store<uint8_t*>(localEntry.data()+8,world.data());
        Store<int32_t>(collection.data(),0); Check(!mxb::LocalBike(),"Empty bike collection accepted");
        VirtualFree(allocation,0,MEM_RELEASE); allocation=nullptr;
        std::puts("PASS: local-player and world bike handle resolution, remote-first collections, read-only lookup, invalid index/handle/count/world guards.");
        return 0;
    } catch(const std::exception& e) {
        if(allocation) VirtualFree(allocation,0,MEM_RELEASE);
        std::fprintf(stderr,"FAIL: %s\n",e.what()); return 1;
    }
}
