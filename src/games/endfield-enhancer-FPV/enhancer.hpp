#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include <detours.h>
#include <glad/vulkan.h>
#include <sl_core_api.h>
#include <sl_dlss_g.h>
#include <Windows.h>
#include <include/reshade.hpp>

// FPV HUD交换链payload: fpv::Tick/OnFrame填充, 代理pass推送常量读取(布局须与fpv_hud.hlsli一致)
// 注意: 尺寸必须与 hdr_output.hpp ImageState::parameters 的 float 个数一致(那边会借用这块注入数据)
namespace endfield::fpv_swapchain {
struct FpvHudPayload {
  float v[20]; // 0,1,6,7=HDR契约哨兵; 2,3=分辨率; 4,5=HUD锚点; 8=总开关;
               // 9..12=摇杆(yaw/thr/roll/pitch, ±1); 13=元素开关位掩码(1准星/2地平线/4摇杆);
               // 14=地平线竖直偏移(比例); 15=横滚角; 16=摇杆方框边长(px); 17=摇杆方框间距(px);
               // 18,19=预留
};
inline FpvHudPayload hud_payload = {{1.f, 1.f, 0.f, 0.f, 0.f, 0.f, 4.f, 1.f,
                                     0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f,
                                     70.f, 10.f, 0.f, 0.f}};
} // namespace endfield::fpv_swapchain

#include "./hdr_output.hpp"
#include "./native_hooks.hpp"
#include "./render_quality.hpp"
#include "./ssr_depth.hpp"

