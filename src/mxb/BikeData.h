#pragma once
#include <cstdint>
#include <vector>

#include "Offsets.h"

namespace mxb {

// Field view over a loaded bike physics object. The game owns the memory; this is
// just typed access to it. Never cache a BikeData across a track reload — the
// registry hands out fresh pointers.

struct SettingRange {
    float   base;
    float   step;
    int32_t numSteps;
    int32_t setting;

    float value() const { return base + step * static_cast<float>(setting); }
};

struct CurvePoint {
    float x;  // revolutions/second
    float y;  // Nm
};

struct Curve {
    int32_t     type;  // 12
    int32_t     _pad0;
    int32_t     numPoints;
    int32_t     _pad1;
    CurvePoint* points;
};

class Tyre {
public:
    explicit Tyre(uint8_t* base) : base_(base) {}

    bool valid() const { return base_ != nullptr; }
    uint8_t* raw() const { return base_; }

    float& pDx1() { return at<float>(off::tyre::kPDx1); }
    float& pDy1() { return at<float>(off::tyre::kPDy1); }

    int numCompounds() const {
        const int n = at<int32_t>(off::tyre::kNumCompounds);
        return (n > 0 && n <= off::tyre::kMaxCompounds) ? n : 0;
    }

    uint8_t* compound(int c) const {
        return base_ + off::tyre::kCompoundBase + off::tyre::kCompoundStride * static_cast<size_t>(c);
    }

    // field is one of off::tyre::kDryGrip / kWetGrip / kLongStiffness / ...
    float& surfaceField(int c, int surface, size_t field) const {
        return *reinterpret_cast<float*>(compound(c) + field + 32u * static_cast<size_t>(surface));
    }

    double& springBase()   { return at<double>(off::tyre::kSpringBase); }
    double& springFactor() { return at<double>(off::tyre::kSpringFactor); }
    double& damper()       { return at<double>(off::tyre::kDamper); }

private:
    template <typename T>
    T& at(size_t o) const { return *reinterpret_cast<T*>(base_ + o); }

    uint8_t* base_;
};

class BikeData {
public:
    explicit BikeData(void* base) : base_(static_cast<uint8_t*>(base)) {}

    bool valid() const { return base_ != nullptr; }
    uint8_t* raw() const { return base_; }

    // ---- engine ----
    int numMappings() const {
        const int n = at<int32_t>(off::bike::kNumMappings);
        return (n > 0 && n <= off::bike::kMaxMappings) ? n : 0;
    }
    uint8_t* mapping(int i) const {
        return base_ + off::bike::kMappingBase + off::bike::kMappingStride * static_cast<size_t>(i);
    }
    Curve* torqueCurve(int i) const {
        return reinterpret_cast<Curve*>(mapping(i) + off::bike::kMappingTorque);
    }
    Curve* brakeCurve(int i) const {
        return reinterpret_cast<Curve*>(mapping(i) + off::bike::kMappingBrake);
    }
    double& clutchMaxTorque() { return at<double>(off::bike::kClutchMaxTorque); }
    float&  engineMaxRPM()    { return at<float>(off::bike::kEngineMaxRPM); }
    float&  engineShiftRPM()  { return at<float>(off::bike::kEngineShiftRPM); }

    // Rev limiters: engine/NumLimiter entries of engine/Limiter%d, doubles in
    // the same revolutions/second units as the RPM fields above. The array ends
    // at the mapping count; accepting a larger count would overwrite mappings.
    int numLimiter() const {
        const int n = at<int32_t>(off::bike::kNumLimiter);
        return (n > 0 && n <= 3) ? n : 0;
    }
    double& limiter(int i) {
        return *reinterpret_cast<double*>(base_ + off::bike::kLimiter0 +
                                          8u * static_cast<size_t>(i));
    }

    // Drag coefficient times frontal area: what sets top speed.
    double& aeroMinCdA() { return at<double>(off::bike::kAeroMinCdA); }
    double& aeroMaxCdA() { return at<double>(off::bike::kAeroMaxCdA); }


    // ---- gearbox / driveline ----
    int    numGears() const    { return at<int32_t>(off::bike::kNumGears); }
    float& upShiftTime()       { return at<float>(off::bike::kUpShiftTime); }
    float& downShiftTime()     { return at<float>(off::bike::kDownShiftTime); }
    float& disengageTime()     { return at<float>(off::bike::kDisengageTime); }
    float& engageTime()        { return at<float>(off::bike::kEngageTime); }
    float& disengageMaxTorque(){ return at<float>(off::bike::kDisengageMaxTq); }
    double& drivelineEff()     { return at<double>(off::bike::kDrivelineEff); }
    double& firstAxisInertia() { return at<double>(off::bike::kFirstAxisInert); }
    double& secondAxisInertia(){ return at<double>(off::bike::kSecondAxisInert); }

