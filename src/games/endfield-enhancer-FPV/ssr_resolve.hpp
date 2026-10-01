#pragma once

#include <atomic>
#include <array>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "./ssr_resolve_spirv.hpp"
#include "./ssr_resolve_dx11.hpp"
#include "../../utils/command_action.hpp"

namespace endfield::ssr_resolve {

inline float improved_override_setting = 0.f;
inline std::atomic_bool enabled = false;
inline std::atomic_bool failed = false;
inline std::atomic_bool override_requested = false;
inline std::atomic_bool override_failed = false;
inline bool registered = false;
inline std::recursive_mutex mutex;
struct DevicePipelines {
  std::unordered_map<uint64_t, std::array<reshade::api::pipeline, 3>> by_layout;
  std::vector<reshade::api::pipeline> retired;
  bool override_active = false;
};
inline std::unordered_map<reshade::api::device*, DevicePipelines> devices;
inline std::unordered_set<reshade::api::command_queue*> queues;

inline void OnInitQueue(reshade::api::command_queue* queue) {
  const std::lock_guard lock(mutex);
  queues.insert(queue);
}
inline void OnDestroyQueue(reshade::api::command_queue* queue) {
  const std::lock_guard lock(mutex);
  queues.erase(queue);
}

inline constexpr auto on_dispatch = []<typename Context>(Context& context)
    -> renodx::utils::command_action::CallbackResult<Context> {
  if constexpr (!std::is_same_v<typename Context::ArgumentType,
                                renodx::utils::command_action::DispatchArguments>) {
    return {};
  } else {
    if ((!enabled.load(std::memory_order_relaxed) && !override_requested.load(std::memory_order_relaxed))
        || context.cmd_list == nullptr) return {};
    const auto api = context.cmd_list->get_device()->get_api();
    if (api != reshade::api::device_api::vulkan && api != reshade::api::device_api::d3d11) return {};
    const auto& hashes = api == reshade::api::device_api::d3d11 ? kDx11Hashes : kVulkanHashes;
    auto* state = renodx::utils::command_action::GetShaderState(&context);
    if (state == nullptr) return {};
    auto& stage = state->stage_states[renodx::utils::shader::COMPUTE_INDEX];
    reshade::api::pipeline_layout layout = {};
    uint32_t hash = 0;
    renodx::utils::shader::GetPipelineShaderDetails(stage.pipeline, [&](const auto& details) {
      hash = details.compatible_shader_infos[renodx::utils::shader::COMPUTE_INDEX].shader_hash;
      if (std::find(hashes.begin(), hashes.end(), hash) == hashes.end()) return;
      layout = details.replacement_layout.handle != 0u ? details.replacement_layout
               : details.injection_layout.handle != 0u ? details.injection_layout
                                                       : details.layout;
    });
    const auto match = std::find(hashes.begin(), hashes.end(), hash);
    if (match == hashes.end() || (api == reshade::api::device_api::vulkan && layout.handle == 0u)) return {};

    const size_t index = static_cast<size_t>(match - hashes.begin());
    const bool improved = index != 0;
    if (!(improved ? override_requested : enabled).load(std::memory_order_relaxed)) return {};
    const std::lock_guard lock(mutex);
    auto& device_pipelines = devices[context.cmd_list->get_device()];
    auto& pipeline = device_pipelines.by_layout[layout.handle][index];
    if (pipeline.handle == 0u) {
      if (improved) device_pipelines.override_active = false;
      std::vector<uint32_t> patched;
      size_t source_size = 0;
      if (index == 2) {
        renodx::utils::shader::shared.data->runtime_replacements.if_contains(
            {context.cmd_list->get_device(), hash}, [&](const auto& replacement) {
              source_size = replacement.second.size();
              patched = api == reshade::api::device_api::d3d11
                            ? PrepareDx11Shader(index, replacement.second)
                            : PatchImprovedBlend(replacement.second);
            });
        if (patched.empty()) {
          renodx::utils::shader::shared.data->compile_time_replacements.if_contains(
              {context.cmd_list->get_device(), hash}, [&](const auto& replacement) {
                source_size = (std::max)(source_size, replacement.second.size());
                patched = api == reshade::api::device_api::d3d11
                              ? PrepareDx11Shader(index, replacement.second)
                              : PatchImprovedBlend(replacement.second);
              });
        }
      } else
        renodx::utils::shader::GetPipelineShaderDetails(stage.pipeline, [&](const auto& details) {
          const auto& info = details.compatible_shader_infos[renodx::utils::shader::COMPUTE_INDEX];
          if (info.index >= details.subobjects.size()) return;
          if (auto original = renodx::utils::shader::GetShaderData(details, info)) {
            source_size = original->size();
            patched = api == reshade::api::device_api::d3d11 ? PrepareDx11Shader(index, *original)
                      : improved                             ? PatchImprovedBlur(*original)
                                                             : PatchFullResolutionResolve(*original);
          }
        });
      reshade::api::shader_desc shader = {};
      shader.code = patched.data();
      shader.code_size = patched.size() * sizeof(uint32_t);
      shader.entry_point = "main";
      reshade::api::pipeline_subobject subobject = {
          reshade::api::pipeline_subobject_type::compute_shader, 1, &shader};
      if (patched.empty() || !context.cmd_list->get_device()->create_pipeline(layout, 1, &subobject, &pipeline)) {
        (improved ? override_requested : enabled).store(false, std::memory_order_relaxed);
        (improved ? override_failed : failed).store(true, std::memory_order_relaxed);
        device_pipelines.override_active = false;
        std::ostringstream message;
        message << "E_E_FPV SSR: " << (improved ? "Improved SSR override" : "full-resolution alignment")
                << " unavailable for shader 0x" << std::hex << hash << ": "
                << (source_size == 0  ? "shader bytecode missing"
                    : patched.empty() ? "required instruction pattern not found or ambiguous"
                                      : "compute pipeline creation failed")
                << "; keeping base shader(s).";
        reshade::log::message(reshade::log::level::error, message.str().c_str());
        return {};
      }
    }

    if (improved && !device_pipelines.override_active) return {};
    context.cmd_list->bind_pipeline(reshade::api::pipeline_stage::all_compute, pipeline);
    return {
        .post_callback = [](Context& completed, const void*) {
          auto* current = renodx::utils::command_action::GetShaderState(&completed);
          if (current == nullptr) return;
          auto restore = current->stage_states[renodx::utils::shader::COMPUTE_INDEX].pipeline;
          renodx::utils::shader::GetPipelineShaderDetails(restore, [&](const auto& details) {
            if (details.replacement_pipeline.handle != 0u) restore = details.replacement_pipeline;
          });
          completed.cmd_list->bind_pipeline(reshade::api::pipeline_stage::all_compute, restore);
        },
        .replay = true,
    };
  }
};

inline void OnDestroyLayout(reshade::api::device* device, reshade::api::pipeline_layout layout) {
  const std::lock_guard lock(mutex);
  auto found = devices.find(device);
  if (found == devices.end()) return;
  auto pipeline = found->second.by_layout.find(layout.handle);
  if (pipeline == found->second.by_layout.end()) return;

  for (auto entry : pipeline->second) {
    if (entry.handle != 0u) found->second.retired.push_back(entry);
  }
  found->second.by_layout.erase(pipeline);
  found->second.override_active = false;
}

inline void OnDestroyDevice(reshade::api::device* device) {
  const std::lock_guard lock(mutex);
  auto found = devices.find(device);
  if (found == devices.end()) return;
  for (const auto& [layout, pipelines] : found->second.by_layout) {
    for (auto pipeline : pipelines) {
      if (pipeline.handle != 0u) device->destroy_pipeline(pipeline);
    }
  }
  for (auto pipeline : found->second.retired) device->destroy_pipeline(pipeline);
  devices.erase(found);
}

inline void OnPresent(bool full_resolution, bool improved_override = false) {
  enabled.store(full_resolution && !failed.load(std::memory_order_relaxed), std::memory_order_relaxed);
  override_requested.store(improved_override && !override_failed.load(std::memory_order_relaxed), std::memory_order_relaxed);
  const std::lock_guard lock(mutex);
  for (auto& [device, cached] : devices) {
    bool blur_ready = false, blend_ready = false;
    for (const auto& [layout, pipelines] : cached.by_layout) {
      blur_ready |= pipelines[1].handle != 0u;
      blend_ready |= pipelines[2].handle != 0u;
    }
    cached.override_active = override_requested.load(std::memory_order_relaxed) && blur_ready && blend_ready;
  }
  if (registered) return;

  for (const auto& hashes : {kVulkanHashes, kDx11Hashes}) {
    for (const auto hash : hashes) {
      renodx::utils::command_action::Register(on_dispatch, {.shader_hash = hash,
                                                            .command_types = renodx::utils::command_action::COMMAND_TYPE_DISPATCH});
    }
  }
  renodx::utils::command_action::Use(DLL_PROCESS_ATTACH);
  registered = true;
}

inline void Use(DWORD reason) {
  if (reason == DLL_PROCESS_ATTACH) {
    renodx::utils::shader::use_shader_cache = true;
    renodx::utils::shader::Use(reason);
    renodx::utils::shader::shared.data->use_shader_cache = true;
    renodx::utils::command_action::Use(reason);
    reshade::register_event<reshade::addon_event::destroy_pipeline_layout>(OnDestroyLayout);
    reshade::register_event<reshade::addon_event::destroy_device>(OnDestroyDevice);
    reshade::register_event<reshade::addon_event::init_command_queue>(OnInitQueue);
    reshade::register_event<reshade::addon_event::destroy_command_queue>(OnDestroyQueue);
  } else if (reason == DLL_PROCESS_DETACH) {
    enabled.store(false, std::memory_order_relaxed);
    override_requested.store(false, std::memory_order_relaxed);
    renodx::utils::command_action::Unregister(on_dispatch);
    registered = false;
    reshade::unregister_event<reshade::addon_event::destroy_pipeline_layout>(OnDestroyLayout);
    reshade::unregister_event<reshade::addon_event::destroy_device>(OnDestroyDevice);
    reshade::unregister_event<reshade::addon_event::init_command_queue>(OnInitQueue);
    reshade::unregister_event<reshade::addon_event::destroy_command_queue>(OnDestroyQueue);
    for (auto* queue : queues) queue->wait_idle();
    queues.clear();
    while (!devices.empty()) {
      auto* device = devices.begin()->first;
      OnDestroyDevice(device);
    }
    renodx::utils::command_action::Use(reason);
    renodx::utils::shader::Use(reason);
  }
}

}