namespace endfield::enhancer {
inline float fps_unlock = 0.f;
inline float fps_limit = 120.f;
inline float frame_generation_fps_limit = 240.f;
inline float background_fps_limit = 60.f;
inline float gtao_resolution = 0.f;
inline float ssr_resolution = 0.f;
inline float ssr_full_depth = 0.f;
inline float dof_resolution = 0.f;
inline float force_dof = 0.f;
inline float dof_focus_distance = 10.f;
inline float dof_near_blur = 3.f;
inline float dof_far_blur = 5.f;
inline float hdr_frame_generation = 0.f;

namespace detail {
inline constexpr uint32_t kHDR10Format =
    static_cast<uint32_t>(VK_FORMAT_A2B10G10R10_UNORM_PACK32);

using Il2CppDomain = void*;
using Il2CppAssembly = void*;
using Il2CppImage = void*;
using Il2CppClass = void*;
using Il2CppMethod = void*;

struct MethodInfo {
  void* method_pointer;
};

using DomainGet = Il2CppDomain (*)();
using ThreadAttach = void* (*)(Il2CppDomain);
using DomainGetAssemblies = Il2CppAssembly** (*)(Il2CppDomain, size_t*);
using AssemblyGetImage = Il2CppImage (*)(Il2CppAssembly);
using ImageGetName = const char* (*)(Il2CppImage);
using ClassFromName = Il2CppClass (*)(Il2CppImage, const char*, const char*);
using ClassGetMethodFromName = Il2CppMethod (*)(Il2CppClass, const char*, int);
using ClassGetFieldFromName = void* (*)(Il2CppClass, const char*);
using FieldGetOffset = size_t (*)(void*);
using RuntimeInvoke = void* (*)(Il2CppMethod, void*, void**, void**);
using ObjectUnbox = void* (*)(void*);
using RenderPath = void (*)(int64_t, void*, void*, void*, void*, void*);
using RenderSsr = void (*)(void*, void*, int32_t, void*, void*, bool);
using ResetSsr = void (*)(void*);
using ResolveICall = void* (*)(const char*);
using ObjectWithInstanceIDExists = bool (*)(int32_t);

inline DomainGet domain_get = nullptr;
inline ThreadAttach thread_attach = nullptr;
inline DomainGetAssemblies domain_get_assemblies = nullptr;
inline AssemblyGetImage assembly_get_image = nullptr;
inline ImageGetName image_get_name = nullptr;
inline ClassFromName class_from_name = nullptr;
inline ClassGetMethodFromName class_get_method_from_name = nullptr;
inline ClassGetFieldFromName class_get_field_from_name = nullptr;
inline FieldGetOffset field_get_offset = nullptr;
inline RuntimeInvoke runtime_invoke = nullptr;
inline ObjectUnbox object_unbox = nullptr;

inline Il2CppMethod get_target_frame_rate = nullptr;
inline Il2CppMethod set_target_frame_rate = nullptr;
inline Il2CppMethod get_vsync_count = nullptr;
inline Il2CppMethod set_vsync_count = nullptr;
inline RenderPath render_path = nullptr;
inline RenderSsr render_ssr = nullptr;
inline ResetSsr reset_ssr = nullptr;
inline PFun_slDLSSGSetOptions* set_frame_generation_options = nullptr;
inline PFun_slSetTag* set_tags = nullptr;
inline PFun_slSetTagForFrame* set_tags_for_frame = nullptr;
inline PFN_vkCreateSwapchainKHR streamline_create_swapchain = nullptr;

inline size_t render_params_gtao_offset = 0;
inline size_t gtao_width_offset = 0;
inline size_t gtao_height_offset = 0;
inline size_t render_params_frame_generation_pause_offset = 0;

inline size_t before_culling_settings_offset = 0;
inline size_t dof_quality_offset = 0;
inline size_t dof_scale_adjust_offset = 0;
inline size_t render_params_dof_offset = 0;
inline size_t dof_scale_offset = 0;
inline bool dof_quality_ready = false;
inline bool dof_resolution_ready = false;
inline bool dof_force_ready = false;
struct DoFManualOffsets {
  size_t enable, camera, physical, focus, aperture;
  size_t near_start, near_end, near_radius;
  size_t far_start, far_end, far_radius, temporal, max_radius, debug;
};
inline DoFManualOffsets dof_manual = {};
inline size_t native_camera_instance_id_offset = 0;
inline ObjectWithInstanceIDExists object_with_instance_id_exists = nullptr;

inline std::atomic_uint64_t ssr_resolution_state = 0;
struct SsrInstanceState {
  void* instance = nullptr;
  uint64_t resolution_state = 0;
};
inline std::array<SsrInstanceState, 64> ssr_instances = {};
inline size_t ssr_next_instance = 0;
inline SRWLOCK ssr_instances_lock = SRWLOCK_INIT;
inline std::atomic_bool ssr_resolution_failed = false;
inline std::atomic<float> dof_resolution_override = 0.f;
inline std::atomic_bool dof_force_override = false;
inline std::atomic<float> dof_focus_override = 10.f;
inline std::atomic<float> dof_near_override = 3.f;
inline std::atomic<float> dof_far_override = 5.f;
inline std::atomic_bool quality_access_failed = false;

inline std::atomic_bool shutting_down = false;
inline std::atomic_uint32_t gtao_resolution_multiplier = 1;

inline bool api_ready = false;
inline bool fps_ready = false;
inline bool streamline_hook_installed = false;
inline bool hdr_hooks_installed = false;
inline std::mutex streamline_options_mutex;
inline std::mutex streamline_install_mutex;
inline std::atomic_bool frame_generation_presenting = false;
inline std::atomic_bool frame_generation_paused = true;
inline bool gtao_ready = false;
inline bool fps_applied = false;
inline bool render_path_hook_installed = false;
inline bool ssr_resolution_hook_installed = false;
inline int original_target_frame_rate = -1;
inline int original_vsync_count = 0;
inline int last_applied_fps = 0;
inline uint32_t present_count = 0;
inline thread_local bool thread_attached = false;

struct GtaoDimensions {
  void* settings = nullptr;
  int32_t source_width = 0;
  int32_t source_height = 0;
  int32_t written_width = 0;
  int32_t written_height = 0;
  bool modified = false;
};
inline thread_local GtaoDimensions gtao_dimensions;

inline void Log(reshade::log::level level, const char* message) {
  reshade::log::message(level, message);
}

template <typename T>
bool ResolveExport(HMODULE module, const char* name, T* target) {
  *target = reinterpret_cast<T>(GetProcAddress(module, name));
  return *target != nullptr;
}

inline bool AttachThread() {
  if (thread_attached) return true;
  if (domain_get == nullptr || thread_attach == nullptr) return false;
  Il2CppDomain domain = domain_get();
  if (domain == nullptr) return false;
  thread_attached = thread_attach(domain) != nullptr;
  return thread_attached;
}

inline bool ResolveApi() {
  if (api_ready) return AttachThread();

  HMODULE game_assembly = GetModuleHandleW(L"GameAssembly.dll");
  if (game_assembly == nullptr) return false;

  api_ready = ResolveExport(game_assembly, "il2cpp_domain_get", &domain_get)
              && ResolveExport(game_assembly, "il2cpp_thread_attach", &thread_attach)
              && ResolveExport(game_assembly, "il2cpp_domain_get_assemblies", &domain_get_assemblies)
              && ResolveExport(game_assembly, "il2cpp_assembly_get_image", &assembly_get_image)
              && ResolveExport(game_assembly, "il2cpp_image_get_name", &image_get_name)
              && ResolveExport(game_assembly, "il2cpp_class_from_name", &class_from_name)
              && ResolveExport(game_assembly, "il2cpp_class_get_method_from_name", &class_get_method_from_name)
              && ResolveExport(game_assembly, "il2cpp_class_get_field_from_name", &class_get_field_from_name)
              && ResolveExport(game_assembly, "il2cpp_field_get_offset", &field_get_offset)
              && ResolveExport(game_assembly, "il2cpp_runtime_invoke", &runtime_invoke)
              && ResolveExport(game_assembly, "il2cpp_object_unbox", &object_unbox);
  return api_ready && AttachThread();
}

inline Il2CppImage FindImage(const char* partial_name) {
  Il2CppDomain domain = domain_get();
  if (domain == nullptr) return nullptr;

  size_t count = 0;
  Il2CppAssembly** assemblies = domain_get_assemblies(domain, &count);
  if (assemblies == nullptr) return nullptr;

  for (size_t index = 0; index < count; ++index) {
    Il2CppImage image = assembly_get_image(assemblies[index]);
    const char* name = image == nullptr ? nullptr : image_get_name(image);
    if (name != nullptr && std::strstr(name, partial_name) != nullptr) return image;
  }
  return nullptr;
}

inline Il2CppClass FindGraphicsClass(Il2CppImage image, const char* name) {
  Il2CppClass type = class_from_name(
      image, "UnityEngine.HyperGryphEngineCode", name);
  return type != nullptr ? type : class_from_name(image, "", name);
}

inline bool ResolveCppFieldOffset(
    Il2CppClass type,
    const char* primary_name,
    const char* fallback_name,
    size_t* offset) {
  constexpr size_t kObjectHeaderSize = 0x10;
  constexpr size_t kMaximumExpectedOffset = 0x10000;

  void* field = class_get_field_from_name(type, primary_name);
  if (field == nullptr && fallback_name != nullptr) {
    field = class_get_field_from_name(type, fallback_name);
  }
  if (field == nullptr) return false;

  const size_t metadata_offset = field_get_offset(field);
  if (metadata_offset < kObjectHeaderSize
      || metadata_offset >= kMaximumExpectedOffset) {
    return false;
  }

  *offset = metadata_offset - kObjectHeaderSize;
  return true;
}

inline void ResetSsrHistoryForResolution(void* self, uint64_t resolution_state) {
  AcquireSRWLockExclusive(&ssr_instances_lock);
  __try {
    SsrInstanceState* state = nullptr;
    for (auto& entry : ssr_instances) {
      if (entry.instance == self) {
        state = &entry;
        break;
      }
    }

    if (resolution_state >= 4
        && (state == nullptr || state->resolution_state != resolution_state)) {
      reset_ssr(self);
    }

    if (state == nullptr) {
      state = &ssr_instances[ssr_next_instance];
      ssr_next_instance = (ssr_next_instance + 1) % ssr_instances.size();
    }
    *state = {self, resolution_state};
  } __finally {
    ReleaseSRWLockExclusive(&ssr_instances_lock);
  }
}

inline void HookedRenderSsr(
    void* self, void* graph, int32_t pass, void* input, void* output, bool wetness) {
  RenderQualityOverrides quality;
  ssr_depth::Context depth_context = {graph, 0, 0};
  const auto* previous_depth_context = ssr_depth::active;
  const uint64_t resolution_state = ssr_resolution_state.load(std::memory_order_relaxed);
  bool prepare_resolution = false;
  if (!shutting_down.load(std::memory_order_relaxed)
      && !ssr_resolution_failed.load(std::memory_order_relaxed)
      && input != nullptr && self != nullptr && reset_ssr != nullptr) {
    __try {
      auto* data = static_cast<uint8_t*>(input);
      const int32_t width = *reinterpret_cast<const int32_t*>(data + 0x04);
      const int32_t height = *reinterpret_cast<const int32_t*>(data + 0x08);
      if (data[0] != 0 && (wetness || data[3] == 0) && width > 0 && height > 0
          && width <= 16384 && height <= 16384) {
        if ((resolution_state & 1) != 0) {
          quality.Set<int32_t>(input, 0x14, 4);
        }
        depth_context.width = width;
        depth_context.height = height;
        prepare_resolution = true;
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      ssr_resolution_failed.store(true, std::memory_order_relaxed);
      quality.Restore();
      Log(reshade::log::level::error,
          "E_E_FPV: native SSR input access failed; override disabled until restart.");
    }
  }

  __try {
    ssr_depth::active = prepare_resolution && (resolution_state & 2) != 0
                            ? &depth_context
                            : nullptr;
    if (prepare_resolution) ResetSsrHistoryForResolution(self, resolution_state);
    render_ssr(self, graph, pass, input, output, wetness);
  } __finally {
    if (!quality.Restore()) {
      ssr_resolution_failed.store(true, std::memory_order_relaxed);
      Log(reshade::log::level::error,
          "E_E_FPV: native SSR input restoration failed; override disabled until restart.");
    }
    ssr_depth::active = previous_depth_context;
  }
}

inline bool ValidateNativeSsrRouter(
    HMODULE unity_player,
    const void* method_pointer) {
  constexpr DWORD kSupportedAssemblyTimestamp = 0x6A85914F;
  constexpr DWORD kSupportedAssemblyImageSize = 0x0208B000;
  constexpr DWORD kSupportedCnAssemblyImageSize = 0x0208A000;
  constexpr uint8_t kExpectedPrologue[] = {
      0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x80, 0x7C,
      0x24, 0x68, 0x00, 0x4C, 0x8B, 0xDA, 0x48, 0x8B,
      0xD9, 0x75, 0x0D, 0x41, 0x80, 0x39, 0x00, 0x74,
      0x07, 0x41, 0x80, 0x79, 0x03, 0x00, 0x75, 0x52,
      0x49, 0x8B, 0x41, 0x18, 0x49, 0x8B, 0x49, 0x28,
      0x0F, 0xB6, 0x90, 0xE1, 0x01, 0x00, 0x00};
  if (unity_player == nullptr || method_pointer == nullptr) return false;

  __try {
    auto has_identity = [](HMODULE module, DWORD timestamp, DWORD image_size) {
      if (module == nullptr) return false;
      auto* module_base = reinterpret_cast<const uint8_t*>(module);
      auto* module_dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module_base);
      if (module_dos->e_magic != IMAGE_DOS_SIGNATURE
          || module_dos->e_lfanew <= 0) {
        return false;
      }
      auto* module_nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
          module_base + module_dos->e_lfanew);
      return module_nt->Signature == IMAGE_NT_SIGNATURE
             && module_nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64
             && module_nt->FileHeader.TimeDateStamp == timestamp
             && module_nt->OptionalHeader.SizeOfImage == image_size;
    };

    if (!has_identity(
            unity_player,
            kSupportedAssemblyTimestamp,
            kSupportedAssemblyImageSize)
        && !has_identity(
            unity_player,
            kSupportedAssemblyTimestamp,
            kSupportedCnAssemblyImageSize)) {
      return false;
    }

    auto* base = reinterpret_cast<const uint8_t*>(unity_player);
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);

