#pragma once

#include <cstdint>

#include <Windows.h>

namespace endfield::vulkan_loader {

inline constexpr char kStatusExport[] =
    "RenoDXEndfieldVulkanBridgeStatusV1";

enum Status : uint32_t {
  SYSTEM_VULKAN_READY = 1u << 0u,
  RESHADE_READY = 1u << 1u,
  STREAMLINE_READY = 1u << 2u,
};

inline bool IsInstalled() {
  const HMODULE loader = GetModuleHandleW(L"vulkan-1.dll");
  return loader != nullptr && GetProcAddress(loader, kStatusExport) != nullptr;
}

}
