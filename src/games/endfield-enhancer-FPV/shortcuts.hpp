#pragma once

#include "./window_enhancements.hpp"

namespace endfield::shortcuts {
using renodx::utils::settings::Setting;
inline Setting* capturing = nullptr;
inline int capture_frame = 0;
inline int capture_started = 0;
inline std::string message;
inline Setting* message_owner = nullptr;
inline constexpr size_t global_shortcut_count = 13;
inline constexpr const char* keys[] = {"ShortcutFreeCamera", "ShortcutHideUI", "ShortcutFirstPerson", "ShortcutFullscreen",
                                       "ShortcutHideUID", "ShortcutHideLatencyBar", "ShortcutHidePing", "ShortcutHideQuestLog",
                                       "ShortcutHideMap", "ShortcutHideMapButtons", "ShortcutHideMenuButtons", "ShortcutHideUtilityWheel",
                                       "ShortcutFPV",
                                       "ShortcutFreeExit", "ShortcutFreeForward", "ShortcutFreeBackward", "ShortcutFreeLeft", "ShortcutFreeRight",
                                       "ShortcutFreeDown", "ShortcutFreeUp", "ShortcutFreeBoost"};

inline Setting* Create(const char* key, float default_key, const char* label, const char* section) {
  return new Setting{.key = key, .value_type = renodx::utils::settings::SettingValueType::INTEGER, .default_value = default_key, .label = label, .section = section, .max = 2047.f};
}

inline bool Conflicts(const Setting* setting, const Setting* other, int binding) {
  if (!binding || !other || other == setting || other->value_as_int != binding) return false;
  if (setting->section == other->section) return setting->section != "UI Shortcuts";

  return !(setting->section == "Freecam Shortcuts" && setting->key != "ShortcutFreeCamera")
         && !(other->section == "Freecam Shortcuts" && other->key != "ShortcutFreeCamera");
}

inline bool Matches(reshade::api::effect_runtime* runtime, int binding, int modifiers, bool pressed) {
  const int key = binding & 255;
  if (!key) return false;

  if (key == VK_SHIFT || key == VK_LSHIFT || key == VK_RSHIFT) modifiers &= ~512;
  if (key == VK_CONTROL || key == VK_LCONTROL || key == VK_RCONTROL) modifiers &= ~256;
  if (key == VK_MENU || key == VK_LMENU || key == VK_RMENU) modifiers &= ~1024;
  if (pressed ? modifiers != (binding & ~255) : (modifiers & (binding & ~255)) != (binding & ~255)) return false;

  return pressed ? runtime->is_key_pressed(key) : runtime->is_key_down(key);
}

inline std::string Name(int binding) {
  if (!(binding & 255)) return "Unbound";
  std::string result;
  if (binding & 256) result += "Ctrl + ";
  if (binding & 512) result += "Shift + ";
  if (binding & 1024) result += "Alt + ";
  switch (binding & 255) {
    case VK_LBUTTON:  return result + "Left Mouse";
    case VK_RBUTTON:  return result + "Right Mouse";
    case VK_MBUTTON:  return result + "Middle Mouse";
    case VK_XBUTTON1: return result + "Mouse 4";
    case VK_XBUTTON2: return result + "Mouse 5";
  }
  char name[64]{};
  UINT scan = MapVirtualKeyA(binding & 255, MAPVK_VK_TO_VSC);
  if ((binding & 255) >= VK_PRIOR && (binding & 255) <= VK_DOWN) scan |= 0x100;
  if ((binding & 255) == VK_INSERT || (binding & 255) == VK_DELETE) scan |= 0x100;
  if (GetKeyNameTextA(static_cast<LONG>(scan << 16), name, sizeof(name)))
    result += name;
  else
    result += "Key " + std::to_string(binding & 255);
  return result;
}

inline bool Draw(Setting* setting) {
  bool changed = false;
  ImGui::PushID(setting->key.c_str());
  if (ImGui::Button(capturing == setting ? "Press a key..." : Name(setting->value_as_int).c_str(), ImVec2(180, 0))) {
    capturing = setting;
    capture_started = ImGui::GetFrameCount();
    message.clear();
  }
  ImGui::SameLine();
  ImGui::TextUnformatted(setting->label.c_str());
  ImGui::SameLine();
  if (ImGui::SmallButton("Clear")) {
    setting->Set(0)->Write();
    capturing = nullptr;
    message.clear();
    changed = true;
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Reset")) {
    bool conflict = false;
    for (const char* key : keys) {
      auto* other = renodx::utils::settings::FindSetting(key);
      if (Conflicts(setting, other, static_cast<int>(setting->default_value))) conflict = true;
    }
    if (conflict) {
      message_owner = setting;
      message = "That shortcut is already assigned. Clear its other binding first.";
    } else {
      setting->Set(setting->default_value)->Write();
      capturing = nullptr;
      message.clear();
      changed = true;
    }
  }
  if (capturing == setting) {
    capture_frame = ImGui::GetFrameCount();
    ImGui::SetNextFrameWantCaptureKeyboard(true);
    ImGui::TextWrapped("Press a key or mouse button. Tap and release a modifier to bind it alone. Escape cancels; Delete clears.");
  }
  if (message_owner == setting && !message.empty()) ImGui::TextWrapped("%s", message.c_str());
  ImGui::PopID();
  return changed;
}

inline void Set(Setting* setting, float value) {
  if (!setting || (setting->is_enabled && !setting->is_enabled())) return;
  const float previous = setting->GetValue();
  setting->Set(value);
  setting->on_change();
  {
    const std::unique_lock lock(renodx::utils::mutex::global_mutex);
    setting->Write();
  }
  setting->on_change_value(previous, value);
}

inline void OnFrame(reshade::api::effect_runtime* runtime) {
  using namespace renodx::utils::settings;
  const bool focused = runtime->get_hwnd() == GetForegroundWindow();
  if (!focused || (capturing && ImGui::GetFrameCount() > capture_frame + 1)) {
    capturing = nullptr;
    message.clear();
  }
  int modifiers = (runtime->is_key_down(VK_CONTROL) ? 256 : 0)
                  | (runtime->is_key_down(VK_SHIFT) ? 512 : 0)
                  | (runtime->is_key_down(VK_MENU) ? 1024 : 0);
  const bool was_capturing = capturing != nullptr;
  if (capturing && ImGui::GetFrameCount() > capture_started) {
    runtime->block_input_next_frame();
    for (int key = 1; key < 256; ++key) {
      if (key == VK_CANCEL || (key >= VK_LSHIFT && key <= VK_RMENU)) continue;
      const bool modifier = key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU;
      if (modifier ? !runtime->is_key_released(key) : !Matches(runtime, key | modifiers, modifiers, true)) continue;
      if (key == VK_ESCAPE) {
        capturing = nullptr;
        message.clear();
        break;
      }
      if (key == VK_LWIN || key == VK_RWIN) continue;
      const int binding = key == VK_DELETE ? 0 : key | modifiers;
      bool conflict = false;
      for (const char* name : keys) {
        auto* other = FindSetting(name);
        if (Conflicts(capturing, other, binding)) conflict = true;
      }
      if (conflict) {
        message_owner = capturing;
        message = "That shortcut is already assigned. Choose another key combination.";
      } else {
        Set(capturing, static_cast<float>(binding));
        capturing = nullptr;
        message.clear();
        SaveSettings();
        SaveGlobalSettings();
      }
      break;
    }
  } else if (!capturing && focused && !runtime->is_key_down(VK_LWIN) && !runtime->is_key_down(VK_RWIN)
             && !ImGui::GetIO().WantCaptureKeyboard && !ImGui::GetIO().WantCaptureMouse) {
    bool settings_changed = false;
    for (size_t i = 0; i < global_shortcut_count; ++i) {
      auto* binding = FindSetting(keys[i]);
      if (!binding || !Matches(runtime, binding->value_as_int, modifiers, true)) continue;
      bool consumed = false;
      if (camera::detail::freecam::active.load()) {
        for (size_t j = global_shortcut_count; j < std::size(keys); ++j) {
          auto* control = FindSetting(keys[j]);
          if (control && (control->value_as_int & 255) == (binding->value_as_int & 255)
              && Matches(runtime, control->value_as_int, modifiers, true)) consumed = true;
        }
      }
      if (consumed) continue;
      if (i == 0) {
        namespace freecam = camera::detail::freecam;
        if (freecam::available.load() && camera::enabled >= .5f) freecam::requested.store(!freecam::requested.load());
      } else if (i == 3) {
        window_enhancements::request_fullscreen_toggle();
      } else if (i == 12) {
        namespace freecam = camera::detail::freecam;
        namespace fpv_ns = camera::detail::fpv;
        if (freecam::available.load() && camera::enabled >= .5f) {
          const bool enable = !fpv_ns::requested.load();
          fpv_ns::requested.store(enable);
          freecam::requested.store(enable);
        }
      } else {
        auto* target = FindSetting(i == 2 ? "CameraFirstPerson" : binding->key.substr(sizeof("Shortcut") - 1));
        if (target && (!target->is_enabled || target->is_enabled())) {
          if (i == 2 && target->GetValue() < .5f) {
            camera::detail::freecam::requested.store(false);
            Set(FindSetting("CameraControls"), 1.f);
          }
          Set(target, target->GetValue() >= .5f ? 0.f : 1.f);
          settings_changed = true;
        }
      }
      if (binding->section != "UI Shortcuts") break;
    }
    if (settings_changed) {
      SaveSettings();
      SaveGlobalSettings();
    }
  }
  camera::detail::freecam::Controls controls;
  bool* actions[] = {&controls.exit, &controls.forward, &controls.backward, &controls.left, &controls.right,
                     &controls.down, &controls.up, &controls.boost};
  for (size_t i = 0; i < std::size(actions); ++i) {
    auto* binding = FindSetting(keys[i + global_shortcut_count]);
    *actions[i] = binding && Matches(runtime, binding->value_as_int, modifiers, i == 0);
  }
  camera::detail::freecam::OnFrame(runtime, controls, !was_capturing);
}
}