    if (!ssr_depth::ValidateTargets(base, nt)) return false;

    auto* method = static_cast<const uint8_t*>(method_pointer);
    if (method < base
        || method + sizeof(kExpectedPrologue) > base + nt->OptionalHeader.SizeOfImage
        || std::memcmp(method, kExpectedPrologue, sizeof(kExpectedPrologue)) != 0) {
      return false;
    }

    constexpr uint8_t kV2Call[] = {0xE8, 0x44, 0x83, 0xD0, 0xFF};
    constexpr uint8_t kV1Call[] = {0xE8, 0xC5, 0x8A, 0x20, 0x01};
    constexpr uint8_t kV2Branch[] = {
        0x41, 0x83, 0x7C, 0x24, 0x14, 0x04, 0x48, 0x89,
        0x5D, 0x48, 0x0F, 0x84, 0x28, 0xF1, 0xD1, 0x00};
    constexpr uint8_t kV1Branch[] = {
        0x41, 0x83, 0x7C, 0x24, 0x14, 0x04, 0x75, 0x13,
        0x49, 0x8B, 0x7C, 0x24, 0x04};
    constexpr uint8_t kV2FullSize[] = {
        0x49, 0x8B, 0x5C, 0x24, 0x04, 0x48, 0x89, 0x5D, 0x90,
        0x44, 0x8B, 0x6D, 0x94, 0x44, 0x8B, 0x75, 0x90,
        0x44, 0x89, 0x6D, 0xD8, 0x44, 0x89, 0x75, 0xE0,
        0x48, 0x89, 0x5D, 0x48, 0xE9, 0xB6, 0x0E, 0x2E, 0xFF};

    constexpr uint8_t kSsrReset[] = {
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B,
        0xD9, 0xC6, 0x41, 0x28, 0x01, 0x33, 0xFF, 0x89, 0x79, 0x04, 0x33, 0xC9,
        0x48, 0x8B, 0x43, 0x08, 0x48, 0x89, 0x4B, 0x08, 0x48, 0x8D, 0x4C, 0x24,
        0x20, 0x48, 0x89, 0x44, 0x24, 0x20, 0x48, 0x8B, 0x43, 0x10, 0x48, 0x89,
        0x44, 0x24, 0x28, 0x48, 0x89, 0x7B, 0x10, 0xE8, 0x04, 0x04, 0x00, 0x00,
        0x48, 0x8B, 0x43, 0x18, 0x33, 0xC9, 0x48, 0x89, 0x4B, 0x18, 0x48, 0x8D,
        0x4C, 0x24, 0x20, 0x48, 0x89, 0x44, 0x24, 0x20, 0x48, 0x8B, 0x43, 0x20,
        0x48, 0x89, 0x44, 0x24, 0x28, 0x48, 0x89, 0x7B, 0x20, 0xE8, 0xDE, 0x03,
        0x00, 0x00, 0x89, 0x7B, 0x2C, 0x33, 0xC9, 0x48, 0x8B, 0x43, 0x38, 0x48,
        0x89, 0x4B, 0x38, 0x48, 0x8D, 0x4C, 0x24, 0x20, 0x48, 0x89, 0x44, 0x24,
        0x20, 0x48, 0x8B, 0x43, 0x40, 0x48, 0x89, 0x44, 0x24, 0x28, 0x48, 0x89,
        0x7B, 0x40, 0xE8, 0xB5, 0x03, 0x00, 0x00, 0x48, 0x8B, 0x43, 0x48, 0x33,
        0xC9, 0x48, 0x89, 0x4B, 0x48, 0x48, 0x8D, 0x4C, 0x24, 0x20, 0x48, 0x89,
        0x44, 0x24, 0x20, 0x48, 0x8B, 0x43, 0x50, 0x48, 0x89, 0x44, 0x24, 0x28,
        0x48, 0x89, 0x7B, 0x50, 0xE8, 0x8F, 0x03, 0x00, 0x00, 0x48, 0x8B, 0x43,
        0x58, 0x33, 0xC9, 0x48, 0x89, 0x4B, 0x58, 0x48, 0x8D, 0x4C, 0x24, 0x20,
        0x48, 0x89, 0x44, 0x24, 0x20, 0x48, 0x8B, 0x43, 0x60, 0x48, 0x89, 0x44,
        0x24, 0x28, 0x48, 0x89, 0x7B, 0x60, 0xE8, 0x69, 0x03, 0x00, 0x00, 0x48,
        0x8B, 0x5C, 0x24, 0x40, 0x48, 0x83, 0xC4, 0x30, 0x5F, 0xC3};
    constexpr uint8_t kSsrRelease[] = {
        0x83, 0x39, 0x00, 0x74, 0x12, 0x8B, 0x11, 0x48,
        0x8B, 0x41, 0x08, 0x48, 0xC1, 0xE2, 0x06, 0x48,
        0x8B, 0x48, 0x30, 0xFF, 0x4C, 0x0A, 0x08, 0xC3};
    constexpr uint8_t kV2ResetCall[] = {0xE8, 0xFC, 0x18, 0x1F, 0x00};
    constexpr uint8_t kV1ResetCall[] = {0xE8, 0x10, 0x32, 0xCF, 0xFE};
    if (std::memcmp(base + 0x3A3B60, kSsrReset, sizeof(kSsrReset)) != 0
        || std::memcmp(base + 0x3A3FA0, kSsrRelease, sizeof(kSsrRelease)) != 0
        || std::memcmp(base + 0x1B225F, kV2ResetCall, sizeof(kV2ResetCall)) != 0
        || std::memcmp(base + 0x16B094B, kV1ResetCall, sizeof(kV1ResetCall)) != 0) {
      return false;
    }
    if (method != base + 0x4A7D70
        || std::memcmp(base + 0x4A7DD7, kV2Call, sizeof(kV2Call)) != 0
        || std::memcmp(base + 0x4A7E16, kV1Call, sizeof(kV1Call)) != 0
        || std::memcmp(base + 0x1B024A, kV2Branch, sizeof(kV2Branch)) != 0
        || std::memcmp(base + 0x16B0A42, kV1Branch, sizeof(kV1Branch)) != 0
        || std::memcmp(base + 0xECF382, kV2FullSize, sizeof(kV2FullSize)) != 0) {
      return false;
    }

