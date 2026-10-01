#pragma once

#include <Windows.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include "./enhancer.hpp"
#include "./game_build.hpp"

namespace endfield::npc_distance {
inline float regular_enabled = 0.f, ambient_enabled = 0.f, limit_enabled = 0.f;
inline float regular_multiplier = 2.f, ambient_multiplier = 2.f, model_limit = 100.f;
inline constexpr float kMaxDistanceMultiplier = 10.f;
inline constexpr float kMaxModelLimit = 500.f;
inline bool unavailable = false;
namespace detail {
using namespace enhancer::detail;
struct Distances {
  float load, unload;
};
static_assert(sizeof(Distances) == 8);

template <typename T>
struct Override {
  using Bits = std::conditional_t<sizeof(T) == 1, char, std::conditional_t<sizeof(T) == 8, LONG64, LONG>>;
  Bits* address = nullptr;
  Bits original{}, replacement{};
  bool owned = false;
  Bits Exchange(Bits desired, Bits expected) {
    if constexpr (sizeof(T) == 1)
      return _InterlockedCompareExchange8(address, desired, expected);
    else if constexpr (sizeof(T) == 8)
      return InterlockedCompareExchange64(address, desired, expected);
    else
      return InterlockedCompareExchange(address, desired, expected);
  }
  template <typename Transform>
  bool Update(bool enabled, Transform transform) {
    if (!address) return !enabled;
    const Bits current = Exchange(0, 0);
    if (owned && current != replacement) owned = false;
    if (!enabled) {
      if (owned) Exchange(original, replacement);
      owned = false;
      return true;
    }
    const T baseline = std::bit_cast<T>(owned ? original : current);
    T desired = baseline;
    if (!transform(&desired)) {
      if (owned) Exchange(original, replacement);
      owned = false;
      return false;
    }
    const Bits next = std::bit_cast<Bits>(desired);
    if (next == current) return true;
    if (Exchange(next, current) != current) return true;
    if (!owned) original = current;
    replacement = next;
    owned = true;
    return true;
  }
};
inline Override<Distances> regular, ambient, ambient_cpu, ambient_gpu;
inline Override<float> ambient_search;
inline Override<int> limit, ambient_limit;
inline bool ready = false;

inline bool ScaleDistances(Distances* value, float multiplier) {
  if (!std::isfinite(value->load) || !std::isfinite(value->unload)
      || value->load <= 0.f || value->unload <= value->load || value->unload > 10000.f) return false;
  multiplier = std::isfinite(multiplier) ? std::clamp(multiplier, 1.f, kMaxDistanceMultiplier) : 1.f;
  value->load *= multiplier;
  value->unload *= multiplier;
  return true;
}
inline bool ScaleRadius(float* value, float multiplier) {
  if (!std::isfinite(*value) || *value <= 0.f || *value > 10000.f) return false;
  *value *= std::isfinite(multiplier) ? std::clamp(multiplier, 1.f, kMaxDistanceMultiplier) : 1.f;
  return true;
}
inline bool SetLimit(int* value, float requested) {
  if (*value <= 0 || *value > 10000) return false;
  *value = std::isfinite(requested) ? static_cast<int>(std::clamp(requested, 50.f, kMaxModelLimit)) : *value;
  return true;
}

inline bool SupportedBuild() {
  return game_build::IsSupportedGameAssembly(GetModuleHandleW(L"GameAssembly.dll"));
}

inline bool Resolve() {
  if (!SupportedBuild()) return false;
  auto image = FindImage("Gameplay.Beyond.dll");
  auto core = FindImage("UnityEngine.CoreModule");
  if (!image || !core) return false;
  auto type = class_from_name(image, "Beyond.NPC.Lod", "NPCCrowdLODSetting");
  auto vector_type = class_from_name(core, "UnityEngine", "Vector2");
  if (!type || !vector_type) return false;
  void* (*field_type)(void*) = nullptr;
  int (*type_kind)(void*) = nullptr;
  int (*field_flags)(void*) = nullptr;
  void* (*type_class)(void*) = nullptr;
  void* (*static_data)(void*) = nullptr;
  void (*class_init)(void*) = nullptr;
  HMODULE module = GetModuleHandleW(L"GameAssembly.dll");
  if (!ResolveExport(module, "il2cpp_field_get_type", &field_type)
      || !ResolveExport(module, "il2cpp_type_get_type", &type_kind)
      || !ResolveExport(module, "il2cpp_field_get_flags", &field_flags)
      || !ResolveExport(module, "il2cpp_class_from_il2cpp_type", &type_class)
      || !ResolveExport(module, "il2cpp_class_get_static_field_data", &static_data)
      || !ResolveExport(module, "il2cpp_runtime_class_init", &class_init)) return false;
  constexpr const char* names[] = {"s_visibleModelDistance", "s_visibileAtmosphericModelDistance", "s_maxNPCRenderNum"};
  constexpr size_t offsets[] = {0, 8, 0x1c};
  for (size_t i = 0; i < 3; ++i) {
    void* field = class_get_field_from_name(type, names[i]);
    if (!field || !(field_flags(field) & 0x10) || (field_flags(field) & 0x40)
        || field_get_offset(field) != offsets[i]) {
      Log(reshade::log::level::warning, names[i]);
      return false;
    }
    void* field_value_type = field_type(field);
    if (i < 2 ? type_kind(field_value_type) != 0x11 || type_class(field_value_type) != vector_type
              : type_kind(field_value_type) != 0x08) {
      Log(reshade::log::level::warning, names[i]);
      return false;
    }
  }
  class_init(type);
  auto* data = static_cast<uint8_t*>(static_data(type));
  if (!data || reinterpret_cast<uintptr_t>(data) % 8 != 0) return false;
  auto ambient_type = class_from_name(image, "Beyond.Gameplay.Core", "AtmosphereNpcAoiSetting");
  if (!ambient_type) return false;
  constexpr const char* ambient_names[] = {"s_cpuLayerInner", "s_cpuLayerOuter", "s_gpuLayerInner", "s_gpuLayerOuter", "s_searchRadius", "s_maxAtmosphereCpuNpcCount"};
  for (size_t i = 0; i < std::size(ambient_names); ++i) {
    void* field = class_get_field_from_name(ambient_type, ambient_names[i]);
    if (!field || !(field_flags(field) & 0x10) || (field_flags(field) & 0x40)
        || field_get_offset(field) != 0x10 + i * 4 || type_kind(field_type(field)) != (i < 5 ? 0x0c : 0x08)) {
      Log(reshade::log::level::warning, ambient_names[i]);
      return false;
    }
  }
  class_init(ambient_type);
  auto* ambient_data = static_cast<uint8_t*>(static_data(ambient_type));
  if (!ambient_data || reinterpret_cast<uintptr_t>(ambient_data) % 8 != 0) return false;
  ambient_cpu.address = reinterpret_cast<LONG64*>(ambient_data + 0x10);
  ambient_gpu.address = reinterpret_cast<LONG64*>(ambient_data + 0x18);
  ambient_search.address = reinterpret_cast<LONG*>(ambient_data + 0x20);
  ambient_limit.address = reinterpret_cast<LONG*>(ambient_data + 0x24);
  regular.address = reinterpret_cast<LONG64*>(data);
  ambient.address = reinterpret_cast<LONG64*>(data + 8);
  limit.address = reinterpret_cast<LONG*>(data + 0x1c);
  return true;
}
inline bool Apply(bool restore = false) {
  const bool regular_ok = regular.Update(!restore && regular_enabled >= 0.5f,
                                         [](Distances* value) { return ScaleDistances(value, regular_multiplier); });
  const bool ambient_ok = ambient.Update(!restore && ambient_enabled >= 0.5f,
                                         [](Distances* value) { return ScaleDistances(value, ambient_multiplier); });
  const bool limit_ok = limit.Update(!restore && limit_enabled >= 0.5f,
                                     [](int* value) { return SetLimit(value, model_limit); });

  const bool ambient_limit_ok = ambient_limit.Update(!restore && limit_enabled >= 0.5f,
                                                     [](int* value) { return SetLimit(value, model_limit); });
  const bool cpu_ok = ambient_cpu.Update(!restore && ambient_enabled >= 0.5f,
                                         [](Distances* value) { return ScaleDistances(value, ambient_multiplier); });
  const bool gpu_ok = ambient_gpu.Update(!restore && ambient_enabled >= 0.5f,
                                         [](Distances* value) { return ScaleDistances(value, ambient_multiplier); });
  const bool search_ok = ambient_search.Update(!restore && ambient_enabled >= 0.5f,
                                               [](float* value) { return ScaleRadius(value, ambient_multiplier); });
  return regular_ok && ambient_ok && limit_ok && ambient_limit_ok && cpu_ok && gpu_ok && search_ok;
}
}

inline void OnPresent() {
  using namespace detail;
  if (unavailable) return;
  if (!ready) {
    if (regular_enabled < 0.5f && ambient_enabled < 0.5f && limit_enabled < 0.5f) return;
    if (!enhancer::detail::ResolveApi()) return;
    __try {
      ready = Resolve();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      ready = false;
    }
    if (!ready) {
      unavailable = true;
      Log(reshade::log::level::warning, "E_E_FPV: NPC distance controls refused: unsupported build or static field layout.");
      return;
    }
  }
  bool ok = false;
  __try {
    ok = Apply();
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    ok = false;
  }
  if (!ok) {
    __try {
      Apply(true);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    unavailable = true;
    Log(reshade::log::level::warning, "E_E_FPV: NPC controls disabled after invalid values or memory access; rollback attempted.");
    return;
  }
}
inline void Shutdown() {
  if (!detail::ready) return;
  __try {
    detail::Apply(true);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}
}
