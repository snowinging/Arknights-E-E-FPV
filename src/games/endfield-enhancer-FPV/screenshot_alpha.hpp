#pragma once
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include "../../utils/command_action.hpp"
#include "./screenshot_resource.hpp"

namespace endfield::screenshots::photo_alpha {
namespace api = reshade::api;

inline std::vector<uint32_t> Patch(std::span<const uint8_t> bytes) {
  if (bytes.size() != 896
      || renodx::utils::hash::ComputeCRC32(bytes.data(), bytes.size()) != 0x105b9793u) return {};
  std::vector<uint32_t> words(bytes.size() / 4);
  std::memcpy(words.data(), bytes.data(), bytes.size());
  if (words[0] != 0x07230203 || words[3] > UINT32_MAX - 2) return {};
  uint32_t scalar = 0, vector = 0, sampled = 0, output = 0;
  size_t function = 0, store = 0;
  for (size_t i = 5; i < words.size();) {
    const auto count = words[i] >> 16, op = words[i] & 0xffff;
    if (!count || count > words.size() - i) return {};
    if (op == 22 && count == 3 && words[i + 2] == 32) scalar = words[i + 1];
    if (op == 23 && count == 4 && words[i + 2] == scalar && words[i + 3] == 4) vector = words[i + 1];
    if (op == 54 && !function) function = i;
    if (op == 88 && count == 7 && words[i + 1] == vector) sampled = words[i + 2];
    if (op == 62) {
      if (store || count != 3 || !sampled || words[i + 2] != sampled) return {};
      store = i;
      output = words[i + 1];
    }
    i += count;
  }
  if (!scalar || !vector || !function || !store || !output) return {};
  const uint32_t one = words[3], opaque = one + 1;

  words[store + 2] = opaque;
  words.insert(words.begin() + store, {(6u << 16) | 82u, vector, opaque, one, sampled, 3u});
  words.insert(words.begin() + function, {(4u << 16) | 43u, scalar, one, 0x3f800000u});
  words[3] += 2;
  return words;
}

inline std::recursive_mutex mutex;
struct Pending {
  api::pipeline_layout layout{};
  std::vector<api::pipeline_layout_param> params;
  std::vector<std::vector<api::descriptor_range>> ranges;
  bool CopyLayout(std::span<const api::pipeline_layout_param> source) {
    params.assign(source.begin(), source.end());
    ranges.resize(params.size());
    for (size_t i = 0; i < params.size(); ++i) {
      auto& param = params[i];
      if (param.type == api::pipeline_layout_param_type::descriptor_table
          || param.type == api::pipeline_layout_param_type::push_descriptors_with_ranges) {
        if (param.descriptor_table.count && !param.descriptor_table.ranges) return false;
        if (param.descriptor_table.count) ranges[i].assign(param.descriptor_table.ranges,
                                                           param.descriptor_table.ranges + param.descriptor_table.count);
        param.descriptor_table.ranges = ranges[i].data();
      } else if (param.type != api::pipeline_layout_param_type::push_constants
                 && param.type != api::pipeline_layout_param_type::push_descriptors)
        return false;
    }
    return !params.empty();
  }
  uint32_t count = 0;
  api::pipeline_subobject* objects = nullptr;
  ~Pending() {
    if (objects) renodx::utils::pipeline::DestroyPipelineSubobjects(objects, count);
  }
};
inline std::map<std::pair<api::device*, uint64_t>, std::shared_ptr<Pending>> pending_pipelines;
struct Cached {
  api::pipeline_layout layout{};
  api::pipeline pipeline{};
  api::pipeline_layout owned_layout{};
};
inline std::map<std::pair<api::device*, uint64_t>, Cached> pipelines;
inline std::map<api::device*, std::vector<Cached>> retired;
inline std::set<api::command_list*> targets;
inline std::set<api::command_queue*> queues;
inline bool registered = false;
inline bool Ready(api::device* device) {
  std::lock_guard lock(mutex);
  for (const auto& [key, cached] : pipelines)
    if (key.first == device && cached.pipeline.handle) return true;
  return false;
}

inline bool IsPhotoTarget(api::device* device, api::resource_view view) {
  bool match = false;
  if (!view.handle) return false;
  renodx::utils::resource::GetResourceInfo(device->get_resource_from_view(view), [&](const auto& info) {
    match = !info.destroyed && info.device == device && info.upgraded
            && info.upgrade_target == &photo_resource::target
            && info.desc.texture.format == api::format::r16g16b16a16_float;
  });
  return match;
}
inline void OnBegin(api::command_list* cmd, uint32_t count,
                    const api::render_pass_render_target_desc* color,
                    const api::render_pass_depth_stencil_desc*) {
  if (cmd->get_device()->get_api() != api::device_api::vulkan) return;
  const bool match = count == 1 && color && IsPhotoTarget(cmd->get_device(), color[0].view);
  std::lock_guard lock(mutex);
  if (match)
    targets.insert(cmd);
  else
    targets.erase(cmd);
}
inline void OnEnd(api::command_list* cmd) {
  std::lock_guard lock(mutex);
  targets.erase(cmd);
}

inline api::pipeline Prepare(api::device* device, api::pipeline_layout layout,
                             uint32_t count, const api::pipeline_subobject* source) {
  uint32_t pixel_index = count;
  std::vector<uint32_t> patched;
  for (uint32_t i = 0; i < count; ++i) {
    if (source[i].type != api::pipeline_subobject_type::pixel_shader || source[i].count != 1 || !source[i].data) continue;
    const auto& shader = *static_cast<const api::shader_desc*>(source[i].data);
    if (!shader.code) return {};
    patched = Patch({static_cast<const uint8_t*>(shader.code), shader.code_size});
    if (patched.empty()) return {};
    pixel_index = i;
  }
  if (pixel_index == count) return {};
  auto* objects = renodx::utils::pipeline::ClonePipelineSubObjects(source, count);
  renodx::utils::shader::AddShaderReplacement(&objects[pixel_index],
                                              {reinterpret_cast<const uint8_t*>(patched.data()), patched.size() * sizeof(uint32_t)});
  bool compatible = false, opaque_copy = false;
  for (uint32_t i = 0; i < count; ++i)
    if (objects[i].type == api::pipeline_subobject_type::render_target_formats) {
      if (objects[i].count == 1 && objects[i].data) {
        auto& format = *static_cast<api::format*>(objects[i].data);
        compatible = format == api::format::r11g11b10_float || format == api::format::r16g16b16a16_float;
        if (compatible) format = api::format::r16g16b16a16_float;
      }
    }
  for (uint32_t i = 0; i < count; ++i)
    if (objects[i].type == api::pipeline_subobject_type::blend_state
        && objects[i].count == 1 && objects[i].data) {
      const auto& blend = *static_cast<const api::blend_desc*>(objects[i].data);

      opaque_copy = !blend.blend_enable[0] && (blend.render_target_write_mask[0] & 15) == 15;
    }
  api::pipeline result{};
  if (compatible && opaque_copy) device->create_pipeline(layout, count, objects, &result);
  renodx::utils::pipeline::DestroyPipelineSubobjects(objects, count);
  return result;
}

inline void OnInitPipeline(api::device* device, api::pipeline_layout layout, uint32_t count,
                           const api::pipeline_subobject* objects, api::pipeline original) {
  if (device->get_api() != api::device_api::vulkan) return;
  bool match = false;
  for (uint32_t i = 0; i < count; ++i) {
    if (objects[i].type != api::pipeline_subobject_type::pixel_shader || objects[i].count != 1 || !objects[i].data) continue;
    const auto& shader = *static_cast<const api::shader_desc*>(objects[i].data);
    match = shader.code && shader.code_size == 896
            && renodx::utils::hash::ComputeCRC32(static_cast<const uint8_t*>(shader.code), shader.code_size) == 0x105b9793u;
  }
  if (!match) return;
  auto pending = std::make_shared<Pending>();
  bool copied = false;
  renodx::utils::pipeline_layout::GetPipelineLayoutData(layout, [&](const auto* data) {
    copied = pending->CopyLayout({data->params.data(), data->params.size()});
  });
  if (!copied) return;
  pending->layout = layout;
  pending->count = count;
  pending->objects = renodx::utils::pipeline::ClonePipelineSubObjects(objects, count);
  std::lock_guard lock(mutex);
  pending_pipelines[{device, original.handle}] = std::move(pending);
}
inline api::pipeline PrepareOne(const std::pair<api::device*, uint64_t>& key) {
  std::shared_ptr<Pending> pending;
  {
    std::lock_guard lock(mutex);
    if (auto found = pipelines.find(key); found != pipelines.end()) return found->second.pipeline;
    auto found = pending_pipelines.find(key);
    if (found == pending_pipelines.end()) return {};
    pending = found->second;
  }

  api::pipeline_layout owned{};
  if (!key.first->create_pipeline_layout(static_cast<uint32_t>(pending->params.size()),
                                         pending->params.data(), &owned)) return {};
  const auto replacement = Prepare(key.first, owned, pending->count, pending->objects);
  if (!replacement.handle) {
    key.first->destroy_pipeline_layout(owned);
    return {};
  }
  std::lock_guard lock(mutex);
  const auto found = pending_pipelines.find(key);
  Cached created{pending->layout, replacement, owned};
  if (found == pending_pipelines.end() || found->second != pending || pipelines.contains(key)) {
    retired[key.first].push_back(created);
    return {};
  }
  pending_pipelines.erase(found);
  pipelines[key] = created;
  return replacement;
}
inline void OnPhotoRequested() {
  std::vector<std::pair<api::device*, uint64_t>> keys;
  {
    std::lock_guard lock(mutex);
    for (const auto& [key, pending] : pending_pipelines) keys.push_back(key);
  }
  for (const auto& key : keys) PrepareOne(key);
}
inline void OnDestroyLayout(api::device* device, api::pipeline_layout layout) {
  std::lock_guard lock(mutex);
  std::erase_if(pending_pipelines, [&](const auto& entry) {
    return entry.first.first == device && entry.second->layout == layout;
  });
  for (auto it = pipelines.begin(); it != pipelines.end();) {
    if (it->first.first != device || it->second.layout != layout) {
      ++it;
      continue;
    }

    retired[device].push_back(it->second);
    it = pipelines.erase(it);
  }
}

inline constexpr auto on_copy = []<typename Context>(Context& context)
    -> renodx::utils::command_action::CallbackResult<Context> {
  if (context.IsDispatch() || !context.cmd_list) return {};
  auto* cmd = context.cmd_list;
  if (cmd->get_device()->get_api() != api::device_api::vulkan) return {};
  {
    std::lock_guard lock(mutex);
    if (!targets.contains(cmd)) return {};
  }
  auto* state = renodx::utils::command_action::GetShaderState(&context);
  if (!state) return {};
  const auto original = state->stage_states[renodx::utils::shader::PIXEL_INDEX].pipeline;
  const auto key = std::make_pair(cmd->get_device(), original.handle);

  const auto replacement = PrepareOne(key);
  if (!replacement.handle) return {};
  cmd->bind_pipeline(api::pipeline_stage::all_graphics, replacement);
  return {.post_callback = [](Context& completed, const void*) {
            auto* state = renodx::utils::command_action::GetShaderState(&completed);
            if (state) completed.cmd_list->bind_pipeline(api::pipeline_stage::all_graphics,
                                                         state->stage_states[renodx::utils::shader::PIXEL_INDEX].pipeline);
          },
          .replay = true};
};
inline void OnDestroyPipeline(api::device* device, api::pipeline pipeline) {
  std::lock_guard lock(mutex);
  pending_pipelines.erase({device, pipeline.handle});
  auto it = pipelines.find({device, pipeline.handle});
  if (it == pipelines.end()) return;
  if (it->second.pipeline.handle) retired[device].push_back(it->second);
  pipelines.erase(it);
}
inline void OnDestroyDevice(api::device* device) {
  std::vector<Cached> destroy;
  {
    std::lock_guard lock(mutex);
    std::erase_if(pending_pipelines, [&](const auto& entry) { return entry.first.first == device; });
    std::erase_if(targets, [&](auto* cmd) { return cmd->get_device() == device; });
    for (auto it = pipelines.begin(); it != pipelines.end();) {
      if (it->first.first != device) {
        ++it;
        continue;
      }
      if (it->second.pipeline.handle) destroy.push_back(it->second);
      it = pipelines.erase(it);
    }
    if (auto it = retired.find(device); it != retired.end()) {
      destroy.insert(destroy.end(), it->second.begin(), it->second.end());
      retired.erase(it);
    }
  }
  for (const auto& cached : destroy) {
    device->destroy_pipeline(cached.pipeline);
    if (cached.owned_layout.handle) device->destroy_pipeline_layout(cached.owned_layout);
  }
}
inline void OnQueue(api::command_queue* q) {
  std::lock_guard lock(mutex);
  queues.insert(q);
}
inline void OnDestroyQueue(api::command_queue* q) {
  std::lock_guard lock(mutex);
  queues.erase(q);
}
inline void OnPresent() {
  if (registered) return;
  renodx::utils::command_action::Register(on_copy, {.shader_hash = 0x105b9793u, .command_types = renodx::utils::command_action::COMMAND_TYPE_DIRECT_DRAW});
  registered = true;
}
inline void Use(DWORD reason) {
  if (reason == DLL_PROCESS_ATTACH) {
    photo_resource::can_upgrade = Ready;
    renodx::utils::shader::Use(reason);
    renodx::utils::command_action::Use(reason);
    reshade::register_event<reshade::addon_event::init_pipeline>(OnInitPipeline);
    reshade::register_event<reshade::addon_event::begin_render_pass>(OnBegin);
    reshade::register_event<reshade::addon_event::end_render_pass>(OnEnd);
    reshade::register_event<reshade::addon_event::reset_command_list>(OnEnd);
    reshade::register_event<reshade::addon_event::destroy_command_list>(OnEnd);
    reshade::register_event<reshade::addon_event::destroy_pipeline>(OnDestroyPipeline);
    reshade::register_event<reshade::addon_event::destroy_pipeline_layout>(OnDestroyLayout);
    reshade::register_event<reshade::addon_event::destroy_device>(OnDestroyDevice);
    reshade::register_event<reshade::addon_event::init_command_queue>(OnQueue);
    reshade::register_event<reshade::addon_event::destroy_command_queue>(OnDestroyQueue);
  } else if (reason == DLL_PROCESS_DETACH) {
    photo_resource::can_upgrade = nullptr;
    renodx::utils::command_action::Unregister(on_copy);
    registered = false;
    reshade::unregister_event<reshade::addon_event::init_pipeline>(OnInitPipeline);
    reshade::unregister_event<reshade::addon_event::begin_render_pass>(OnBegin);
    reshade::unregister_event<reshade::addon_event::end_render_pass>(OnEnd);
    reshade::unregister_event<reshade::addon_event::reset_command_list>(OnEnd);
    reshade::unregister_event<reshade::addon_event::destroy_command_list>(OnEnd);
    reshade::unregister_event<reshade::addon_event::destroy_pipeline>(OnDestroyPipeline);
    reshade::unregister_event<reshade::addon_event::destroy_pipeline_layout>(OnDestroyLayout);
    reshade::unregister_event<reshade::addon_event::destroy_device>(OnDestroyDevice);
    reshade::unregister_event<reshade::addon_event::init_command_queue>(OnQueue);
    reshade::unregister_event<reshade::addon_event::destroy_command_queue>(OnDestroyQueue);
    for (auto* q : queues) q->wait_idle();
    queues.clear();
    targets.clear();
    pending_pipelines.clear();
    while (!pipelines.empty()) OnDestroyDevice(pipelines.begin()->first.first);
    while (!retired.empty()) OnDestroyDevice(retired.begin()->first);
    renodx::utils::command_action::Use(reason);
    renodx::utils::shader::Use(reason);
  }
}
}