    size_t matches = 0;
    size_t reset_matches = 0;
    auto* section = IMAGE_FIRST_SECTION(nt);
    for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index) {
      if ((section[index].Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) continue;
      const size_t size = section[index].Misc.VirtualSize;
      const size_t virtual_address = section[index].VirtualAddress;
      if (size < sizeof(kExpectedPrologue)
          || virtual_address >= nt->OptionalHeader.SizeOfImage
          || size > nt->OptionalHeader.SizeOfImage - virtual_address) {
        continue;
      }
      const uint8_t* start = base + section[index].VirtualAddress;
      for (size_t offset = 0; offset <= size - sizeof(kExpectedPrologue); ++offset) {
        if (size >= sizeof(kSsrReset) && offset <= size - sizeof(kSsrReset)
            && std::memcmp(start + offset, kSsrReset, sizeof(kSsrReset)) == 0) {
          if (++reset_matches > 1) return false;
        }
        if (std::memcmp(start + offset, kExpectedPrologue, sizeof(kExpectedPrologue)) == 0) {
          ++matches;
          if (matches > 1) return false;
        }
      }
    }
    return matches == 1 && reset_matches == 1;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

inline bool InstallSsrResolutionHook() {
  if (ssr_resolution_hook_installed) return true;
  if (ssr_resolution_failed.load(std::memory_order_relaxed)) return false;
  HMODULE unity_player = GetModuleHandleW(L"UnityPlayer.dll");
  if (unity_player == nullptr) return false;
  render_ssr = reinterpret_cast<RenderSsr>(
      reinterpret_cast<uint8_t*>(unity_player) + 0x4A7D70);
  if (!ValidateNativeSsrRouter(unity_player, reinterpret_cast<void*>(render_ssr))) {
    render_ssr = nullptr;
    ssr_resolution_failed.store(true, std::memory_order_relaxed);
    Log(reshade::log::level::error,
        "E_E_FPV: unsupported UnityPlayer SSR router; resolution override disabled.");
    return false;
  }

  reset_ssr = reinterpret_cast<ResetSsr>(
      reinterpret_cast<uint8_t*>(unity_player) + 0x3A3B60);
  ssr_depth::build_pyramid = reinterpret_cast<ssr_depth::BuildPyramid>(
      reinterpret_cast<uint8_t*>(unity_player) + 0x1AEFF0);
  ssr_depth::register_full = reinterpret_cast<ssr_depth::RegisterRay>(
      reinterpret_cast<uint8_t*>(unity_player) + 0xE623B0);
  ssr_depth::register_low = reinterpret_cast<ssr_depth::RegisterRay>(
      reinterpret_cast<uint8_t*>(unity_player) + 0xE62580);
  ssr_depth::add_read = reinterpret_cast<ssr_depth::AddRead>(
      reinterpret_cast<uint8_t*>(unity_player) + 0xF4E5F8);

  ssr_resolution_state.store((ssr_resolution == 1.f ? 1u : 0u)
                                 | (ssr_full_depth == 1.f ? 2u : 0u),
                             std::memory_order_relaxed);
  if (ssr_resolution == 2.f) {
    Log(reshade::log::level::warning,
        "E_E_FPV: obsolete Double SSR selection is unsupported; using vanilla.");
  }
  if (native_hooks::Begin() != NO_ERROR) return false;
  if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR
      || DetourAttach(&render_ssr, HookedRenderSsr) != NO_ERROR
      || DetourAttach(&ssr_depth::build_pyramid, ssr_depth::HookedBuildPyramid) != NO_ERROR
      || DetourAttach(&ssr_depth::register_full, ssr_depth::HookedRegisterFull) != NO_ERROR
      || DetourAttach(&ssr_depth::register_low, ssr_depth::HookedRegisterLow) != NO_ERROR) {
    DetourTransactionAbort();
    render_ssr = nullptr;
    return false;
  }
  if (DetourTransactionCommit() != NO_ERROR) {
    render_ssr = nullptr;
    return false;
  }
  ssr_resolution_hook_installed = true;
  return true;
}

inline Il2CppMethod FindMethod(
    Il2CppImage image,
    const char* namespaze,
    const char* class_name,
    const char* method_name,
    int parameter_count) {
  Il2CppClass type = class_from_name(image, namespaze, class_name);
  return type == nullptr
             ? nullptr
             : class_get_method_from_name(type, method_name, parameter_count);
}

inline bool ReadInt(Il2CppMethod method, int* value) {
  void* exception = nullptr;
  void* result = runtime_invoke(method, nullptr, nullptr, &exception);
  if (result == nullptr || exception != nullptr) return false;
  void* unboxed = object_unbox(result);
  if (unboxed == nullptr) return false;
  std::memcpy(value, unboxed, sizeof(*value));
  return true;
}

inline bool WriteInt(Il2CppMethod method, int value) {
  void* parameters[] = {&value};
  void* exception = nullptr;
  runtime_invoke(method, nullptr, parameters, &exception);
  return exception == nullptr;
}

inline bool ResolveFpsMethods() {
  if (fps_ready) return true;
  if (!ResolveApi()) return false;

  Il2CppImage core_image = FindImage("UnityEngine.CoreModule");
  if (core_image == nullptr) return false;

  get_target_frame_rate = FindMethod(
      core_image, "UnityEngine", "Application", "get_targetFrameRate", 0);
  set_target_frame_rate = FindMethod(
      core_image, "UnityEngine", "Application", "set_targetFrameRate", 1);
  get_vsync_count = FindMethod(
      core_image, "UnityEngine", "QualitySettings", "get_vSyncCount", 0);
  set_vsync_count = FindMethod(
      core_image, "UnityEngine", "QualitySettings", "set_vSyncCount", 1);
  fps_ready = get_target_frame_rate != nullptr
              && set_target_frame_rate != nullptr
              && get_vsync_count != nullptr
              && set_vsync_count != nullptr;

  return fps_ready;
}

__declspec(noinline) inline sl::Result ForwardFrameGenerationOptionsLocked(
    const sl::ViewportHandle& viewport,
    const sl::DLSSGOptions& options) {
  sl::DLSSGOptions forwarded_options = options;
  if (hdr_hooks_installed) {
    forwarded_options.colorBufferFormat = kHDR10Format;
  }

  frame_generation_presenting.store(false, std::memory_order_release);

  sl::Result result;
  __try {
    result = set_frame_generation_options(viewport, forwarded_options);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Log(reshade::log::level::error,
        "E_E_FPV: Streamline SetOptions forwarding failed.");
    return sl::Result::eErrorExceptionHandler;
  }

  if (result != sl::Result::eOk) return result;

  __try {
    frame_generation_presenting.store(
        options.mode != sl::DLSSGMode::eOff && options.numFramesToGenerate != 0u,
        std::memory_order_relaxed);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    frame_generation_presenting.store(false, std::memory_order_relaxed);
  }
  return result;
}

inline sl::Result HookedSetFrameGenerationOptions(
    const sl::ViewportHandle& viewport,
    const sl::DLSSGOptions& options) {
  const std::lock_guard transition_lock(streamline_options_mutex);
  return ForwardFrameGenerationOptionsLocked(viewport, options);
}

inline const sl::ResourceTag* FilterFrameGenerationTags(
    const sl::ResourceTag* tags,
    uint32_t num_tags) {
  if (tags == nullptr || num_tags == 0u || !hdr_hooks_installed) return tags;

  bool has_hudless_color = false;
  for (uint32_t i = 0u; i < num_tags; ++i) {
    if (tags[i].type == sl::kBufferTypeHUDLessColor
        && tags[i].resource != nullptr) {
      has_hudless_color = true;
      break;
    }
  }
  if (!has_hudless_color) return tags;

  thread_local std::vector<sl::ResourceTag> filtered_tags;
  filtered_tags.assign(tags, tags + num_tags);
  for (auto& tag : filtered_tags) {
    if (tag.type == sl::kBufferTypeHUDLessColor) {
      tag.resource = nullptr;
    }
  }

  return filtered_tags.data();
}

inline sl::Result HookedSetTags(
    const sl::ViewportHandle& viewport,
    const sl::ResourceTag* tags,
    uint32_t num_tags,
    sl::CommandBuffer* command_buffer) {
  return set_tags(
      viewport,
      FilterFrameGenerationTags(tags, num_tags),
      num_tags,
      command_buffer);
}

inline sl::Result HookedSetTagsForFrame(
    const sl::FrameToken& frame,
    const sl::ViewportHandle& viewport,
    const sl::ResourceTag* tags,
    uint32_t num_tags,
    sl::CommandBuffer* command_buffer) {
  return set_tags_for_frame(
      frame,
      viewport,
      FilterFrameGenerationTags(tags, num_tags),
      num_tags,
      command_buffer);
}

inline VkResult VKAPI_CALL HookedStreamlineCreateSwapchain(
    VkDevice device,
    const VkSwapchainCreateInfoKHR* create_info,
    const VkAllocationCallbacks* allocator,
    VkSwapchainKHR* swapchain) {
  VkSwapchainCreateInfoKHR hdr_create_info = {};
  const bool use_hdr = create_info != nullptr && hdr_hooks_installed;
  if (use_hdr) {
    hdr_create_info = *create_info;
    hdr_create_info.imageFormat = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    hdr_create_info.imageColorSpace = VK_COLOR_SPACE_HDR10_ST2084_EXT;
  }
  const VkResult result = streamline_create_swapchain(
      device,
      use_hdr ? &hdr_create_info : create_info,
      allocator,
      swapchain);
  if (result == VK_SUCCESS && use_hdr && swapchain != nullptr) {
    endfield::hdr_output::TrackSwapchain(*swapchain, hdr_create_info);
  }
  return result;
}

inline bool InstallStreamlineHook(reshade::api::device* device) {
  const std::lock_guard install_lock(streamline_install_mutex);
  if (streamline_hook_installed) return true;
  if (device == nullptr || device->get_api() != reshade::api::device_api::vulkan) return false;
  if (!endfield::hdr_output::events_registered && fps_limit <= 0.f && frame_generation_fps_limit <= 0.f) return false;

  HMODULE interposer = GetModuleHandleW(L"sl.interposer.dll");
  if (interposer == nullptr) return false;

  auto* get_feature_function = reinterpret_cast<PFun_slGetFeatureFunction*>(
      GetProcAddress(interposer, "slGetFeatureFunction"));
  if (get_feature_function == nullptr) return false;

  void* set_options = nullptr;
  if (get_feature_function(
          sl::kFeatureDLSS_G,
          "slDLSSGSetOptions",
          set_options)
          != sl::Result::eOk
      || set_options == nullptr) {
    return false;
  }

  set_frame_generation_options =
      reinterpret_cast<PFun_slDLSSGSetOptions*>(set_options);

  const bool enable_hdr = endfield::hdr_output::events_registered;
  if (enable_hdr) {
    set_tags = reinterpret_cast<PFun_slSetTag*>(
        GetProcAddress(interposer, "slSetTag"));
    set_tags_for_frame = reinterpret_cast<PFun_slSetTagForFrame*>(
        GetProcAddress(interposer, "slSetTagForFrame"));
    streamline_create_swapchain = reinterpret_cast<PFN_vkCreateSwapchainKHR>(
        GetProcAddress(interposer, "vkCreateSwapchainKHR"));
    if (set_tags == nullptr || set_tags_for_frame == nullptr || streamline_create_swapchain == nullptr) {
      set_frame_generation_options = nullptr;
      set_tags = nullptr;
      set_tags_for_frame = nullptr;
      streamline_create_swapchain = nullptr;
      return false;
    }
  }

  if (native_hooks::Begin() != NO_ERROR) return false;
  if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR
      || DetourAttach(&set_frame_generation_options, HookedSetFrameGenerationOptions)
             != NO_ERROR
      || (enable_hdr && (DetourAttach(&set_tags, HookedSetTags) != NO_ERROR || DetourAttach(&set_tags_for_frame, HookedSetTagsForFrame) != NO_ERROR || DetourAttach(&streamline_create_swapchain, HookedStreamlineCreateSwapchain) != NO_ERROR || !endfield::hdr_output::AttachHooks(interposer, reinterpret_cast<VkDevice>(device->get_native()))))) {
    DetourTransactionAbort();
    set_frame_generation_options = nullptr;
    set_tags = nullptr;
    set_tags_for_frame = nullptr;
    streamline_create_swapchain = nullptr;
    return false;
  }
  if (DetourTransactionCommit() != NO_ERROR) {
    set_frame_generation_options = nullptr;
    set_tags = nullptr;
    set_tags_for_frame = nullptr;
    streamline_create_swapchain = nullptr;
    return false;
  }

