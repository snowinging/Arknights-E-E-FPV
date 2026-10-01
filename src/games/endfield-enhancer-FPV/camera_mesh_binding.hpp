struct Binding {
  uint32_t renderer = 0, source = 0, filtered = 0, proxy = 0, model = 0;
  bool applied = false, failed = false;
  uint32_t bones = 0;
  bool owns_offscreen_update = false;
  bool has_visible_triangles = false;
};
inline std::vector<Binding> bindings;
struct Seen {
  uint32_t renderer = 0, mesh = 0, bones = 0, model = 0;
  bool owns_offscreen_update = false;
};
inline std::vector<Seen> seen;
inline uint32_t candidate_renderer = 0, candidate_model = 0, candidate_bones = 0;
inline void* (*get_bones_array)(void*) = nullptr;
inline bool PaletteMatches(void* renderer, uint32_t saved) {
  if (!renderer || !saved || !get_bones_array) return false;
  void *actual = get_bones_array(renderer), *expected = gc_target(saved);
  if (!actual || !expected || array_length(actual) != array_length(expected)) return false;
  return std::memcmp(static_cast<uint8_t*>(actual) + 0x20, static_cast<uint8_t*>(expected) + 0x20, array_length(expected) * sizeof(void*)) == 0;
}
inline bool (*get_update_offscreen)(void*) = nullptr;
inline void (*set_update_offscreen)(void*, bool) = nullptr;
inline void* (*get_shared)(void*) = nullptr;
inline void (*set_shared)(void*, void*) = nullptr;
inline void* (*get_proxy)(void*) = nullptr;
inline void (*set_proxy)(void*, void*) = nullptr;
inline bool PrepareBinding() {
  get_update_offscreen = reinterpret_cast<decltype(get_update_offscreen)>(resolve("UnityEngine.SkinnedMeshRenderer::get_updateWhenOffscreen"));
  set_update_offscreen = reinterpret_cast<decltype(set_update_offscreen)>(resolve("UnityEngine.SkinnedMeshRenderer::set_updateWhenOffscreen"));
  get_shared = reinterpret_cast<decltype(get_shared)>(resolve("UnityEngine.SkinnedMeshRenderer::get_sharedMesh"));
  set_shared = reinterpret_cast<decltype(set_shared)>(resolve("UnityEngine.SkinnedMeshRenderer::set_sharedMesh"));
  get_proxy = reinterpret_cast<decltype(get_proxy)>(resolve("UnityEngine.Renderer::get_shadowProxyMesh"));
  set_proxy = reinterpret_cast<decltype(set_proxy)>(resolve("UnityEngine.Renderer::set_shadowProxyMesh"));
  get_bones_array = reinterpret_cast<decltype(get_bones_array)>(resolve("UnityEngine.SkinnedMeshRenderer::get_bones"));
  return get_shared && set_shared && get_proxy && set_proxy && get_bones_array && get_update_offscreen && set_update_offscreen;
}
inline bool NativeAlive(uint32_t root) {
  void* object = root ? gc_target(root) : nullptr;
  return object && Read<void*>(object, 0x10) != nullptr;
}

