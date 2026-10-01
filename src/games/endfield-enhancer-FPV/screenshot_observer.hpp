#pragma once
#include <Windows.h>
#include "./native_hooks.hpp"
#include <detours.h>
#include <include/reshade.hpp>
#include <array>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <vector>
#include "../../utils/hash.hpp"
#include "./screenshot_color.hpp"

namespace endfield::screenshots::observer {
using namespace reshade::api;
using CreatePipeline = bool (*)(device*, pipeline_layout, uint32_t, const pipeline_subobject*, pipeline*);
using DestroyLayout = void (*)(device*, pipeline_layout);
using PushConstants = void (*)(command_list*, shader_stage, pipeline_layout, uint32_t, uint32_t, uint32_t, const void*);
inline CreatePipeline create_pipeline = nullptr;
inline DestroyLayout destroy_layout = nullptr;
inline PushConstants push_constants = nullptr;
inline bool device_hooked = false, commands_hooked = false;
inline std::mutex mutex;
inline std::set<std::pair<device*, uint64_t>> layouts;
inline std::map<std::pair<device*, uint64_t>, uint32_t> calibration_layouts;
struct Calibration {
  float value = 0;
  uint64_t time = 0, layout = 0;
};
inline std::map<device*, Calibration> calibrations;
inline ColorConfig latest;
inline device* latest_device = nullptr;
inline uint64_t latest_layout = 0, timestamp = 0;

inline bool IsOutputShader(const shader_desc& shader) {
  return shader.code && shader.code_size == 18416
         && renodx::utils::hash::ComputeCRC32(static_cast<const uint8_t*>(shader.code), shader.code_size) == 0xa2fa88dcu;
}
inline bool HookCreatePipeline(device* dev, pipeline_layout layout, uint32_t count,
                               const pipeline_subobject* objects, pipeline* output) {
  const bool result = create_pipeline(dev, layout, count, objects, output);
  if (!result) return result;
  bool matches = false;
  uint32_t calibration_count = 0;
  for (uint32_t i = 0; i < count; ++i) {
    if (objects[i].type == pipeline_subobject_type::pixel_shader && objects[i].count == 1 && objects[i].data) {
      const auto& shader = *static_cast<const shader_desc*>(objects[i].data);
      matches |= IsOutputShader(shader);

      if (shader.code && (shader.code_size == 10500 || shader.code_size == 10084)) {
        const auto crc = renodx::utils::hash::ComputeCRC32(static_cast<const uint8_t*>(shader.code), shader.code_size);
        if (shader.code_size == 10500 && crc == 0xadf9a37au)
          calibration_count = 61;
        else if (shader.code_size == 10084 && crc == 0x7b147985u)
          calibration_count = 62;
      }
    }
  }
  std::lock_guard lock(mutex);
  if (matches) {
    layouts.insert({dev, layout.handle});
  }

  if (calibration_count != 0) {
    calibration_layouts[{dev, layout.handle}] = calibration_count;
  }
  return result;
}
inline void HookDestroyLayout(device* dev, pipeline_layout layout) {
  {
    std::lock_guard lock(mutex);
    layouts.erase({dev, layout.handle});
    calibration_layouts.erase({dev, layout.handle});
    if (auto it = calibrations.find(dev); it != calibrations.end() && it->second.layout == layout.handle) calibrations.erase(it);
    if (latest_device == dev && latest_layout == layout.handle) timestamp = 0;
  }
  destroy_layout(dev, layout);
}
inline void HookPushConstants(command_list* cmd, shader_stage stages, pipeline_layout layout,
                              uint32_t param, uint32_t first, uint32_t count, const void* values) {
  if (first == 0 && count == 8 && values && (stages & shader_stage::pixel) == shader_stage::pixel) {
    std::lock_guard lock(mutex);
    auto* dev = cmd->get_device();
    if (layouts.contains({dev, layout.handle})) {
      std::array<float, 8> data;
      std::memcpy(data.data(), values, sizeof(data));
      latest = {.white_nits = data[1], .peak_nits = data[0], .decoding = data[2], .gamma = data[3], .custom_color_space = data[4], .output_encoding = data[6]};
      latest_device = dev;
      latest_layout = layout.handle;
      timestamp = GetTickCount64();
    }
  }

  if (first == 0 && (count == 61 || count == 62) && values && (stages & shader_stage::pixel) == shader_stage::pixel) {
    std::lock_guard lock(mutex);
    auto* dev = cmd->get_device();
    if (const auto it = calibration_layouts.find({dev, layout.handle});
        it != calibration_layouts.end() && count == it->second) {
      float value;
      std::memcpy(&value, static_cast<const uint8_t*>(values) + 56 * sizeof(float), sizeof(value));
      calibrations[dev] = {value, GetTickCount64(), layout.handle};
    }
  }

  push_constants(cmd, stages, layout, param, first, count, values);
}
inline bool ReadColor(ColorConfig* config) {
  std::lock_guard lock(mutex);
  if (!timestamp || GetTickCount64() - timestamp > 2000) return false;
  *config = latest;
  if (auto it = calibrations.find(latest_device); it != calibrations.end()) {
    if (GetTickCount64() - it->second.time > 2000) return false;
    config->tech_test_look = it->second.value;
  }
  return true;
}
inline bool ChangeHooks(bool attach, bool device_hooks) {
  return native_hooks::Update(attach ? "Screenshot observer install" : "Screenshot observer removal", [attach, device_hooks]() -> LONG {
    if (!device_hooks) return attach ? DetourAttach(&push_constants, HookPushConstants) : DetourDetach(&push_constants, HookPushConstants);
    LONG result = attach ? DetourAttach(&create_pipeline, HookCreatePipeline) : DetourDetach(&create_pipeline, HookCreatePipeline);
    if (result == NO_ERROR) result = attach ? DetourAttach(&destroy_layout, HookDestroyLayout) : DetourDetach(&destroy_layout, HookDestroyLayout);
    return result;
  });
}
inline bool IsHostFunction(const void* address) {
  MEMORY_BASIC_INFORMATION info{};
  return address && VirtualQuery(address, &info, sizeof(info)) == sizeof(info)
         && info.AllocationBase == reshade::internal::get_reshade_module_handle()
         && info.State == MEM_COMMIT && !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS))
         && (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}