  streamline_hook_installed = true;
  hdr_hooks_installed = enable_hdr;
  if (enable_hdr) {
    endfield::hdr_output::capture_graphics.store(true, std::memory_order_relaxed);
    endfield::hdr_output::RegisterDrawCallbacks();
  }
  return true;
}

inline void HookedRenderPath(
    int64_t pointer,
    void* render_path_params,
    void* before_culling_params,
    void* camera,
    void* render_context,
    void* command) {
  RenderQualityOverrides quality;
  if (!shutting_down.load(std::memory_order_relaxed)
      && render_path_params != nullptr) {
    __try {
      auto* parameters = static_cast<uint8_t*>(render_path_params);
      frame_generation_paused.store(
          *reinterpret_cast<const uint8_t*>(
              parameters + render_params_frame_generation_pause_offset)
              != 0,
          std::memory_order_relaxed);
      void* gtao = *reinterpret_cast<void**>(
          parameters + render_params_gtao_offset);
      if (gtao != nullptr) {
        auto* settings = static_cast<uint8_t*>(gtao);
        auto* width = reinterpret_cast<int32_t*>(settings + gtao_width_offset);
        auto* height = reinterpret_cast<int32_t*>(settings + gtao_height_offset);
        const uint32_t multiplier = gtao_resolution_multiplier.load(std::memory_order_relaxed);

        if (multiplier == 2) {
          if (!gtao_dimensions.modified
              || gtao_dimensions.settings != gtao
              || *width != gtao_dimensions.written_width
              || *height != gtao_dimensions.written_height) {
            gtao_dimensions.settings = gtao;
            gtao_dimensions.source_width = *width;
            gtao_dimensions.source_height = *height;
          }

          if (gtao_dimensions.source_width > 0
              && gtao_dimensions.source_width <= 16384 / static_cast<int32_t>(multiplier)
              && gtao_dimensions.source_height > 0
              && gtao_dimensions.source_height <= 16384 / static_cast<int32_t>(multiplier)) {
            gtao_dimensions.written_width = gtao_dimensions.source_width * multiplier;
            gtao_dimensions.written_height = gtao_dimensions.source_height * multiplier;
            *width = gtao_dimensions.written_width;
            *height = gtao_dimensions.written_height;
            gtao_dimensions.modified = true;
          } else if (gtao_dimensions.modified) {
            if (*width == gtao_dimensions.written_width && *height == gtao_dimensions.written_height) {
              *width = gtao_dimensions.source_width;
              *height = gtao_dimensions.source_height;
            }
            gtao_dimensions = {};
          }
        } else {
          if (gtao_dimensions.modified
              && gtao_dimensions.settings == gtao
              && *width == gtao_dimensions.written_width
              && *height == gtao_dimensions.written_height) {
            *width = gtao_dimensions.source_width;
            *height = gtao_dimensions.source_height;
          }
          gtao_dimensions = {};
        }
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      frame_generation_paused.store(true, std::memory_order_relaxed);
      gtao_resolution_multiplier.store(1, std::memory_order_relaxed);
      Log(
          reshade::log::level::error,
          "E_E_FPV: render-path metadata access failed; dependent features disabled.");
    }
  }

  __try {
    if (!shutting_down.load(std::memory_order_relaxed)
        && !quality_access_failed.load(std::memory_order_relaxed)
        && render_path_params != nullptr && before_culling_params != nullptr) {
      __try {
        const float dof_scale = dof_resolution_override.load(std::memory_order_relaxed);
        const bool manual_dof = dof_force_override.load(std::memory_order_relaxed);
        if ((dof_resolution_ready && dof_scale != 0.f)
            || (dof_force_ready && manual_dof)) {
          void* native_settings = *reinterpret_cast<void**>(
              static_cast<uint8_t*>(before_culling_params) + before_culling_settings_offset);
          if (native_settings != nullptr) {
            void* dof = *reinterpret_cast<void**>(
                static_cast<uint8_t*>(render_path_params) + render_params_dof_offset);
            if (dof != nullptr) {
              if (dof_resolution_ready && dof_scale != 0.f) {
                quality.Set<uint8_t>(native_settings, dof_scale_adjust_offset, 0);
                quality.Set<float>(dof, dof_scale_offset, dof_scale);
              }

              if (dof_force_ready && manual_dof && camera != nullptr
                  && object_with_instance_id_exists != nullptr) {
                const int32_t camera_id = *reinterpret_cast<const int32_t*>(
                    static_cast<const uint8_t*>(camera) + native_camera_instance_id_offset);
                if (camera_id != 0 && object_with_instance_id_exists(camera_id)) {
                  quality.Set<int32_t>(native_settings, dof_quality_offset, 0);
                  quality.Set<int32_t>(dof, dof_manual.camera, camera_id);
                  const float focus = dof_focus_override.load(std::memory_order_relaxed);

                  quality.Set<uint8_t>(dof, dof_manual.physical, 0);
                  quality.Set<float>(dof, dof_manual.focus, focus);
                  quality.Set<float>(dof, dof_manual.aperture, 16.f);
                  quality.Set<float>(dof, dof_manual.near_start, focus * 0.25f);
                  quality.Set<float>(dof, dof_manual.near_end, focus * 0.75f);
                  quality.Set<float>(dof, dof_manual.near_radius, dof_near_override.load(std::memory_order_relaxed));
                  quality.Set<float>(dof, dof_manual.far_start, focus * 1.25f);
                  quality.Set<float>(dof, dof_manual.far_end, focus * 2.f);
                  quality.Set<float>(dof, dof_manual.far_radius, dof_far_override.load(std::memory_order_relaxed));
                  quality.Set<float>(dof, dof_manual.temporal, 0.5f);
                  quality.Set<float>(native_settings, dof_manual.max_radius, 10.f);

                  quality.Set<uint8_t>(dof, dof_manual.debug, 0);
                  if (!(dof_resolution_ready && dof_scale != 0.f)) quality.Set<float>(dof, dof_scale_offset, 0.5f);

                  quality.Set<uint8_t>(dof, dof_manual.enable,
                                       dof_near_override.load(std::memory_order_relaxed) > 0.f
                                           || dof_far_override.load(std::memory_order_relaxed) > 0.f);
                }
              }
            }
          }
        }
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        quality.Restore();
        quality_access_failed.store(true, std::memory_order_relaxed);
        Log(reshade::log::level::error, "E_E_FPV: DoF parameter access failed; quality overrides disabled until restart.");
      }
    }
    render_path(pointer, render_path_params, before_culling_params, camera, render_context, command);
  } __finally {
    if (!quality.Restore()) {
      quality_access_failed.store(true, std::memory_order_relaxed);
      Log(reshade::log::level::error, "E_E_FPV: DoF parameter restoration failed; quality overrides disabled until restart.");
    }
  }
}

inline bool InstallRenderPathHook() {
  if (render_path_hook_installed) return true;
  if (!ResolveApi()) return false;

  Il2CppImage graphics_image = FindImage("UnityEngine.HGGraphicsCPPModule");
  if (graphics_image == nullptr) return false;

  Il2CppClass render_params = FindGraphicsClass(
      graphics_image, "HGRenderPathParamsCPP");
  Il2CppClass gtao_settings = FindGraphicsClass(
      graphics_image, "HGGTAmbientOcclusionSettingParameters");
  Il2CppClass render_graph = FindGraphicsClass(
      graphics_image, "HGRenderGraphCPP");
  if (render_params == nullptr || gtao_settings == nullptr
      || render_graph == nullptr) {
    return false;
  }

  if (!ResolveCppFieldOffset(
          render_params,
          "gtaoSettingParameters",
          "gtao",
          &render_params_gtao_offset)
      || !ResolveCppFieldOffset(
          gtao_settings, "screenWidth", nullptr, &gtao_width_offset)
      || !ResolveCppFieldOffset(
          gtao_settings, "screenHeight", nullptr, &gtao_height_offset)
      || !ResolveCppFieldOffset(
          render_params,
          "disableFrameGenTemporarily",
          nullptr,
          &render_params_frame_generation_pause_offset)) {
    return false;
  }

  Il2CppMethod method = class_get_method_from_name(
      render_graph, "HGRenderPath_Render", 6);
  if (method == nullptr) return false;
  render_path = reinterpret_cast<RenderPath>(
      static_cast<MethodInfo*>(method)->method_pointer);
  if (render_path == nullptr) return false;

  Il2CppClass before_culling = FindGraphicsClass(graphics_image, "HGRenderPathBeforeCullingParamsCPP");
  Il2CppClass quality_settings = FindGraphicsClass(graphics_image, "HGSettingParametersCpp");
  Il2CppClass dof_settings = FindGraphicsClass(graphics_image, "HGDepthOfFieldParameters");
  if (before_culling != nullptr && quality_settings != nullptr
      && ResolveCppFieldOffset(before_culling, "settingParameters", nullptr, &before_culling_settings_offset)) {
    dof_quality_ready = ResolveCppFieldOffset(quality_settings, "depthOfFieldQuality", nullptr, &dof_quality_offset);
    const bool dof_parameters_ready = dof_settings != nullptr
                                      && ResolveCppFieldOffset(render_params, "dofParameters", nullptr, &render_params_dof_offset)
                                      && ResolveCppFieldOffset(dof_settings, "scale", nullptr, &dof_scale_offset);
    dof_resolution_ready = dof_parameters_ready
                           && ResolveCppFieldOffset(quality_settings, "depthOfFieldScaleAdjust", nullptr, &dof_scale_adjust_offset);
    dof_force_ready = dof_parameters_ready && dof_quality_ready
                      && ResolveCppFieldOffset(dof_settings, "enable", nullptr, &dof_manual.enable)
                      && ResolveCppFieldOffset(dof_settings, "debug", nullptr, &dof_manual.debug)
                      && ResolveCppFieldOffset(dof_settings, "camera", nullptr, &dof_manual.camera)
                      && ResolveCppFieldOffset(dof_settings, "usePhysicalCamera", nullptr, &dof_manual.physical)
                      && ResolveCppFieldOffset(dof_settings, "focusDistance", nullptr, &dof_manual.focus)
                      && ResolveCppFieldOffset(dof_settings, "aperture", nullptr, &dof_manual.aperture)
                      && ResolveCppFieldOffset(dof_settings, "nearFocusStart", nullptr, &dof_manual.near_start)
                      && ResolveCppFieldOffset(dof_settings, "nearFocusEnd", nullptr, &dof_manual.near_end)
                      && ResolveCppFieldOffset(dof_settings, "nearRadius", nullptr, &dof_manual.near_radius)
                      && ResolveCppFieldOffset(dof_settings, "farFocusStart", nullptr, &dof_manual.far_start)
                      && ResolveCppFieldOffset(dof_settings, "farFocusEnd", nullptr, &dof_manual.far_end)
                      && ResolveCppFieldOffset(dof_settings, "farRadius", nullptr, &dof_manual.far_radius)
                      && ResolveCppFieldOffset(dof_settings, "temporalFactor", nullptr, &dof_manual.temporal)
                      && ResolveCppFieldOffset(quality_settings, "depthOfFieldMaxRadius", nullptr, &dof_manual.max_radius);
    if (dof_force_ready) {
      ResolveICall resolve_icall = nullptr;
      dof_force_ready = ResolveExport(GetModuleHandleW(L"GameAssembly.dll"), "il2cpp_resolve_icall", &resolve_icall);
      if (dof_force_ready) {
        auto get_instance_id_offset = reinterpret_cast<int32_t (*)()>(
            resolve_icall("UnityEngine.Object::GetOffsetOfInstanceIDInCPlusPlusObject()"));
        object_with_instance_id_exists = reinterpret_cast<ObjectWithInstanceIDExists>(
            resolve_icall("UnityEngine.Object::DoesObjectWithInstanceIDExist(System.Int32)"));
        const int32_t offset = get_instance_id_offset == nullptr ? -1 : get_instance_id_offset();
        dof_force_ready = offset >= static_cast<int32_t>(sizeof(void*)) && offset < 0x100
                          && offset % alignof(int32_t) == 0 && object_with_instance_id_exists != nullptr;
        if (dof_force_ready) native_camera_instance_id_offset = static_cast<size_t>(offset);
      }
      if (!dof_force_ready) {
        Log(reshade::log::level::warning, "E_E_FPV: Force DoF unavailable; native camera validation could not be resolved.");
      }
    }
  }

  if (native_hooks::Begin() != NO_ERROR) return false;
  if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR
      || DetourAttach(&render_path, HookedRenderPath) != NO_ERROR) {
    DetourTransactionAbort();
    render_path = nullptr;
    return false;
  }
  if (DetourTransactionCommit() != NO_ERROR) {
    render_path = nullptr;
    return false;
  }

  render_path_hook_installed = true;
  gtao_ready = true;
  if (!dof_quality_ready || !dof_resolution_ready || !dof_force_ready) {
    Log(reshade::log::level::warning,
        "E_E_FPV: some DoF quality fields are unavailable; their overrides will be skipped.");
  }
  return true;
}

