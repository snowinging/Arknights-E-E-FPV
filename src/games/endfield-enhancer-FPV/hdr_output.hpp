#pragma once

#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <sstream>
#include <unordered_map>
#include <vector>

#include <detours.h>
#include <embed/shaders.h>
#include <include/reshade.hpp>

#include "../../mods/swapchain.hpp"
#include "../../utils/render.hpp"
#include "../../utils/resource_upgrade.hpp"
#include "./hdr_output_contract.hpp"
#include "./hdr_graphics_bindings.hpp"

namespace endfield::hdr_output {

struct OutputState {
  struct CachedPhysicalPipeline {
    uint64_t back_buffer;
    std::span<const uint8_t> shader;
    reshade::api::pipeline pipeline;
  };
  std::span<const uint8_t> vertex_shader;
  std::span<const uint8_t> pixel_shader;
  const float* parameters = nullptr;
  bool copy_only = false;
  bool want_copy_only = false;
  bool compatibility_mode = false;

  std::unordered_map<uint64_t, uint64_t> working_copies;

  std::vector<CachedPhysicalPipeline> physical_pipelines;

  std::vector<reshade::api::pipeline> retired_pipelines;
};

struct ImageState {
  reshade::api::device* device = nullptr;
  reshade::api::resource original = {};
  reshade::api::resource clone = {};
  uint32_t width = 0;
  uint32_t height = 0;
  bool active = false;
  bool acquired = false;
  bool encoded = false;
  bool failed = false;
  bool drawn = false;
  const char* preparation_failure = nullptr;
  std::array<float, 20> parameters = {}; // 与 FpvHudPayload 对齐(8 HDR契约 + 12 FPV HUD)
  renodx::utils::render::RenderPass pass;
};

struct SwapchainState {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint64_t> images;
};

struct CommandState {
  reshade::api::command_list* command_list = nullptr;
  bool in_render_pass = false;
  uint64_t output_image = 0u;
  GraphicsBindings graphics_bindings;
};

struct GraphicsPipeline {
  reshade::api::device* device = nullptr;
  reshade::api::pipeline_layout layout = {};
  uint32_t count = 0u;
  reshade::api::pipeline_subobject* subobjects = nullptr;
  reshade::api::pipeline fp16 = {};
  reshade::api::pipeline restore = {};
};

inline std::recursive_mutex mutex;
inline std::unordered_map<uint64_t, SwapchainState> swapchains;
inline std::unordered_map<uint64_t, std::unique_ptr<ImageState>> images;
inline std::unordered_map<reshade::api::device*, OutputState> outputs;
inline std::unordered_map<uint64_t, CommandState> commands;
inline std::unordered_map<uint64_t, GraphicsPipeline> graphics;
inline thread_local bool forwarding = false;
inline std::atomic_bool error_logged = false;
inline std::atomic_bool capture_graphics = false;
inline bool events_registered = false;

inline PFN_vkGetSwapchainImagesKHR get_images = nullptr;
inline PFN_vkAcquireNextImageKHR acquire_image = nullptr;
inline PFN_vkDestroySwapchainKHR destroy_swapchain = nullptr;
inline PFN_vkQueuePresentKHR queue_present = nullptr;
inline PFN_vkCmdPipelineBarrier pipeline_barrier = nullptr;
inline PFN_vkCmdPipelineBarrier2 pipeline_barrier2 = nullptr;
inline PFN_vkCmdBindDescriptorSets bind_descriptor_sets = nullptr;

inline void VKAPI_CALL HookBindDescriptorSets(VkCommandBuffer command, VkPipelineBindPoint point,
                                              VkPipelineLayout layout, uint32_t first, uint32_t count, const VkDescriptorSet* sets,
                                              uint32_t offset_count, const uint32_t* offsets) {
  if (capture_graphics.load(std::memory_order_relaxed) && point == VK_PIPELINE_BIND_POINT_GRAPHICS) {
    const std::lock_guard lock(mutex);
    if (const auto found = commands.find(reinterpret_cast<uint64_t>(command)); found != commands.end()) {
      found->second.graphics_bindings.Record(layout, first, count, sets, offset_count, offsets);
    }
  }
  bind_descriptor_sets(command, point, layout, first, count, sets, offset_count, offsets);
}

inline renodx::utils::resource::ResourceUpgradeInfo clone_target = {
    .old_format = reshade::api::format::r10g10b10a2_unorm,
    .new_format = reshade::api::format::r16g16b16a16_float,
    .ignore_size = true,
    .usage_set = static_cast<uint32_t>(
        reshade::api::resource_usage::shader_resource
        | reshade::api::resource_usage::render_target),
    .view_upgrades = renodx::utils::resource::VIEW_UPGRADES_RGBA16F,
    .use_resource_view_cloning_and_upgrade = true,
    .name = "Endfield application output (FP16 before UI/PQ)",
};

inline void ReportFailure(const char* reason, const ImageState* image, uint64_t command_buffer = 0u) {
  if (error_logged.exchange(true)) return;

  std::ostringstream message;
  message << "E_E_FPV HDR: " << reason
          << std::hex << " image=0x" << image->original.handle
          << " command=0x" << command_buffer
          << std::dec << " extent=" << image->width << 'x' << image->height
          << " drawn=" << image->drawn << " encoded=" << image->encoded
          << " preparation=" << (image->preparation_failure == nullptr ? "none" : image->preparation_failure);
  reshade::log::message(reshade::log::level::error, message.str().c_str());
}

inline constexpr auto on_output_draw = []<typename Context>(Context& context)
    -> renodx::utils::command_action::CallbackResult<Context> {
  if (!capture_graphics.load(std::memory_order_relaxed) || context.IsDispatch()) return {};
  const std::lock_guard lock(mutex);
  const auto command = commands.find(context.cmd_list->get_native());
  if (command == commands.end() || command->second.output_image == 0u) return {};
  const auto target = images.find(command->second.output_image);
  if (target == images.end() || !target->second->active) return {};
  auto* image = target->second.get();

  if (renodx::utils::shader::shared.data == nullptr) {
    image->failed = true;
    ReportFailure("draw.shader_shared_missing", image, context.cmd_list->get_native());
    return {.bypass = true};
  }
  auto* shader_state = renodx::utils::command_action::GetShaderState(&context);
  auto* stage = shader_state == nullptr ? nullptr : renodx::utils::shader::GetCurrentPixelState(shader_state);
  if (shader_state != nullptr) {
    const uint32_t pixel_hash = renodx::utils::shader::GetCurrentPixelShaderHash(shader_state);
    const auto& registrations = renodx::utils::command_action::internal::shared.data->registrations;
    const bool has_pixel_callback = pixel_hash != 0u && registrations.contains(pixel_hash);

    if (!context.matched_shader_stage.has_value()) {
      const uint32_t vertex_hash = renodx::utils::shader::GetCurrentVertexShaderHash(shader_state);
      if (has_pixel_callback || (vertex_hash != 0u && registrations.contains(vertex_hash))) return {};
    } else if (context.matched_shader_stage != renodx::utils::shader::PIXEL_INDEX && has_pixel_callback) {
      return {};
    }
  }
  const auto found = stage == nullptr ? graphics.end() : graphics.find(stage->pipeline.handle);
  if (found == graphics.end()) {
    image->failed = true;
    ReportFailure(shader_state == nullptr ? "draw.command_shader_state_missing"
                  : stage == nullptr      ? "draw.pixel_stage_missing"
                                          : "draw.pipeline_untracked",
                  image, context.cmd_list->get_native());
    return {.bypass = true};
  }
  auto& pipeline = found->second;
  if (pipeline.fp16.handle == 0u) {
    auto* subobjects = renodx::utils::pipeline::ClonePipelineSubObjects(pipeline.subobjects, pipeline.count);
    bool found_format = false;
    for (uint32_t i = 0u; i < pipeline.count; ++i) {
      if (subobjects[i].type == reshade::api::pipeline_subobject_type::render_target_formats
          && subobjects[i].count == 1u) {
        *static_cast<reshade::api::format*>(subobjects[i].data) = reshade::api::format::r16g16b16a16_float;
        found_format = true;
      }
    }

    auto layout = pipeline.layout;
    const bool has_details = renodx::utils::shader::GetPipelineShaderDetails(stage->pipeline,
                                                                             [&](const renodx::utils::shader::PipelineShaderDetails& details) {
                                                                               if (details.injection_layout.handle != 0u) layout = details.injection_layout;
                                                                               if (details.replacement_layout.handle != 0u) layout = details.replacement_layout;
                                                                               pipeline.restore = details.replacement_pipeline.handle != 0u ? details.replacement_pipeline : stage->pipeline;
                                                                               for (const auto& shader : details.subobject_shaders) {
                                                                                 renodx::utils::shader::shared.data->runtime_replacements.if_contains(
                                                                                     {pipeline.device, shader.shader_hash},
                                                                                     [&](const auto& replacement) {
                                                                                       renodx::utils::shader::AddShaderReplacement(&subobjects[shader.index], replacement.second);
                                                                                     });
                                                                               }
                                                                             });
    const bool created = found_format && has_details && pipeline.restore.handle != 0u && pipeline.device->create_pipeline(layout, pipeline.count, subobjects, &pipeline.fp16);
    renodx::utils::pipeline::DestroyPipelineSubobjects(subobjects, pipeline.count);
    if (!created) {
      image->failed = true;
      ReportFailure(!found_format                   ? "draw.render_target_format_missing"
                    : !has_details                  ? "draw.pipeline_details_missing"
                    : pipeline.restore.handle == 0u ? "draw.restore_pipeline_missing"
                                                    : "draw.fp16_pipeline_creation_failed",
                    image, context.cmd_list->get_native());
      return {.bypass = true};
    }
  }
  context.cmd_list->bind_pipeline(reshade::api::pipeline_stage::all_graphics, pipeline.fp16);
  image->drawn = true;
  return {
      .post_callback = [](Context& draw_context, const void* data) {
        draw_context.cmd_list->bind_pipeline(reshade::api::pipeline_stage::all_graphics,
                                             static_cast<const GraphicsPipeline*>(data)->restore);
      },
      .post_data = &pipeline,
      .replay = true,
  };
};

inline void OnInitPipeline(reshade::api::device* device, reshade::api::pipeline_layout layout,
                           uint32_t count, const reshade::api::pipeline_subobject* subobjects, reshade::api::pipeline pipeline) {
  if (!capture_graphics.load(std::memory_order_relaxed)
      || device->get_api() != reshade::api::device_api::vulkan) return;
  bool compatible = false;
  for (uint32_t i = 0u; i < count; ++i) {
    if (subobjects[i].type != reshade::api::pipeline_subobject_type::render_target_formats
        || subobjects[i].count != 1u) continue;
    const auto format = *static_cast<const reshade::api::format*>(subobjects[i].data);
    compatible = format == reshade::api::format::r10g10b10a2_unorm
                 || format == reshade::api::format::r8g8b8a8_unorm
                 || format == reshade::api::format::r8g8b8a8_unorm_srgb;
  }
  if (!compatible) return;
  const std::lock_guard lock(mutex);
  graphics.emplace(pipeline.handle, GraphicsPipeline{
                                        device, layout, count, renodx::utils::pipeline::ClonePipelineSubObjects(subobjects, count)});
}

inline void OnDestroyPipeline(reshade::api::device* device, reshade::api::pipeline pipeline) {
  const std::lock_guard lock(mutex);
  const auto found = graphics.find(pipeline.handle);
  if (found == graphics.end()) return;
  renodx::utils::pipeline::DestroyPipelineSubobjects(found->second.subobjects, found->second.count);
  if (found->second.fp16.handle != 0u) outputs[device].retired_pipelines.push_back(found->second.fp16);
  graphics.erase(found);
}

inline void SetCloneActive(ImageState* image, bool active) {
  std::vector<uint64_t> views;
  renodx::utils::resource::UpdateResourceInfo(image->original,
                                              [&](renodx::utils::resource::ResourceInfo* info) {
                                                if (info->destroyed) return;
                                                info->clone_enabled = active;
                                                info->clone_can_deactivate = false;
                                                views.assign(info->resource_view_handles.begin(), info->resource_view_handles.end());
                                              });
  renodx::utils::resource::upgrade::UpdateResourceViewsCloneState(
      views, active, false, &clone_target);
  image->active = active;
}

inline void SelectPhysicalShader(OutputState* output, uint64_t back_buffer,
                                 renodx::utils::draw::SwapchainProxyPass* proxy, std::span<const uint8_t> shader) {
  if (shader.empty()) return;
  if (proxy->pixel_shader.data() == shader.data() && proxy->pixel_shader.size() == shader.size()) return;
  if (proxy->pass.pipeline.handle != 0u) {
    output->physical_pipelines.push_back({back_buffer, proxy->pixel_shader, proxy->pass.pipeline});
  }
  proxy->pixel_shader = shader;
  proxy->pass.pipeline = {};
  for (auto it = output->physical_pipelines.begin(); it != output->physical_pipelines.end(); ++it) {
    if (it->back_buffer != back_buffer || it->shader.data() != shader.data() || it->shader.size() != shader.size()) continue;
    proxy->pass.pipeline = it->pipeline;
    output->physical_pipelines.erase(it);
    break;
  }
  proxy->pass.generated_pipeline = proxy->pass.pipeline.handle != 0u;
}

inline void SetPhysicalCopyOnly(reshade::api::device* device, bool copy_only, uint64_t back_buffer = 0u) {
  auto found = outputs.find(device);
  if (found == outputs.end()) return;

  if (found->second.parameters == nullptr || found->second.vertex_shader.empty()
      || found->second.pixel_shader.empty()) return;
  auto* data = renodx::utils::data::Get<renodx::mods::swapchain::v2::DeviceData>(device);
  if (data == nullptr) return;
  auto& output = found->second;
  const std::unique_lock data_lock(data->mutex);
  data->swap_chain_proxy_pixel_shader = copy_only
                                            ? std::span<const uint8_t>(__hdr_pq_copy)
                                            : output.pixel_shader;
  for (auto& [handle, proxy] : data->swapchain_proxy_passes) {
    if (proxy == nullptr || (back_buffer != 0u && handle != back_buffer)) continue;
    SelectPhysicalShader(&output, handle, proxy, data->swap_chain_proxy_pixel_shader);
  }
  output.copy_only = copy_only;
}

inline bool ReadOutputContract(ImageState* image) {
  auto* data = renodx::utils::data::Get<renodx::mods::swapchain::v2::DeviceData>(image->device);
  if (data == nullptr) {
    image->preparation_failure = "prepare.base_device_data_missing";
    return false;
  }
  const std::shared_lock data_lock(data->mutex);
  auto& output = outputs[image->device];
  if (output.parameters == nullptr) {
    for (const auto& [handle, proxy] : data->swapchain_proxy_passes) {
      if (proxy == nullptr || proxy->shader_injection == nullptr
          || proxy->shader_injection_size != image->parameters.size()
          || proxy->vertex_shader.empty() || proxy->pixel_shader.empty()) continue;
      output.vertex_shader = proxy->vertex_shader;
      output.pixel_shader = proxy->pixel_shader;
      output.parameters = proxy->shader_injection;
      output.compatibility_mode = proxy->use_compatibility_mode;
      break;
    }
  }

  if (output.parameters == nullptr) {
    image->preparation_failure = "prepare.base_output_payload_missing";
    return false;
  }
  std::memcpy(image->parameters.data(), output.parameters, sizeof(image->parameters));
  for (float value : image->parameters) {
    if (!std::isfinite(value)) {
      image->preparation_failure = "prepare.nonfinite_output_parameter";
      return false;
    }
  }
  if (!(image->parameters[0] > 0.f && image->parameters[1] > 0.f
        && image->parameters[6] == 4.f && image->parameters[7] == 1.f)) {
    image->preparation_failure = "prepare.output_parameter_contract";
    return false;
  }
  return true;
}

inline bool PrepareImage(ImageState* image) {
  image->preparation_failure = nullptr;
  if (!ReadOutputContract(image)) return false;
  if (image->clone.handle != 0u) return true;
  const auto usage = image->device->get_resource_desc(image->original).usage
                     | reshade::api::resource_usage::render_target
                     | reshade::api::resource_usage::shader_resource;
  if (!image->device->check_format_support(reshade::api::format::r16g16b16a16_float, usage)) {
    image->preparation_failure = "prepare.fp16_format_unsupported";
    return false;
  }
  if (!image->device->check_format_support(reshade::api::format::r10g10b10a2_unorm,
                                           reshade::api::resource_usage::render_target)) {
    image->preparation_failure = "prepare.rgb10_render_target_unsupported";
    return false;
  }

  renodx::utils::resource::UpdateResourceInfo(image->original,
                                              [](renodx::utils::resource::ResourceInfo* info) {
                                                info->clone_target = &clone_target;
                                                info->clone_enabled = false;
                                                info->clone_can_deactivate = false;
                                              });
  image->clone = renodx::utils::resource::upgrade::GetResourceClone(
      image->original, {.require_enabled = false, .allow_create = true});
  if (image->clone.handle == 0u) {
    image->preparation_failure = "prepare.clone_creation_failed";
    return false;
  }

  auto& pass = image->pass;

  reshade::api::resource_view rtv = {}, srv = {};
  const bool rtv_created = image->device->create_resource_view(image->original,
                                                               reshade::api::resource_usage::render_target,
                                                               reshade::api::resource_view_desc(reshade::api::format::r10g10b10a2_unorm), &rtv);
  if (!rtv_created || !image->device->create_resource_view(image->clone, reshade::api::resource_usage::shader_resource, reshade::api::resource_view_desc(reshade::api::format::r16g16b16a16_float), &srv)) {
    if (rtv.handle != 0u) image->device->destroy_resource_view(rtv);
    if (srv.handle != 0u) image->device->destroy_resource_view(srv);
    image->failed = true;
    image->preparation_failure = rtv_created ? "prepare.fp16_srv_creation_failed" : "prepare.rgb10_rtv_creation_failed";
    return false;
  }
  pass.render_target_slots.views = {rtv};
  pass.render_target_slots.generated_views = {rtv};
  pass.shader_resource_slots.views = {srv};
  pass.shader_resource_slots.generated_views = {srv};
  pass.pipeline_subobjects.vertex_shader = outputs[image->device].vertex_shader;
  pass.pipeline_subobjects.pixel_shader = outputs[image->device].pixel_shader;
  pass.pipeline_subobjects.render_target_formats = {reshade::api::format::r10g10b10a2_unorm};
  pass.sampler_descs.emplace_back();
  pass.auto_generate_render_target_formats = false;
  pass.auto_generate_viewport = false;
  pass.auto_generate_scissors = false;
  pass.viewports = {{0.f, 0.f, static_cast<float>(image->width),
                     static_cast<float>(image->height), 0.f, 1.f}};
  pass.scissors = {{0, 0, static_cast<int32_t>(image->width), static_cast<int32_t>(image->height)}};
  pass.render_target_load_op = reshade::api::render_pass_load_op::discard;
  pass.render_target_store_op = reshade::api::render_pass_store_op::store;
  pass.flush_after_render = false;

  pass.revert_state_after_render = false;
  pass.push_constants[{.slot = 0u, .space = 0u}] = std::span<const float>(image->parameters);
  HMODULE module = nullptr;

  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                          reinterpret_cast<LPCWSTR>(&clone_target), &module)
      || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                             reinterpret_cast<LPCWSTR>(outputs[image->device].parameters), &module)) {
    image->failed = true;
    image->preparation_failure = "prepare.module_pin_failed";
    return false;
  }
  return true;
}