inline void OnInitDevice(device* dev) {
  if (dev->get_api() != device_api::vulkan || device_hooked) return;

  auto** table = *reinterpret_cast<void***>(dev);
  if (!IsHostFunction(table[21]) || !IsHostFunction(table[24])) return;
  create_pipeline = reinterpret_cast<CreatePipeline>(table[21]);
  destroy_layout = reinterpret_cast<DestroyLayout>(table[24]);
  device_hooked = ChangeHooks(true, true);
}
inline void OnInitCommandList(command_list* cmd) {
  if (cmd->get_device()->get_api() != device_api::vulkan || commands_hooked) return;
  auto** table = *reinterpret_cast<void***>(cmd);
  if (!IsHostFunction(table[12])) return;
  push_constants = reinterpret_cast<PushConstants>(table[12]);
  commands_hooked = ChangeHooks(true, false);
}
inline void OnInitQueue(command_queue* queue) {
  if (queue->get_device()->get_api() != device_api::vulkan || commands_hooked) return;
  if (auto* cmd = queue->get_immediate_command_list()) OnInitCommandList(cmd);
}
inline void OnDestroyDevice(device* dev) {
  std::lock_guard lock(mutex);
  std::erase_if(layouts, [dev](const auto& item) { return item.first == dev; });
  std::erase_if(calibration_layouts, [dev](const auto& item) { return item.first.first == dev; });
  calibrations.erase(dev);
  if (latest_device == dev) {
    timestamp = 0;
    latest_device = nullptr;
    latest_layout = 0;
  }
}
inline void Shutdown() {
  if (commands_hooked && ChangeHooks(false, false)) commands_hooked = false;
  if (device_hooked && ChangeHooks(false, true)) device_hooked = false;
  std::lock_guard lock(mutex);
  calibration_layouts.clear();
  calibrations.clear();
  layouts.clear();
  timestamp = 0;
  latest_device = nullptr;
  latest_layout = 0;
}
}
