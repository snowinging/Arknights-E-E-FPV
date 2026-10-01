#pragma once

#include <algorithm>
#include <cctype>
#include <string>

#include "../../utils/settings.hpp"

namespace endfield::menu {

inline bool DrawSetting(renodx::utils::settings::Setting* setting) {
  if (setting->key.starts_with("Shortcut")) return shortcuts::Draw(setting);
  using renodx::utils::settings::SettingValueType;
  if (setting->value_type == SettingValueType::CUSTOM) return setting->on_draw();
  if (setting->value_type == SettingValueType::TEXT) {
    if (setting->tint) ImGui::PushStyleColor(ImGuiCol_Text, renodx::utils::settings::ImVec4FromHex(*setting->tint));
    ImGui::TextWrapped("%s", setting->label.c_str());
    if (setting->tint) ImGui::PopStyleColor();
    return false;
  }

  bool changed = false;
  const float previous = setting->GetValue();
  ImGui::PushID(setting->key.c_str());
  ImGui::BeginDisabled(setting->is_enabled && !setting->is_enabled());
  if (ImGui::BeginTable("control", 3, ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.58f);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.42f);
    ImGui::TableSetupColumn("Reset", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-1.f);
    if (!setting->labels.empty()) {
      float required_width = 0.f;
      for (const auto& label : setting->labels) {
        required_width = std::max(required_width, ImGui::CalcTextSize(label.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.f);
      }
      const float width = ImGui::GetContentRegionAvail().x;
      const float spacing = ImGui::GetStyle().ItemSpacing.x;
      const float button_width = (width - spacing * (setting->labels.size() - 1)) / setting->labels.size();
      if ((setting->labels.size() == 2 && setting->labels[0] == "Off" && setting->labels[1] == "On")
          || button_width >= required_width) {
        for (int i = 0; i < static_cast<int>(setting->labels.size()); ++i) {
          if (i != 0) ImGui::SameLine();
          ImGui::PushID(i);
          ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(
                                                     setting->value_as_int == i ? ImGuiCol_SliderGrabActive : ImGuiCol_FrameBg));
          if (ImGui::Button(setting->labels[i].c_str(), ImVec2(button_width, 0.f))) {
            changed = setting->value_as_int != i;
            setting->Set(static_cast<float>(i));
          }
          ImGui::PopStyleColor();
          ImGui::PopID();
        }
      } else if (ImGui::BeginCombo("##value", setting->labels[std::clamp(setting->value_as_int, 0, static_cast<int>(setting->labels.size()) - 1)].c_str())) {
        for (int i = 0; i < static_cast<int>(setting->labels.size()); ++i) {
          if (ImGui::Selectable(setting->labels[i].c_str(), setting->value_as_int == i)) {
            changed = setting->value_as_int != i;
            setting->Set(static_cast<float>(i));
          }
        }
        ImGui::EndCombo();
      }
    } else if (setting->value_type == SettingValueType::INTEGER) {
      changed = ImGui::SliderInt("##value", &setting->value_as_int, static_cast<int>(setting->min), static_cast<int>(setting->GetMax()), setting->format.c_str(), ImGuiSliderFlags_AlwaysClamp);
    } else {
      changed = ImGui::SliderFloat("##value", &setting->value, setting->min, setting->max, setting->format.c_str(),
                                   ImGuiSliderFlags_AlwaysClamp | (setting->is_logarithmic ? ImGuiSliderFlags_Logarithmic : 0));
    }
    if (changed) setting->on_change();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextWrapped("%s", setting->label.c_str());
    if (!setting->tooltip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip("%s", setting->tooltip.c_str());
    }
    ImGui::TableNextColumn();
    if (setting->can_reset) {
      ImGui::BeginDisabled(setting->GetValue() == setting->default_value);
      if (ImGui::Button(renodx::utils::icons::View(renodx::utils::icons::UNDO), ImVec2(ImGui::GetFrameHeight(), 0.f))) {
        setting->Set(setting->default_value);
        changed = true;
      }
      ImGui::SetItemTooltip("Restore default");
      ImGui::EndDisabled();
    }
    ImGui::EndTable();
  }
  if (changed) {
    const std::unique_lock lock(renodx::utils::mutex::global_mutex);
    setting->Write();
    setting->on_change_value(previous, setting->GetValue());
  }
  ImGui::EndDisabled();
  ImGui::PopID();
  return changed;
}