inline bool Encode(ImageState* image, VkCommandBuffer command_buffer) {
  const auto found = commands.find(reinterpret_cast<uint64_t>(command_buffer));
  if (!image->acquired || !image->drawn || image->encoded || image->failed
      || found == commands.end() || found->second.in_render_pass) {
    ReportFailure(!image->acquired          ? "encode.not_acquired"
                  : !image->drawn           ? "encode.no_output_draw"
                  : image->encoded          ? "encode.already_encoded"
                  : image->failed           ? "encode.image_already_failed"
                  : found == commands.end() ? "encode.command_untracked"
                                            : "encode.render_pass_still_open",
                  image, reinterpret_cast<uint64_t>(command_buffer));
    return false;
  }
  auto* cmd = found->second.command_list;
  if (cmd->get_device() != image->device) {
    ReportFailure("encode.device_mismatch", image, reinterpret_cast<uint64_t>(command_buffer));
    return false;
  }
  const auto* tracked_state = renodx::utils::state::GetCurrentState(cmd);
  if (tracked_state == nullptr) {
    ReportFailure("encode.render_state_missing", image, reinterpret_cast<uint64_t>(command_buffer));
    return false;
  }
  auto previous_state = *tracked_state;
  std::vector<uint64_t> expected_sets;
  expected_sets.reserve(previous_state.graphics_descriptor_tables.size());
  for (auto table : previous_state.graphics_descriptor_tables) expected_sets.push_back(table.handle);
  if (bind_descriptor_sets == nullptr
      || !found->second.graphics_bindings.Matches(previous_state.graphics_pipeline_layout.handle, expected_sets)) {
    ReportFailure("encode.native_graphics_bindings_incomplete", image, reinterpret_cast<uint64_t>(command_buffer));
    return false;
  }

  previous_state.render_targets.clear();
  previous_state.depth_stencil = {};

  previous_state.graphics_descriptor_tables.clear();
  previous_state.compute_descriptor_tables.clear();

  cmd->barrier(image->clone, reshade::api::resource_usage::render_target,
               reshade::api::resource_usage::shader_resource_pixel);
  const bool rendered = image->pass.Render(cmd);
  previous_state.Apply(cmd);
  found->second.graphics_bindings.Restore(command_buffer, bind_descriptor_sets);
  cmd->barrier(image->clone, reshade::api::resource_usage::shader_resource_pixel,
               reshade::api::resource_usage::render_target);
  if (!rendered) {
    ReportFailure("encode.output_render_failed", image, reinterpret_cast<uint64_t>(command_buffer));
    return false;
  }
  image->encoded = true;
  outputs[image->device].want_copy_only = true;
  return true;
}

