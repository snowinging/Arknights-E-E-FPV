#pragma once

namespace effects {
inline Il2CppMethod refresh_method, instance_id;
inline Il2CppMethod list_count, list_item;
inline void* instance_class = nullptr;
inline void* list_class = nullptr;
inline void* buckets_class = nullptr;
inline void* bucket_class = nullptr;
inline void* buckets_field = nullptr;
inline void* list_field = nullptr;
inline uintptr_t (*array_length)(void*) = nullptr;
inline void* manager_field = nullptr;
inline void* manager_class = nullptr;
inline void* target_field = nullptr;
inline void* target_rotation_field = nullptr;
inline void (*get_field)(void*, void*, void*) = nullptr;
inline void (*set_object_field)(void*, void*, void*) = nullptr;
inline void (*get_static)(void*, void*) = nullptr;
struct Binding {
  Root owner, original, replacement;
  void* field = nullptr;
  int id = -1;
};
inline std::vector<Binding> bindings;
inline Root animator_root;

inline void* Reference(void* owner, void* field) {
  void* value = nullptr;
  get_field(owner, field, &value);
  return value;
}

inline void Restore() {
  for (auto& binding : bindings) {
    if (binding.id >= 0 && Value<int>(instance_id, binding.owner.Get()) != binding.id) continue;
    if (Reference(binding.owner.Get(), binding.field) != binding.replacement.Get()) continue;
    set_object_field(binding.owner.Get(), binding.field, binding.original.Get());
  }
  bindings.clear();
  animator_root = Root();
}

inline bool FollowsSkeleton(void* target) {
  Root ancestor(target);
  for (int depth = 0; depth < 64 && Alive(ancestor.Get()); ++depth) {
    if (ancestor.Get() == visual_transform.Get()) return true;
    ancestor = Root(Call(parent_transform, ancestor.Get()));
  }
  return false;
}

inline void Redirect(void* owner, void* field, void* model, int id) {
  void* original = Reference(owner, field);
  if (!original || original == visual_transform.Get()) return;

  Root ancestor(model);
  bool match = false;
  for (int depth = 0; depth < 3 && Alive(ancestor.Get()); ++depth) {
    if (original == ancestor.Get()) {
      match = true;
      break;
    }
    ancestor = Root(Call(parent_transform, ancestor.Get()));
  }
  if (!match || bindings.size() >= 256) return;
  bindings.push_back({Root(owner), Root(original), Root(visual_transform.Get()), field, id});

  set_object_field(owner, field, visual_transform.Get());
  if (Reference(owner, field) != visual_transform.Get())
    throw std::runtime_error("Attachment target assignment did not match");
}

inline void Update(void* animator, void* model) {
  if (!buckets_field || !list_field) return;
  if (animator_root.Get() != animator) {
    Restore();
    animator_root = Root(animator);
  }

  void* manager_value = nullptr;
  get_static(manager_field, &manager_value);
  Root manager(manager_value);
  if (!manager.Get() || !assignable(manager_class, object_class(manager.Get()))) return;

  Root buckets(Reference(manager.Get(), buckets_field));
  if (!buckets.Get() || object_class(buckets.Get()) != buckets_class) return;
  const uintptr_t bucket_count = array_length(buckets.Get());
  if (bucket_count > 64) return;
  int total = 0;
  bindings.erase(std::remove_if(bindings.begin(), bindings.end(), [](const Binding& binding) {
                   return Value<int>(instance_id, binding.owner.Get()) != binding.id
                          || Reference(binding.owner.Get(), binding.field) != binding.replacement.Get();
                 }),
                 bindings.end());
  for (uintptr_t bucket_index = 0; bucket_index < bucket_count; ++bucket_index) {
    Root bucket(Read<void*>(buckets.Get(), 0x20 + bucket_index * sizeof(void*)));
    if (!bucket.Get() || !assignable(bucket_class, object_class(bucket.Get()))) continue;
    Root list(Reference(bucket.Get(), list_field));
    if (!list.Get() || !assignable(list_class, object_class(list.Get()))) continue;
    const int count = Value<int>(list_count, list.Get());
    if (count < 0 || count > 8192 - total) continue;
    total += count;
    for (int i = 0; i < count; ++i) {
      void* item_args[]{&i};
      Root effect(Call(list_item, list.Get(), item_args));
      if (!effect.Get() || !assignable(instance_class, object_class(effect.Get()))) continue;
      const int id = Value<int>(instance_id, effect.Get());
      Redirect(effect.Get(), target_field, model, id);
      Redirect(effect.Get(), target_rotation_field, model, id);
      if (!FollowsSkeleton(Reference(effect.Get(), target_field))
          && !FollowsSkeleton(Reference(effect.Get(), target_rotation_field))) continue;
      Call(refresh_method, effect.Get());
    }
  }
}

inline bool Resolve(Il2CppImage game, Il2CppImage unity) {
  const auto module = GetModuleHandleW(L"GameAssembly.dll");
  const void* (*field_type)(void*) = nullptr;
  void* (*type_class)(const void*) = nullptr;
  const void* (*return_type)(Il2CppMethod) = nullptr;
  void* (*element_class)(void*) = nullptr;
  void* (*fields)(void*, void**) = nullptr;
  int (*type_kind)(const void*) = nullptr;
  Il2CppMethod (*method)(void*, const char*, int) = nullptr;
  int (*field_flags)(void*) = nullptr;
  if (!ResolveExport(module, "il2cpp_field_get_value", &get_field)
      || !ResolveExport(module, "il2cpp_field_set_value_object", &set_object_field)
      || !ResolveExport(module, "il2cpp_field_get_type", &field_type)
      || !ResolveExport(module, "il2cpp_class_from_type", &type_class)
      || !ResolveExport(module, "il2cpp_method_get_return_type", &return_type)
      || !ResolveExport(module, "il2cpp_class_get_element_class", &element_class)
      || !ResolveExport(module, "il2cpp_class_get_fields", &fields)
      || !ResolveExport(module, "il2cpp_type_get_type", &type_kind)
      || !ResolveExport(module, "il2cpp_array_length", &array_length)
      || !ResolveExport(module, "il2cpp_class_get_method_from_name", &method)
      || !ResolveExport(module, "il2cpp_field_static_get_value", &get_static)
      || !ResolveExport(module, "il2cpp_field_get_flags", &field_flags)) return false;
  instance_class = class_from_name(game, "Beyond.Gameplay", "EffectInstance");
  manager_class = class_from_name(game, "Beyond.Gameplay", "EffectManager");
  const auto instance = class_from_name(game, "Beyond.Gameplay", "GameInstance");
  if (!instance_class || !manager_class || !instance) return false;
  manager_field = class_get_field_from_name(instance, "effectManager");
  if (!manager_field || !(field_flags(manager_field) & 0x10)
      || type_class(field_type(manager_field)) != manager_class) return false;
  target_field = class_get_field_from_name(instance_class, "m_followTarget");
  target_rotation_field = class_get_field_from_name(instance_class, "m_followTargetRot");
  for (void* field : {target_field, target_rotation_field})
    if (!field || (field_flags(field) & 0x10)
        || type_class(field_type(field)) != class_from_name(unity, "UnityEngine", "Transform")) return false;
  refresh_method = FindMethod(game, "Beyond.Gameplay", "EffectInstance", "ManualUpdateFollow", 0);
  instance_id = FindMethod(game, "Beyond.Gameplay", "EffectInstance", "get_instanceId", 0);
  buckets_field = class_get_field_from_name(manager_class, "m_activeEffects");
  if (!buckets_field || (field_flags(buckets_field) & 0x10)
      || type_kind(field_type(buckets_field)) != 0x1d) return false;
  buckets_class = type_class(field_type(buckets_field));
  bucket_class = element_class(buckets_class);
  if (!bucket_class) return false;

  void* iterator = nullptr;
  while (void* field = fields(bucket_class, &iterator)) {
    if (field_flags(field) & 0x10) continue;
    void* klass = type_class(field_type(field));
    if (!klass) continue;
    auto count = method(klass, "get_Count", 0), item = method(klass, "get_Item", 1);
    if (!count || !item || type_class(return_type(item)) != instance_class) continue;
    if (list_field) return false;
    list_field = field;
    list_class = klass;
    list_count = count;
    list_item = item;
  }
  return list_field && refresh_method && instance_id;
}
}
