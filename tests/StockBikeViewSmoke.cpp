#include <windows.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

#include "mxb/StockBikeView.h"
#include "mxb/Modifiers.h"
#include "mxb/Process.h"
#include "mxb/Session.h"
#include "mxb/CrashPatch.h"

namespace {
std::vector<uint8_t> image(mxb::off::bike::kSize);
bool present = true;
std::array<mxb::CurvePoint, 2> torque{{{100, 10}, {200, 20}}};
std::array<mxb::CurvePoint, 2> brake{{{100, -2}, {200, -4}}};
std::array<mxb::CurvePoint, 2> torque2{{{100, 8}, {200, 18}}};
std::array<mxb::CurvePoint, 2> brake2{{{100, -1}, {200, -3}}};
template<class T> void Store(size_t offset, const T& value) { std::memcpy(image.data()+offset,&value,sizeof(value)); }
void Check(bool okay, const char* what) { if (!okay) throw std::runtime_error(what); }
void Near(double a, double b) { if (std::abs(a-b)>0.0001) throw std::runtime_error("Value mismatch"); }
double ParameterSum(mxb::BikeData& bike) {
    return bike.activeTyre(0).pDx1()+bike.activeTyre(1).pDy1()+bike.frontSpring().base+
           bike.clutchMaxTorque()+bike.torqueCurve(0)->points[0].y+bike.engineMaxRPM()+bike.aeroMinCdA();
}
}

namespace mxb {
uintptr_t ModuleBase() { return reinterpret_cast<uintptr_t>(image.data()); }
bool WritesBlocked() { return false; }
bool IsReadable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION region{};
    return p && VirtualQuery(p,&region,sizeof(region)) && region.State==MEM_COMMIT &&
           !(region.Protect&(PAGE_NOACCESS|PAGE_GUARD)) && reinterpret_cast<uintptr_t>(p)+n<=
           reinterpret_cast<uintptr_t>(region.BaseAddress)+region.RegionSize;
}
std::vector<BikeData> LoadedBikes() { return present ? std::vector<BikeData>{BikeData(image.data())} : std::vector<BikeData>{}; }
namespace host { void Logf(const char*, ...) {} }
namespace crash {
bool Apply(float) { return true; }
bool SetNoCrash(bool) { return true; }
void Restore() {}
}
}

// Replay a read-only process capture in this test process. Game code is never
// invoked; external curves and embedded tyre pointers are relocated locally.
void ReplayLiveFixture(const char* path) {
    using namespace mxb;
    std::ifstream file(path,std::ios::binary);
    auto read=[&](void* p,size_t n) { file.read(static_cast<char*>(p),n); Check(bool(file),"Truncated fixture"); };
    char magic[8]{}; uint32_t bytes=0,count=0; uint64_t originalBase=0;
    read(magic,8); read(&bytes,4); read(&count,4); read(&originalBase,8);
    Check(std::memcmp(magic,"MXBLIVE2",8)==0 && bytes==145896 && count>=1 && count<=5,"Invalid fixture header");
    image.resize(bytes); read(image.data(),bytes);
    std::vector<std::vector<CurvePoint>> curves(count*2);
    for (unsigned m=0;m<count;++m) for(unsigned which=0;which<2;++which) {
        uint32_t points=0; read(&points,4); Check(points>0 && points<=4096,"Invalid fixture curve size");
        auto& curve=curves[m*2+which]; curve.resize(points); read(curve.data(),points*sizeof(CurvePoint));
        auto* record=reinterpret_cast<Curve*>(image.data()+2232+264*m+24*which);
        Check(record->type==12 && record->numPoints==static_cast<int>(points),"Fixture curve layout changed");
        record->points=curve.data();
    }
    for (size_t offset : {size_t(73960),size_t(132088)}) {
        uint64_t active=0; std::memcpy(&active,image.data()+offset,8);
        Check(active>=originalBase && active-originalBase+4912<=bytes,"Invalid fixture tyre pointer");
        Store<uint8_t*>(offset,image.data()+active-originalBase);
    }
    const auto baseline=image;
    const auto originalCurves=curves;
    Get()=Settings{}; Get().crash.enabled=false;
    Get().grip.peakLong=1.7f; Get().grip.surfaceDry=1.3f;
    Get().engine.torque=1.8f; Get().engine.engineBraking=1.6f; Get().engine.revRange=1.2f;
    Get().engine.topSpeed=1.3f; Get().engine.clutchTorque=1.4f;
    Get().suspension.frontSpring=1.2f; Get().suspension.rearDamping=1.3f;
    Get().transmission.gearRatio=1.3f; Get().transmission.finalDrive=1.1f;
    Tick();
    Check(image!=baseline,"Replay modifiers did not alter bike");
    const auto modified=image;
    const auto modifiedCurves=curves;
    StockBikeView stock; const char* failure=nullptr;
    if (!BuildStockBikeView(image.data(),stock,&failure)) throw std::runtime_error(failure);
    auto normalized=stock.bytes;
    for(unsigned m=0;m<count;++m) for(unsigned which=0;which<2;++which) {
        const size_t offset=2232+264*m+24*which;
        const auto* restored=reinterpret_cast<const Curve*>(stock.bytes.data()+offset);
        const auto& original=originalCurves[m*2+which];
        Check(std::memcmp(restored->points,original.data(),original.size()*sizeof(CurvePoint))==0,"Replay curve restoration differs from baseline");
        std::memcpy(normalized.data()+offset+16,baseline.data()+offset+16,8);
        Check(std::memcmp(curves[m*2+which].data(),modifiedCurves[m*2+which].data(),original.size()*sizeof(CurvePoint))==0,
              "Replay stock view wrote live curves");
    }
    for(size_t offset : {size_t(73960),size_t(132088)}) std::memcpy(normalized.data()+offset,baseline.data()+offset,8);
    Check(normalized==baseline,"Replay stock view differs from captured baseline");
    Check(image==modified,"Replay stock view wrote live bike");
    RestoreAll();
    std::puts("PASS: captured live bike replay, byte-exact restoration, relocated mappings/selected tyres, unchanged live copy and curves.");
}