inline void TrackSwapchain(VkSwapchainKHR swapchain,
                           const VkSwapchainCreateInfoKHR& desc) {
  if (desc.imageArrayLayers != 1u || desc.imageSharingMode != VK_SHARING_MODE_EXCLUSIVE) {
    reshade::log::message(reshade::log::level::warning,
                          "E_E_FPV HDR: unsupported application swapchain sharing/layer contract; FP16 bridge not activated.");
    return;
  }
  const std::lock_guard lock(mutex);
  swapchains[reinterpret_cast<uint64_t>(swapchain)] = {
      desc.imageExtent.width, desc.imageExtent.height, {}};
}

inline VkResult VKAPI_CALL HookGetImages(VkDevice device, VkSwapchainKHR swapchain,
                                         uint32_t* count, VkImage* result_images) {
  const VkResult result = get_images(device, swapchain, count, result_images);
  if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || result_images == nullptr || count == nullptr) return result;
  const std::lock_guard lock(mutex);
  auto found = swapchains.find(reinterpret_cast<uint64_t>(swapchain));
  if (found == swapchains.end()) return result;
  auto& state = found->second;
  state.images.resize(*count);
  for (uint32_t i = 0; i < *count; ++i) {
    const uint64_t handle = reinterpret_cast<uint64_t>(result_images[i]);
    state.images[i] = handle;
    if (images.contains(handle)) continue;
    auto image = std::make_unique<ImageState>();
    renodx::utils::resource::GetResourceInfo({handle},
                                             [&](const renodx::utils::resource::ResourceInfo& info) {
                                               const auto& desc = info.desc;
                                               if (info.destroyed || info.is_swap_chain || info.is_clone || info.clone_target != nullptr
                                                   || info.device == nullptr || info.device->get_native() != reinterpret_cast<uint64_t>(device)
                                                   || desc.type != reshade::api::resource_type::texture_2d
                                                   || desc.texture.format != reshade::api::format::r10g10b10a2_unorm
                                                   || desc.texture.width != state.width || desc.texture.height != state.height
                                                   || desc.texture.levels != 1u || desc.texture.depth_or_layers != 1u || desc.texture.samples != 1u
                                                   || (desc.usage & reshade::api::resource_usage::render_target) == reshade::api::resource_usage::undefined) return;
                                               image->device = info.device;
                                               image->original = {handle};
                                               image->width = state.width;
                                               image->height = state.height;
                                             });
    if (image->device != nullptr) images.emplace(handle, std::move(image));
  }
  return result;
}