inline void UpdateFps(bool enabled) {
  constexpr int kUnlockedFps = 9999;
  if (!fps_ready || !AttachThread()) return;

  if (enabled && !fps_applied) {
    if (!ReadInt(get_target_frame_rate, &original_target_frame_rate)
        || !ReadInt(get_vsync_count, &original_vsync_count)) {
      return;
    }
    fps_applied = true;
    last_applied_fps = 0;
  }

  if (enabled) {
    if ((kUnlockedFps != last_applied_fps || present_count % 60 == 0)
        && WriteInt(set_vsync_count, 0)
        && WriteInt(set_target_frame_rate, kUnlockedFps)) {
      last_applied_fps = kUnlockedFps;
    }
  } else if (fps_applied) {
    WriteInt(set_target_frame_rate, original_target_frame_rate);
    WriteInt(set_vsync_count, original_vsync_count);
    fps_applied = false;
    last_applied_fps = 0;
  }
}
}

inline bool TryInstallStreamlineHook(reshade::api::device* device) {
  if (detail::shutting_down.load(std::memory_order_relaxed)) return false;
  return detail::InstallStreamlineHook(device);
}

inline bool TryInstallSsrResolutionHook() {
  using namespace detail;
  if (shutting_down.load(std::memory_order_relaxed)) return false;
  return InstallSsrResolutionHook();
}

