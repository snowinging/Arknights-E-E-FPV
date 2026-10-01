#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>

#include <cstdint>
#include <cstring>

#include <glad/vulkan.h>

#include "./vulkan_loader_api.hpp"
#include "./vulkan_loader_exports.hpp"

namespace {

HMODULE GetSystemVulkan() {
  static HMODULE module = [] {
    wchar_t path[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(path, MAX_PATH);
    if (length == 0u || length + 14u >= MAX_PATH) return HMODULE{};
    std::memcpy(path + length, L"\\vulkan-1.dll", 14u * sizeof(wchar_t));
    return LoadLibraryW(path);
  }();
  return module;
}

PFN_vkGetInstanceProcAddr GetSystemInstanceProcAddr() {
  static const auto function = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
      GetProcAddress(GetSystemVulkan(), "vkGetInstanceProcAddr"));
  return function;
}

PFN_vkGetDeviceProcAddr GetSystemDeviceProcAddr() {
  static const auto function = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
      GetProcAddress(GetSystemVulkan(), "vkGetDeviceProcAddr"));
  return function;
}

}

extern "C" __declspec(dllexport) PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char* name);
extern "C" __declspec(dllexport) PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char* name);

extern "C" __declspec(dllexport) PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char* name) {
  if (name != nullptr && std::strcmp(name, "vkGetInstanceProcAddr") == 0) {
    return reinterpret_cast<PFN_vkVoidFunction>(vkGetInstanceProcAddr);
  }
  if (name != nullptr && std::strcmp(name, "vkGetDeviceProcAddr") == 0) {
    return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceProcAddr);
  }
  const auto get_instance_proc_addr = GetSystemInstanceProcAddr();
  return get_instance_proc_addr == nullptr
             ? nullptr
             : get_instance_proc_addr(instance, name);
}

extern "C" __declspec(dllexport) PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char* name) {
  if (name != nullptr && std::strcmp(name, "vkGetDeviceProcAddr") == 0) {
    return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceProcAddr);
  }
  const auto get_device_proc_addr = GetSystemDeviceProcAddr();
  return get_device_proc_addr == nullptr
             ? nullptr
             : get_device_proc_addr(device, name);
}

extern "C" __declspec(dllexport) uint32_t WINAPI
RenoDXEndfieldVulkanBridgeStatusV1() {
  uint32_t value = GetSystemVulkan() == nullptr
                       ? 0u
                       : endfield::vulkan_loader::SYSTEM_VULKAN_READY;
  if (GetModuleHandleW(L"ReShade64.dll") != nullptr
      || GetModuleHandleW(L"ReShade.dll") != nullptr) {
    value |= endfield::vulkan_loader::RESHADE_READY;
  }
  if (GetModuleHandleW(L"sl.interposer.dll") != nullptr) {
    value |= endfield::vulkan_loader::STREAMLINE_READY;
  }
  return value;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  if (reason != DLL_PROCESS_ATTACH) return TRUE;

  DisableThreadLibraryCalls(module);
  SetEnvironmentVariableW(L"RESHADE_DISABLE_LOADING_CHECK", L"1");

  wchar_t module_path[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameW(module, module_path, MAX_PATH);
  if (length == 0u || length >= MAX_PATH) return TRUE;
  for (DWORD index = length; index != 0u; --index) {
    if (module_path[index - 1u] != L'\\') continue;
    module_path[index - 1u] = L'\0';
    SetEnvironmentVariableW(L"VK_IMPLICIT_LAYER_PATH", module_path);
    break;
  }
  return TRUE;
}