inline VkResult VKAPI_CALL HookAcquire(VkDevice device, VkSwapchainKHR swapchain,
                                       uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t* index) {
  const VkResult result = acquire_image(device, swapchain, timeout, semaphore, fence, index);
  if ((result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) || index == nullptr) return result;
  const std::lock_guard lock(mutex);
  const auto chain = swapchains.find(reinterpret_cast<uint64_t>(swapchain));
  if (chain == swapchains.end() || *index >= chain->second.images.size()) return result;
  const auto found = images.find(chain->second.images[*index]);
  if (found == images.end()) return result;
  auto* image = found->second.get();
  image->acquired = true;
  image->encoded = false;
  image->drawn = false;
  if (image->failed) return result;
  if (PrepareImage(image)) {
    SetCloneActive(image, true);
  } else if (image->clone.handle != 0u || outputs[image->device].copy_only) {
    image->failed = true;
    ReportFailure(image->preparation_failure == nullptr ? "acquire.preparation_failed" : image->preparation_failure, image);
  }
  return result;
}

template <typename Barrier, typename Forward, typename Mirror>
inline void ProcessBarriers(VkCommandBuffer command_buffer, uint32_t count,
                            const Barrier* barriers, Forward&& forward, Mirror&& mirror) {
  if (!capture_graphics.load(std::memory_order_relaxed) || forwarding || count == 0u || barriers == nullptr) {
    forward();
    return;
  }
  const std::lock_guard lock(mutex);
  std::vector<std::pair<ImageState*, Barrier>> working;
  for (uint32_t i = 0; i < count; ++i) {
    const auto found = images.find(reinterpret_cast<uint64_t>(barriers[i].image));
    if (found == images.end() || !found->second->active) continue;
    auto* image = found->second.get();
    SetCloneActive(image, false);
    if (IsOutputBoundary(barriers[i]) && !Encode(image, command_buffer)) {
      image->failed = true;
    }
    auto barrier = barriers[i];
    barrier.image = reinterpret_cast<VkImage>(image->clone.handle);
    barrier.oldLayout = WorkingLayout(barrier.oldLayout);
    barrier.newLayout = WorkingLayout(barrier.newLayout);
    working.emplace_back(image, barrier);
  }
  forwarding = true;
  forward();
  for (const auto& [image, barrier] : working) {
    mirror(barrier);
    if (!image->encoded && !image->failed) SetCloneActive(image, true);
  }
  forwarding = false;
}

