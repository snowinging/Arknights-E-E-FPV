#pragma once
#include "./npc_distance.hpp"
#include "./native_hooks.hpp"
#include <cstdio>

namespace endfield::world_distance {
inline float enemies_enabled = 0.f, enemies_multiplier = 2.f;
inline float interactive_enabled = 0.f, interactive_multiplier = 2.f;
inline bool unavailable = false;
namespace detail {
using namespace enhancer::detail;
using Tick = void (*)(void*, float, void*);
using RefreshGrid = void (*)(void*, void*);
inline Tick tick = nullptr;
inline RefreshGrid refresh_grid = nullptr;
inline std::atomic<float> requested_enemies{0.f}, requested_interactive{0.f};
inline std::atomic_bool failed{false};
inline bool installed = false;
inline void (*static_get)(void*, void*) = nullptr;
inline void* (*field_type)(void*) = nullptr;
inline int (*type_kind)(void*) = nullptr;
inline uint32_t (*pin)(void*, bool) = nullptr;
inline void (*unpin)(uint32_t) = nullptr;
inline void* (*pin_target)(uint32_t) = nullptr;
inline std::array<void*, 3> radius_fields{};
inline uint32_t manager_handle = 0;
inline void* manager_object = nullptr;
struct Binding {
  npc_distance::detail::Override<float> value;
  void* object = nullptr;
  uint32_t handle = 0;
  void Clear() {
    value.Update(false, [](float*) { return true; });
    value = {};
    if (handle) unpin(handle);
    handle = 0;
    object = nullptr;
  }
  bool Bind(void* next, size_t offset) {
    if (object == next) return next != nullptr;
    Clear();
    if (!next) return false;
    handle = pin(next, true);
    if (!handle) return false;
    object = pin_target(handle);
    if (!object) {
      Clear();
      return false;
    }
    value.address = reinterpret_cast<LONG*>(static_cast<uint8_t*>(object) + offset);
    return true;
  }
};
inline std::array<std::array<Binding, 3>, 2> entities;

inline bool Scale(float* value, float multiplier, bool squared) {
  if (!std::isfinite(*value) || *value <= 0.f || *value > (squared ? 100000000.f : 10000.f)) return false;
  multiplier = std::isfinite(multiplier) ? std::clamp(multiplier, 1.f, 10.f) : 1.f;
  *value *= squared ? multiplier * multiplier : multiplier;
  return std::isfinite(*value);
}
inline bool ApplyEntities(void* manager, float enemy_multiplier, float interactive_multiplier) {
  const std::array<float, 2> multipliers{enemy_multiplier, interactive_multiplier};
  if (enemy_multiplier == 0.f && interactive_multiplier == 0.f
      && std::all_of(entities.begin(), entities.end(), [](const auto& bindings) {
           return std::all_of(bindings.begin(), bindings.end(), [](const Binding& b) { return !b.value.address; });
         })) return true;
  bool changed = manager != manager_object;
  if (changed) {
    if (manager_handle) unpin(manager_handle);
    manager_handle = pin(manager, true);
    manager_object = manager_handle ? pin_target(manager_handle) : nullptr;
    if (!manager_object) return false;
  }
  for (size_t kind = 0; kind < entities.size(); ++kind) {
    const float multiplier = multipliers[kind];
    for (size_t i = 0; i < radius_fields.size(); ++i) {
      auto& binding = entities[kind][i];
      if (multiplier == 0.f) {
        changed |= binding.value.owned;
        binding.Clear();
        continue;
      }
      void* array = nullptr;
      static_get(radius_fields[i], &array);
      if (!array || *reinterpret_cast<const uintptr_t*>(static_cast<uint8_t*>(array) + 0x18) != 5) return false;
      changed |= binding.object != array;

      if (!binding.Bind(array, 0x24 + kind * sizeof(float))) return false;
      const auto before = binding.value.Exchange(0, 0);
      if (!binding.value.Update(true, [multiplier, i](float* value) { return Scale(value, multiplier, i != 0); })) return false;
      changed |= before != binding.value.Exchange(0, 0);
    }
  }
  if (changed) refresh_grid(manager, nullptr);
  return true;
}
inline void HookedTick(void* self, float delta, void* method) {
  if (!failed.load(std::memory_order_relaxed)) {
    bool ok = false;
    __try {
      ok = ApplyEntities(self, requested_enemies.load(std::memory_order_relaxed),
                         requested_interactive.load(std::memory_order_relaxed));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      ok = false;
    }
    if (!ok) {
      __try {
        ApplyEntities(self, 0.f, 0.f);
      } __except (EXCEPTION_EXECUTE_HANDLER) {
      }
      failed = true;
      Log(reshade::log::level::warning, "E_E_FPV: entity distance override refused invalid runtime data; rollback attempted.");
    }
  }
  tick(self, delta, method);
}
inline bool UpdateHooks(bool attach) {
  return native_hooks::Update(attach ? "Entity distance install" : "Entity distance removal", [attach]() -> LONG {
    return attach ? DetourAttach(&tick, HookedTick) : DetourDetach(&tick, HookedTick);
  });
}
inline const char* resolve_stage = "not started";
inline bool Resolve() {
  resolve_stage = "build identity";
  if (!npc_distance::detail::SupportedBuild()) return false;
  resolve_stage = "IL2CPP exports";
  HMODULE module = GetModuleHandleW(L"GameAssembly.dll");
  void (*class_init)(void*) = nullptr;
  int (*field_flags)(void*) = nullptr;
  if (!ResolveExport(module, "il2cpp_field_static_get_value", &static_get)
      || !ResolveExport(module, "il2cpp_field_get_type", &field_type)
      || !ResolveExport(module, "il2cpp_type_get_type", &type_kind)
      || !ResolveExport(module, "il2cpp_field_get_flags", &field_flags)
      || !ResolveExport(module, "il2cpp_runtime_class_init", &class_init)
      || !ResolveExport(module, "il2cpp_gchandle_new", &pin)
      || !ResolveExport(module, "il2cpp_gchandle_free", &unpin)
      || !ResolveExport(module, "il2cpp_gchandle_get_target", &pin_target)) return false;
  resolve_stage = "assembly lookup";
  auto gameplay = FindImage("Gameplay.Beyond.dll");
  if (!gameplay) return false;
  resolve_stage = "class lookup";
  auto manager = class_from_name(gameplay, "Beyond.Gameplay.Core", "EntityManager");
  auto types = class_from_name(gameplay, "Beyond.Gameplay.Core", "EntityDataType");
  if (!manager || !types) return false;
  constexpr const char* arrays[] = {"s_EntityLoadRadius", "s_EntityLoadRadiusSq", "s_EntityUnloadRadiusSq"};
  for (size_t i = 0; i < radius_fields.size(); ++i) {
    resolve_stage = arrays[i];
    radius_fields[i] = class_get_field_from_name(manager, arrays[i]);
    if (!radius_fields[i] || !(field_flags(radius_fields[i]) & 0x10) || field_get_offset(radius_fields[i]) != 0x20 + i * 0x10
        || type_kind(field_type(radius_fields[i])) != 0x1d) return false;
  }
  constexpr const char* type_names[] = {"Enemy", "Interactive"};
  for (size_t kind = 0; kind < entities.size(); ++kind) {
    resolve_stage = type_names[kind];
    void* field = class_get_field_from_name(types, type_names[kind]);
    int index = -1;
    if (!field) return false;
    static_get(field, &index);
    if (index != static_cast<int>(kind + 1)) return false;
  }
  resolve_stage = "class initialization";
  class_init(manager);
  const auto* base = reinterpret_cast<const uint8_t*>(module);
  constexpr uint8_t tick_prefix[] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b, 0xd9, 0x0f, 0x29, 0x74, 0x24, 0x20, 0x48, 0x8b, 0x0d, 0xcb, 0x0a, 0x42, 0x09, 0x0f, 0x28, 0xf1, 0x83, 0xb9, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x74, 0x7c};
  constexpr uint8_t grid_prefix[] = {0x40, 0x57, 0x48, 0x83, 0xec, 0x30, 0x80, 0x3d, 0x90, 0x8a, 0xde, 0x09, 0x00, 0x48, 0x8b, 0xf9, 0x75, 0x13, 0x48, 0x8d, 0x0d, 0xdf, 0x97, 0xf8, 0x08, 0xe8, 0xf2, 0x56, 0xf8, 0xfb, 0xc6, 0x05, 0x78, 0x8a, 0xde, 0x09, 0x01};
  if (std::memcmp(base + 0x3BD2AB0, tick_prefix, sizeof(tick_prefix))
      || std::memcmp(base + 0x40BBB50, grid_prefix, sizeof(grid_prefix))) return false;
  tick = reinterpret_cast<Tick>(const_cast<uint8_t*>(base + 0x3BD2AB0));
  refresh_grid = reinterpret_cast<RefreshGrid>(const_cast<uint8_t*>(base + 0x40BBB50));
  return true;
}
}
inline void OnPresent() {
  using namespace detail;
  if (failed) unavailable = true;
  if (unavailable) return;
  requested_enemies = enemies_enabled >= 0.5f ? enemies_multiplier : 0.f;
  requested_interactive = interactive_enabled >= 0.5f ? interactive_multiplier : 0.f;
  if (!installed) {
    if (enemies_enabled < 0.5f && interactive_enabled < 0.5f) return;
    if (!enhancer::detail::ResolveApi()) return;
    bool ok = false;
    __try {
      ok = Resolve();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      ok = false;
    }
    if (ok) {
      resolve_stage = "hook transaction";
      ok = UpdateHooks(true);
    }
    if (!ok) {
      unavailable = true;
      char message[256];
      std::snprintf(message, sizeof(message), "E_E_FPV: entity distance unavailable at %s.", resolve_stage);
      Log(reshade::log::level::warning, message);
      return;
    }
    installed = true;
  }
}

inline void Shutdown() {
  using namespace detail;
  if (!installed) return;
  requested_enemies = 0;
  requested_interactive = 0;
  if (!UpdateHooks(false)) {
    Log(reshade::log::level::error, "E_E_FPV: entity distance hook removal failed.");
    return;
  }
  __try {
    if (manager_object) ApplyEntities(manager_object, 0.f, 0.f);
    if (manager_handle) unpin(manager_handle);
    manager_handle = 0;
    manager_object = nullptr;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  installed = false;
}
}