inline float GetActiveFpsLimit(bool foreground) {
  if (!foreground) return background_fps_limit;
  if (detail::frame_generation_presenting.load(std::memory_order_relaxed)
      && !detail::frame_generation_paused.load(std::memory_order_relaxed)) {
    return frame_generation_fps_limit;
  }
  return fps_limit;
}

inline void OnPresent(reshade::api::device* device) {
  using namespace detail;
  if (shutting_down.load(std::memory_order_relaxed)) return;

  ++present_count;
  ssr_depth::epoch.fetch_add(1, std::memory_order_relaxed);

  const bool unlock_enabled = fps_unlock >= 0.5f;
  const bool frame_generation_detection_requested =
      device != nullptr && device->get_api() == reshade::api::device_api::vulkan
      && (fps_limit > 0.f || frame_generation_fps_limit > 0.f
          || endfield::hdr_output::events_registered);
  if (unlock_enabled && !fps_ready
      && (present_count == 1 || present_count % 120 == 0)) {
    ResolveFpsMethods();
  }

  UpdateFps(unlock_enabled);

  if (frame_generation_detection_requested
      && !streamline_hook_installed
      && (present_count == 1 || present_count % 120 == 0)) {
    TryInstallStreamlineHook(device);
  }

  const bool gtao_enabled = gtao_resolution == 1.f;
  if (ssr_resolution_hook_installed && !ssr_resolution_failed.load(std::memory_order_relaxed)) {
    uint64_t previous = ssr_resolution_state.load(std::memory_order_relaxed);
    const uint64_t requested = (ssr_resolution == 1.f ? 1u : 0u)
                               | (ssr_full_depth == 1.f ? 2u : 0u);
    while ((previous & 3) != requested) {
      if (ssr_resolution_state.compare_exchange_weak(
              previous, ((previous + 4) & ~uint64_t{3}) | requested,
              std::memory_order_relaxed)) {
        break;
      }
    }
  }
  const bool render_path_hook_requested =
      gtao_enabled || frame_generation_detection_requested
      || dof_resolution == 1.f || dof_resolution == 2.f || force_dof >= 0.5f;
  if (render_path_hook_requested && !render_path_hook_installed
      && (present_count == 1 || present_count % 120 == 0)) {
    InstallRenderPathHook();
  }
  gtao_resolution_multiplier.store(
      gtao_ready && gtao_enabled ? 2u : 1u, std::memory_order_relaxed);
  dof_resolution_override.store(dof_resolution == 1.f || dof_resolution == 2.f ? dof_resolution : 0.f, std::memory_order_relaxed);
  dof_focus_override.store(dof_focus_distance >= 0.5f && dof_focus_distance <= 200.f ? dof_focus_distance : 10.f, std::memory_order_relaxed);
  dof_near_override.store(dof_near_blur >= 0.f && dof_near_blur <= 10.f ? dof_near_blur : 3.f, std::memory_order_relaxed);
  dof_far_override.store(dof_far_blur >= 0.f && dof_far_blur <= 10.f ? dof_far_blur : 5.f, std::memory_order_relaxed);
  dof_force_override.store(force_dof >= 0.5f, std::memory_order_relaxed);
}