inline void VKAPI_CALL HookBarrier(VkCommandBuffer cmd, VkPipelineStageFlags src,
                                   VkPipelineStageFlags dst, VkDependencyFlags flags, uint32_t memory_count,
                                   const VkMemoryBarrier* memory, uint32_t buffer_count,
                                   const VkBufferMemoryBarrier* buffers, uint32_t image_count,
                                   const VkImageMemoryBarrier* image_barriers) {
  ProcessBarriers(cmd, image_count, image_barriers, [&] { pipeline_barrier(cmd, src, dst, flags, memory_count, memory, buffer_count, buffers, image_count, image_barriers); }, [&](const VkImageMemoryBarrier& barrier) { pipeline_barrier(cmd, src, dst, flags, 0u, nullptr, 0u, nullptr, 1u, &barrier); });
}

inline void VKAPI_CALL HookBarrier2(VkCommandBuffer cmd, const VkDependencyInfo* dependency) {
  if (dependency == nullptr) return;
  ProcessBarriers(cmd, dependency->imageMemoryBarrierCount, dependency->pImageMemoryBarriers, [&] { pipeline_barrier2(cmd, dependency); }, [&](const VkImageMemoryBarrier2& barrier) {
        VkDependencyInfo mirrored = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        mirrored.dependencyFlags = dependency->dependencyFlags;
        mirrored.imageMemoryBarrierCount = 1u;
        mirrored.pImageMemoryBarriers = &barrier;
        pipeline_barrier2(cmd, &mirrored); });
}

