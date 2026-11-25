#include "Modifiers.h"

#include <chrono>
#include <cmath>
#include <iterator>
#include <unordered_map>
#include <utility>
#include <vector>

#include "BikeData.h"
#include "CrashPatch.h"
#include "Offsets.h"
#include "Process.h"
#include "Session.h"
#include "StockBikeView.h"
#include "host/Log.h"

namespace mxb {
namespace {

// Stock values for one bike, captured the first time we see its pointer.
struct Snapshot {
    struct TyreStock {
        float  pDx1 = 0, pDy1 = 0;
        double springBase = 0, damper = 0;
        std::vector<float> dryGrip;  // numCompounds * kNumSurfaces
        std::vector<float> wetGrip;
    };
    std::vector<TyreStock> tyres[2];  // [wheel][slot]

    // suspension
    SettingRange frontSpring{}, rearSpring{};
    double frontBump0 = 0, frontBump1 = 0, frontReb0 = 0, frontReb1 = 0;
    double rearBump0 = 0, rearBump1 = 0, rearReb0 = 0, rearReb1 = 0;
    float  frontTopOutK = 0, rearTopOutK = 0, rearBumpStopK = 0, rearBumpStopDamp = 0;

    // engine
    std::vector<std::vector<float>> torqueY;  // [mapping][point]
    std::vector<std::vector<float>> brakeY;
    std::vector<const CurvePoint*> torqueSource, brakeSource;
    uint32_t engineIdentity = 0;
    double clutchMaxTorque = 0;
    float  maxRPM = 0, shiftRPM = 0;
    std::vector<double> limiters;
    double aeroMinCdA = 0, aeroMaxCdA = 0;

    // True while the engine section last wrote something other than stock.
    // Without it, dragging a slider back to exactly 1.0 stops the writes and
    // leaves the bike on the last scaled values forever, because "multiplier is
    // 1" and "already restored" are not the same state.
    bool engineDirty = false;

    // Whether the captured engine scalars are plausible. A snapshot taken
    // before the cfg finished loading would otherwise be multiplied into the
    // live engine every frame.
    bool engineScalarsOk = false;

    // Same reason as engineDirty: the gearing tables are only rewritten when a
    // multiplier is off-stock, so without this, returning one to exactly 1.0
    // would leave the gearbox on its last scaled ratios.
    bool transDirty = false;

    // transmission
    std::vector<std::pair<int32_t, int32_t>> gearing;  // driving, driven teeth
    std::vector<int32_t> rearSprocket;
    float  upShift = 0, downShift = 0, engage = 0, disengage = 0;
    double drivelineEff = 0, firstAxisInertia = 0, secondAxisInertia = 0;

    bool captured = false;

    // When this bike was first seen crashed, for the auto-recover delay.
    std::chrono::steady_clock::time_point crashedAt{};
    bool crashSeen = false;

