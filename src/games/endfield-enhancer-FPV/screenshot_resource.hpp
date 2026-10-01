#pragma once

#include <atomic>
#include <optional>
#include "../../utils/resource.hpp"

namespace endfield::screenshots::photo_resource {
namespace api = reshade::api;
inline std::atomic_uint64_t deadline = 0;
inline std::atomic_uint32_t capture_width = 0;
inline thread_local std::optional<api::resource_desc> pending;
inline bool (*can_upgrade)(api::device*) = nullptr;
inline renodx::utils::resource::ResourceUpgradeInfo target = {
    .old_format = api::format::r11g11b10_float,
    .new_format = api::format::r16g16b16a16_float,
    .view_upgrades = renodx::utils::resource::VIEW_UPGRADES_RGBA16F,
    .name = "Endfield native photo composition intermediate",
};

inline void Arm(uint32_t width) {
  capture_width.store(width, std::memory_order_relaxed);
  deadline.store(GetTickCount64() + 2000, std::memory_order_release);
}

inline bool Matches(const api::resource_desc& desc, const api::resource_desc& backbuffer,
                    uint32_t width) {
  return width != 0 && desc.type == api::resource_type::texture_2d
         && backbuffer.type == api::resource_type::texture_2d
         && desc.texture.format == api::format::r11g11b10_float
         && desc.texture.width == width && desc.texture.width == backbuffer.texture.width
         && desc.texture.height == backbuffer.texture.height
         && desc.texture.levels == 1 && desc.texture.depth_or_layers == 1 && desc.texture.samples == 1
         && (desc.usage & api::resource_usage::render_target) != 0
         && (desc.usage & api::resource_usage::shader_resource) != 0
         && (desc.usage & api::resource_usage::unordered_access) == 0;
}

inline bool OnCreate(api::device* device, api::resource_desc& desc,
                     api::subresource_data* initial_data, api::resource_usage) {
  pending.reset();
  auto until = deadline.load(std::memory_order_acquire);
  if (!until || GetTickCount64() > until || initial_data || device->get_api() != api::device_api::vulkan) return false;
  if (!Matches(desc, renodx::utils::swapchain::GetBackBufferDesc(device), capture_width.load(std::memory_order_relaxed))
      || !device->check_format_support(api::format::r16g16b16a16_float, desc.usage)) return false;

  if (!can_upgrade || !can_upgrade(device)) {
    deadline.compare_exchange_strong(until, 0, std::memory_order_acq_rel);
    return false;
  }

  if (!deadline.compare_exchange_strong(until, 0, std::memory_order_acq_rel)) return false;
  pending = desc;
  desc.texture.format = api::format::r16g16b16a16_float;
  return true;
}

inline void OnInit(renodx::utils::resource::ResourceInfo* info) {
  if (!pending) return;

  if (info->desc.type == api::resource_type::texture_2d
      && info->desc.texture.format == api::format::r16g16b16a16_float
      && info->desc.texture.width == pending->texture.width
      && info->desc.texture.height == pending->texture.height) {
    info->upgraded = true;
    info->upgrade_target = &target;
    info->fallback_desc = *pending;
    info->extra_vram = renodx::utils::resource::ComputeTextureSize(info->desc)
                       - renodx::utils::resource::ComputeTextureSize(*pending);
  }
  pending.reset();
}

inline bool OnCreateView(api::device* device, api::resource resource,
                         api::resource_usage, api::resource_view_desc& desc) {
  if (device->get_api() != api::device_api::vulkan
      || desc.type != api::resource_view_type::texture_2d
      || (desc.format != api::format::r11g11b10_float
          && desc.format != api::format::unknown)) return false;
  bool photo = false;
  renodx::utils::resource::GetResourceInfo(resource, [&](const auto& info) {
    photo = !info.destroyed && info.device == device && info.upgraded
            && info.upgrade_target == &target
            && info.desc.texture.format == api::format::r16g16b16a16_float;
  });
  if (!photo) return false;
  desc.format = api::format::r16g16b16a16_float;
  return true;
}

inline void Use(DWORD reason) {
  if (reason == DLL_PROCESS_ATTACH) {
    renodx::utils::resource::Use(reason);
    renodx::utils::resource::RegisterOnInitResourceInfoCallback(OnInit);
    reshade::register_event<reshade::addon_event::create_resource>(OnCreate);
    reshade::register_event<reshade::addon_event::create_resource_view>(OnCreateView);
  } else if (reason == DLL_PROCESS_DETACH) {
    deadline = 0;
    reshade::unregister_event<reshade::addon_event::create_resource_view>(OnCreateView);
    reshade::unregister_event<reshade::addon_event::create_resource>(OnCreate);
    renodx::utils::resource::UnregisterOnInitResourceInfoCallback(OnInit);
    renodx::utils::resource::Use(reason);
  }
}
}
