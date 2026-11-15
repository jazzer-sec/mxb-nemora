#include "ModMenu.h"

#include <cstdio>

#include "ModMenuControls.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "mxb/CarMode.h"
#include "mxb/Chams.h"
#include "mxb/CrashPatch.h"
#include "mxb/Diagnostics.h"
#include "mxb/Modifiers.h"
#include "mxb/BikeData.h"
#include "mxb/Probe.h"
#include "mxb/Process.h"
#include "mxb/Session.h"
#include "mxb/Visuals.h"
#include "mxb/CleanLens.h"
#include "mxb/World.h"
#include "mxb/Spinny.h"
#include "mxb/NetworkRam.h"
#include "mxb/GuidSpoofer.h"
#include "host/Hooks.h"
#include "ui/Overlays.h"
#include "ui/Watermark.h"

// Supplied by the host (the render backend owns the font and background atlas).
extern ImFont* menuFont;
extern ImFont* tabFont;
extern void*   menuBgTexture;  // ImTextureID

namespace mxb {
namespace ui {
namespace {

bool g_open = false;
int  g_tab = kTabTyres;
int  g_probeMode = 0;

// The reference menu used icon glyphs for its tabs and the READ ME says those
// were not shipped. Short words read as deliberate where bare initials just look
// unfinished. (There is a recovered third font in MenuFonts.h that is probably
// the missing icon set -- see the note in that file -- but its codepoints are
// unknown, so this stays text until someone maps them.)
const char* kTabGlyph[kTabCount] = {"TYRE", "SUSP",  "ENG", "GEAR",  "CRASH",
                                    "VIS",  "WORLD", "CAR", "CHAMS", "INFO"};

void DrawSidebar() {
    ImGui::TabSpacer("##Top Spacer", ImVec2(75.f, 10.f));
    for (int i = 0; i < kTabCount; ++i) {
        const bool selected = (g_tab == i);
        // The strip is 542 tall and the spacers take 17 of it, so ten tabs have
        // to fit in 525: 10 + 10*52 + 7 = 537.
        const bool clicked = selected ? ImGui::SelectedTab(kTabGlyph[i], ImVec2(75.f, 52.f))
                                      : ImGui::Tab(kTabGlyph[i], ImVec2(75.f, 52.f));
        if (clicked) g_tab = i;
    }
    ImGui::TabSpacer2("##Bottom Spacer", ImVec2(75.f, 7.f));
}

// Multiplier slider, laid out exactly like the reference menu's InsertSlider.
// The run of Spacing() calls is not padding for its own sake: this ImGui fork
// draws a slider's label on the line above the bar, and that is the gap it
// needs. Using a single Spacing() here is what made every slider overlap the
// control above it.
// `power` above 1 bunches the travel toward the low end, which is what keeps a
// slider usable once its range runs to 20x: without it, everything from 0.25 to
// 3 is squeezed into the first eighth of the bar and stock values become
// impossible to hit.
void Multiplier(const char* label, float& value, float lo, float hi, float power = 1.0f) {
    ImGui::Spacing(); ImGui::Spacing(); ImGui::Spacing(); ImGui::Spacing();
    ImGui::Spacing(); ImGui::Spacing(); ImGui::Spacing(); ImGui::Spacing();
    ImGui::NewLine();
    ImGui::SameLine(42.f);
    ImGui::PushItemWidth(159.f);
    ImGui::SliderFloat(label, &value, lo, hi, "%.2fx", power);
    ImGui::PopItemWidth();
}

// Same layout as Multiplier, different unit suffix. The reference menu's slider
// macro needs the eight Spacing() calls above the bar for its label.
void UnitSlider(const char* label, float& value, float lo, float hi, const char* fmt) {
    ImGui::Spacing(); ImGui::Spacing(); ImGui::Spacing(); ImGui::Spacing();
    ImGui::Spacing(); ImGui::Spacing(); ImGui::Spacing(); ImGui::Spacing();
    ImGui::NewLine();
    ImGui::SameLine(42.f);
    ImGui::PushItemWidth(159.f);
    ImGui::SliderFloat(label, &value, lo, hi, fmt);
    ImGui::PopItemWidth();
}

void Degrees(const char* label, float& v, float lo, float hi) {
    UnitSlider(label, v, lo, hi, "%.0f deg");
}
void Seconds(const char* label, float& v, float lo, float hi) {
    UnitSlider(label, v, lo, hi, "%.2f s");
}
// InsertButton discards ImGui::Button's result, so anything that needs the
// click has to lay the button out itself.
bool FlatButton(const char* label, float w, float x) {
    ImGui::NewLine();
    ImGui::SameLine(x);
    return ImGui::Button(label, ImVec2(w, 22.f));
}

void Metres(const char* label, float& v, float lo, float hi) {
    UnitSlider(label, v, lo, hi, "%.2f m");
}
void Fraction(const char* label, float& v, float lo, float hi) {
    UnitSlider(label, v, lo, hi, "%.2f");
}

void TyresTab() {
    auto& s = Get().grip;
    InsertSpacer("Tyres Top Spacer");

    ImGui::Columns(2, nullptr, false);
    {
        InsertGroupBoxLeft("Grip", 240.f);
        {
            InsertCheckbox("Enable tyre modifier", s.enabled);
            Multiplier("Peak longitudinal (p_Dx1)", s.peakLong, 0.50f, 2.00f);
            Multiplier("Peak lateral (p_Dy1)", s.peakLat, 0.50f, 2.00f);
            Multiplier("Dry surface grip", s.surfaceDry, 0.50f, 2.00f);
            Multiplier("Wet surface grip", s.surfaceWet, 0.50f, 2.00f);
        }
        InsertEndGroupBoxLeft("Grip Cover", "Grip");
    }
    ImGui::NextColumn();
    {
        InsertGroupBoxRight("Carcass", 150.f);
        {
            Multiplier("Spring base", s.carcassSpring, 0.50f, 2.00f);
            Multiplier("Damper", s.carcassDamper, 0.50f, 2.00f);
        }
        InsertEndGroupBoxRight("Carcass Cover", "Carcass");

        InsertSpacer("Carcass - Notes Spacer");

        InsertGroupBoxRight("Notes", 90.f);
        {
            InsertText("Applies to every tyre slot on");
            InsertText("both wheels, so swapping tyres");
            InsertText("in setup keeps the modifier.");
        }
        InsertEndGroupBoxRight("Notes Cover", "Notes");
    }
    ImGui::Columns(1);
}

void SuspensionTab() {
    auto& s = Get().suspension;
    InsertSpacer("Suspension Top Spacer");

    ImGui::Columns(2, nullptr, false);
    {
        InsertGroupBoxLeft("Springs", 180.f);
        {
            InsertCheckbox("Enable suspension modifier", s.enabled);
            Multiplier("Front spring rate", s.frontSpring, 0.50f, 2.00f);
            Multiplier("Rear spring rate", s.rearSpring, 0.50f, 2.00f);
        }
        InsertEndGroupBoxLeft("Springs Cover", "Springs");

        InsertSpacer("Springs - Limits Spacer");

        InsertGroupBoxLeft("Limits", 150.f);
        {
            Multiplier("Bump stop", s.bumpStop, 0.50f, 2.00f);
            Multiplier("Top-out spring", s.topOut, 0.50f, 2.00f);
        }
        InsertEndGroupBoxLeft("Limits Cover", "Limits");
    }
    ImGui::NextColumn();
    {
        InsertGroupBoxRight("Damping", 150.f);
        {
            Multiplier("Front damping", s.frontDamping, 0.50f, 2.00f);
            Multiplier("Rear damping", s.rearDamping, 0.50f, 2.00f);
        }
        InsertEndGroupBoxRight("Damping Cover", "Damping");

        InsertSpacer("Damping - Notes Spacer");

        InsertGroupBoxRight("Notes", 110.f);
        {
            InsertText("Spring scales the base and the");
            InsertText("click step together, so every");
            InsertText("clicker position moves with it.");
            InsertText("Damping scales the bump and");
            InsertText("rebound thresholds.");
        }
        InsertEndGroupBoxRight("Notes Cover", "Notes");
    }
    ImGui::Columns(1);
}

void EngineTab() {
    auto& s = Get().engine;
    InsertSpacer("Engine Top Spacer");

    ImGui::Columns(2, nullptr, false);
    {
        InsertGroupBoxLeft("Power", 230.f);
        {
            InsertCheckbox("Enable engine modifier", s.enabled);
            Multiplier("Torque curve", s.torque, 0.10f, 25.00f, 3.0f);
            Multiplier("Clutch max torque", s.clutchTorque, 0.10f, 25.00f, 3.0f);
            Multiplier("Horsepower (revs)", s.revRange, 0.25f, 3.00f, 1.0f);
        }
        InsertEndGroupBoxLeft("Power Cover", "Power");
    }
    ImGui::NextColumn();
    {
        InsertGroupBoxRight("Braking", 110.f);
        {
            Multiplier("Engine braking", s.engineBraking, 0.00f, 10.00f);
        }
        InsertEndGroupBoxRight("Braking Cover", "Braking");

        InsertSpacer("Braking - Speed Spacer");

        InsertGroupBoxRight("Top speed", 120.f);
        {
            Multiplier("Top speed", s.topSpeed, 0.50f, 6.00f, 2.0f);
            InsertText("Cuts aero drag (CdA).");
        }
        InsertEndGroupBoxRight("Speed Cover", "Top speed");

        InsertSpacer("Speed - Notes Spacer");

        InsertGroupBoxRight("Notes", 190.f);
        {
            InsertText("Torque scales every point of");
            InsertText("every mapping's curve, so map");
            InsertText("switching still works. Raise");
            InsertText("clutch torque with it or the");
            InsertText("clutch slips and the power");
            InsertText("never reaches the wheel.");
            InsertText("");
            InsertText("Horsepower = torque x revs, so");
            InsertText("the revs slider is the HP one.");
            InsertText("Top speed only bites if you are");
            InsertText("drag-limited, not revving out.");
        }
        InsertEndGroupBoxRight("Notes Cover", "Notes");
    }
    ImGui::Columns(1);
}

void TransmissionTab() {
    auto& s = Get().transmission;
    InsertSpacer("Transmission Top Spacer");

    ImGui::Columns(2, nullptr, false);
    {
        InsertGroupBoxLeft("Gearing", 180.f);
        {
            InsertCheckbox("Enable transmission modifier", s.enabled);
            Multiplier("Gear ratios (taller >1)", s.gearRatio, 0.50f, 2.00f);
            Multiplier("Final drive", s.finalDrive, 0.60f, 1.60f);
        }
        InsertEndGroupBoxLeft("Gearing Cover", "Gearing");

        InsertSpacer("Gearing - Driveline Spacer");

        InsertGroupBoxLeft("Driveline", 150.f);
        {
            Multiplier("Efficiency", s.drivelineEff, 0.50f, 1.10f);
            Multiplier("Rotating inertia", s.drivelineInertia, 0.25f, 3.00f);
        }
        InsertEndGroupBoxLeft("Driveline Cover", "Driveline");
    }
    ImGui::NextColumn();
    {
        InsertGroupBoxRight("Shifting", 150.f);
        {
            Multiplier("Shift speed", s.shiftSpeed, 0.25f, 4.00f);
            Multiplier("Clutch speed", s.clutchSpeed, 0.25f, 4.00f);
        }
        InsertEndGroupBoxRight("Shifting Cover", "Shifting");

        InsertSpacer("Shifting - Notes Spacer");

        InsertGroupBoxRight("Notes", 110.f);
        {
            InsertText("Final drive is whole sprocket");
            InsertText("teeth, so it steps in ~2% jumps.");
            InsertText("Efficiency is clamped at 1.00.");
        }
        InsertEndGroupBoxRight("Notes Cover", "Notes");
    }
    ImGui::Columns(1);
}

void CrashTab() {
    auto& s = Get().crash;
    InsertSpacer("Crash Top Spacer");

    ImGui::Columns(2, nullptr, false);
    {
        InsertGroupBoxLeft("Tolerance", 240.f);
        {
            InsertCheckbox("Enable crash modifier", s.enabled);
            InsertCheckbox("Never crash", s.noCrash);
            InsertCheckbox("Auto-recover", s.autoRecover);
            Multiplier("Crash tolerance", s.tolerance, 0.25f, 8.00f);
            Multiplier("Recover delay (s)", s.recoverDelay, 0.00f, 5.00f);
        }
        InsertEndGroupBoxLeft("Tolerance Cover", "Tolerance");
    }
    ImGui::NextColumn();
    {
        InsertGroupBoxRight("Notes", 190.f);
        {
            InsertText("Scales the four impact thresholds");
            InsertText("that make the rider let go:");
            InsertText("  lateral     4000");
            InsertText("  vertical   10000");
            InsertText("  forward    15000");
            InsertText("  sustained  -2000");
            InsertText("This one patches game code, not");
            InsertText("bike data, so it is global.");
            InsertText("");
            InsertText("Never crash covers the force");
            InsertText("thresholds and the 75 deg lean");
            InsertText("limit. It overrides the slider.");
            InsertText("");
            InsertText("Putting your body in the dirt");
            InsertText("still ends the run: that exit");
            InsertText("means the rider is inside the");
            InsertText("terrain, and coming off is the");
            InsertText("only response the game models.");
            InsertText("");
            InsertText("Auto-recover works on its own,");
            InsertText("without Enable crash modifier.");
            InsertText("");
            InsertText("It handles that case");
            InsertText("instead: it lets the crash");
            InsertText("happen, then calls the game's");
            InsertText("own rider reset to put you back");
            InsertText("on without moving the bike.");
            if (!crash::SitesVerified() && s.enabled) {
                InsertText("");
                InsertText("Byte pattern did not match -");
                InsertText("not applied. Wrong game build?");
            }
        }
        InsertEndGroupBoxRight("Notes Cover", "Notes");
    }
    ImGui::Columns(1);
}

void VisualsTab() {
    auto& v = Visuals();
    auto& hat = ChinaHat();
    auto& wm = Watermark();

    InsertSpacer("Visuals Top Spacer");

    ImGui::Columns(2, nullptr, false);
    {
        InsertGroupBoxLeft("Camera", 130.f);
        {
            InsertCheckbox("FOV override", v.fovOverride);
            // The game's own scale, not a measured angle: View_BuildFrustum
            // widens it by aspect/1.3333 on anything wider than 4:3, so this is
            // the number a cameras.cfg entry carries.
            Degrees("FOV (game scale)", v.fov, 40.0f, 150.0f);

            char stock[48];
            const float sf = StockCameraFov();
            if (sf > 0.0f) std::snprintf(stock, sizeof(stock), "stock: %.1f", sf);
            else           std::snprintf(stock, sizeof(stock), "stock: not seen yet");
            InsertText(stock);
        }
        InsertEndGroupBoxLeft("Camera Cover", "Camera");

        InsertSpacer("Camera - Gesture Spacer");

        InsertGroupBoxLeft("Auto gesture", 160.f);
        {
            InsertCheckbox("Enable", v.autoGesture);
            InsertCombo("Gesture", v.gesture, kGestureNames);
            InsertCombo("Trigger", v.gestureTrigger, kGestureTriggers);
            Seconds("Cooldown", v.gestureCooldown, 0.50f, 10.00f);
        }
        InsertEndGroupBoxLeft("Gesture Cover", "Auto gesture");

        InsertSpacer("Gesture - Spinny Spacer");
        InsertGroupBoxLeft("Spinny", 135.f);
        {
            const auto status=spinny::GetStatus();
            if(FlatButton(status.enabled?"Disable Spinny":"Enable Spinny",159.f,42.f))
                spinny::SetEnabled(!status.enabled);
            InsertText("Straight up / 1.5 turns per second");
            InsertText("Wheel clearance: 0.20 m");
            InsertText("Your view and riding stay normal.");
            if(!status.ready || (status.enabled && status.skipped && status.note[0])) {
                InsertText(status.note);
            }
        }
        InsertEndGroupBoxLeft("Spinny Cover", "Spinny");

        InsertSpacer("Spinny - Ram Spacer");
        InsertGroupBoxLeft("Network ram", 340.f);
        {
            const auto status = networkram::GetStatus();
            if (FlatButton(status.enabled ? "Disable network ram" : "Enable network ram",159.f,42.f))
                networkram::SetEnabled(!status.enabled);
            InsertText("Front to back / 6 turns/sec pose.");
            bool bikeOnly = status.bikeOnly;
            InsertCheckbox("Bike only (limits spin)",bikeOnly);
            if (bikeOnly != status.bikeOnly) networkram::SetBikeOnly(bikeOnly);
            float lead = status.leadMilliseconds;
            UnitSlider("Prediction lead",lead,0.0f,500.0f,"%.0f ms");
            if (lead != status.leadMilliseconds) networkram::SetLeadMilliseconds(lead);
            InsertText("Local bike and camera stay normal.");
            InsertText("Height follows target snapshot.");
            InsertText("Nametag still follows the bike.");
            char target[80];
            std::snprintf(target,sizeof(target),"Target slot: %d / active: %d",status.targetSlot,status.targets);
            InsertText(target);
            char yaw[64];
            std::snprintf(yaw,sizeof(yaw),"Outgoing yaw: %.0f deg",status.outgoingYaw);
            InsertText(yaw);
            char scan[80];
            std::snprintf(scan,sizeof(scan),"Remote: %d / receiving: %d / poses: %d",
                          status.scan.remote,status.scan.receiving,status.scan.snapshots);
            InsertText(scan);
            InsertText(status.note);
        }
        InsertEndGroupBoxLeft("Network Ram Cover", "Network ram");
    }
    ImGui::NextColumn();
    {
        InsertGroupBoxRight("China hat", 250.f);
        {
            InsertCheckbox("Enable", hat.enabled);
            InsertCheckbox("Outline", hat.outline);
            InsertCombo("Style", hat.style, kHatStyles);
            Metres("Height", hat.height, 0.00f, 4.00f);
            Metres("Radius", hat.radius, 0.05f, 1.50f);
            Metres("Cone height", hat.coneHeight, 0.05f, 1.50f);
            InsertColorPicker("Colour", hat.color, true);
        }
        InsertEndGroupBoxRight("Hat Cover", "China hat");

        InsertSpacer("Hat - Watermark Spacer");

        InsertGroupBoxRight("Watermark", 180.f);
        {
            InsertCheckbox("Enabled", wm.enabled);
            InsertCheckbox("Show name", wm.showName);
            InsertCheckbox("Show FPS", wm.showFps);
            InsertCheckbox("Show clock", wm.showClock);
            InsertColorPicker("Accent", wm.color, true);
        }
        InsertEndGroupBoxRight("Watermark Cover", "Watermark");

        InsertSpacer("Watermark - Lens Spacer");
        InsertGroupBoxRight("Lens", 110.f);
        {
            const auto status = cleanlens::GetStatus();
            bool enabled = status.enabled;
            InsertCheckbox("Clean lens", enabled);
            if (enabled != status.enabled) cleanlens::SetEnabled(enabled);
            InsertText(status.ready ? "Hides visor dirt and rain." : "Visor hook unavailable.");
        }
        InsertEndGroupBoxRight("Lens Cover", "Lens");
    }
    ImGui::Columns(1);
}

void WorldTab() {
    auto& w = World();

    InsertSpacer("World Top Spacer");

    ImGui::Columns(2, nullptr, false);
    {
        InsertGroupBoxLeft("Light", 260.f);
        {
            InsertCheckbox("Enable world modifier", w.enabled);
            InsertCheckbox("Night mode", w.night);
            Fraction("Night amount", w.nightAmount, 0.00f, 1.00f);
            Fraction("Light level", w.lightLevel, 0.00f, 2.00f);
            InsertColorPicker("Light tint", w.tint, false);
            InsertCheckbox("Override clear col", w.overrideClear);
            InsertColorPicker("Clear", w.clearColor, false);
        }
        InsertEndGroupBoxLeft("Light Cover", "Light");

        InsertSpacer("Light - Notes Spacer");

        InsertGroupBoxLeft("What this reaches", 150.f);
        {
            InsertText("Clear colour sits BEHIND the");
            InsertText("track's sky dome, so it is");
            InsertText("usually invisible. Nothing here");
            InsertText("tints the sky itself - that");
            InsertText("needs the sky draw path. Use");
            InsertText("the screen filter for the sky.");
        }
        InsertEndGroupBoxLeft("Notes Cover", "What this reaches");
    }
    ImGui::NextColumn();
    {
        InsertGroupBoxRight("Fog", 190.f);
        {
            InsertCombo("Mode", w.fogMode, kFogModes);
            Fraction("Density", w.fogDensity, 0.00f, 5.00f);
            InsertCheckbox("Override colour", w.overrideFogColor);
            InsertColorPicker("Fog", w.fogColor, false);
        }
        InsertEndGroupBoxRight("Fog Cover", "Fog");

        InsertSpacer("Fog - Filter Spacer");

        InsertGroupBoxRight("Screen filter", 160.f);
        {
            Fraction("Strength", w.screenTint, 0.00f, 1.00f);
            InsertColorPicker("Colour", w.filterColor, false);
            InsertText("Catches the sky and anything");
            InsertText("the sun does not light.");
            InsertText("Night mode drives it on its own.");
        }
        InsertEndGroupBoxRight("Filter Cover", "Screen filter");
    }
    ImGui::Columns(1);
}

void CarTab() {
    auto& c = CarMode();
    char line[160];

    InsertSpacer("Car Top Spacer");

    ImGui::Columns(2, nullptr, false);
    {
        InsertGroupBoxLeft("Steering", 240.f);
        {
            InsertCheckbox("Enable car mode", c.enabled);
            Degrees("Yaw rate", c.yawRate, 10.0f, 360.0f);
            UnitSlider("Full-rate speed", c.speedRef, 0.5f, 25.0f, "%.1f m/s");
            Fraction("Grip", c.grip, 0.00f, 1.00f);
            Fraction("Upright", c.upright, 0.00f, 1.00f);
        }
        InsertEndGroupBoxLeft("Steering Cover", "Steering");

        InsertSpacer("Steering - Opts Spacer");

        InsertGroupBoxLeft("Options", 90.f);
        {
            InsertCheckbox("Apply to AI bikes", c.allBikes);
        }
        InsertEndGroupBoxLeft("Opts Cover", "Options");
    }
    ImGui::NextColumn();
    {
        InsertGroupBoxRight("Live", 160.f);
        {
            const CarModeStatus st = CarModeState();
            std::snprintf(line, sizeof(line), "%s  bikes=%d",
                          st.active ? "active" : (st.note[0] ? st.note : "idle"), st.bikes);
            InsertText(line);
            std::snprintf(line, sizeof(line), "steer=%+.3f  speed=%.1f m/s", st.steer, st.speed);
            InsertText(line);
            std::snprintf(line, sizeof(line), "yaw=%+.1f deg/s", st.yaw);
            InsertText(line);
            if (st.fwdAxis < 0) {
                InsertText("heading: not resolved yet");
            } else {
                std::snprintf(line, sizeof(line), "heading: axis %d %s   rejected=%d", st.fwdAxis,
                              st.fwdSign < 0.0f ? "neg" : "pos", st.rejected);
                InsertText(line);
            }
        }
        InsertEndGroupBoxRight("Live Cover", "Live");

        InsertSpacer("Live - Note Spacer");

        InsertGroupBoxRight("How it works", 220.f);
        {
            InsertText("Steer drives the chassis yaw");
            InsertText("rate, and velocity is pulled");
            InsertText("onto the bike's forward axis");
            InsertText("so it goes where it points.");
            InsertText("");
            InsertText("Grip 1.0 removes the slide.");
            InsertText("");
            InsertText("Which axis is forward is found");
            InsertText("by watching it ride straight,");
            InsertText("not assumed - so hold a");
            InsertText("straight line once at speed.");
        }
        InsertEndGroupBoxRight("Note Cover", "How it works");
    }
    ImGui::Columns(1);
}

// One chams part. Rider and bike get identical controls, so they share this.
// The group-box macros expand to several statements, so every branch needs
// braces, and each box needs its own cover id or the two collide in ImGui.
void ChamsPartBox(const char* id, const char* title, const char* cover, ChamsPart& p,
                  float height, bool left) {
    if (left) {
        InsertGroupBoxLeft(id, height);
    } else {
        InsertGroupBoxRight(id, height);
    }
    {
        InsertCheckbox("Enable", p.enabled);
        InsertCombo("Material", p.style, kChamStyles);
        InsertColorPicker("Colour", p.color, true);
    }
    if (left) {
        InsertEndGroupBoxLeft(cover, title);
    } else {
        InsertEndGroupBoxRight(cover, title);
    }
}

void ChamsTab() {
    auto& c = Chams();
    char line[160];

    InsertSpacer("Chams Top Spacer");

    ImGui::Columns(2, nullptr, false);
    {
        ChamsPartBox("RiderChams", "Rider", "RiderChams Cover", c.rider, 160.f, true);

        InsertSpacer("Rider - Opts Spacer");

        InsertGroupBoxLeft("Options", 150.f);
        {
            InsertCheckbox("Force flat", c.forceFlat);
            InsertCheckbox("Swap rider / bike", c.swapParts);
            InsertText("Force flat drops to fixed");
            InsertText("function so the skin texture");
            InsertText("cannot win over the colour.");
        }
        InsertEndGroupBoxLeft("ChamOpts Cover", "Options");
    }
    ImGui::NextColumn();
    {
        ChamsPartBox("BikeChams", "Bike", "BikeChams Cover", c.bike, 160.f, false);

        InsertSpacer("Bike - Status Spacer");

        InsertGroupBoxRight("Status", 165.f);
        {
            const ChamsStatus st = ChamsState();
            if (!st.hooked) {
                InsertText("Mat_SetupMaterial hook MISSING");
            } else if (!st.glHooked) {
                InsertText("glMaterialfv hook MISSING");
            } else {
                std::snprintf(line, sizeof(line), "entities %d, type1 %d, vehicles %d",
                              st.sceneEntities, st.typeOneEntities, st.vehicleEntities);
                InsertText(line);
                std::snprintf(line, sizeof(line), "meshes: %d all, %d rider, %d bike",
                              st.totalMeshes, st.riderMeshes, st.vehicleMeshes);
                InsertText(line);
                std::snprintf(line, sizeof(line), "chammed: rider=%d bike=%d", st.riderDraws,
                              st.bikeDraws);
                InsertText(line);
                std::snprintf(line, sizeof(line), "material writes: %d", st.materialCalls);
                InsertText(line);
            }
        }
        InsertEndGroupBoxRight("ChamStatus Cover", "Status");
    }
    ImGui::Columns(1);
}

void StatusTab() {
    const Status st = GetStatus();
    char line[160];

    InsertSpacer("Status Top Spacer");

    InsertGroupBoxTop("Module", ImVec2(535.f, 170.f));
    {
#if MXB_TESTING_NO_GUARDS
        InsertText("TESTING BUILD - offline guard disabled,");
        InsertText("all sections on by default.");
        InsertText("");
#endif

        std::snprintf(line, sizeof(line), "mxbikes.exe: %s",
                      st.moduleFound ? "found" : "not found");
        InsertText(line);

        const char* sessionName = "?";
        switch (QueryOnline()) {
            case Online::Offline:    sessionName = "offline"; break;
            case Online::Connecting: sessionName = "connecting"; break;
            case Online::Connected:  sessionName = "connected"; break;
            case Online::Unknown:    sessionName = "unknown"; break;
        }
        std::snprintf(line, sizeof(line), "Session: %s (%s)", sessionName,
                      st.writesBlocked ? "writes blocked" : "writes allowed");
        InsertText(line);

        std::snprintf(line, sizeof(line), "Bikes loaded: %d   patched: %d",
                      st.bikesSeen, st.bikesPatched);
        InsertText(line);

        const RegistryProbe probe = LastProbe();
        std::snprintf(line, sizeof(line), "Bikes coll: %p  worlds=%d  n=%d  ok=%d  bad=%d",
                      probe.collection, probe.worldsSeen, probe.rawCount, probe.accepted,
                      probe.rejected);
        InsertText(line);
        std::snprintf(line, sizeof(line), "  %s", probe.note);
        InsertText(line);

        std::snprintf(line, sizeof(line), "Crash patch: %s  never-crash: %s  recoveries: %d",
                      crash::IsApplied() ? "applied" : "not applied",
                      crash::NoCrashActive() ? "on" : "off", st.recoveries);
        InsertText(line);

        if (st.note && st.note[0]) InsertText(st.note);
        const auto verification = host::GetVerificationStatus();
        std::snprintf(line, sizeof(line), "Stock verification: %s  replies=%u  unavailable=%u",
                      verification.ready ? "ready" : "inactive", verification.stockReplies, verification.unavailable);
        InsertText(line);
        if (verification.unavailable) {
            std::snprintf(line, sizeof(line), "Last stock view failure: %s", verification.lastFailure);
            InsertText(line);
        }
    }
    InsertEndGroupBoxTop("Module Cover", "Module", ImVec2(536.f, 11.f));

    InsertSpacer("Module - GUID Spacer");
    InsertGroupBoxTop("Outgoing GUID", ImVec2(535.f, 220.f));
    {
        const auto status = guid::GetStatus();
        static char input[19]{};
        static const char* error = "";
        if (!input[0] && status.replacement[0])
            std::snprintf(input,sizeof(input),"%s",status.replacement.data());
        std::snprintf(line,sizeof(line),"GUID spoofer: %s   %s",status.ready ? "ready" : "unavailable",status.enabled ? "enabled" : "disabled");
        InsertText(line);
        ImGui::Spacing(); ImGui::NewLine(); ImGui::SameLine(19.f);
        ImGui::PushItemWidth(190.f);
        ImGui::InputText("##OutgoingGuid",input,sizeof(input),ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase);
        ImGui::PopItemWidth(); ImGui::SameLine();
        if (ImGui::Button("Apply GUID")) error = guid::SetValue(input) ? "" : "Enter 1-18 hex digits; zero/reserved GUIDs are invalid.";
        ImGui::SameLine();
        if (ImGui::Button("Random GUID")) {
            if (guid::Generate()) { const auto next = guid::GetStatus(); std::snprintf(input,sizeof(input),"%s",next.replacement.data()); error = ""; }
            else error = "Could not generate a GUID.";
        }
        if (FlatButton(status.enabled ? "Disable GUID spoofer" : "Enable GUID spoofer",220.f,42.f)) {
            error = guid::SetEnabled(!status.enabled) ? "" : "Hook unavailable or no valid GUID selected.";
        }
        std::snprintf(line,sizeof(line),"Selected: %s   payloads=%llu   blocked=%llu",status.replacement.data(),
            static_cast<unsigned long long>(status.changed),static_cast<unsigned long long>(status.skipped));
        InsertText(line);
        InsertText(error[0] ? error : status.note);
        InsertText("Reconnect after changes. Stored client GUID is unchanged.");
        InsertText("Server acceptance is unverified. Selection resets on unload.");
    }
    InsertEndGroupBoxTop("GUID Cover", "Outgoing GUID", ImVec2(536.f, 11.f));

    InsertSpacer("GUID - Diagnostics Spacer");
    InsertGroupBoxTop("Ban diagnostics", ImVec2(535.f, 110.f));
    {
        InsertText("F7 saves the current ban reason and active settings.");
        if (FlatButton("Save ban diagnostics (F7)", 240.f, 42.f)) diagnostics::Capture();
        const auto capture = diagnostics::GetStatus();
        InsertText(capture.message);
        InsertText("File: mxb_physics.ban-diagnostics.log (beside DLL)");
    }
    InsertEndGroupBoxTop("Diagnostics Cover", "Ban diagnostics", ImVec2(536.f, 11.f));
    InsertSpacer("Diagnostics - Actions Spacer");

    InsertGroupBoxTop("Actions", ImVec2(535.f, 185.f));
    {
        ImGui::Spacing();
        ImGui::NewLine();
        ImGui::SameLine(42.f);
        if (ImGui::Button("Restore all bikes to stock", ImVec2(220.f, 24.f))) RestoreAll();

        // Field probe. Capture the bike under two (or three) deliberately
        // opposite conditions, then ask which fields can only be the thing you
        // are looking for. Results go to the log, not here -- there can be
        // dozens and they need to be read side by side.
        ImGui::Spacing();
        if (FlatButton("Capture A", 100.f, 42.f)) ProbeCapture(0);
        ImGui::SameLine();
        if (ImGui::Button("Capture B", ImVec2(100.f, 22.f))) ProbeCapture(1);
        ImGui::SameLine();
        if (ImGui::Button("Capture C", ImVec2(100.f, 22.f))) ProbeCapture(2);
        ImGui::SameLine();
        if (ImGui::Button("Clear", ImVec2(70.f, 22.f))) ProbeClear();

        ImGui::Spacing();
        ImGui::NewLine();
        ImGui::SameLine(42.f);
        ImGui::PushItemWidth(158.f);
        ImGui::Combo("##ProbeMode", &g_probeMode, kProbeModes, IM_ARRAYSIZE(kProbeModes));
        ImGui::PopItemWidth();
        ImGui::SameLine();
        if (ImGui::Button("Compare to log", ImVec2(140.f, 22.f))) ProbeCompare(g_probeMode);

        const ProbeStatus ps = ProbeState();
        if (ps.mismatch) {
            InsertText("captures are from different bikes");
        } else if (ps.matches >= 0) {
            std::snprintf(line, sizeof(line), "A=%d B=%d C=%d   %d matches, %d logged",
                          ps.have[0] ? 1 : 0, ps.have[1] ? 1 : 0, ps.have[2] ? 1 : 0, ps.matches,
                          ps.reported);
            InsertText(line);
        } else {
            std::snprintf(line, sizeof(line), "A=%d B=%d C=%d   no comparison run",
                          ps.have[0] ? 1 : 0, ps.have[1] ? 1 : 0, ps.have[2] ? 1 : 0);
            InsertText(line);
        }
    }
    InsertEndGroupBoxTop("Actions Cover", "Actions and field probe", ImVec2(536.f, 11.f));

    InsertSpacer("Actions - Fields Spacer");

    // Reads a few fields whose plausible range is known. If these come back as
    // nonsense, the offsets do not match this object and nothing else here can
    // be trusted; if they look right, the layout is good and any lack of effect
    // is the game caching values rather than us writing the wrong place.
    InsertGroupBoxTop("Bike fields (sanity check)", ImVec2(535.f, 120.f));
    {
        auto bikes = LoadedBikes();
        if (bikes.empty()) {
            InsertText("no bike to read");
        } else {
            BikeData b = bikes.front();
            std::snprintf(line, sizeof(line), "gears=%d  maps=%d  maxRPM=%.0f  riderMass=%.1f",
                          b.numGears(), b.numMappings(), b.engineMaxRPM(),
                          *reinterpret_cast<double*>(b.raw() + 142800));
            InsertText(line);

            const SettingRange fs = b.frontSpring();
            std::snprintf(line, sizeof(line), "frontSpring base=%.1f step=%.1f n=%d set=%d",
                          fs.base, fs.step, fs.numSteps, fs.setting);
            InsertText(line);

            std::snprintf(line, sizeof(line), "upShift=%.3f  drivelineEff=%.3f  tyreSlots=%d/%d",
                          b.upShiftTime(), b.drivelineEff(), b.numTyreSlots(0),
                          b.numTyreSlots(1));
            InsertText(line);

            Tyre t = b.activeTyre(0);
            if (t.valid()) {
                std::snprintf(line, sizeof(line), "front tyre %p  p_Dx1=%.3f  p_Dy1=%.3f",
                              static_cast<void*>(t.raw()), t.pDx1(), t.pDy1());
            } else {
                std::snprintf(line, sizeof(line), "front tyre: null active pointer");
            }
            InsertText(line);
        }
    }
    InsertEndGroupBoxTop("Fields Cover", "Bike fields (sanity check)", ImVec2(536.f, 11.f));
}

}  // namespace

bool& IsOpen() { return g_open; }

void Render() {
    if (!g_open) return;

    ImGuiStyle* style = &ImGui::GetStyle();
    style->WindowPadding = ImVec2(6, 6);

    if (menuFont) ImGui::PushFont(menuFont);

    ImGui::SetNextWindowSize(ImVec2(660.f, 560.f));
    ImGui::BeginMenuBackground(
        "MX Bikes physics", &g_open,
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
            ImGuiWindowFlags_NoTitleBar);
    {
        ImGui::BeginChild("Complete Border", ImVec2(648.f, 548.f), false);
        {
            if (menuBgTexture)
                ImGui::Image(static_cast<ImTextureID>(menuBgTexture), ImVec2(648.f, 548.f));
        }
        ImGui::EndChild();

        ImGui::SameLine(6.f);
        style->Colors[ImGuiCol_ChildBg] = ImColor(0, 0, 0, 0);

        ImGui::BeginChild("Menu Contents", ImVec2(648.f, 548.f), false);
        {
            ImGui::ColorBar("accent", ImVec2(648.f, 2.f));
            style->ItemSpacing = ImVec2(0.f, -1.f);

            ImGui::BeginTabs("Tabs", ImVec2(75.f, 542.f), false);
            {
                style->ItemSpacing = ImVec2(0.f, 0.f);
                style->ButtonTextAlign = ImVec2(0.5f, 0.47f);

                if (menuFont) ImGui::PopFont();
                if (tabFont) ImGui::PushFont(tabFont);

                DrawSidebar();

                if (tabFont) ImGui::PopFont();
                if (menuFont) ImGui::PushFont(menuFont);

                style->ButtonTextAlign = ImVec2(0.5f, 0.5f);
            }
            ImGui::EndTabs();

            ImGui::SameLine(75.f);

            ImGui::BeginChild("Tab Contents", ImVec2(572.f, 542.f), false);
            {
                style->Colors[ImGuiCol_Border] = ImColor(0, 0, 0, 0);

                // ImGui::Spacing() advances by ItemSpacing.y, and the tab strip
                // above sets that to zero. Leaving it at zero here makes every
                // Spacing() in the control macros a no-op, which collapses the
                // whole tab: the fork draws a slider's label 18px above its bar,
                // so without that gap each label lands on the row above it.
                style->ItemSpacing = ImVec2(4.f, 4.f);

                switch (g_tab) {
                    case kTabTyres:        TyresTab(); break;
                    case kTabSuspension:   SuspensionTab(); break;
                    case kTabEngine:       EngineTab(); break;
                    case kTabTransmission: TransmissionTab(); break;
                    case kTabCrash:        CrashTab(); break;
                    case kTabVisuals:      VisualsTab(); break;
                    case kTabWorld:        WorldTab(); break;
                    case kTabCar:          CarTab(); break;
                    case kTabChams:        ChamsTab(); break;
                    case kTabStatus:       StatusTab(); break;
                    default: break;
                }

                // ImGui's style is persistent, so the values this function
                // overrides have to be put back or they leak into the next
                // frame -- the reference menu restores exactly these three and
                // the first port of it dropped them, which left the background
                // child and group box borders drawing with whatever the last
                // spacer macro happened to set.
                style->Colors[ImGuiCol_Border] = ImColor(10, 10, 10, 255);
            }
            ImGui::EndChild();

            style->ItemSpacing = ImVec2(4.f, 4.f);
            style->Colors[ImGuiCol_ChildBg] = ImColor(17, 17, 17, 255);
        }
        ImGui::EndChild();

        if (menuFont) ImGui::PopFont();
    }
    ImGui::End();
}

}  // namespace ui
}  // namespace mxb
