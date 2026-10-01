inline void BuildDetachedMesh() {
  if (!candidate_source) return;
  uint32_t source_root = candidate_source;
  candidate_source = 0;
  uint32_t mesh_root = 0, bind_root = 0;
  auto destroy = reinterpret_cast<void (*)(void*, float)>(resolve("UnityEngine.Object::Destroy"));
  try {
    if (!candidate_renderer || !candidate_model || !PrepareBinding()) throw std::runtime_error("Renderer binding API unavailable");
    const auto core = FindImage("UnityEngine.CoreModule.dll");
    auto* mesh_class = class_from_name(core, "UnityEngine", "Mesh");
    auto clone = reinterpret_cast<void* (*)(void*)>(resolve("UnityEngine.Object::Internal_CloneSingle"));
    auto vertex_params = reinterpret_cast<void (*)(void*, int, const void*, int)>(resolve("UnityEngine.Mesh::SetVertexBufferParamsFromPtr"));
    auto vertex_data = reinterpret_cast<void (*)(void*, int, const void*, int, int, int, int, int)>(resolve("UnityEngine.Mesh::InternalSetVertexBufferData"));
    auto index_params = reinterpret_cast<void (*)(void*, int, int)>(resolve("UnityEngine.Mesh::SetIndexBufferParams"));
    auto index_data = reinterpret_cast<void (*)(void*, const void*, int, int, int, int, int)>(resolve("UnityEngine.Mesh::InternalSetIndexBufferData"));
    auto get_submesh = reinterpret_cast<void (*)(void*, int, void*)>(resolve("UnityEngine.Mesh::GetSubMesh_Injected"));
    auto set_submesh = reinterpret_cast<void (*)(void*, int, const void*, int)>(resolve("UnityEngine.Mesh::SetSubMesh_Injected"));
    auto get_bounds = reinterpret_cast<void (*)(void*, void*)>(resolve("UnityEngine.Mesh::get_bounds_Injected"));
    auto set_bounds = reinterpret_cast<void (*)(void*, const void*)>(resolve("UnityEngine.Mesh::set_bounds_Injected"));
    auto index_buffer = reinterpret_cast<void* (*)(void*)>(resolve("UnityEngine.Mesh::GetIndexBufferImpl"));
    auto get_stride = reinterpret_cast<int (*)(void*, int)>(resolve("UnityEngine.Mesh::GetVertexBufferStride"));
    if (!destroy || !mesh_class || !clone || !vertex_params || !vertex_data || !index_params || !index_data
        || !get_submesh || !set_submesh || !get_bounds || !set_bounds || !index_buffer || !get_stride)
      throw std::runtime_error("Required mesh API absent");
    auto get_int = [&](const char* name) {
      void* value = Invoke(FindMethod(core, "UnityEngine", "Mesh", name, 0), gc_target(source_root));
      if (!value) throw std::runtime_error("Missing mesh property");
      return *static_cast<int*>(object_unbox(value));
    };
    const int vertices = get_int("get_vertexCount"), attributes = get_int("get_vertexAttributeCount"), submeshes = get_int("get_subMeshCount");
    const int index_format = get_int("get_indexFormat"), blend_shapes = get_int("get_blendShapeCount");

    if (vertices < 1 || vertices > 200000 || attributes < 1 || attributes > 16 || submeshes < 1 || submeshes > 64
        || (index_format != 0 && index_format != 1))
      throw std::runtime_error("Unsupported detached mesh shape");
    std::vector<std::array<int, 4>> descriptors;
    for (int i = 0; i < attributes; ++i) {
      void* args[] = {&i};
      void* value = Invoke(FindMethod(core, "UnityEngine", "Mesh", "GetVertexAttribute", 1), gc_target(source_root), args);
      if (!value) throw std::runtime_error("Missing attribute");
      descriptors.push_back(*static_cast<std::array<int, 4>*>(object_unbox(value)));
    }
    std::array<int, 4> strides{};
    for (const auto& d : descriptors) {
      if (d[3] < 0 || d[3] > 3) throw std::runtime_error("Invalid vertex stream");
      strides[d[3]] = get_stride(gc_target(source_root), d[3]);
      if (strides[d[3]] < 1 || strides[d[3]] > 256) throw std::runtime_error("Invalid stride");
    }
    const bool all_head = candidate_dedicated_head || std::all_of(candidate_head.begin(), candidate_head.end(), [](uint8_t v) { return v == endfield::camera::mesh::kHeadBone; });
    std::array<int, 4> position{}, weight{}, bone{};
    int po = 0, wo = 0, bo = 0;
    if (!all_head) {
      const auto attr = [&](int id) {for(const auto& d:descriptors)if(d[0]==id)return d;throw std::runtime_error("Missing skin/position attribute"); };
      position = attr(0);
      weight = attr(12);
      bone = attr(13);
      if (position[1] != 0 || position[2] < 3 || (weight[1] != 4 && weight[1] != 0) || weight[2] < 1 || weight[2] > 4
          || (bone[1] != 6 && bone[1] != 8 && bone[1] != 10) || bone[2] < weight[2]) throw std::runtime_error("Unsupported skin encoding");
      auto attribute_offset = reinterpret_cast<int (*)(void*, int)>(resolve("UnityEngine.Mesh::GetVertexAttributeOffset"));
      if (!attribute_offset) throw std::runtime_error("Attribute offset API absent");
      po = attribute_offset(gc_target(source_root), 0);
      wo = attribute_offset(gc_target(source_root), 12);
      bo = attribute_offset(gc_target(source_root), 13);
      if (po < 0 || wo < 0 || bo < 0 || po + 12 > strides[position[3]] || wo + weight[2] * (weight[1] == 4 ? 2 : 4) > strides[weight[3]]
          || bo + bone[2] * (bone[1] == 6 ? 1 : bone[1] == 8 ? 2
                                                             : 4)
                 > strides[bone[3]]) throw std::runtime_error("Attribute exceeds stride");
    }
    auto load = [&](const std::string& suffix) {
      const auto key = candidate_name + suffix + ".bin";
      auto found = captures.find(key);
      if (found == captures.end()) throw std::runtime_error("Capture data missing");
      auto data = std::move(found->second);
      captures.erase(found);
      return data;
    };
    std::array<std::vector<uint8_t>, 4> streams;
    for (int i = 0; i < 4; ++i)
      if (strides[i]) streams[i] = load("-stream" + std::to_string(i));
    const size_t index_size = index_format == 0 ? 2 : 4;
    auto original = load("-indices");
    if (original.empty() || original.size() % (3 * index_size)) throw std::runtime_error("Invalid index byte count");
    for (int i = 0; i < 4; ++i)
      if (strides[i] && streams[i].size() < static_cast<size_t>(vertices) * strides[i]) throw std::runtime_error("Short vertex stream");
    std::vector<endfield::camera::mesh::Vertex> decoded(vertices);
    for (int i = 0; i < vertices; ++i) {
      if (all_head) {
        decoded[i].weights[0] = 65535;
        continue;
      }
      std::memcpy(decoded[i].position.data(), streams[position[3]].data() + i * strides[position[3]] + po, 12);
      for (int j = 0; j < weight[2]; ++j) {
        const auto* weights = streams[weight[3]].data() + i * strides[weight[3]] + wo;
        if (weight[1] == 4)
          decoded[i].weights[j] = Read<uint16_t>(weights, j * 2);
        else {
          const float w = Read<float>(weights, j * 4);
          if (!std::isfinite(w) || w < 0.f || w > 1.f) throw std::runtime_error("Invalid float skin weight");
          decoded[i].weights[j] = static_cast<uint16_t>(std::lround(w * 65535.f));
        }
        const auto* indices = streams[bone[3]].data() + i * strides[bone[3]] + bo;
        const uint32_t id = bone[1] == 6 ? indices[j] : bone[1] == 8 ? Read<uint16_t>(indices, j * 2)
                                                                     : Read<uint32_t>(indices, j * 4);
        if (decoded[i].weights[j] && id >= candidate_head.size()) throw std::runtime_error("Bone index outside renderer palette");
        decoded[i].bones[j] = decoded[i].weights[j] ? static_cast<uint16_t>(id) : 0;
      }
    }
    std::vector<uint32_t> indices(original.size() / index_size), filtered;
    for (size_t i = 0; i < indices.size(); ++i) indices[i] = index_format == 0 ? Read<uint16_t>(original.data(), i * 2) : Read<uint32_t>(original.data(), i * 4);
    if (!endfield::camera::mesh::FilterHeadComponents(decoded, indices, candidate_head, &filtered, candidate_dedicated_head, candidate_body_skin)) throw std::runtime_error("Mesh validation failed");
    size_t removed = 0;
    for (size_t i = 0; i < indices.size(); i += 3)
      if (filtered[i + 1] != indices[i + 1] || filtered[i + 2] != indices[i + 2]) ++removed;

    struct SubMesh {
      std::array<float, 6> bounds;
      int topology, index_start, index_count, base_vertex, first_vertex, vertex_count;
    };
    static_assert(sizeof(SubMesh) == 48 && offsetof(SubMesh, index_start) == 28 && offsetof(SubMesh, base_vertex) == 36);
    auto* submesh_class = class_from_name(core, "UnityEngine.Rendering", "SubMeshDescriptor");
    const std::array<const char*, 7> field_names{"<bounds>k__BackingField", "<topology>k__BackingField", "<indexStart>k__BackingField", "<indexCount>k__BackingField", "<baseVertex>k__BackingField", "<firstVertex>k__BackingField", "<vertexCount>k__BackingField"};
    const std::array<size_t, 7> offsets{0, 24, 28, 32, 36, 40, 44};
    if (!submesh_class) throw std::runtime_error("Submesh type absent");
    for (size_t i = 0; i < offsets.size(); ++i) {
      auto* field = class_get_field_from_name(submesh_class, field_names[i]);
      if (!field || field_get_offset(field) != offsets[i] + 0x10) throw std::runtime_error("Submesh metadata layout changed");
    }
    std::vector<SubMesh> submesh_data(submeshes);
    for (int i = 0; i < submeshes; ++i) {
      get_submesh(gc_target(source_root), i, &submesh_data[i]);
      const auto& d = submesh_data[i];
      const int start = d.index_start, count = d.index_count;

      if (start < 0 || count < 0 || start % 3 || count % 3 || size_t(start) + count > indices.size() || d.topology != 0 || d.base_vertex != 0)
        throw std::runtime_error("Unsupported submesh topology/base vertex");
    }
    const auto caps = fill_holes && !all_head
                          ? endfield::camera::mesh::BuildNeckCaps(decoded, indices, candidate_head, filtered, candidate_neck)
                          : std::vector<endfield::camera::mesh::NeckCapTriangle>{};

    if (filtered == indices && caps.empty()) {
      seen.push_back({gc_new(gc_target(candidate_renderer), false), gc_new(gc_target(source_root), false),
                      gc_new(gc_target(candidate_bones), false), gc_new(gc_target(candidate_model), false)});
      throw std::runtime_error("No head components or neck caps selected");
    }
    if (!caps.empty()) {
      std::vector<uint32_t> composed;
      composed.reserve(filtered.size() + caps.size() * 3);
      std::array<float, 6> mesh_bounds{};
      get_bounds(gc_target(source_root), mesh_bounds.data());
      for (auto& part : submesh_data) {
        const size_t start = part.index_start, end = start + part.index_count;
        part.index_start = static_cast<int>(composed.size());
        composed.insert(composed.end(), filtered.begin() + start, filtered.begin() + end);
        for (const auto& cap : caps)
          if (cap.material_triangle >= start && cap.material_triangle < end)
            composed.insert(composed.end(), cap.vertices.begin(), cap.vertices.end());
        part.index_count = static_cast<int>(composed.size()) - part.index_start;

        part.bounds = mesh_bounds;
        auto first = composed.begin() + part.index_start, last = composed.end();
        if (first != last) {
          auto range = std::minmax_element(first, last);
          part.first_vertex = *range.first;
          part.vertex_count = *range.second - part.first_vertex + 1;
        }
      }
      filtered = std::move(composed);
    }
    std::vector<uint8_t> packed(filtered.size() * index_size);
    for (size_t i = 0; i < filtered.size(); ++i) {
      if (index_format == 0) {
        if (filtered[i] > 65535) throw std::runtime_error("Index overflow");
        Write<uint16_t>(packed.data(), i * 2, static_cast<uint16_t>(filtered[i]));
      } else
        Write<uint32_t>(packed.data(), i * 4, filtered[i]);
    }

    void* binds = Invoke(FindMethod(core, "UnityEngine", "Mesh", "get_bindposes", 0), gc_target(source_root));
    bind_root = binds ? gc_new(binds, false) : 0;
    if (!bind_root || array_length(gc_target(bind_root)) != candidate_head.size()) throw std::runtime_error("Bind poses do not match bones");

    if (!PaletteMatches(gc_target(candidate_renderer), candidate_bones)) throw std::runtime_error("Bone palette changed during capture");
    void* object = nullptr;
    void* deferred_native = nullptr;
    bool unexpected_awake = false;
    {
      CloneAwakeScope scope(Read<void*>(gc_target(source_root), 0x10));
      object = clone(gc_target(source_root));
      deferred_native = scope.deferred;
      unexpected_awake = scope.unexpected;
    }
    mesh_root = object ? gc_new(object, false) : 0;
    if (mesh_root && (unexpected_awake || !deferred_native || deferred_native != Read<void*>(gc_target(mesh_root), 0x10)))
      throw std::runtime_error("Deferred initialization did not belong to the returned clone");
    if (!mesh_root || gc_target(mesh_root) == gc_target(source_root)) throw std::runtime_error("Cannot clone mesh independently");

    vertex_params(gc_target(mesh_root), vertices, descriptors.data(), static_cast<int>(descriptors.size()));
    for (int i = 0; i < 4; ++i)
      if (strides[i]) {
        if (get_stride(gc_target(mesh_root), i) != strides[i]) throw std::runtime_error("Clone stream layout changed");
        vertex_data(gc_target(mesh_root), i, streams[i].data(), 0, 0, vertices, strides[i], 2);
      }

    index_params(gc_target(mesh_root), static_cast<int>(filtered.size()) + 3, index_format);
    index_params(gc_target(mesh_root), static_cast<int>(filtered.size()), index_format);

    index_data(gc_target(mesh_root), packed.data(), 0, 0, static_cast<int>(filtered.size()), static_cast<int>(index_size), 2);
    void* exception = nullptr;
    auto method = FindMethod(core, "UnityEngine", "Mesh", "set_subMeshCount", 1);
    if (!method) throw std::runtime_error("Submesh setter absent");
    int count = submeshes;
    void* count_args[] = {&count};
    runtime_invoke(method, gc_target(mesh_root), count_args, &exception);
    if (exception) throw std::runtime_error("Cannot set submesh count");

    constexpr int preserve_submesh_bounds = 2 | 8;
    for (int i = 0; i < submeshes; ++i) set_submesh(gc_target(mesh_root), i, &submesh_data[i], preserve_submesh_bounds);
    std::array<float, 6> bounds{};
    get_bounds(gc_target(source_root), bounds.data());
    set_bounds(gc_target(mesh_root), bounds.data());
    void* copied_binds = Invoke(FindMethod(core, "UnityEngine", "Mesh", "get_bindposes", 0), gc_target(mesh_root));
    if (!copied_binds || array_length(copied_binds) != candidate_head.size()
        || std::memcmp(static_cast<uint8_t*>(copied_binds) + 0x20, static_cast<uint8_t*>(gc_target(bind_root)) + 0x20, candidate_head.size() * 64))
      throw std::runtime_error("Clone bind poses changed");
    void* copied_shapes = Invoke(FindMethod(core, "UnityEngine", "Mesh", "get_blendShapeCount", 0), gc_target(mesh_root));
    if (!copied_shapes || *static_cast<int*>(object_unbox(copied_shapes)) != blend_shapes) throw std::runtime_error("Clone blend shapes changed");

    native_awake(deferred_native);

    const auto get_vertex = FindMethod(core, "UnityEngine", "Mesh", "GetVertexBuffer", 1);
    if (!get_vertex) throw std::runtime_error("Vertex buffer accessor absent");
    for (int stream = 0; stream < 4; ++stream) {
      const int stride = strides[stream];
      if (!stride) continue;
      if (get_stride(gc_target(mesh_root), stream) != stride) throw std::runtime_error("Uploaded layout changed");
      void* args[] = {&stream};
      void* buffer = Invoke(get_vertex, gc_target(mesh_root), args);
      const auto buffer_root = buffer ? gc_new(buffer, false) : 0;
      if (!buffer_root) throw std::runtime_error("Missing uploaded vertex buffer");
      const size_t bytes = static_cast<size_t>(vertices) * stride;
      const auto array_root = gc_new(new_array(byte_class, bytes), false);
      if (!array_root) {
        Invoke(dispose, gc_target(buffer_root));
        gc_free(buffer_root);
        throw std::runtime_error("Vertex readback allocation failed");
      }
      bool match = false;
      try {
        ReadAlias alias;
        void* source = gc_target(buffer_root);
        void* descriptor = Read<void*>(source, pointer_offset);
        const int target = buffer_target(source);
        if (!descriptor || Read<uint32_t>(descriptor, 0x1c) != static_cast<uint32_t>(target)
            || Read<uint64_t>(descriptor, 8) * Read<uint64_t>(descriptor, 0x10) < bytes)
          throw std::runtime_error("Uploaded buffer descriptor mismatch");
        std::memcpy(alias.native.data(), descriptor, alias.native.size());
        Write<uint32_t>(alias.native.data(), 0x1c, static_cast<uint32_t>(target) & ~2u);
        void* object = new_object(buffer_class);
        alias.root = object ? gc_new(object, false) : 0;
        if (!alias.root) throw std::runtime_error("Cannot root vertex read alias");
        Write<void*>(gc_target(alias.root), pointer_offset, alias.native.data());
        get_data(gc_target(alias.root), gc_target(array_root), 0, 0, static_cast<int>(bytes), 1);
        match = std::memcmp(static_cast<uint8_t*>(gc_target(array_root)) + 0x20, streams[stream].data(), bytes) == 0;

      } catch (...) {
        gc_free(array_root);
        Invoke(dispose, gc_target(buffer_root));
        gc_free(buffer_root);
        throw;
      }
      gc_free(array_root);
      Invoke(dispose, gc_target(buffer_root));
      gc_free(buffer_root);
      if (!match) throw std::runtime_error("Uploaded vertex content mismatch");
    }

    for (bool check_original : {false, true}) {
      void* buffer = index_buffer(gc_target(check_original ? source_root : mesh_root));
      const auto buffer_root = buffer ? gc_new(buffer, false) : 0;
      if (!buffer_root) throw std::runtime_error("No uploaded index buffer");
      const auto array_root = gc_new(new_array(byte_class, check_original ? original.size() : packed.size()), false);
      if (!array_root) {
        Invoke(dispose, gc_target(buffer_root));
        gc_free(buffer_root);
        throw std::runtime_error("Readback allocation failed");
      }
      bool match = false;
      try {
        get_data(gc_target(buffer_root), gc_target(array_root), 0, 0, static_cast<int>(check_original ? original.size() : packed.size()), 1);
        match = std::memcmp(static_cast<uint8_t*>(gc_target(array_root)) + 0x20, check_original ? original.data() : reinterpret_cast<const uint8_t*>(packed.data()), check_original ? original.size() : packed.size()) == 0;

      } catch (...) {
        gc_free(array_root);
        Invoke(dispose, gc_target(buffer_root));
        gc_free(buffer_root);
        throw;
      }
      gc_free(array_root);
      Invoke(dispose, gc_target(buffer_root));
      gc_free(buffer_root);
      if (!match) throw std::runtime_error("Index ownership/content check failed");
    }

    if (get_shared(gc_target(candidate_renderer)) != gc_target(source_root)) throw std::runtime_error("Renderer source changed during capture");
    void* proxy = get_proxy(gc_target(candidate_renderer));
    const auto proxy_root = proxy ? gc_new(proxy, false) : 0;
    if (proxy && !proxy_root) throw std::runtime_error("Cannot retain original shadow mesh");
    if (!PaletteMatches(gc_target(candidate_renderer), candidate_bones)) throw std::runtime_error("Bone palette changed before binding");
    bindings.push_back({candidate_renderer, source_root, mesh_root, proxy_root, candidate_model, false, false, candidate_bones});
    bindings.back().has_visible_triangles = removed < indices.size() / 3;
    candidate_bones = 0;
    candidate_renderer = candidate_model = source_root = mesh_root = 0;

  } catch (...) {
  }
  if (mesh_root) {
    if (destroy) destroy(gc_target(mesh_root), 0.f);
    gc_free(mesh_root);
  }
  if (bind_root) gc_free(bind_root);
  if (source_root) gc_free(source_root);
  if (candidate_renderer) {
    gc_free(candidate_renderer);
    candidate_renderer = 0;
  }
  if (candidate_model) {
    gc_free(candidate_model);
    candidate_model = 0;
  }
  if (candidate_bones) {
    gc_free(candidate_bones);
    candidate_bones = 0;
  }
}