inline void Shutdown() {
  using namespace detail;
  shutting_down.store(true, std::memory_order_relaxed);
  gtao_resolution_multiplier.store(1, std::memory_order_relaxed);
  if (fps_applied && fps_ready && AttachThread()) {
    WriteInt(set_target_frame_rate, original_target_frame_rate);
    WriteInt(set_vsync_count, original_vsync_count);
    fps_applied = false;
  }

  if (!ssr_resolution_hook_installed
      && !render_path_hook_installed && !streamline_hook_installed) {
    return;
  }
  if (native_hooks::Begin() != NO_ERROR) return;
  if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR) {
    DetourTransactionAbort();
    return;
  }
  if (ssr_resolution_hook_installed
      && (DetourDetach(&ssr_depth::register_low, ssr_depth::HookedRegisterLow) != NO_ERROR
          || DetourDetach(&ssr_depth::register_full, ssr_depth::HookedRegisterFull) != NO_ERROR
          || DetourDetach(&ssr_depth::build_pyramid, ssr_depth::HookedBuildPyramid) != NO_ERROR
          || DetourDetach(&render_ssr, HookedRenderSsr) != NO_ERROR)) {
    DetourTransactionAbort();
    return;
  }
  if (streamline_hook_installed
      && DetourDetach(&set_frame_generation_options, HookedSetFrameGenerationOptions)
             != NO_ERROR) {
    DetourTransactionAbort();
    return;
  }
  if (hdr_hooks_installed
      && DetourDetach(&set_tags, HookedSetTags) != NO_ERROR) {
    DetourTransactionAbort();
    return;
  }
  if (hdr_hooks_installed
      && DetourDetach(&set_tags_for_frame, HookedSetTagsForFrame) != NO_ERROR) {
    DetourTransactionAbort();
    return;
  }
  if (hdr_hooks_installed
      && DetourDetach(
             &streamline_create_swapchain,
             HookedStreamlineCreateSwapchain)
             != NO_ERROR) {
    DetourTransactionAbort();
    return;
  }
  if (render_path_hook_installed
      && DetourDetach(&render_path, HookedRenderPath) != NO_ERROR) {
    DetourTransactionAbort();
    return;
  }
  if (hdr_hooks_installed && !endfield::hdr_output::DetachHooks()) {
    DetourTransactionAbort();
    return;
  }
  if (DetourTransactionCommit() == NO_ERROR) {
    ssr_resolution_hook_installed = false;
    render_ssr = nullptr;
    reset_ssr = nullptr;
    ssr_depth::build_pyramid = nullptr;
    ssr_depth::register_full = nullptr;
    ssr_depth::register_low = nullptr;
    ssr_depth::add_read = nullptr;
    render_path_hook_installed = false;
    gtao_ready = false;
    streamline_hook_installed = false;
    hdr_hooks_installed = false;
    set_frame_generation_options = nullptr;
    set_tags = nullptr;
    set_tags_for_frame = nullptr;
    streamline_create_swapchain = nullptr;
  }
}
}
