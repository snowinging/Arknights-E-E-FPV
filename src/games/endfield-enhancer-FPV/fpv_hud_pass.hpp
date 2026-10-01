#pragma once

// FPV HUD 独立叠加 pass
//
// 背景: 早先 HUD 是借 swapchain v2 的代理通道画的, 那次注册会顶掉原本给 HDR 输出用的
// 代理 pixel shader, 而那条链还要求把交换链升级成 fp16 才能跑。结果游戏画面的色彩空间
// 转换环节被挖掉, 在 Windows 上表现为整屏偏色、"像加了一层滤镜", ImGui 底色也发黑。
//
// 现在改成完全独立的 pass:
//   - 不碰 swapchain, 不升级格式, 不动色彩空间
//   - 用 alpha 混合直接叠到 backbuffer 上, 由硬件合成, 因此不需要读回画面内容,
//     也就不需要克隆资源和来回拷贝
//   - HUD 关掉时 shader 输出 alpha=0, 混合后画面等于没动过
//
// shader 侧约定: 只输出 HUD 自己的颜色与 alpha, 画面不参与采样。

#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

#include <embed/0xF01D0001.h>
#include <embed/0xF01D0002.h>
#include <embed/0xF01D0003.h>
#include <embed/0xF01D0004.h>
#include <include/reshade.hpp>

