#pragma once
#include "imgui.h"

// Same control vocabulary as the reference menu's MenuControls.h, minus its
// dependency on the original Config.h, and with the style pointer fetched per
// use instead of defined at file scope in a header.

#define MC_STYLE (&ImGui::GetStyle())

// =====================================
// - Layout
// =====================================

#define InsertSpacer(x1) \
    MC_STYLE->Colors[ImGuiCol_ChildBg] = ImColor(0, 0, 0, 0); \
    ImGui::BeginChild(x1, ImVec2(210.f, 18.f), false); {} ImGui::EndChild(); \
    MC_STYLE->Colors[ImGuiCol_ChildBg] = ImColor(49, 49, 49, 255);

#define InsertGroupboxSpacer(x1) \
    MC_STYLE->Colors[ImGuiCol_ChildBg] = ImColor(0, 0, 0, 0); \
    ImGui::BeginChild(x1, ImVec2(210.f, 9.f), false); {} ImGui::EndChild(); \
    MC_STYLE->Colors[ImGuiCol_ChildBg] = ImColor(49, 49, 49, 255);

#define InsertGroupboxTitle(x1) \
    ImGui::Spacing(); ImGui::NewLine(); ImGui::SameLine(11.f); ImGui::GroupBoxTitle(x1);

#define InsertGroupBoxLeft(x1, x2) \
    ImGui::NewLine(); ImGui::SameLine(19.f); ImGui::BeginGroupBox(x1, ImVec2(258.f, x2), true);
#define InsertGroupBoxRight(x1, x2) \
    ImGui::NewLine(); ImGui::SameLine(10.f); ImGui::BeginGroupBox(x1, ImVec2(258.f, x2), true);
#define InsertEndGroupBoxLeft(x1, x2) \
    ImGui::EndGroupBox(); ImGui::SameLine(19.f); \
    ImGui::BeginGroupBoxScroll(x1, x2, ImVec2(258.f, 11.f), true); ImGui::EndGroupBoxScroll();
#define InsertEndGroupBoxRight(x1, x2) \
    ImGui::EndGroupBox(); ImGui::SameLine(10.f); \
    ImGui::BeginGroupBoxScroll(x1, x2, ImVec2(258.f, 11.f), true); ImGui::EndGroupBoxScroll();

#define InsertGroupBoxTop(x1, x2) \
    ImGui::NewLine(); ImGui::SameLine(19.f); ImGui::BeginGroupBox(x1, x2, true);
#define InsertEndGroupBoxTop(x1, x2, x3) \
    ImGui::EndGroupBox(); ImGui::SameLine(19.f); \
    ImGui::BeginGroupBoxScroll(x1, x2, x3, true); ImGui::EndGroupBoxScroll();

// =====================================
// - Controls
// =====================================

#define InsertCheckbox(x1, x2) \
    ImGui::Spacing(); ImGui::NewLine(); ImGui::SameLine(19.f); ImGui::Checkbox(x1, &x2);

#define InsertSlider(x1, x2, x3, x4, x5) \
    ImGui::Spacing(); ImGui::Spacing(); ImGui::Spacing(); ImGui::Spacing(); \
    ImGui::Spacing(); ImGui::Spacing(); ImGui::Spacing(); ImGui::Spacing(); \
    ImGui::NewLine(); ImGui::SameLine(42.f); ImGui::PushItemWidth(159.f); \
    ImGui::SliderFloat(x1, &x2, x3, x4, x5); ImGui::PopItemWidth();

#define InsertSliderWithoutText(x1, x2, x3, x4, x5) \
    ImGui::Spacing(); ImGui::Spacing(); ImGui::NewLine(); ImGui::SameLine(42.f); \
    ImGui::PushItemWidth(159.f); ImGui::SliderFloat(x1, &x2, x3, x4, x5); ImGui::PopItemWidth();

#define InsertCombo(x1, x2, x3) \
    ImGui::Spacing(); ImGui::NewLine(); ImGui::NewLine(); ImGui::SameLine(42.f); \
    ImGui::PushItemWidth(158.f); ImGui::Combo(x1, &x2, x3, IM_ARRAYSIZE(x3)); \
    ImGui::PopItemWidth(); ImGui::CustomSpacing(1.f);

#define InsertButton(x1, x2) \
    ImGui::Spacing(); ImGui::NewLine(); ImGui::SameLine(42.f); ImGui::Button(x1, ImVec2(159.f, x2));

// Right-aligned swatch on the same line as the preceding control, like the
// reference menu's ColorPicker helper.
#define InsertColorPicker(x1, x2, x3)     ImGui::Spacing(); ImGui::NewLine(); ImGui::SameLine(19.f); ImGui::TextUnformatted(x1);     ImGui::SameLine(219.f);     ImGui::ColorEdit4("##" x1 "Picker", x2,         ((x3) ? ImGuiColorEditFlags_AlphaBar : ImGuiColorEditFlags_NoAlpha) |         ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoTooltip);

#define InsertText(x1) \
    ImGui::Spacing(); ImGui::NewLine(); ImGui::SameLine(19.f); ImGui::TextUnformatted(x1);