inline VkResult VKAPI_CALL HookPresent(VkQueue queue, const VkPresentInfoKHR* present) {
  if (present != nullptr) {
    const std::lock_guard lock(mutex);
    for (uint32_t i = 0; i < present->swapchainCount; ++i) {
      const auto chain = swapchains.find(reinterpret_cast<uint64_t>(present->pSwapchains[i]));
      if (chain == swapchains.end() || present->pImageIndices[i] >= chain->second.images.size()) continue;
      const auto found = images.find(chain->second.images[present->pImageIndices[i]]);
      if (found == images.end()) continue;
      auto* image = found->second.get();
      if (image->failed || (image->clone.handle != 0u && !image->encoded)) {
        ReportFailure(image->failed ? "present.image_already_failed" : "present.clone_not_encoded", image);
        if (present->pResults != nullptr) present->pResults[i] = VK_ERROR_OUT_OF_DATE_KHR;
        return VK_ERROR_OUT_OF_DATE_KHR;
      }
      image->acquired = false;
    }
  }
  return queue_present(queue, present);
}

inline void VKAPI_CALL HookDestroySwapchain(VkDevice device, VkSwapchainKHR swapchain,
                                            const VkAllocationCallbacks* allocator) {
  {
    const std::lock_guard lock(mutex);
    swapchains.erase(reinterpret_cast<uint64_t>(swapchain));
  }

  destroy_swapchain(device, swapchain, allocator);
  const std::lock_guard lock(mutex);
  if (swapchains.empty()) {
    for (auto& [api_device, output] : outputs) {
      output.want_copy_only = false;
      SetPhysicalCopyOnly(api_device, false);
    }
  }
}

inline void OnPresent(reshade::api::swapchain* swapchain) {
  if (!capture_graphics.load(std::memory_order_relaxed) || swapchain == nullptr) return;
  const std::lock_guard lock(mutex);
  const auto found = outputs.find(swapchain->get_device());
  if (found == outputs.end()) return;

  auto& output = found->second;
  const auto back_buffer = swapchain->get_current_back_buffer();
  bool compatibility_mode = output.compatibility_mode;
  if (auto* data = renodx::utils::data::Get<renodx::mods::swapchain::v2::DeviceData>(swapchain->get_device())) {
    const std::shared_lock data_lock(data->mutex);
    if (const auto proxy = data->swapchain_proxy_passes.find(back_buffer.handle);
        proxy != data->swapchain_proxy_passes.end() && proxy->second != nullptr) {
      compatibility_mode = proxy->second->use_compatibility_mode;
    }
  }

  const bool working = !compatibility_mode && output.working_copies.contains(back_buffer.handle);
  SetPhysicalCopyOnly(swapchain->get_device(), output.want_copy_only && !working, back_buffer.handle);
}

inline bool OnPresentationCopy(reshade::api::command_list*, reshade::api::resource source,
                               reshade::api::resource dest) {
  if (!capture_graphics.load(std::memory_order_relaxed)) return false;
  const std::lock_guard lock(mutex);
  if (outputs.empty() || renodx::utils::resource::shared.data == nullptr) return false;
  reshade::api::device* device = nullptr;
  bool destination_cloned = false;
  renodx::utils::resource::GetResourceInfo(dest, [&](const auto& info) {
    if (info.destroyed || !info.is_swap_chain) return;
    device = info.device;
    destination_cloned = info.clone_enabled && info.clone.handle != 0u
                         && info.clone_desc.texture.format == reshade::api::format::r16g16b16a16_float;
  });
  const auto output = outputs.find(device);
  if (output == outputs.end() || !output->second.want_copy_only) return false;
  output->second.working_copies.erase(dest.handle);
  const auto image = images.find(source.handle);
  if (!destination_cloned || image == images.end() || image->second->device != device
      || image->second->clone.handle == 0u || !image->second->encoded || image->second->failed) return false;
  renodx::utils::resource::GetResourceInfo(source, [&](const auto& info) {
    if (!info.destroyed && info.clone.handle == image->second->clone.handle
        && info.clone_desc.texture.format == reshade::api::format::r16g16b16a16_float) {
      output->second.working_copies[dest.handle] = source.handle;
    }
  });
  return false;
}

inline bool OnPresentationCopyRegion(reshade::api::command_list* cmd, reshade::api::resource source,
                                     uint32_t source_subresource, const reshade::api::subresource_box* source_box,
                                     reshade::api::resource dest, uint32_t dest_subresource,
                                     const reshade::api::subresource_box* dest_box, reshade::api::filter_mode) {
  if (!capture_graphics.load(std::memory_order_relaxed)
      || source_subresource != 0u || dest_subresource != 0u
      || renodx::utils::resource::shared.data == nullptr) return false;
  bool full_source = false, full_dest = false;
  renodx::utils::resource::GetResourceInfo(source, [&](const auto& info) {
    full_source = !info.destroyed && renodx::utils::resource::IsFullSubresourceUpdate(info.desc, 0u, source_box);
  });
  renodx::utils::resource::GetResourceInfo(dest, [&](const auto& info) {
    full_dest = !info.destroyed && info.is_swap_chain
                && renodx::utils::resource::IsFullSubresourceUpdate(info.desc, 0u, dest_box);
  });
  if (!full_source || !full_dest) return false;
  return OnPresentationCopy(cmd, source, dest);
}