namespace endfield::fpv_hud_pass {

struct DeviceState {
  reshade::api::pipeline_layout layout = {};
  reshade::api::pipeline pipeline = {};
  reshade::api::format target_format = reshade::api::format::unknown;
  std::unordered_map<uint64_t, reshade::api::resource_view> targets;
};

inline std::unordered_map<reshade::api::device*, DeviceState> states;
inline const float* payload = nullptr;
inline uint32_t payload_count = 0;
inline float enabled = 1.f;              // 面板开关, 关掉就完全不介入 present
inline uint32_t failure_count = 0;       // 连续失败到一定次数就不再反复尝试

// 由 addon 在初始化时把 payload 指针交进来(与 FpvHudPayload 保持一致)
inline void SetPayload(const float* data, size_t count) {
  payload = data;
  payload_count = static_cast<uint32_t>(count);
}

inline void OnSwapchainReset(reshade::api::device* device);   // 定义见下
inline bool EnsurePipeline(reshade::api::device* device, DeviceState& state, reshade::api::format format);

// 交换链建立起来的时候把 pipeline 与全部 back buffer 视图建好。
// 放在这里而不是 present 里, 是为了让 present 路径保持"只绑定只绘制"。
inline void OnInitSwapchain(reshade::api::swapchain* swapchain) {
  if (swapchain == nullptr) return;
  auto* device = swapchain->get_device();
  if (device == nullptr) return;

  auto& state = states[device];
  OnSwapchainReset(device);          // 先丢掉上一轮的视图
  failure_count = 0;

  const uint32_t buffer_count = swapchain->get_back_buffer_count();
  if (buffer_count == 0u) return;
  const auto first_buffer = swapchain->get_back_buffer(0);
  if (first_buffer.handle == 0u) return;
  const auto format = device->get_resource_desc(first_buffer).texture.format;

  if (!EnsurePipeline(device, state, format)) {
    reshade::log::message(reshade::log::level::warning, "E_E_FPV HUD: pipeline creation failed at swapchain init");
    return;
  }

  uint32_t created = 0u;
  for (uint32_t i = 0; i < buffer_count; ++i) {
    const auto buffer = swapchain->get_back_buffer(i);
    if (buffer.handle == 0u) continue;
    reshade::api::resource_view view = {};
    if (!device->create_resource_view(buffer, reshade::api::resource_usage::render_target,
                                      reshade::api::resource_view_desc(format), &view)) {
      continue;
    }
    state.targets[buffer.handle] = view;
    ++created;
  }
  char buf[176];
  std::snprintf(buf, sizeof(buf), "E_E_FPV HUD: overlay pass ready (format=%u, buffers=%u/%u)",
                static_cast<uint32_t>(format), created, buffer_count);
  reshade::log::message(reshade::log::level::info, buf);
}

// 交换链重建时丢弃缓存的 back buffer 视图。
// DXGI 在 resize 后可能复用同一批资源句柄, 而旧视图已经失效,
// 继续绑定它会让 GPU 侧挂起(表现为画面冻住但游戏逻辑还在跑)。
inline void OnSwapchainReset(reshade::api::device* device) {
  const auto found = states.find(device);
  if (found == states.end()) return;
  for (const auto& [handle, view] : found->second.targets) device->destroy_resource_view(view);
  found->second.targets.clear();
}

inline bool EnsurePipeline(reshade::api::device* device, DeviceState& state, reshade::api::format format) {
  if (state.pipeline.handle != 0u && state.target_format == format) return true;

  // 目标格式变了(比如全屏/窗口切换)就整个重建
  if (state.pipeline.handle != 0u) {
    device->destroy_pipeline(state.pipeline);
    state.pipeline = {};
  }
  for (const auto& [handle, view] : state.targets) device->destroy_resource_view(view);
  state.targets.clear();
  state.target_format = format;

  const bool is_vulkan = device->get_api() == reshade::api::device_api::vulkan;
  const auto vertex_shader = is_vulkan
                                 ? std::span<const uint8_t>(__0xF01D0003_base, sizeof(__0xF01D0003_base))
                                 : std::span<const uint8_t>(__0xF01D0001_base, sizeof(__0xF01D0001_base));
  const auto pixel_shader = is_vulkan
                                ? std::span<const uint8_t>(__0xF01D0004_base, sizeof(__0xF01D0004_base))
                                : std::span<const uint8_t>(__0xF01D0002_base, sizeof(__0xF01D0002_base));

  if (state.layout.handle == 0u) {
    reshade::api::pipeline_layout_param param = {};
    param.type = reshade::api::pipeline_layout_param_type::push_constants;
    param.push_constants.dx_register_index = 13;   // DX11: b13, 与 shader 里的 cbuffer 对应
    param.push_constants.dx_register_space = 0;
    // DX11 用寄存器数描述, DX12/Vulkan 用 32 位值个数
    param.push_constants.count = is_vulkan ? payload_count : 1;
    if (!device->create_pipeline_layout(1, &param, &state.layout)) {
      reshade::log::message(reshade::log::level::warning, "E_E_FPV HUD: create_pipeline_layout failed");
      return false;
    }
  }

  // 标准 alpha 混合: dst = src.rgb * src.a + dst * (1 - src.a)
  reshade::api::blend_desc blend = {};
  blend.blend_enable[0] = true;
  blend.source_color_blend_factor[0] = reshade::api::blend_factor::source_alpha;
  blend.dest_color_blend_factor[0] = reshade::api::blend_factor::one_minus_source_alpha;
  blend.color_blend_op[0] = reshade::api::blend_op::add;
  blend.source_alpha_blend_factor[0] = reshade::api::blend_factor::one;
  blend.dest_alpha_blend_factor[0] = reshade::api::blend_factor::one_minus_source_alpha;
  blend.alpha_blend_op[0] = reshade::api::blend_op::add;
  blend.render_target_write_mask[0] = 0xF;

  reshade::api::shader_desc vs_desc = {.code = vertex_shader.data(), .code_size = vertex_shader.size()};
  reshade::api::shader_desc ps_desc = {.code = pixel_shader.data(), .code_size = pixel_shader.size()};
  reshade::api::rasterizer_desc rasterizer = {.cull_mode = reshade::api::cull_mode::none};
  reshade::api::depth_stencil_desc depth_stencil = {.depth_enable = false};
  auto topology = reshade::api::primitive_topology::triangle_list;
  uint32_t vertex_count = 3;
  std::vector<reshade::api::input_element> input_layout;

  const reshade::api::pipeline_subobject subobjects[] = {
      {reshade::api::pipeline_subobject_type::vertex_shader, 1, &vs_desc},
      {reshade::api::pipeline_subobject_type::pixel_shader, 1, &ps_desc},
      {reshade::api::pipeline_subobject_type::render_target_formats, 1, &format},
      {reshade::api::pipeline_subobject_type::blend_state, 1, &blend},
      {reshade::api::pipeline_subobject_type::rasterizer_state, 1, &rasterizer},
      {reshade::api::pipeline_subobject_type::depth_stencil_state, 1, &depth_stencil},
      {reshade::api::pipeline_subobject_type::primitive_topology, 1, &topology},
      {reshade::api::pipeline_subobject_type::max_vertex_count, 1, &vertex_count},
      {reshade::api::pipeline_subobject_type::input_layout,
       static_cast<uint32_t>(input_layout.size()), input_layout.data()},
  };

  constexpr uint32_t subobject_count = sizeof(subobjects) / sizeof(subobjects[0]);
  if (!device->create_pipeline(state.layout, subobject_count, subobjects, &state.pipeline)) {
    reshade::log::message(reshade::log::level::warning, "E_E_FPV HUD: create_pipeline failed");
    state.pipeline = {};
    return false;
  }
  return true;
}

// 在 present 事件里调用, 画在 ReShade 的 ImGui 之前
inline void Render(reshade::api::command_queue* queue, reshade::api::swapchain* swapchain) {
  if (enabled < 0.5f) return;
  if (failure_count >= 8u) return;   // 反复失败就别再每帧试了
  if (swapchain == nullptr || queue == nullptr || payload == nullptr || payload_count == 0u) return;
  auto* device = swapchain->get_device();
  if (device == nullptr) return;
  auto* cmd_list = queue->get_immediate_command_list();
  if (cmd_list == nullptr) return;

  const auto back_buffer = swapchain->get_current_back_buffer();
  if (back_buffer.handle == 0u) return;
  const auto desc = device->get_resource_desc(back_buffer);
  if (desc.type != reshade::api::resource_type::texture_2d) return;
  if (desc.texture.width == 0u || desc.texture.height == 0u) return;

  // present 路径上只做绑定与绘制: 设备对象(pipeline / 视图)一律在 init_swapchain 时建好。
  // 在 present 里创建设备对象有跟 ReShade 内部锁打架的风险, 一旦阻塞住, 游戏主线程会
  // 卡在 Present 上 —— 表现就是画面冻住、但音频和输入还在响应。
  auto found_state = states.find(device);
  if (found_state == states.end()) return;
  auto& state = found_state->second;
  if (state.pipeline.handle == 0u) return;
  const auto found_target = state.targets.find(back_buffer.handle);
  if (found_target == state.targets.end()) return;
  const auto view = found_target->second;
  if (view.handle == 0u) return;

  cmd_list->bind_pipeline(reshade::api::pipeline_stage::all_graphics, state.pipeline);
  cmd_list->bind_render_targets_and_depth_stencil(1, &view);
  cmd_list->push_constants(reshade::api::shader_stage::all_graphics, state.layout, 0, 0, payload_count, payload);

  const reshade::api::viewport viewport = {
      0.0f, 0.0f,
      static_cast<float>(desc.texture.width), static_cast<float>(desc.texture.height),
      0.0f, 1.0f};
  const reshade::api::rect scissor = {
      0, 0,
      static_cast<int32_t>(desc.texture.width), static_cast<int32_t>(desc.texture.height)};
  cmd_list->bind_viewports(0, 1, &viewport);
  cmd_list->bind_scissor_rects(0, 1, &scissor);
  cmd_list->draw(3, 1, 0, 0);
}

inline void DestroyDevice(reshade::api::device* device) {
  const auto found = states.find(device);
  if (found == states.end()) return;
  for (const auto& [handle, view] : found->second.targets) device->destroy_resource_view(view);
  if (found->second.pipeline.handle != 0u) device->destroy_pipeline(found->second.pipeline);
  if (found->second.layout.handle != 0u) device->destroy_pipeline_layout(found->second.layout);
  states.erase(found);
}

}  // namespace endfield::fpv_hud_pass
