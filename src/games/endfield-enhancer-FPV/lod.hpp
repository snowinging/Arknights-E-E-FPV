#pragma once

#include "./enhancer.hpp"

namespace endfield::lod {
inline float force_highest_geometry_lod = 0.f;

namespace detail {
using namespace endfield::enhancer::detail;

using FieldStaticGetValue = void (*)(void*, void*);
using ObjectGetClass = Il2CppClass (*)(void*);

inline FieldStaticGetValue field_static_get_value = nullptr;
inline ObjectGetClass object_get_class = nullptr;

inline Il2CppMethod get_maximum_lod_level = nullptr;
inline Il2CppMethod set_maximum_lod_level = nullptr;
inline void* current_pipeline_field = nullptr;

inline bool api_ready = false;
inline bool force_lod_dirty = false;
inline bool maximum_lod_captured = false;
inline bool force_lod0_applied = false;
inline int original_maximum_lod = 0;
inline float observed_force_highest_geometry_lod = 0.f;
inline void* pipeline_object = nullptr;

inline bool InvokeVoid(Il2CppMethod method, void* object) {
  if (method == nullptr) return false;
  void* exception = nullptr;
  runtime_invoke(method, object, nullptr, &exception);
  return exception == nullptr;
}

inline bool ResolveApi() {
  if (api_ready) return AttachThread();
  if (!endfield::enhancer::detail::ResolveApi()) return false;

  HMODULE game_assembly = GetModuleHandleW(L"GameAssembly.dll");
  if (game_assembly == nullptr
      || !ResolveExport(
          game_assembly,
          "il2cpp_field_static_get_value",
          &field_static_get_value)
      || !ResolveExport(
          game_assembly, "il2cpp_object_get_class", &object_get_class)) {
    return false;
  }

  Il2CppImage core = FindImage("UnityEngine.CoreModule");
  if (core == nullptr) return false;

  get_maximum_lod_level = FindMethod(
      core, "UnityEngine", "QualitySettings", "get_maximumLODLevel", 0);
  set_maximum_lod_level = FindMethod(
      core, "UnityEngine", "QualitySettings", "set_maximumLODLevel", 1);

  Il2CppClass pipeline_manager = class_from_name(
      core, "UnityEngine.Rendering", "RenderPipelineManager");
  current_pipeline_field =
      pipeline_manager == nullptr
          ? nullptr
          : class_get_field_from_name(pipeline_manager, "s_currentPipeline");

  api_ready = get_maximum_lod_level != nullptr
              && set_maximum_lod_level != nullptr
              && current_pipeline_field != nullptr;
  return api_ready && AttachThread();
}

inline bool ApplyForceHighestGeometryLod() {
  const bool enabled = force_highest_geometry_lod >= 0.5f;

  if (enabled && !maximum_lod_captured) {
    if (!ReadInt(get_maximum_lod_level, &original_maximum_lod)) return false;
    maximum_lod_captured = true;
  }
  if (maximum_lod_captured
      && !WriteInt(
          set_maximum_lod_level,
          enabled ? 0 : original_maximum_lod)) {
    return false;
  }

  if (current_pipeline_field == nullptr) return false;
  void* current_pipeline = nullptr;
  field_static_get_value(current_pipeline_field, &current_pipeline);
  if (current_pipeline == nullptr) return false;
  if (pipeline_object != current_pipeline) {
    pipeline_object = current_pipeline;
    force_lod0_applied = false;
  }

  Il2CppClass pipeline_class = object_get_class(current_pipeline);
  Il2CppMethod toggle_force_lod0 =
      pipeline_class == nullptr
          ? nullptr
          : class_get_method_from_name(
                pipeline_class,
                enabled ? "EnableForceLOD0" : "DisableForceLOD0",
                0);
  if (toggle_force_lod0 == nullptr) return false;

  if (enabled) {
    if (!force_lod0_applied
        && !InvokeVoid(toggle_force_lod0, current_pipeline)) {
      return false;
    }
    force_lod0_applied = true;
  } else if (force_lod0_applied) {
    if (!InvokeVoid(toggle_force_lod0, current_pipeline)) return false;
    force_lod0_applied = false;
  }

  if (!enabled) maximum_lod_captured = false;
  return true;
}
}

inline void OnRendererReset() {
  using namespace detail;
  pipeline_object = nullptr;
  force_lod0_applied = false;
  if (force_highest_geometry_lod >= 0.5f) force_lod_dirty = true;
}

inline void OnPresent() {
  using namespace detail;

  if (observed_force_highest_geometry_lod
      != force_highest_geometry_lod) {
    observed_force_highest_geometry_lod = force_highest_geometry_lod;
    force_lod_dirty = true;
  }
  if (!force_lod_dirty || !ResolveApi()) return;
  if (ApplyForceHighestGeometryLod()) force_lod_dirty = false;
}

inline void Shutdown() {
  using namespace detail;

  force_highest_geometry_lod = 0.f;
  if (api_ready && AttachThread()) ApplyForceHighestGeometryLod();
}
}