    // Last values seen by the state trace, so changes can be logged as they happen.
    int32_t lastFlag = -1;
    int32_t lastState = -1;
};

std::unordered_map<void*, Snapshot> g_stock;
Settings g_settings;
Status   g_status;

enum Section { kGrip = 0, kSusp, kEngine, kTrans, kCrash, kNumSections };

// Section-level "was on last tick", so turning one off restores just that section.
bool g_wasOn[kNumSections] = {};

// Every value we write goes through this. A zero, negative or non-finite number
// reaching the solver is not a small error: a NaN propagates into the bike's
// position, and NaN converted to an integer is INT_MIN, which sails straight
// past the terrain sampler's bounds clamp and faults on an out-of-range
// heightmap read. That is the crash signature we actually saw.
//
// On anything suspicious we write the stock value instead, so the worst case is
// "the slider did nothing" rather than a dead game.
template <typename T>
T Scaled(T stock, double mult, double lo, double hi) {
    if (!std::isfinite(mult)) return stock;
    const double v = static_cast<double>(stock) * mult;
    if (!std::isfinite(v)) return stock;
    if (v < lo) return static_cast<T>(lo);
    if (v > hi) return static_cast<T>(hi);
    return static_cast<T>(v);
}

// Positive-definite fields: a zero here divides by zero somewhere downstream.
template <typename T>
T ScaledPositive(T stock, double mult, double floorValue) {
    return Scaled<T>(stock, mult, floorValue, 1e12);
}

// Calls the game's own rider reset.
//
// This module's rule is to read and write game memory but not call game code,
// after a guessed three-argument signature wrote a string through a garbage
// pointer on the first inject. This call is a deliberate exception and a
// different situation: the signature is read straight off the disassembly --
// one pointer in rcx, no float arguments, int return -- it is invoked exactly as
// the game invokes it at 0x140197A45, and it runs on the same thread, since the
// swap hook and the physics both live on the main thread.
//
// Everything it touches is inside the bike object except one pointer at
// the rider body pointer, which it dereferences, so that is checked first.
bool RiderReset(BikeData& bike) {
    const uintptr_t fn = Rva(off::kRiderReset);
    if (!fn) return false;
    if (!IsReadable(reinterpret_cast<void*>(fn), 16)) return false;

    void* body = *reinterpret_cast<void* const*>(bike.raw() + off::bike::kRiderBody);
    if (!IsReadable(body, sizeof(void*))) return false;

    using RiderResetFn = int32_t(__fastcall*)(void*);
    reinterpret_cast<RiderResetFn>(fn)(bike.raw());
    return true;
}

int32_t* GearingPair(BikeData& b, int i) {
    return reinterpret_cast<int32_t*>(b.raw() + off::bike::kGearingPairs + 8 * static_cast<size_t>(i));
}
int32_t GearingCount(BikeData& b) {
    const int32_t n = *reinterpret_cast<int32_t*>(b.raw() + off::bike::kGearingCount);
    return (n > 0 && n <= 100) ? n : 0;
}
int32_t* RearSprocketTable(BikeData& b) {
    return reinterpret_cast<int32_t*>(b.raw() + off::bike::kRearSprocketTable);
}
int32_t RearSprocketCount(BikeData& b) {
    const int32_t n = *reinterpret_cast<int32_t*>(b.raw() + off::bike::kRearSprocketCount);
    return (n > 0 && n <= 100) ? n : 0;
}

void CaptureTyres(BikeData& bike, Snapshot& s) {
    for (int w = 0; w < 2; ++w) {
        const int slots = bike.numTyreSlots(w);
        s.tyres[w].resize(static_cast<size_t>(slots));
        for (int i = 0; i < slots; ++i) {
            Tyre t = bike.tyreSlot(w, i);
            auto& st = s.tyres[w][static_cast<size_t>(i)];
            st.pDx1 = t.pDx1();
            st.pDy1 = t.pDy1();
            st.springBase = t.springBase();
            st.damper = t.damper();

            const int nc = t.numCompounds();
            st.dryGrip.resize(static_cast<size_t>(nc) * off::tyre::kNumSurfaces);
            st.wetGrip.resize(st.dryGrip.size());
            for (int c = 0; c < nc; ++c) {
                for (int sf = 0; sf < off::tyre::kNumSurfaces; ++sf) {
                    const size_t k = static_cast<size_t>(c) * off::tyre::kNumSurfaces + sf;
                    st.dryGrip[k] = t.surfaceField(c, sf, off::tyre::kDryGrip);
                    st.wetGrip[k] = t.surfaceField(c, sf, off::tyre::kWetGrip);
                }
            }
        }
    }
}

void Capture(BikeData& bike, Snapshot& s) {
    CaptureTyres(bike, s);

    s.frontSpring = bike.frontSpring();
    s.rearSpring  = bike.rearSpring();
    s.frontBump0 = bike.frontBumpThr0();    s.frontBump1 = bike.frontBumpThr1();
    s.frontReb0  = bike.frontReboundThr0(); s.frontReb1  = bike.frontReboundThr1();
    s.rearBump0  = bike.rearBumpThr0();     s.rearBump1  = bike.rearBumpThr1();
    s.rearReb0   = bike.rearReboundThr0();  s.rearReb1   = bike.rearReboundThr1();
    s.frontTopOutK = bike.frontTopOutK();
    s.rearTopOutK  = bike.rearTopOutK();
    s.rearBumpStopK    = bike.rearBumpStopK();
    s.rearBumpStopDamp = bike.rearBumpStopDamp();

    const int nm = bike.numMappings();
    s.engineIdentity = *reinterpret_cast<const uint32_t*>(bike.raw() + off::bike::kEngineId);
    s.torqueY.resize(static_cast<size_t>(nm));
    s.brakeY.resize(static_cast<size_t>(nm));
    s.torqueSource.resize(static_cast<size_t>(nm));
    s.brakeSource.resize(static_cast<size_t>(nm));
    for (int m = 0; m < nm; ++m) {
        for (int which = 0; which < 2; ++which) {
            Curve* c = (which == 0) ? bike.torqueCurve(m) : bike.brakeCurve(m);
            auto& dst = (which == 0) ? s.torqueY[static_cast<size_t>(m)]
                                     : s.brakeY[static_cast<size_t>(m)];
            if (!c || c->numPoints <= 0 || c->numPoints > 4096) continue;
            if (!IsReadable(c->points, sizeof(CurvePoint) * static_cast<size_t>(c->numPoints))) continue;
            (which == 0 ? s.torqueSource : s.brakeSource)[static_cast<size_t>(m)] = c->points;
            dst.resize(static_cast<size_t>(c->numPoints));
            for (int k = 0; k < c->numPoints; ++k) dst[static_cast<size_t>(k)] = c->points[k].y;
        }
    }
    s.clutchMaxTorque = bike.clutchMaxTorque();
    s.maxRPM   = bike.engineMaxRPM();
    s.shiftRPM = bike.engineShiftRPM();
    const int nl = bike.numLimiter();
    s.limiters.resize(static_cast<size_t>(nl));
    for (int i = 0; i < nl; ++i) s.limiters[static_cast<size_t>(i)] = bike.limiter(i);
    s.aeroMinCdA = bike.aeroMinCdA();
    s.aeroMaxCdA = bike.aeroMaxCdA();

    // These are raw scalars with no pointer to check, so plausibility is the
    // only guard there is. RPM fields hold revolutions/second. CdA for a bike
    // sits well under 5.
    auto sane = [](double v, double lo, double hi) {
        return std::isfinite(v) && v > lo && v < hi;
    };
    s.engineScalarsOk = sane(s.maxRPM, 1.0, 3000.0) && sane(s.shiftRPM, 1.0, 3000.0) &&
                        sane(s.aeroMinCdA, 0.0, 5.0) && sane(s.aeroMaxCdA, 0.0, 5.0);
    for (double l : s.limiters)
        if (!sane(l, 1.0, 3000.0)) s.engineScalarsOk = false;

    host::Logf("capture %p: maxRPM=%.1f shiftRPM=%.1f limiters=%d cdA=%.4f/%.4f scalarsOk=%d",
               bike.raw(), s.maxRPM, s.shiftRPM, static_cast<int>(s.limiters.size()),
               s.aeroMinCdA, s.aeroMaxCdA, s.engineScalarsOk ? 1 : 0);

    const int32_t ng = GearingCount(bike);
    s.gearing.resize(static_cast<size_t>(ng));
    for (int32_t i = 0; i < ng; ++i) {
        const int32_t* p = GearingPair(bike, i);
        s.gearing[static_cast<size_t>(i)] = {p[0], p[1]};
    }
    const int32_t nr = RearSprocketCount(bike);
    s.rearSprocket.assign(RearSprocketTable(bike), RearSprocketTable(bike) + nr);

    s.upShift   = bike.upShiftTime();
    s.downShift = bike.downShiftTime();
    s.engage    = bike.engageTime();
    s.disengage = bike.disengageTime();
    s.drivelineEff      = bike.drivelineEff();
    s.firstAxisInertia  = bike.firstAxisInertia();
    s.secondAxisInertia = bike.secondAxisInertia();

    s.captured = true;
}

// --- apply: always stock * multiplier, never current * multiplier ---------

void ApplyGrip(BikeData& bike, const Snapshot& s, const GripSettings& g, bool on) {
    // At 1.0 every write would put back the value already there, so skip the
    // whole pass. Not just wasted work: the snapshot is keyed by bike pointer,
    // and if the engine recycles an address for a different bike, writing a
    // stale snapshot into it would feed the solver another bike's numbers.
    if (on && g.peakLong == 1.0f && g.peakLat == 1.0f && g.surfaceDry == 1.0f &&
        g.surfaceWet == 1.0f && g.carcassSpring == 1.0f && g.carcassDamper == 1.0f)
        return;

    for (int w = 0; w < 2; ++w) {
        const int slots = static_cast<int>(s.tyres[w].size());
        const int live  = bike.numTyreSlots(w);
        for (int i = 0; i < slots && i < live; ++i) {
            Tyre t = bike.tyreSlot(w, i);
            const auto& st = s.tyres[w][static_cast<size_t>(i)];

            t.pDx1() = on ? ScaledPositive(st.pDx1, g.peakLong, 0.01) : st.pDx1;
            t.pDy1() = on ? ScaledPositive(st.pDy1, g.peakLat, 0.01) : st.pDy1;
            t.springBase() = on ? ScaledPositive(st.springBase, g.carcassSpring, 1.0)
                                : st.springBase;
            t.damper() = on ? ScaledPositive(st.damper, g.carcassDamper, 0.0) : st.damper;

            const int nc = t.numCompounds();
            for (int c = 0; c < nc; ++c) {
                for (int sf = 0; sf < off::tyre::kNumSurfaces; ++sf) {
                    const size_t k = static_cast<size_t>(c) * off::tyre::kNumSurfaces + sf;
                    if (k >= st.dryGrip.size()) continue;
                    t.surfaceField(c, sf, off::tyre::kDryGrip) =
                        on ? ScaledPositive(st.dryGrip[k], g.surfaceDry, 0.001) : st.dryGrip[k];
                    t.surfaceField(c, sf, off::tyre::kWetGrip) =
                        on ? ScaledPositive(st.wetGrip[k], g.surfaceWet, 0.001) : st.wetGrip[k];
                }
            }
        }
    }
}

void ApplySuspension(BikeData& bike, const Snapshot& s, const SuspensionSettings& p, bool on) {
    // Restoring a captured value must be exact, including unused zero fields.
    // Bounds used for active modifications must not change a stock checksum.
    if (!on) {
        bike.frontSpring().base = s.frontSpring.base;
        bike.frontSpring().step = s.frontSpring.step;
        bike.rearSpring().base = s.rearSpring.base;
        bike.rearSpring().step = s.rearSpring.step;
        bike.frontBumpThr0() = s.frontBump0; bike.frontBumpThr1() = s.frontBump1;
        bike.frontReboundThr0() = s.frontReb0; bike.frontReboundThr1() = s.frontReb1;
        bike.rearBumpThr0() = s.rearBump0; bike.rearBumpThr1() = s.rearBump1;
        bike.rearReboundThr0() = s.rearReb0; bike.rearReboundThr1() = s.rearReb1;
        bike.frontTopOutK() = s.frontTopOutK; bike.rearTopOutK() = s.rearTopOutK;
        bike.rearBumpStopK() = s.rearBumpStopK; bike.rearBumpStopDamp() = s.rearBumpStopDamp;
        return;
    }
    if (on && p.frontSpring == 1.0f && p.rearSpring == 1.0f && p.frontDamping == 1.0f &&
        p.rearDamping == 1.0f && p.bumpStop == 1.0f && p.topOut == 1.0f)
        return;

    // Scale base and step together so every clicker position moves with the slider
    // and the setting the rider picked still lands where they expect it to.
    const float fs = on ? p.frontSpring : 1.0f;
    const float rs = on ? p.rearSpring  : 1.0f;
    // A non-positive spring rate makes the suspension solver blow up.
    bike.frontSpring().base = ScaledPositive(s.frontSpring.base, fs, 0.01);
    bike.frontSpring().step = Scaled<float>(s.frontSpring.step, fs, 0.0, 1e9);
    bike.rearSpring().base  = ScaledPositive(s.rearSpring.base, rs, 0.01);
    bike.rearSpring().step  = Scaled<float>(s.rearSpring.step, rs, 0.0, 1e9);

    const double fd = on ? p.frontDamping : 1.0;
    const double rd = on ? p.rearDamping  : 1.0;
    bike.frontBumpThr0()    = Scaled<double>(s.frontBump0, fd, -1e9, 1e9);
    bike.frontBumpThr1()    = Scaled<double>(s.frontBump1, fd, -1e9, 1e9);
    bike.frontReboundThr0() = Scaled<double>(s.frontReb0, fd, -1e9, 1e9);
    bike.frontReboundThr1() = Scaled<double>(s.frontReb1, fd, -1e9, 1e9);
    bike.rearBumpThr0()     = Scaled<double>(s.rearBump0, rd, -1e9, 1e9);
    bike.rearBumpThr1()     = Scaled<double>(s.rearBump1, rd, -1e9, 1e9);
    bike.rearReboundThr0()  = Scaled<double>(s.rearReb0, rd, -1e9, 1e9);
    bike.rearReboundThr1()  = Scaled<double>(s.rearReb1, rd, -1e9, 1e9);

    const float bs = on ? p.bumpStop : 1.0f;
    bike.rearBumpStopK()    = Scaled<float>(s.rearBumpStopK, bs, 0.0, 1e9);
    bike.rearBumpStopDamp() = Scaled<float>(s.rearBumpStopDamp, bs, 0.0, 1e9);

    const float to = on ? p.topOut : 1.0f;
    bike.frontTopOutK() = Scaled<float>(s.frontTopOutK, to, 0.0, 1e9);
    bike.rearTopOutK()  = Scaled<float>(s.rearTopOutK, to, 0.0, 1e9);
}

// A curve is only safe to write through if it still looks like one. Curve_LoadFromCfg
// stamps type 12, and the points array belongs to the config heap, so validate
// both every time rather than trusting a pointer captured earlier.
bool CurveWritable(const Curve* c, size_t expectedPoints) {
    if (!c || c->type != 12) return false;
    if (c->numPoints <= 0 || static_cast<size_t>(c->numPoints) != expectedPoints) return false;
    if (!c->points) return false;
    return IsReadable(c->points, sizeof(CurvePoint) * expectedPoints);
}

void ApplyEngine(BikeData& bike, Snapshot& s, const EngineSettings& e, bool on) {
    const float tq = on ? e.torque : 1.0f;
    const float eb = on ? e.engineBraking : 1.0f;
    const float cl = on ? e.clutchTorque : 1.0f;
    const float rev = on ? e.revRange : 1.0f;
    const float ts = on ? e.topSpeed : 1.0f;

    // Write while anything is off-stock, AND for one more pass after everything
    // returns to stock, so the bike actually lands back on its own numbers.
    const bool want = on && (tq != 1.0f || eb != 1.0f || cl != 1.0f || rev != 1.0f || ts != 1.0f);
    const bool write = want || s.engineDirty || !on;
    s.engineDirty = want;
    if (!write) return;

    const int nm = bike.numMappings();
    for (int m = 0; m < nm && m < static_cast<int>(s.torqueY.size()); ++m) {
        const auto& src = s.torqueY[static_cast<size_t>(m)];
        {
            Curve* c = bike.torqueCurve(m);
            if (CurveWritable(c, src.size()))
                for (int k = 0; k < c->numPoints; ++k)
                    c->points[k].y = on ? Scaled<float>(src[static_cast<size_t>(k)], tq, -1e7, 1e7)
                                        : src[static_cast<size_t>(k)];
        }

        const auto& bsrc = s.brakeY[static_cast<size_t>(m)];
        {
            Curve* bc = bike.brakeCurve(m);
            if (CurveWritable(bc, bsrc.size()))
                for (int k = 0; k < bc->numPoints; ++k)
                    bc->points[k].y = on ? Scaled<float>(bsrc[static_cast<size_t>(k)], eb, -1e7, 1e7)
                                         : bsrc[static_cast<size_t>(k)];
        }
    }

    bike.clutchMaxTorque() = on ? ScaledPositive(s.clutchMaxTorque, cl, 0.1) : s.clutchMaxTorque;

    // Horsepower. The ceiling, the shift point and every limiter move together:
    // raising MaxRPM alone just means the limiter cuts first and nothing
    // changes, which would look exactly like the feature not working.
    // Only if the captured values looked like real engine numbers. Multiplying
    // a garbage snapshot into the live engine every frame is the one way these
    // writes could wreck the bike, since unlike the curves there is no pointer
    // here to sanity-check.
    if (s.engineScalarsOk || !on) {
        bike.engineMaxRPM()   = on ? ScaledPositive(s.maxRPM, rev, 1.0) : s.maxRPM;
        bike.engineShiftRPM() = on ? ScaledPositive(s.shiftRPM, rev, 1.0) : s.shiftRPM;
        const int nl = bike.numLimiter();
        for (int i = 0; i < nl && i < static_cast<int>(s.limiters.size()); ++i)
            bike.limiter(i) = on ? ScaledPositive(s.limiters[static_cast<size_t>(i)], rev, 1.0)
                                 : s.limiters[static_cast<size_t>(i)];
    }

    // Top speed. Drag goes as CdA * v^2, so terminal velocity goes as
    // 1/sqrt(CdA): dividing CdA by the square of the multiplier makes the
    // slider read as the speed multiple it is actually worth.
    if (s.engineScalarsOk || !on) {
        const double inv = (ts > 0.01f) ? 1.0 / (static_cast<double>(ts) * ts) : 1.0;
        bike.aeroMinCdA() = on ? ScaledPositive(s.aeroMinCdA, inv, 0.0001) : s.aeroMinCdA;
        bike.aeroMaxCdA() = on ? ScaledPositive(s.aeroMaxCdA, inv, 0.0001) : s.aeroMaxCdA;
    }
}

void ApplyTransmission(BikeData& bike, Snapshot& s, const TransmissionSettings& t, bool on) {
    const bool gearWant = on && t.gearRatio != 1.0f;
    const bool sprWant  = on && t.finalDrive != 1.0f;
    const bool restore  = s.transDirty || !on;
    s.transDirty = gearWant || sprWant;

    // Gearbox ratios are stored as integer teeth pairs. Rewriting each pair against a
    // fixed 1000-tooth driving gear keeps the ratio continuous instead of quantising
    // it to whole teeth. This table is internal, so nothing renders these numbers.
    const int32_t ng = (gearWant || restore) ? GearingCount(bike) : 0;
    for (int32_t i = 0; i < ng && i < static_cast<int32_t>(s.gearing.size()); ++i) {
        int32_t* p = GearingPair(bike, i);
        const int32_t driving = s.gearing[static_cast<size_t>(i)].first;
        const int32_t driven  = s.gearing[static_cast<size_t>(i)].second;
        if (!on || driving <= 0) {
            p[0] = driving;
            p[1] = driven;
            continue;
        }
        const double ratio = static_cast<double>(driven) / static_cast<double>(driving);
        const double scaled = 1000.0 * ratio * static_cast<double>(t.gearRatio);
        if (!std::isfinite(scaled) || scaled < 1.0 || scaled > 1e7) {
            p[0] = driving;  // leave the gear alone rather than write a degenerate ratio
            p[1] = driven;
            continue;
        }
        p[0] = 1000;
        p[1] = static_cast<int32_t>(std::lround(scaled));
    }

    // Final drive stays on whole teeth, so it steps in roughly 2% increments.
    const int32_t nr = (sprWant || restore) ? RearSprocketCount(bike) : 0;
    int32_t* rst = RearSprocketTable(bike);
    for (int32_t i = 0; i < nr && i < static_cast<int32_t>(s.rearSprocket.size()); ++i) {
        const int32_t stock = s.rearSprocket[static_cast<size_t>(i)];
        if (!on) {
            rst[i] = stock;
            continue;
        }
        const double teeth = static_cast<double>(stock) * static_cast<double>(t.finalDrive);
        // Zero or negative teeth is a divide-by-zero in the driveline.
        rst[i] = (std::isfinite(teeth) && teeth >= 1.0 && teeth < 1e6)
                     ? static_cast<int32_t>(std::lround(teeth))
                     : stock;
    }

    const float sh = (on && t.shiftSpeed  > 0.01f) ? t.shiftSpeed  : 1.0f;
    const float cl = (on && t.clutchSpeed > 0.01f) ? t.clutchSpeed : 1.0f;
    bike.upShiftTime()   = on ? Scaled<float>(s.upShift, 1.0 / sh, 0.0, 10.0) : s.upShift;
    bike.downShiftTime() = on ? Scaled<float>(s.downShift, 1.0 / sh, 0.0, 10.0) : s.downShift;
    bike.engageTime()    = on ? Scaled<float>(s.engage, 1.0 / cl, 0.0, 10.0) : s.engage;
    bike.disengageTime() = on ? Scaled<float>(s.disengage, 1.0 / cl, 0.0, 10.0) : s.disengage;

    // Efficiency above 1.0 would be free energy; the solver is not written for it.
    // Efficiency above 1.0 would be free energy and the solver is not written for
    // it; at exactly 0 the driveline transmits nothing and can divide by zero.
    bike.drivelineEff() = on ? Scaled<double>(s.drivelineEff, t.drivelineEff, 0.01, 1.0) : s.drivelineEff;

    // Inertia is a denominator in the engine/driveline integration.
    const double in = on ? t.drivelineInertia : 1.0;
    bike.firstAxisInertia()  = on ? ScaledPositive(s.firstAxisInertia, in, 1e-6) : s.firstAxisInertia;
    bike.secondAxisInertia() = on ? ScaledPositive(s.secondAxisInertia, in, 1e-6) : s.secondAxisInertia;
}

}  // namespace

Settings& Get() { return g_settings; }

Status GetStatus() { return g_status; }

bool BuildStockBikeView(void* liveBike, StockBikeView& result, const char** failureReason) {
    if (failureReason) *failureReason = "none";
    auto fail = [&](const char* reason) {
        if (failureReason) *failureReason = reason;
        return false;
    };
    const auto found = g_stock.find(liveBike);
    if (found == g_stock.end() || !found->second.captured) return fail("missing_stock_snapshot");
    if (!IsReadable(liveBike, off::bike::kSize)) return fail("bike_memory_unreadable");
    try {
        // Copy current setup selections, then restore only the values captured
        // by this module. Mutate a copy of Snapshot as well: the restore helpers
        // update their dirty flags.
        Snapshot stock = found->second;
        const auto* live = static_cast<const uint8_t*>(liveBike);
        if (*reinterpret_cast<const uint32_t*>(live + off::bike::kEngineId) != stock.engineIdentity)
            return fail("engine_identity_changed");
        result.bytes.assign(live, live + off::bike::kSize);
        result.curves.clear();
        BikeData shadow(result.bytes.data());
        const int mappings = shadow.numMappings();
        const int rawMappings = *reinterpret_cast<const int32_t*>(live + off::bike::kNumMappings);
        if (rawMappings < 1 || rawMappings > off::bike::kMaxMappings) return fail("invalid_mapping_layout");
        if (mappings != static_cast<int>(stock.torqueY.size()) ||
            mappings != static_cast<int>(stock.brakeY.size())) return fail("mapping_count_changed");
        result.curves.reserve(static_cast<size_t>(mappings) * 2);
        for (int m = 0; m < mappings; ++m) {
            Curve* curves[] = {shadow.torqueCurve(m), shadow.brakeCurve(m)};
            const size_t counts[] = {stock.torqueY[m].size(), stock.brakeY[m].size()};
            const CurvePoint* sources[] = {stock.torqueSource[m], stock.brakeSource[m]};
            for (int which = 0; which < 2; ++which) {
                Curve* c = curves[which];
                if (!counts[which]) {
                    if (c->numPoints != 0) return fail("curve_missing_from_snapshot");
                    continue;
                }
                if (!CurveWritable(c, counts[which])) return fail("invalid_curve_layout");
                if (c->points != sources[which]) return fail("curve_allocation_changed");
                result.curves.emplace_back(c->points, c->points + counts[which]);
                c->points = result.curves.back().data();
            }
        }
        for (int wheel = 0; wheel < 2; ++wheel) {
            if (!shadow.numTyreSlots(wheel) ||
                shadow.numTyreSlots(wheel) != static_cast<int>(stock.tyres[wheel].size()))
                return fail("tyre_slot_count_changed");
            const size_t offset = off::bike::kActiveTyrePtr0 + off::bike::kWheelStride * wheel;
            const auto* active = *reinterpret_cast<uint8_t* const*>(live + offset);
            bool located = false;
            for (int i = 0; i < shadow.numTyreSlots(wheel); ++i) {
                const size_t tyreOffset = off::bike::kTyreArray0 + off::bike::kWheelStride * wheel + off::bike::kTyreStride * i;
                if (active == live + tyreOffset) {
                    *reinterpret_cast<uint8_t**>(result.bytes.data() + offset) = result.bytes.data() + tyreOffset;
                    located = true;
                }
                const Tyre tyre = shadow.tyreSlot(wheel, i);
                const size_t expected = stock.tyres[wheel][i].dryGrip.size();
                if (static_cast<size_t>(tyre.numCompounds()) * off::tyre::kNumSurfaces != expected ||
                    expected != stock.tyres[wheel][i].wetGrip.size()) return fail("tyre_compound_count_changed");
                const size_t compoundsEnd = off::tyre::kCompoundBase + off::tyre::kCompoundStride * tyre.numCompounds();
                if (compoundsEnd > off::bike::kTyreStride) return fail("tyre_compounds_out_of_bounds");
            }
            // The checksum follows this pointer. Never leave it pointing at a
            // modified live tyre or invent a default when it cannot be resolved.
            if (!located) return fail("active_tyre_not_resolved");
        }
        ApplyGrip(shadow, stock, g_settings.grip, false);
        ApplySuspension(shadow, stock, g_settings.suspension, false);
        ApplyEngine(shadow, stock, g_settings.engine, false);
        ApplyTransmission(shadow, stock, g_settings.transmission, false);
        return true;
    } catch (...) {
        return fail("stock_view_allocation_failed");
    }
}

void RestoreAll() {
    for (auto& bike : LoadedBikes()) {
        auto it = g_stock.find(bike.raw());
        if (it == g_stock.end() || !it->second.captured) continue;
        ApplyGrip(bike, it->second, g_settings.grip, false);
        ApplySuspension(bike, it->second, g_settings.suspension, false);
        ApplyEngine(bike, it->second, g_settings.engine, false);
        ApplyTransmission(bike, it->second, g_settings.transmission, false);
    }
    crash::Restore();
    g_stock.clear();
    for (bool& b : g_wasOn) b = false;
    g_status.bikesPatched = 0;
}

void Tick() {
    g_status.moduleFound   = ModuleBase() != 0;
    g_status.writesBlocked = WritesBlocked();
    g_status.note = "";

    if (!g_status.moduleFound) {
        g_status.note = "mxbikes.exe not found";
        return;
    }
    if (g_status.writesBlocked) {
        // Online, or the session state could not be read. Put everything back and
        // stay out of the way until we are positively offline again.
        if (!g_stock.empty()) RestoreAll();
        g_status.note = "online or unknown session - writes blocked";
        return;
    }

    auto bikes = LoadedBikes();
    g_status.bikesSeen = static_cast<int>(bikes.size());

    const bool master = g_settings.masterEnabled;
    const bool on[kNumSections] = {
        master && g_settings.grip.enabled,
        master && g_settings.suspension.enabled,
        master && g_settings.engine.enabled,
        master && g_settings.transmission.enabled,
        master && g_settings.crash.enabled,
    };

    // Per-bike state trace. Logs on every crashed-flag or rider-state change, and
    // otherwise once a second, with the bike transform so a position going wild
    // or non-finite is visible in the log rather than inferred from a fault
    // address afterwards.
    {
        static auto lastTrace = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        const bool due = (now - lastTrace) >= std::chrono::seconds(1);
        if (due) lastTrace = now;

        for (auto& bike : bikes) {
            Snapshot& st = g_stock[bike.raw()];
            const int32_t flag = bike.crashedFlag();
            const int32_t state = bike.riderState();
            const bool changed = (flag != st.lastFlag) || (state != st.lastState);
            st.lastFlag = flag;
            st.lastState = state;
            if (!changed && !due) continue;

            // The bike's transform is logged raw rather than guessing
            // which triplet is the translation, and flag any non-finite float.
            const float* xf = reinterpret_cast<const float*>(bike.raw() + off::bike::kWorldMatrix);
            bool finite = true;
            for (int i = 0; i < 12; ++i)
                if (!std::isfinite(xf[i])) finite = false;

            host::Logf("bike %p crashed=%d state=%d finite=%d xf=[%.2f %.2f %.2f | %.2f %.2f "
                       "%.2f | %.2f %.2f %.2f | %.2f %.2f %.2f]%s",
                       bike.raw(), flag, state, finite ? 1 : 0, xf[0], xf[1], xf[2], xf[3],
                       xf[4], xf[5], xf[6], xf[7], xf[8], xf[9], xf[10], xf[11],
                       changed ? "  <- CHANGED" : "");
        }
    }

    int patched = 0;
    for (auto& bike : bikes) {
        Snapshot& s = g_stock[bike.raw()];
        if (!s.captured) Capture(bike, s);

        // Write whenever the section is on, and once more on the tick it goes off
        // so the bike lands back on its stock numbers.
        if (on[kGrip]   || g_wasOn[kGrip])   ApplyGrip(bike, s, g_settings.grip, on[kGrip]);
        if (on[kSusp]   || g_wasOn[kSusp])   ApplySuspension(bike, s, g_settings.suspension, on[kSusp]);
        if (on[kEngine] || g_wasOn[kEngine]) ApplyEngine(bike, s, g_settings.engine, on[kEngine]);
        if (on[kTrans]  || g_wasOn[kTrans])  ApplyTransmission(bike, s, g_settings.transmission, on[kTrans]);
        ++patched;
    }
    g_status.bikesPatched = patched;

    // Auto-recovery is deliberately NOT gated on the crash section's toggle. It
    // does not patch anything, and it is the only thing that covers
    // body-in-the-dirt, so making it depend on "Enable crash modifier" just
    // produced combinations that silently did nothing.
    if (master && g_settings.crash.autoRecover) {
        const auto now = std::chrono::steady_clock::now();
        const auto delay = std::chrono::duration<float>(
            g_settings.crash.recoverDelay < 0.0f ? 0.0f : g_settings.crash.recoverDelay);
        for (auto& bike : bikes) {
            Snapshot& s = g_stock[bike.raw()];
            if (bike.crashedFlag() == 0) {
                s.crashSeen = false;
                continue;
            }
            if (!s.crashSeen) {
                s.crashSeen = true;
                s.crashedAt = now;
                continue;
            }
            if (now - s.crashedAt < delay) continue;
            if (RiderReset(bike)) {
                s.crashSeen = false;
                ++g_status.recoveries;
                host::Logf("recovered bike %p (total %d)", bike.raw(), g_status.recoveries);
            } else {
                host::Logf("recovery FAILED for bike %p - reset not callable", bike.raw());
            }
        }
    }

    // Crash handling is global code, not per-bike.
    if (on[kCrash]) {
        // Never crash needs both halves: the force thresholds raised out of
        // reach, AND the other two detach exits made unreachable. Fixing only
        // the thresholds still let the ground-contact exit abort the rider
        // physics on every landing.
        crash::Apply(g_settings.crash.noCrash ? crash::kNoCrashTolerance
                                              : g_settings.crash.tolerance);
        crash::SetNoCrash(g_settings.crash.noCrash);
    } else if (g_wasOn[kCrash]) {
        crash::Restore();
    }

    for (int i = 0; i < kNumSections; ++i) g_wasOn[i] = on[i];

    // Drop snapshots for bikes that are gone, so a track reload starts clean.
    for (auto it = g_stock.begin(); it != g_stock.end();) {
        bool alive = false;
        for (auto& b : bikes) {
            if (b.raw() == it->first) { alive = true; break; }
        }
        it = alive ? std::next(it) : g_stock.erase(it);
    }
}

}  // namespace mxb