inline void Draw(const renodx::utils::settings::Settings& settings) {
  static char search[128] = {};
  const auto matches = [&](const std::string& text) {
    return search[0] == 0 || std::search(text.begin(), text.end(), std::begin(search), std::begin(search) + std::char_traits<char>::length(search), [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); }) != text.end();
  };
  static int page = 0;
  constexpr const char* pages[] = {"FPS Limiter", "Graphics", "Entities", "Screenshots", "Patches", "Camera", "UI", "Shortcuts", "System"};
  constexpr const char* descriptions[] = {
      "Unlock FPS and adjust frame limits.",
      "Adjust rendering quality.",
      "Adjust entity population and visibility.",
      "Configure photo output.",
      "HDR compatibility and uncensor options.",
      "Adjust camera and first person settings.",
      "Hide game UI elements.",
      "Customize keyboard shortcuts.",
      "Runtime information and addon credits."};
  constexpr struct {
    const char* name;
    int page;
    bool advanced;
  } sections[] = {
      {"Camera Controls", 5, false},
      {"FPV 穿越机", 5, false},
      {"Freecam Shortcuts", 7, false},
      {"UI Shortcuts", 7, false},
      {"First Person Shortcuts", 7, false},
      {"Window Shortcuts", 7, false},
      {"Camera Position", 5, false},
      {"Camera Rotation", 5, false},
      {"First Person", 5, false},
      {"FPS Limit", 0, false},
      {"Ambient Occlusion", 1, false},
      {"Screen Space Reflections", 1, false},
      {"Geometry", 1, false},
      {"Depth of Field", 1, false},
      {"Entity Population & Distance", 2, false},
      {"NPC Culling", 2, true},
      {"NPC Loading", 2, true},
      {"NPC Unloading (Experimental)", 2, true},
      {"Screenshots", 3, false},
      {"UI Visibility", 6, false},
      {"DLSS-G HDR Patch", 4, false},
      {"Uncensor", 4, false},
      {"Runtime Information", 8, false},
      {"About", 8, true}};

  ImGui::PushID("EndfieldEnhancerMenu");
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2.f);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.f, 4.f));
  ImGui::SetNextItemWidth(-1.f);
  ImGui::InputTextWithHint("##search", "Search all settings...", search, IM_ARRAYSIZE(search));
  if (search[0] != 0) {
    if (ImGui::SmallButton("Clear search")) search[0] = 0;
  } else {
    if (ImGui::BeginTabBar("Pages", ImGuiTabBarFlags_FittingPolicyScroll)) {
      for (int i = 0; i < static_cast<int>(std::size(pages)); ++i) {
        if (ImGui::BeginTabItem(pages[i])) {
          page = i;
          ImGui::EndTabItem();
        }
      }
      ImGui::EndTabBar();
    }
    ImGui::TextWrapped("%s", descriptions[page]);
  }
  ImGui::Spacing();
  bool changed = false;
  bool found = false;
  for (const auto& section : sections) {
    if (search[0] == 0 && section.page != page) continue;
    bool has_match = false;
    for (auto* setting : settings) {
      if (setting->section != section.name || (setting->is_visible && !setting->is_visible())) continue;
      if (matches(setting->label + " " + setting->section + " " + setting->tooltip)) {
        has_match = true;
        break;
      }
    }
    if (!has_match) continue;
    found = true;
    ImGui::PushID(search[0] != 0 ? "search" : "page");
    if (search[0] != 0) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    if (ImGui::CollapsingHeader(section.name, section.advanced ? ImGuiTreeNodeFlags_None : ImGuiTreeNodeFlags_DefaultOpen)) {
      for (auto* setting : settings) {
        if (setting->section != section.name || (setting->is_visible && !setting->is_visible())) continue;
        if (!matches(setting->label + " " + setting->section + " " + setting->tooltip)) continue;
        changed |= DrawSetting(setting);
      }
      ImGui::Spacing();
    }
    ImGui::PopID();
  }
  if (!found) ImGui::TextWrapped("No matching settings. Try FPS, reflections, NPC or photo.");
  if (changed) {
    renodx::utils::settings::SaveSettings();
    renodx::utils::settings::SaveGlobalSettings();
  }
  ImGui::PopStyleVar(2);
  ImGui::PopID();
}

}
