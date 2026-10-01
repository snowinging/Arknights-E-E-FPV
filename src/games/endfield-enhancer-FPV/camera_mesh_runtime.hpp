namespace mesh_runtime {
struct Pending {
  uint32_t buffer = 0, mesh = 0;
  int bytes = 0;
  std::string file;
};
inline std::vector<Pending> pending;
inline bool started = false, reset_failed = false, fill_holes = false;
inline ULONGLONG last_scan = 0, settling_until = 0;
inline uint32_t session_model = 0;
struct Candidate {
  uint32_t mesh = 0, renderer = 0, model = 0, bones = 0;
  std::string name;
  bool dedicated_head = false, body_skin = false;
  std::vector<uint8_t> head;
  endfield::camera::mesh::NeckFrame neck;
};
inline std::vector<uint32_t> recent_models;
inline std::vector<Candidate> candidates;
inline std::unordered_map<std::string, std::vector<uint8_t>> captures;
inline uint32_t candidate_source = 0;
inline std::string candidate_name;
inline bool candidate_dedicated_head = false, candidate_body_skin = false;
inline std::vector<uint8_t> candidate_head;
inline endfield::camera::mesh::NeckFrame candidate_neck;
inline void BuildDetachedMesh();
inline uintptr_t (*array_length)(void*) = nullptr;
inline void* (*resolve)(const char*) = nullptr;
inline void (*get_data)(void*, void*, int, int, int, int) = nullptr;
inline void* (*new_array)(void*, uintptr_t) = nullptr;
inline void* byte_class = nullptr;
inline void* buffer_class = nullptr;
inline void* (*new_object)(void*) = nullptr;
inline int (*buffer_target)(void*) = nullptr;
inline size_t pointer_offset = 0;

struct ReadAlias {
  alignas(8) std::array<uint8_t, 0x58> native{};
  uint32_t root = 0;
  ~ReadAlias() {
    if (root) {
      Write<void*>(gc_target(root), pointer_offset, nullptr);
      gc_free(root);
    }
  }
};
inline Il2CppMethod dispose = nullptr;
#include "./camera_mesh_clone.hpp"
#include "./camera_mesh_binding.hpp"
inline bool ResetTarget(bool keep_prepared = false) {
  try {
    for (auto& b : bindings) RestoreBinding(&b);
    for (auto& item : seen) UpdateSkinningPolicy(item.renderer, false, &item.owns_offscreen_update);
    auto destroy = resolve ? reinterpret_cast<void (*)(void*, float)>(resolve("UnityEngine.Object::Destroy")) : nullptr;
    if (!keep_prepared)
      for (auto& b : bindings) {
        if (destroy && NativeAlive(b.filtered)) destroy(gc_target(b.filtered), 0.f);
        for (auto root : {b.renderer, b.source, b.filtered, b.proxy, b.model, b.bones})
          if (root) gc_free(root);
        b = {};
      }
    if (!keep_prepared) bindings.clear();
    for (auto& item : pending) {
      Invoke(dispose, gc_target(item.buffer));
      gc_free(item.buffer);
      gc_free(item.mesh);
    }
    pending.clear();
    for (auto& item : candidates)
      for (auto root : {item.mesh, item.renderer, item.model, item.bones})
        if (root) gc_free(root);
    if (!keep_prepared) {
      for (auto& item : seen)
        for (auto root : {item.renderer, item.mesh, item.bones, item.model})
          if (root) gc_free(root);
      seen.clear();
      for (auto root : recent_models) gc_free(root);
      recent_models.clear();
    }
    last_scan = 0;
    settling_until = GetTickCount64() + 1500;
    candidates.clear();
    captures.clear();
    if (session_model) gc_free(session_model);
    session_model = 0;
    started = false;
    return true;
  } catch (...) {
    reset_failed = true;
    return false;
  }
}
inline void PollOne() {
  if (candidates.empty()) return;

  const auto prefix = candidates.front().name + "-";
  const auto next = std::find_if(pending.begin(), pending.end(), [&](const Pending& item) { return item.file.starts_with(prefix); });
  if (next == pending.end()) {
    auto item = std::move(candidates.front());
    candidates.erase(candidates.begin());
    candidate_source = item.mesh;
    candidate_renderer = item.renderer;
    candidate_model = item.model;
    candidate_bones = item.bones;
    candidate_body_skin = item.body_skin;
    candidate_dedicated_head = item.dedicated_head;
    candidate_name = std::move(item.name);
    candidate_head = std::move(item.head);
    candidate_neck = item.neck;
    BuildDetachedMesh();
    if (candidates.empty()) captures.clear();
    return;
  }
  const auto item = std::move(*next);
  pending.erase(next);
  void* array = new_array(byte_class, item.bytes);
  const auto root = array ? gc_new(array, false) : 0;
  if (root) {
    try {
      ReadAlias alias;
      void* source = gc_target(item.buffer);
      if (item.file.find("-stream") != std::string::npos) {
        void* descriptor = Read<void*>(source, pointer_offset);
        if (!descriptor) throw std::runtime_error("Missing native buffer descriptor");
        const auto target = buffer_target(source);
        if (Read<uint32_t>(descriptor, 0x1c) != static_cast<uint32_t>(target)
            || Read<uint64_t>(descriptor, 8) * Read<uint64_t>(descriptor, 0x10) != static_cast<uint64_t>(item.bytes))
          throw std::runtime_error("Buffer descriptor mismatch");
        std::memcpy(alias.native.data(), descriptor, alias.native.size());

        Write<uint32_t>(alias.native.data(), 0x1c, static_cast<uint32_t>(target) & ~2u);
        void* object = new_object(buffer_class);
        alias.root = object ? gc_new(object, false) : 0;
        if (!alias.root) throw std::runtime_error("Cannot root read alias");
        Write<void*>(gc_target(alias.root), pointer_offset, alias.native.data());
        source = gc_target(alias.root);
        if (buffer_target(source) != (target & ~2)) throw std::runtime_error("Alias target mismatch");
      }
      get_data(source, gc_target(root), 0, 0, item.bytes, 1);
      const auto* bytes = static_cast<uint8_t*>(gc_target(root)) + 0x20;
      captures[item.file] = std::vector<uint8_t>(bytes, bytes + item.bytes);
    } catch (...) {
    }
    gc_free(root);
  }
  Invoke(dispose, gc_target(item.buffer));
  gc_free(item.buffer);
  gc_free(item.mesh);
}
inline void Start(bool requested_fill) {
  if (fill_holes != requested_fill) {
    if (!ResetTarget()) return;
    fill_holes = requested_fill;
  }
  if (reset_failed || !model_root || !head_root) return;
  if (session_model && gc_target(session_model) != gc_target(model_root) && !ResetTarget(true)) return;
  if (!settling_until) settling_until = GetTickCount64() + 1500;
  const ULONGLONG interval = GetTickCount64() < settling_until ? 0 : 500;
  if (!pending.empty() || !candidates.empty() || (last_scan && GetTickCount64() - last_scan < interval)) return;
  last_scan = GetTickCount64();
  if (!session_model) session_model = gc_new(gc_target(model_root), false);
  if (!session_model) return;

  auto recent = std::find_if(recent_models.begin(), recent_models.end(), [&](uint32_t root) { return gc_target(root) == gc_target(model_root); });
  if (recent != recent_models.end()) {
    auto root = *recent;
    recent_models.erase(recent);
    recent_models.push_back(root);
  } else {
    auto root = gc_new(gc_target(model_root), false);
    if (!root) return;
    recent_models.push_back(root);
  }
  if (recent_models.size() > 4) {
    gc_free(recent_models.front());
    recent_models.erase(recent_models.begin());
  }
  const auto retained = [&](uint32_t model) { return NativeAlive(model) && std::any_of(recent_models.begin(), recent_models.end(), [&](uint32_t root) { return gc_target(root) == gc_target(model); }); };
  auto destroy_cached = resolve ? reinterpret_cast<void (*)(void*, float)>(resolve("UnityEngine.Object::Destroy")) : nullptr;
  for (auto it = bindings.begin(); it != bindings.end();) {
    if (!it->failed && retained(it->model) && NativeAlive(it->renderer) && NativeAlive(it->source) && NativeAlive(it->filtered)) {
      ++it;
      continue;
    }
    RestoreBinding(&*it);
    if (destroy_cached && NativeAlive(it->filtered)) destroy_cached(gc_target(it->filtered), 0.f);
    for (auto root : {it->renderer, it->source, it->filtered, it->proxy, it->model, it->bones})
      if (root) gc_free(root);
    it = bindings.erase(it);
  }
  for (auto it = seen.begin(); it != seen.end();) {
    if (retained(it->model) && NativeAlive(it->renderer) && NativeAlive(it->mesh)) {
      ++it;
      continue;
    }
    UpdateSkinningPolicy(it->renderer, false, &it->owns_offscreen_update);
    for (auto root : {it->renderer, it->mesh, it->bones, it->model})
      if (root) gc_free(root);
    it = seen.erase(it);
  }

  if (!ResolveExport(GetModuleHandleW(L"GameAssembly.dll"), "il2cpp_resolve_icall", &resolve)) return;
  if (!started && !endfield::game_build::IsSupportedUnityPlayer(GetModuleHandleW(L"UnityPlayer.dll"))) {
    return;
  }
  ResolveExport(GetModuleHandleW(L"GameAssembly.dll"), "il2cpp_object_new", &new_object);
  buffer_target = reinterpret_cast<decltype(buffer_target)>(resolve("UnityEngine.GraphicsBuffer::get_target"));
  get_data = reinterpret_cast<decltype(get_data)>(resolve("UnityEngine.GraphicsBuffer::InternalGetData"));
  ResolveExport(GetModuleHandleW(L"GameAssembly.dll"), "il2cpp_array_new", &new_array);
  byte_class = class_from_name(FindImage("mscorlib.dll"), "System", "Byte");
  auto index_buffer = reinterpret_cast<void* (*)(void*)>(resolve("UnityEngine.Mesh::GetIndexBufferImpl"));
  const auto core = FindImage("UnityEngine.CoreModule.dll");
  buffer_class = class_from_name(core, "UnityEngine", "GraphicsBuffer");
  void* field = buffer_class ? class_get_field_from_name(buffer_class, "m_Ptr") : nullptr;
  pointer_offset = field ? field_get_offset(field) : 0;
  if (pointer_offset != 0x10) return;
  constexpr uint8_t target_load[] = {0x8b, 0x40, 0x1c, 0x48, 0x83, 0xc4, 0x48, 0xc3};
  if (!buffer_target || std::memcmp(reinterpret_cast<const uint8_t*>(buffer_target) + 0x40, target_load, sizeof(target_load))) {
    return;
  }
  dispose = FindMethod(core, "UnityEngine", "GraphicsBuffer", "Dispose", 0);
  auto get_mesh = FindMethod(core, "UnityEngine", "SkinnedMeshRenderer", "get_sharedMesh", 0);
  auto get_buffer = FindMethod(core, "UnityEngine", "Mesh", "GetVertexBuffer", 1);
  auto get_attribute = FindMethod(core, "UnityEngine", "Mesh", "GetVertexAttribute", 1);
  auto get_attr_count = FindMethod(core, "UnityEngine", "Mesh", "get_vertexAttributeCount", 0);
  auto get_vertices = FindMethod(core, "UnityEngine", "Mesh", "get_vertexCount", 0);
  auto is_child = FindMethod(core, "UnityEngine", "Transform", "IsChildOf", 1);
  auto get_parent = FindMethod(core, "UnityEngine", "Transform", "get_parent", 0);
  auto get_bones = FindMethod(core, "UnityEngine", "SkinnedMeshRenderer", "get_bones", 0);
  auto get_count = FindMethod(core, "UnityEngine", "GraphicsBuffer", "get_count", 0);
  auto get_stride = FindMethod(core, "UnityEngine", "GraphicsBuffer", "get_stride", 0);
  bool (*assignable)(void*, void*) = nullptr;
  ResolveExport(GetModuleHandleW(L"GameAssembly.dll"), "il2cpp_class_is_assignable_from", &assignable);
  auto* skinned = class_from_name(core, "UnityEngine", "SkinnedMeshRenderer");
  if (!new_object || !buffer_class || !get_data || !new_array || !byte_class || !index_buffer || !dispose || !get_mesh || !get_buffer || !get_attribute || !get_attr_count || !get_vertices || !is_child || !get_parent || !get_bones || !get_count || !get_stride || !assignable || !skinned) return;
  if (!PrepareBinding()) return;
  const void* (*class_type)(void*) = nullptr;
  void* (*type_object)(const void*) = nullptr;
  const auto module = GetModuleHandleW(L"GameAssembly.dll");
  if (!ResolveExport(module, "il2cpp_class_get_type", &class_type)
      || !ResolveExport(module, "il2cpp_type_get_object", &type_object)
      || !ResolveExport(module, "il2cpp_array_length", &array_length)) return;
  const auto type_root = gc_new(type_object(class_type(skinned)), false);
  if (!type_root) return;
  bool inactive = true;
  void* component_args[] = {gc_target(type_root), &inactive};
  void* renderers = Invoke(FindMethod(core, "UnityEngine", "GameObject", "GetComponentsInChildren", 2), gc_target(model_root), component_args);
  const auto array_root = renderers ? gc_new(renderers, false) : 0;
  gc_free(type_root);
  if (!array_root) return;
  started = true;

  struct NeckRoot {
    uint32_t handle = 0;
    ~NeckRoot() {
      if (handle) gc_free(handle);
    }
  } neck;
  if (void* parent = Invoke(get_parent, gc_target(head_root))) {
    neck.handle = gc_new(parent, false);
    void* name = neck.handle ? Invoke(name_method, gc_target(neck.handle)) : nullptr;
    std::wstring token = name ? std::wstring(string_chars(name), string_length(name)) : L"";
    const auto separator = token.find_last_of(L"_:.|/");
    if (separator != std::wstring::npos) token.erase(0, separator + 1);
    while (!token.empty() && token.back() >= L'0' && token.back() <= L'9') token.pop_back();
    std::transform(token.begin(), token.end(), token.begin(), [](wchar_t c) { return std::towlower(c); });
    if (token != L"neck" && neck.handle) {
      gc_free(neck.handle);
      neck.handle = 0;
    }
  }

  std::vector<uint32_t> shadows;
  for (uintptr_t r = 0; r < std::min<uintptr_t>(array_length(gc_target(array_root)), 512); ++r) {
    void* renderer = Read<void*>(gc_target(array_root), 0x20 + r * sizeof(void*));
    if (!renderer) continue;
    void* proxy = get_proxy(renderer);
    if (proxy) {
      auto root = gc_new(proxy, false);
      if (root) shadows.push_back(root);
    }
  }
  int captured = 0;
  size_t queued_bytes = 0;
  for (uintptr_t r = 0; r < std::min<uintptr_t>(array_length(gc_target(array_root)), 512); ++r) {
    void* renderer = Read<void*>(gc_target(array_root), 0x20 + r * sizeof(void*));
    if (!renderer || !assignable(skinned, object_class(renderer))) continue;
    void* current = Invoke(get_mesh, renderer);
    if (!current) continue;
    if (std::any_of(shadows.begin(), shadows.end(), [&](uint32_t root) { return gc_target(root) == current; })) continue;
    bool known = false;
    for (auto& b : bindings)
      if (!b.failed && gc_target(b.renderer) == renderer) {
        if (!PaletteMatches(renderer, b.bones)) {
          RestoreBinding(&b);
          b.failed = true;
          current = Invoke(get_mesh, renderer);
        } else if (!b.failed && (current == gc_target(b.source) || current == gc_target(b.filtered)))
          known = true;
      }
    if (known) continue;
    for (const auto& item : seen)
      if (gc_target(item.renderer) == renderer && gc_target(item.mesh) == current && PaletteMatches(renderer, item.bones)) {
        known = true;
        break;
      }
    if (known) continue;

    void* name = Invoke(name_method, renderer);
    if (!name) continue;
    std::wstring wide(string_chars(name), string_length(name));
    std::wstring role = wide;
    std::transform(role.begin(), role.end(), role.begin(), [](wchar_t c) { return std::towlower(c); });
    if (role.find(L"shadowproxy") != std::wstring::npos) continue;
    if (++captured > 128) break;
    std::string label(wide.begin(), wide.end());
    label += "-" + std::to_string(r);

    void* mesh = Invoke(get_mesh, renderer);
    if (!mesh) continue;
    const auto mesh_root = gc_new(mesh, false);
    if (!mesh_root) continue;
    void* vertices = Invoke(get_vertices, gc_target(mesh_root));
    void* attr_count = Invoke(get_attr_count, gc_target(mesh_root));
    if (!vertices || !attr_count) {
      gc_free(mesh_root);
      continue;
    }
    const int vertex_count = *static_cast<int*>(object_unbox(vertices));
    const int attributes = *static_cast<int*>(object_unbox(attr_count));

    if (vertex_count <= 0 || vertex_count > 200000 || attributes < 1 || attributes > 16) {
      gc_free(mesh_root);
      continue;
    }
    unsigned streams = 0;
    for (int i = 0; i < attributes; ++i) {
      void* args[] = {&i};
      void* result = Invoke(get_attribute, gc_target(mesh_root), args);
      if (!result) continue;
      const auto descriptor = *static_cast<std::array<int, 4>*>(object_unbox(result));
      if (descriptor[3] < 0 || descriptor[3] > 3) continue;
      streams |= 1u << descriptor[3];
    }
    Candidate selected;
    selected.name = label;
    selected.dedicated_head = endfield::camera::mesh::IsDedicatedHeadMesh(role);
    selected.body_skin = endfield::camera::mesh::IsBodyMesh(role);
    void* bones = Invoke(get_bones, renderer);
    if (bones) {
      auto bone_root = gc_new(bones, false);
      if (bone_root) {
        auto n = std::min<uintptr_t>(array_length(gc_target(bone_root)), 512);
        const bool candidate = n == array_length(gc_target(bone_root));
        if (candidate) selected.head.assign(n, 0);
        for (uintptr_t i = 0; i < n; ++i) {
          void* bone = Read<void*>(gc_target(bone_root), 0x20 + i * sizeof(void*));
          if (!bone) continue;
          void* child_args[] = {gc_target(head_root)};
          void* result = Invoke(is_child, bone, child_args);
          const bool attached = result && *static_cast<bool*>(object_unbox(result));
          if (candidate) selected.head[i] = attached ? endfield::camera::mesh::kHeadBone : (neck.handle && bone == gc_target(neck.handle) ? endfield::camera::mesh::kNeckBone : 0);
        }
        selected.bones = bone_root;
      }
    }

    if (neck.handle && selected.bones) {
      auto inverse_point = reinterpret_cast<void (*)(void*, const Vec3*, Vec3*)>(resolve("UnityEngine.Transform::InverseTransformPoint_Injected"));
      auto bind_method = FindMethod(core, "UnityEngine", "Mesh", "get_bindposes", 0);
      uint32_t ancestor = gc_new(gc_target(neck.handle), false);
      for (int depth = 0; ancestor && depth < 16; ++depth) {
        size_t index = 0, n = array_length(gc_target(selected.bones));
        while (index < n && Read<void*>(gc_target(selected.bones), 0x20 + index * sizeof(void*)) != gc_target(ancestor)) ++index;
        if (index < n && inverse_point && bind_method) {
          void* poses = Invoke(bind_method, gc_target(mesh_root));
          uint32_t pose_root = poses ? gc_new(poses, false) : 0;
          if (pose_root && array_length(gc_target(pose_root)) == n) {
            std::array<float, 16> m;
            std::memcpy(m.data(), static_cast<uint8_t*>(gc_target(pose_root)) + 0x20 + index * 64, 64);
            Vec3 head_world{}, neck_world{}, head_local{}, neck_local{};
            position_injected(gc_target(head_root), &head_world);
            position_injected(gc_target(neck.handle), &neck_world);
            inverse_point(gc_target(ancestor), &head_world, &head_local);
            inverse_point(gc_target(ancestor), &neck_world, &neck_local);
            selected.neck = endfield::camera::mesh::MakeNeckFrame(m, {neck_local.x, neck_local.y, neck_local.z}, {head_local.x, head_local.y, head_local.z});
          }
          if (pose_root) gc_free(pose_root);
          break;
        }
        void* parent = Invoke(get_parent, gc_target(ancestor));
        uint32_t next = parent ? gc_new(parent, false) : 0;
        gc_free(ancestor);
        ancestor = next;
      }
      if (ancestor) gc_free(ancestor);
    }
    if (!selected.dedicated_head && selected.neck.length == 0 && std::none_of(selected.head.begin(), selected.head.end(), [](uint8_t v) { return v != 0; })) {
      if (selected.bones && array_length(gc_target(selected.bones))) {
        seen.push_back({gc_new(renderer, false), gc_new(gc_target(mesh_root), false), selected.bones, gc_new(gc_target(model_root), false)});
      } else if (selected.bones)
        gc_free(selected.bones);
      gc_free(mesh_root);
      continue;
    }
    selected.mesh = gc_new(gc_target(mesh_root), false);
    selected.renderer = gc_new(renderer, false);
    selected.model = gc_new(gc_target(model_root), false);
    if (!selected.mesh || !selected.renderer || !selected.model) {
      for (auto root : {selected.mesh, selected.renderer, selected.model, selected.bones})
        if (root) gc_free(root);
      gc_free(mesh_root);
      continue;
    }
    const size_t queued_before = pending.size();
    size_t required_buffers = 1;
    for (int i = 0; i < 4; ++i)
      if (streams & (1u << i)) ++required_buffers;
    for (int stream = -1; stream < 4; ++stream) {
      if (stream >= 0 && !(streams & (1u << stream))) continue;
      void* buffer = nullptr;
      try {
        void* args[] = {&stream};
        buffer = stream < 0 ? index_buffer(gc_target(mesh_root)) : Invoke(get_buffer, gc_target(mesh_root), args);
      } catch (...) {
        continue;
      }
      if (!buffer) continue;
      const auto root = gc_new(buffer, false);
      if (!root) continue;
      void *c = Invoke(get_count, gc_target(root)), *s = Invoke(get_stride, gc_target(root));
      int64_t bytes = c && s ? int64_t(*static_cast<int*>(object_unbox(c))) * (*static_cast<int*>(object_unbox(s))) : 0;
      if (bytes <= 0 || bytes > 8 * 1024 * 1024) {
        Invoke(dispose, gc_target(root));
        gc_free(root);
        continue;
      }
      Pending item{};
      item.buffer = root;
      item.mesh = gc_new(gc_target(mesh_root), false);
      item.bytes = static_cast<int>(bytes);
      item.file = label + (stream < 0 ? "-indices" : "-stream" + std::to_string(stream)) + ".bin";
      if (!item.mesh) {
        Invoke(dispose, gc_target(root));
        gc_free(root);
        continue;
      }
      if (queued_bytes + item.bytes > 128 * 1024 * 1024) {
        Invoke(dispose, gc_target(root));
        gc_free(root);
        gc_free(item.mesh);
        continue;
      }
      queued_bytes += item.bytes;
      pending.push_back(std::move(item));
    }
    if (pending.size() - queued_before == required_buffers) {
      candidates.push_back(std::move(selected));
    } else {
      while (pending.size() > queued_before) {
        const auto item = pending.back();
        pending.pop_back();
        Invoke(dispose, gc_target(item.buffer));
        gc_free(item.buffer);
        gc_free(item.mesh);
      }
      for (auto root : {selected.mesh, selected.renderer, selected.model, selected.bones})
        if (root) gc_free(root);
    }
    gc_free(mesh_root);
  }
  for (auto root : shadows) gc_free(root);
  gc_free(array_root);

  std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
    const auto lod = [](const std::string& name) {
      const auto at = name.find("_lod");
      return at != std::string::npos && at + 4 < name.size() && name[at + 4] >= '0' && name[at + 4] <= '9' ? name[at + 4] - '0' : 0;
    };
    return lod(a.name) < lod(b.name);
  });
}
#include "./camera_mesh_copy.hpp"
}