inline void OnInitCommandList(reshade::api::command_list* cmd) {
  if (cmd->get_device()->get_api() != reshade::api::device_api::vulkan) return;
  const std::lock_guard lock(mutex);
  commands[cmd->get_native()] = {cmd, false};
}
inline void OnDestroyCommandList(reshade::api::command_list* cmd) {
  const std::lock_guard lock(mutex);
  commands.erase(cmd->get_native());
}
inline void OnBeginRenderPass(reshade::api::command_list* cmd, uint32_t count,
                              const reshade::api::render_pass_render_target_desc* targets, const reshade::api::render_pass_depth_stencil_desc*) {
  const std::lock_guard lock(mutex);
  if (auto found = commands.find(cmd->get_native()); found != commands.end()) {
    found->second.in_render_pass = true;
    found->second.output_image = 0u;
    if (count == 1u && targets != nullptr) {
      auto resource = cmd->get_device()->get_resource_from_view(targets[0].view);
      renodx::utils::resource::GetResourceInfo(resource, [&](const auto& info) {
        if (info.is_clone) resource = info.fallback;
      });
      if (images.contains(resource.handle)) found->second.output_image = resource.handle;
    }
  }
}
inline void OnEndRenderPass(reshade::api::command_list* cmd) {
  const std::lock_guard lock(mutex);
  if (auto found = commands.find(cmd->get_native()); found != commands.end()) {
    found->second.in_render_pass = false;
    found->second.output_image = 0u;
  }
}
inline void OnDestroyResource(reshade::api::device* device, reshade::api::resource resource) {
  const std::lock_guard lock(mutex);
  if (auto output = outputs.find(device); output != outputs.end()) {
    std::erase_if(output->second.physical_pipelines, [&](const auto& cached) {
      if (cached.back_buffer != resource.handle) return false;
      output->second.retired_pipelines.push_back(cached.pipeline);
      return true;
    });
    std::erase_if(output->second.working_copies, [resource](const auto& entry) {
      return entry.first == resource.handle || entry.second == resource.handle;
    });
  }
  const auto found = images.find(resource.handle);
  if (found == images.end()) return;
  found->second->pass.DestroyAll(device);
  images.erase(found);
}
inline void OnDestroyDevice(reshade::api::device* device) {
  const std::lock_guard lock(mutex);
  std::erase_if(graphics, [device](const auto& pair) {
    if (pair.second.device != device) return false;
    renodx::utils::pipeline::DestroyPipelineSubobjects(pair.second.subobjects, pair.second.count);
    if (pair.second.fp16.handle != 0u) device->destroy_pipeline(pair.second.fp16);
    return true;
  });
  if (const auto found = outputs.find(device); found != outputs.end()) {
    for (const auto& cached : found->second.physical_pipelines) device->destroy_pipeline(cached.pipeline);
    for (auto pipeline : found->second.retired_pipelines) device->destroy_pipeline(pipeline);
    outputs.erase(found);
  }
  std::erase_if(commands, [device](const auto& pair) { return pair.second.command_list->get_device() == device; });
  std::erase_if(images, [device](const auto& pair) {
    if (pair.second->device != device) return false;
    pair.second->pass.DestroyAll(device);
    return true;
  });
}

inline bool ResolveBarrierDispatch(PFN_vkGetDeviceProcAddr get_proc, VkDevice device,
                                   PFN_vkCmdPipelineBarrier* legacy, PFN_vkCmdPipelineBarrier2* sync2) {
  *legacy = nullptr;
  *sync2 = nullptr;
  if (get_proc == nullptr || device == VK_NULL_HANDLE) return false;
  const auto first = get_proc(device, "vkCmdPipelineBarrier");
  const auto core = get_proc(device, "vkCmdPipelineBarrier2");
  const auto khr = get_proc(device, "vkCmdPipelineBarrier2KHR");
  if (first == nullptr || (core != nullptr && khr != nullptr && core != khr)
      || first == core || first == khr) return false;
  *legacy = reinterpret_cast<PFN_vkCmdPipelineBarrier>(first);
  *sync2 = reinterpret_cast<PFN_vkCmdPipelineBarrier2>(core != nullptr ? core : khr);
  return true;
}