inline void UpdateSkinningPolicy(uint32_t renderer, bool active, bool* owned) {
  if (!NativeAlive(renderer)) {
    *owned = false;
    return;
  }
  void* r = gc_target(renderer);
  if (active) {
    if (!get_update_offscreen(r)) {
      *owned = true;
      set_update_offscreen(r, true);
      if (!get_update_offscreen(r)) throw std::runtime_error("Skinning update setting rejected");
    }
  } else if (*owned) {
    if (get_update_offscreen(r)) set_update_offscreen(r, false);
    if (get_update_offscreen(r)) throw std::runtime_error("Skinning update restoration failed");
    *owned = false;
  }
}
inline void RestoreBinding(Binding* state) {
  auto& binding = *state;
  if (!NativeAlive(binding.renderer)) {
    binding.applied = false;
    binding.owns_offscreen_update = false;
    return;
  }
  UpdateSkinningPolicy(binding.renderer, false, &binding.owns_offscreen_update);
  if (!NativeAlive(binding.source) || !NativeAlive(binding.filtered)) {
    if (NativeAlive(binding.filtered) && get_shared(gc_target(binding.renderer)) == gc_target(binding.filtered))
      throw std::runtime_error("Original mesh unavailable; retaining assigned copy");
    binding.applied = false;
    binding.failed = true;
    return;
  }
  void* r = gc_target(binding.renderer);

  if (get_shared(r) == gc_target(binding.filtered)) set_shared(r, gc_target(binding.source));
  void* shadow = binding.proxy ? gc_target(binding.proxy) : gc_target(binding.source);
  const bool owned_shadow = get_proxy(r) == shadow;
  if (owned_shadow && get_proxy(r) != (binding.proxy ? gc_target(binding.proxy) : nullptr)) set_proxy(r, binding.proxy ? gc_target(binding.proxy) : nullptr);
  if (get_shared(r) == gc_target(binding.filtered)) throw std::runtime_error("Visible mesh restoration failed");
  if (owned_shadow && get_proxy(r) != (binding.proxy ? gc_target(binding.proxy) : nullptr)) throw std::runtime_error("Shadow mesh restoration did not match");
  binding.applied = false;
}
inline void UpdateBinding(bool requested) {
  for (auto& item : seen) {
    try {
      const bool active = requested && model_root && gc_target(model_root) == gc_target(item.model)
                          && NativeAlive(item.renderer) && NativeAlive(item.mesh)
                          && get_shared(gc_target(item.renderer)) == gc_target(item.mesh)
                          && PaletteMatches(gc_target(item.renderer), item.bones);
      UpdateSkinningPolicy(item.renderer, active, &item.owns_offscreen_update);
    } catch (...) {
      try {
        UpdateSkinningPolicy(item.renderer, false, &item.owns_offscreen_update);
      } catch (...) {
      }
    }
  }
  for (auto& binding : bindings) {
    if (!NativeAlive(binding.renderer) || !NativeAlive(binding.source) || !NativeAlive(binding.filtered) || binding.failed) continue;
    const bool active = requested && model_root && gc_target(model_root) == gc_target(binding.model);
    try {
      void* r = gc_target(binding.renderer);

      if (active && !PaletteMatches(r, binding.bones))
        throw std::runtime_error("Renderer bone palette changed; stale copy retired");
      if (active && binding.applied) {
        const auto visible = get_shared(r);
        const auto proxy = get_proxy(r);
        const auto expected_shadow = binding.proxy ? gc_target(binding.proxy) : gc_target(binding.source);
        if (visible == gc_target(binding.filtered) && proxy == expected_shadow) continue;

        binding.applied = false;
      }
      if (!active && !binding.applied) continue;
      if (active) {
        const auto visible = get_shared(r), proxy = get_proxy(r);
        const auto original_proxy = binding.proxy ? gc_target(binding.proxy) : nullptr;
        const auto shadow = binding.proxy ? gc_target(binding.proxy) : gc_target(binding.source);

        if ((visible != gc_target(binding.source) && visible != gc_target(binding.filtered)) || (proxy != original_proxy && proxy != shadow))
          throw std::runtime_error("Renderer changed since capture; assignment refused");
        if (proxy != shadow) set_proxy(r, shadow);
        if (visible != gc_target(binding.filtered)) set_shared(r, gc_target(binding.filtered));
        if (get_shared(r) != gc_target(binding.filtered) || get_proxy(r) != shadow)
          throw std::runtime_error("Renderer mesh assignment did not match");
        UpdateSkinningPolicy(binding.renderer, binding.has_visible_triangles, &binding.owns_offscreen_update);
        binding.applied = true;

      } else {
        RestoreBinding(&binding);
      }
    } catch (...) {
      try {
        RestoreBinding(&binding);
      } catch (...) {
      }
      binding.failed = true;
    }
  }
}