    // ---- suspension ----
    SettingRange& frontSpring()  { return at<SettingRange>(off::bike::kFrontSpring); }
    SettingRange& rearSpring()   { return at<SettingRange>(off::bike::kRearSpring); }
    SettingRange& frontOil()     { return at<SettingRange>(off::bike::kFrontOil); }
    SettingRange& frontPreload() { return at<SettingRange>(off::bike::kFrontPreload); }
    SettingRange& rearPreload()  { return at<SettingRange>(off::bike::kRearPreload); }

    double& frontBumpThr0()    { return at<double>(off::bike::kFrontBumpThr0); }
    double& frontBumpThr1()    { return at<double>(off::bike::kFrontBumpThr1); }
    double& frontReboundThr0() { return at<double>(off::bike::kFrontReboundThr0); }
    double& frontReboundThr1() { return at<double>(off::bike::kFrontReboundThr1); }
    double& rearBumpThr0()     { return at<double>(off::bike::kRearBumpThr0); }
    double& rearBumpThr1()     { return at<double>(off::bike::kRearBumpThr1); }
    double& rearReboundThr0()  { return at<double>(off::bike::kRearReboundThr0); }
    double& rearReboundThr1()  { return at<double>(off::bike::kRearReboundThr1); }

    float& frontTopOutK()      { return at<float>(off::bike::kFrontTopOutK); }
    float& rearTopOutK()       { return at<float>(off::bike::kRearTopOutK); }
    float& rearBumpStopK()     { return at<float>(off::bike::kRearBumpStopK); }
    float& rearBumpStopDamp()  { return at<float>(off::bike::kRearBumpStopDamp); }

    // ---- tyres ----
    // The pointer the solver actually reads for this wheel.
    Tyre activeTyre(int wheel) const {
        const size_t o = off::bike::kActiveTyrePtr0 + off::bike::kWheelStride * static_cast<size_t>(wheel);
        return Tyre(*reinterpret_cast<uint8_t**>(base_ + o));
    }
    int numTyreSlots(int wheel) const {
        const size_t o = off::bike::kTyreCount0 + off::bike::kWheelStride * static_cast<size_t>(wheel);
        const int n = *reinterpret_cast<const int32_t*>(base_ + o);
        return (n > 0 && n <= off::bike::kMaxTyres) ? n : 0;
    }
    // Every tyre record loaded for this wheel, so a modifier survives a tyre swap
    // in the setup screen.
    Tyre tyreSlot(int wheel, int i) const {
        return Tyre(base_ + off::bike::kTyreArray0
                          + off::bike::kWheelStride * static_cast<size_t>(wheel)
                          + off::bike::kTyreStride  * static_cast<size_t>(i));
    }
    double& wheelFriction(int wheel) {
        return at<double>(off::bike::kWheelFriction0 + off::bike::kWheelStride * static_cast<size_t>(wheel));
    }

    // ---- placement ----
    // World position: the midpoint of the two wheel positions, which is the
    // engine's own reference point for a bike.
    //
    // Deliberately NOT bike+928's translation. That matrix is the inverse
    // transform, so its translation is -R*pos -- right magnitude, scrambled
    // axes, which parked the china hat at a fixed point far off the map. The
    // wheel positions are plain world-space float[3]; see off::bike::kWheelPos0.
    void worldPos(float out[3]) const {
        const float* w0 = reinterpret_cast<const float*>(base_ + off::bike::kWheelPos0);
        const float* w1 = reinterpret_cast<const float*>(base_ + off::bike::kWheelPos0 +
                                                         off::bike::kWheelStride);
        out[0] = (w0[0] + w1[0]) * 0.5f;
        out[1] = (w0[1] + w1[1]) * 0.5f;
        out[2] = (w0[2] + w1[2]) * 0.5f;
    }

    // ---- rider ----
    int32_t& crashedFlag() { return at<int32_t>(off::bike::kCrashedFlag); }
    int32_t& riderState()  { return at<int32_t>(off::bike::kRiderState); }

private:
    template <typename T>
    T& at(size_t o) const { return *reinterpret_cast<T*>(base_ + o); }

    uint8_t* base_;
};

}  // namespace mxb