inline bool IsBarrierHostCode(HMODULE host, const void* address) {
  MEMORY_BASIC_INFORMATION info = {};
  if (address == nullptr || VirtualQuery(address, &info, sizeof(info)) != sizeof(info)
      || info.AllocationBase != host || info.State != MEM_COMMIT
      || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
  return (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

inline bool AttachHooks(HMODULE interposer, VkDevice device) {
  const HMODULE host = reshade::internal::get_reshade_module_handle();
  if (host == nullptr
      || !ResolveBarrierDispatch(reinterpret_cast<PFN_vkGetDeviceProcAddr>(GetProcAddress(host, "vkGetDeviceProcAddr")),
                                 device, &pipeline_barrier, &pipeline_barrier2)
      || !IsBarrierHostCode(host, reinterpret_cast<const void*>(pipeline_barrier))
      || (pipeline_barrier2 != nullptr && !IsBarrierHostCode(host, reinterpret_cast<const void*>(pipeline_barrier2)))) {
    reshade::log::message(reshade::log::level::error,
                          "E_E_FPV HDR: unavailable or invalid ReShade device barrier dispatch; hook transaction refused.");
    return false;
  }
  bind_descriptor_sets = reinterpret_cast<PFN_vkCmdBindDescriptorSets>(
      reinterpret_cast<PFN_vkGetDeviceProcAddr>(GetProcAddress(host, "vkGetDeviceProcAddr"))(device, "vkCmdBindDescriptorSets"));
  if (!IsBarrierHostCode(host, reinterpret_cast<const void*>(bind_descriptor_sets))) return false;
  get_images = reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(GetProcAddress(interposer, "vkGetSwapchainImagesKHR"));
  acquire_image = reinterpret_cast<PFN_vkAcquireNextImageKHR>(GetProcAddress(interposer, "vkAcquireNextImageKHR"));
  destroy_swapchain = reinterpret_cast<PFN_vkDestroySwapchainKHR>(GetProcAddress(interposer, "vkDestroySwapchainKHR"));
  queue_present = reinterpret_cast<PFN_vkQueuePresentKHR>(GetProcAddress(interposer, "vkQueuePresentKHR"));
  return get_images != nullptr && acquire_image != nullptr && destroy_swapchain != nullptr
         && queue_present != nullptr && pipeline_barrier != nullptr
         && DetourAttach(&get_images, HookGetImages) == NO_ERROR
         && DetourAttach(&acquire_image, HookAcquire) == NO_ERROR
         && DetourAttach(&destroy_swapchain, HookDestroySwapchain) == NO_ERROR
         && DetourAttach(&queue_present, HookPresent) == NO_ERROR
         && DetourAttach(&pipeline_barrier, HookBarrier) == NO_ERROR
         && DetourAttach(&bind_descriptor_sets, HookBindDescriptorSets) == NO_ERROR
         && (pipeline_barrier2 == nullptr || DetourAttach(&pipeline_barrier2, HookBarrier2) == NO_ERROR);
}
inline bool DetachHooks() {
  return DetourDetach(&get_images, HookGetImages) == NO_ERROR
         && DetourDetach(&acquire_image, HookAcquire) == NO_ERROR
         && DetourDetach(&destroy_swapchain, HookDestroySwapchain) == NO_ERROR
         && DetourDetach(&queue_present, HookPresent) == NO_ERROR
         && DetourDetach(&pipeline_barrier, HookBarrier) == NO_ERROR
         && DetourDetach(&bind_descriptor_sets, HookBindDescriptorSets) == NO_ERROR
         && (pipeline_barrier2 == nullptr || DetourDetach(&pipeline_barrier2, HookBarrier2) == NO_ERROR);
}

inline void RegisterDrawCallbacks() {
  constexpr uint32_t command_types = renodx::utils::command_action::COMMAND_TYPE_DIRECT_DRAW | renodx::utils::command_action::COMMAND_TYPE_INDIRECT;

  renodx::utils::command_action::Register(on_output_draw, {.command_types = command_types});
  for (const auto& [hash, callbacks] : renodx::utils::command_action::internal::shared.data->registrations) {
    if (hash == 0u) continue;
    renodx::utils::command_action::Register(on_output_draw,
                                            {.shader_hash = hash, .command_types = command_types});
  }

  renodx::utils::command_action::Use(DLL_PROCESS_ATTACH);
}

inline void UseEvents(DWORD reason) {
  if (reason == DLL_PROCESS_ATTACH) {
    if (events_registered) return;
    events_registered = true;

    renodx::utils::shader::Use(reason);
    renodx::utils::state::Use(reason);
    renodx::utils::command_action::Use(reason);
    reshade::register_event<reshade::addon_event::init_pipeline>(OnInitPipeline);
    reshade::register_event<reshade::addon_event::destroy_pipeline>(OnDestroyPipeline);
    reshade::register_event<reshade::addon_event::init_command_list>(OnInitCommandList);
    reshade::register_event<reshade::addon_event::reset_command_list>(OnInitCommandList);
    reshade::register_event<reshade::addon_event::destroy_command_list>(OnDestroyCommandList);
    reshade::register_event<reshade::addon_event::begin_render_pass>(OnBeginRenderPass);
    reshade::register_event<reshade::addon_event::end_render_pass>(OnEndRenderPass);
    reshade::register_event<reshade::addon_event::destroy_resource>(OnDestroyResource);
    reshade::register_event<reshade::addon_event::destroy_device>(OnDestroyDevice);
  } else if (reason == DLL_PROCESS_DETACH) {
    if (!events_registered) return;
    events_registered = false;
    capture_graphics.store(false, std::memory_order_relaxed);
    renodx::utils::command_action::Unregister(on_output_draw);
    reshade::unregister_event<reshade::addon_event::init_pipeline>(OnInitPipeline);
    reshade::unregister_event<reshade::addon_event::destroy_pipeline>(OnDestroyPipeline);
    reshade::unregister_event<reshade::addon_event::init_command_list>(OnInitCommandList);
    reshade::unregister_event<reshade::addon_event::reset_command_list>(OnInitCommandList);
    reshade::unregister_event<reshade::addon_event::destroy_command_list>(OnDestroyCommandList);
    reshade::unregister_event<reshade::addon_event::begin_render_pass>(OnBeginRenderPass);
    reshade::unregister_event<reshade::addon_event::end_render_pass>(OnEndRenderPass);
    reshade::unregister_event<reshade::addon_event::destroy_resource>(OnDestroyResource);
    reshade::unregister_event<reshade::addon_event::destroy_device>(OnDestroyDevice);
    renodx::utils::command_action::Use(reason);
    renodx::utils::state::Use(reason);
    renodx::utils::shader::Use(reason);
  }
}

}