int main(int argc, char** argv) {
    try {
        using namespace mxb;
        BikeData bike(image.data());
        // Literal locations from the executable's checksum and the live memory
        // sample, independently of the production offset constants. The old
        // fixture initialized through accessors and therefore hid wrong offsets.
        Store<int32_t>(2020,2);
        Store<Curve>(2232,{12,0,2,0,torque.data()});
        Store<Curve>(2256,{12,0,2,0,brake.data()});
        Store<Curve>(2496,{12,0,2,0,torque2.data()});
        Store<Curve>(2520,{12,0,2,0,brake2.data()});
        Check(bike.numMappings()==2 && bike.torqueCurve(0)->points==torque.data() &&
              bike.brakeCurve(1)->points==brake2.data(),"Live engine layout mismatch");
        Store<float>(3648,226.66667f); Store<float>(3652,200);
        Store<double>(3504,111); Store<double>(145576,0.408); Store<double>(145584,0.456);
        Near(bike.engineMaxRPM(),226.66667); Near(bike.engineShiftRPM(),200);
        Near(bike.clutchMaxTorque(),111); Near(bike.aeroMinCdA(),0.408); Near(bike.aeroMaxCdA(),0.456);
        bike.clutchMaxTorque()=100; bike.engineMaxRPM()=1000; bike.engineShiftRPM()=900;
        Store<int32_t>(off::bike::kNumLimiter,1); bike.limiter(0)=1100;
        bike.aeroMinCdA()=0.3; bike.aeroMaxCdA()=0.4;
        bike.frontSpring()={30000,1000,5,2}; bike.rearSpring()={40000,1000,5,3};
        bike.frontBumpThr0()=10; bike.frontBumpThr1()=20; bike.frontReboundThr0()=30; bike.frontReboundThr1()=40;
        bike.rearBumpThr0()=50; bike.rearBumpThr1()=60; bike.rearReboundThr0()=70; bike.rearReboundThr1()=80;
        bike.frontTopOutK()=10; bike.rearTopOutK()=20; bike.rearBumpStopK()=30; bike.rearBumpStopDamp()=40;
        bike.upShiftTime()=0.05f; bike.downShiftTime()=0.06f; bike.engageTime()=0.07f; bike.disengageTime()=0.08f;
        bike.drivelineEff()=0.9; bike.firstAxisInertia()=1; bike.secondAxisInertia()=2;
        Store<int32_t>(3664,1); Store<int32_t>(3668,10); Store<int32_t>(3672,20);
        Store<int32_t>(13528,1); Store<int32_t>(13532,48);
        Store<uint32_t>(145892,0x12345678);
        for(int wheel=0;wheel<2;++wheel) {
            Store<int32_t>(off::bike::kTyreCount0+wheel*off::bike::kWheelStride,wheel+1);
            for (int slot=0;slot<=wheel;++slot) {
            Tyre tyre=bike.tyreSlot(wheel,slot);
            if (slot==wheel) Store<uint8_t*>(off::bike::kActiveTyrePtr0+wheel*off::bike::kWheelStride,tyre.raw());
            *reinterpret_cast<int32_t*>(tyre.raw()+off::tyre::kNumCompounds)=1;
            tyre.pDx1()=1.2f; tyre.pDy1()=1.1f; tyre.springBase()=10000; tyre.damper()=10;
            for(int surface=0;surface<off::tyre::kNumSurfaces;++surface) {
                tyre.surfaceField(0,surface,off::tyre::kDryGrip)=1;
                tyre.surfaceField(0,surface,off::tyre::kWetGrip)=0.8f;
            }
            }
        }
        const auto pristineBytes=image;
        const double stockSum=ParameterSum(bike);
        StockBikeView absent;
        Check(!BuildStockBikeView(image.data(),absent),"Missing baseline was accepted");
        Settings& settings=Get();
        settings.grip.peakLong=2; settings.grip.peakLat=1.5f;
        settings.engine.torque=2; settings.engine.topSpeed=2; settings.engine.revRange=1.2f;
        settings.suspension.frontSpring=1.5f; settings.transmission.gearRatio=1.2f;
        settings.crash.enabled=false;
        Tick();
        Check(std::abs(ParameterSum(bike)-stockSum)>1,"Test modifiers did not change values");
        const auto liveBefore=image; const auto torqueBefore=torque; const auto brakeBefore=brake;
        StockBikeView stock;
        Check(BuildStockBikeView(image.data(),stock),"Stock view unavailable");
        BikeData shadow(stock.data());
        Near(ParameterSum(shadow),stockSum);
        Near(shadow.activeTyre(0).pDx1(),1.2); Near(shadow.activeTyre(1).pDy1(),1.1);
        Near(shadow.torqueCurve(0)->points[0].y,10); Near(shadow.brakeCurve(0)->points[0].y,-2);
        Check(shadow.torqueCurve(0)->points!=torque.data(),"Shadow curve aliases live torque curve");
        Check(shadow.activeTyre(0).raw()==shadow.tyreSlot(0,0).raw(),"Active tyre was not rebased");
        Check(shadow.activeTyre(1).raw()==shadow.tyreSlot(1,1).raw(),"Selected rear tyre was not rebased");
        Near(shadow.torqueCurve(1)->points[1].y,18);
        Check(stock.bytes.size()>=145896,"Checksum/validation tail truncated");
        auto normalized=stock.bytes;
        // Remove intentional pointer rebasing, then require an exact stock copy.
        for (size_t offset : {size_t(2248),size_t(2272),size_t(2512),size_t(2536),size_t(73960),size_t(132088)})
            std::memcpy(normalized.data()+offset,pristineBytes.data()+offset,sizeof(void*));
        Check(normalized==pristineBytes,"Stock view was not byte-exact");
        Check(shadow.frontSpring().setting==2,"Setup selection was overwritten");
        Check(image==liveBefore && std::memcmp(torque.data(),torqueBefore.data(),sizeof(torque))==0 &&
              std::memcmp(brake.data(),brakeBefore.data(),sizeof(brake))==0,"Building view changed live physics");
        Store<int32_t>(off::bike::kNumMappings,3);
        Check(!BuildStockBikeView(image.data(),absent),"Changed mapping layout was accepted");
        Store<int32_t>(off::bike::kNumMappings,2);
        const auto curveSource=bike.torqueCurve(0)->points;
        std::array<CurvePoint,2> replacement=torque;
        bike.torqueCurve(0)->points=replacement.data();
        Check(!BuildStockBikeView(image.data(),absent),"Replaced curve allocation was accepted");
        bike.torqueCurve(0)->points=curveSource;
        Store<uint32_t>(off::bike::kEngineId,1);
        Check(!BuildStockBikeView(image.data(),absent),"Changed engine identity was accepted");
        Store<uint32_t>(off::bike::kEngineId,0);
        const auto active=bike.activeTyre(0).raw();
        Store<uint8_t*>(off::bike::kActiveTyrePtr0,nullptr);
        Check(!BuildStockBikeView(image.data(),absent),"Unresolved active tyre was accepted");
        Store<uint8_t*>(off::bike::kActiveTyrePtr0,active);
        RestoreAll();
        Check(!BuildStockBikeView(image.data(),absent),"Released baseline remained usable");
        if(argc==2) ReplayLiveFixture(argv[1]);
        std::puts("PASS: stock parameter view, independent curves/tyres, unchanged live physics, preserved setup selection, missing/stale baseline guards.");
        return 0;
    } catch(const std::exception& e) { std::fprintf(stderr,"FAIL: %s\n",e.what()); return 1; }
}
