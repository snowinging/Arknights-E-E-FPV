#pragma once

namespace motion {
using CameraTick = void (*)(void*, float, MethodInfo*);
inline CameraTick tail_tick = nullptr, input_tick = nullptr;
inline Il2CppMethod output_camera, frame_number, get_components, get_bones, get_mesh, get_bindposes;
inline void* skin_class = nullptr;
inline const void* (*class_type)(void*) = nullptr;
inline void* (*type_object)(const void*) = nullptr;
inline uintptr_t (*array_length)(void*) = nullptr;
inline void (*set_position)(void*, const Vec3*) = nullptr;
inline void (*set_rotation)(void*, const Quat*) = nullptr;
inline uint32_t camera_root = 0, body_root = 0;
inline int captured_frame = -1;
inline bool keep_direction = false, head_axes = false, body_axes = false, tried_axes = false;
inline Quat input_view{0, 0, 0, 1};
inline Quat animation_offset{0, 0, 0, 1};
inline Quat written_view{0, 0, 0, 1};
inline bool wrote_view = false;
inline double motion_time = 0;
inline int motion_frame = -1;
inline Quat frame_start_offset{0, 0, 0, 1};
inline float frame_alpha = 0.f;
inline bool following_interaction = false;
inline float interaction_body_yaw = 0.f;
inline Vec3 head_forward{}, head_up{}, body_forward{}, body_up{};

inline void RestoreControlView() {
  if (!wrote_view) return;
  wrote_view = false;
  if (!camera_root) return;
  void* transform = Invoke(movement::component_transform, gc_target(camera_root));
  void* rotation = transform ? Invoke(movement::world_rotation, transform) : nullptr;
  if (rotation && movement::SameRotation(*static_cast<Quat*>(object_unbox(rotation)), written_view))
    set_rotation(transform, &input_view);
}

inline void Reset() {
  RestoreControlView();
  if (camera_root) gc_free(camera_root);
  if (body_root) gc_free(body_root);
  camera_root = body_root = 0;
  captured_frame = -1;
  head_axes = body_axes = tried_axes = false;
  animation_offset = {0, 0, 0, 1};
  motion_time = 0;
  motion_frame = -1;
  following_interaction = false;
}

inline bool AlignInteraction(void* controller, Quat* view) {
  if (!movement::interaction_alignment || !model_root) {
    following_interaction = false;
    return false;
  }
  void* transform = Invoke(transform_method, gc_target(model_root));
  void* rotation = transform ? Invoke(movement::world_rotation, transform) : nullptr;
  void* param = Invoke(dialogue::camera_param, controller);
  void* horizontal = param ? Invoke(dialogue::horizontal, param) : nullptr;
  if (!rotation || !horizontal || !dialogue::set_horizontal) return false;
  const Quat body = *static_cast<Quat*>(object_unbox(rotation));
  if (!Unit(body)) return false;
  const Vec3 forward = Rotate(body, {0, 0, 1}), look = Rotate(*view, {0, 0, 1});
  const float yaw = std::atan2(forward.x, forward.z) * 57.295779513f;
  const float delta = std::remainder(yaw - (following_interaction ? interaction_body_yaw : std::atan2(look.x, look.z) * 57.295779513f), 360.f);
  float angle = *static_cast<float*>(object_unbox(horizontal)) + delta;
  if (!std::isfinite(angle)) return false;
  bool tween = false;
  void* args[]{&angle, &tween};
  void* exception = nullptr;
  runtime_invoke(dialogue::set_horizontal, param, args, &exception);
  if (exception) return false;

  *view = AxisAngle({0, 1, 0}, delta) * (*view);
  interaction_body_yaw = yaw;
  following_interaction = true;
  return true;
}

inline void Capture(void* brain, bool active, bool preserve_direction, const Quat& control_view) {
  captured_frame = -1;
  if (!active) return;
  void* camera = Invoke(output_camera, brain);
  void* frame = Invoke(frame_number, nullptr);
  if (!camera || !frame) return;
  if (!camera_root || gc_target(camera_root) != camera) {
    if (camera_root) gc_free(camera_root);
    camera_root = gc_new(camera, false);
  }
  if (!camera_root) return;

  input_view = control_view;
  if (!Unit(input_view)) return;
  captured_frame = *static_cast<int*>(object_unbox(frame));
  keep_direction = preserve_direction;
}

inline void ResolveAxes() {
  tried_axes = true;
  using movement::Root;
  Root ancestor(Invoke(movement::parent_transform, gc_target(head_root)));
  for (int depth = 0; ancestor.Get() && depth < 8; ++depth) {
    void* name = Invoke(name_method, ancestor.Get());
    std::wstring token;
    if (name && string_length(name) > 0 && string_length(name) < 256) {
      token.assign(string_chars(name), string_length(name));
      std::transform(token.begin(), token.end(), token.begin(), [](wchar_t c) { return std::towlower(c); });
    }
    if (token.find(L"spine") != std::wstring::npos || token == L"chest") {
      body_root = gc_new(ancestor.Get(), false);
      break;
    }
    ancestor = Root(Invoke(movement::parent_transform, ancestor.Get()));
  }
  Root type(type_object(class_type(skin_class)));
  bool inactive = true;
  void* args[]{type.Get(), &inactive};
  Root renderers(type.Get() ? Invoke(get_components, gc_target(model_root), args) : nullptr);
  Root model_transform(Invoke(transform_method, gc_target(model_root)));
  if (!renderers.Get() || !model_transform.Get()) return;
  const Quat model_rotation = movement::Value<Quat>(movement::world_rotation, model_transform.Get());
  for (uintptr_t i = 0; i < std::min<uintptr_t>(array_length(renderers.Get()), 256) && !(head_axes && body_axes); ++i) {
    Root renderer(Read<void*>(renderers.Get(), 0x20 + i * sizeof(void*)));
    if (!renderer.Get()) continue;
    Root bones(Invoke(get_bones, renderer.Get()));
    Root mesh(Invoke(get_mesh, renderer.Get()));
    Root poses(mesh.Get() ? Invoke(get_bindposes, mesh.Get()) : nullptr);
    Root transform(Invoke(movement::component_transform, renderer.Get()));
    if (!bones.Get() || !poses.Get() || !transform.Get()) continue;
    const uintptr_t count = array_length(bones.Get());
    if (count > 512 || count != array_length(poses.Get())) continue;
    const Quat r = movement::Value<Quat>(movement::world_rotation, transform.Get());
    if (!Unit(r) || !Unit(model_rotation)) continue;
    const Quat model_to_mesh = Quat{-r.x, -r.y, -r.z, r.w} * model_rotation;
    for (uintptr_t j = 0; j < count; ++j) {
      void* bone = Read<void*>(bones.Get(), 0x20 + j * sizeof(void*));
      const bool head = bone == gc_target(head_root);
      const bool body = body_root && bone == gc_target(body_root);
      if ((!head || head_axes) && (!body || body_axes)) continue;
      const auto bind = Read<std::array<float, 16>>(poses.Get(), 0x20 + j * 64);
      const auto direction = [&](Vec3 v) {
        v = Rotate(model_to_mesh, v);
        return Vec3{bind[0] * v.x + bind[4] * v.y + bind[8] * v.z,
                    bind[1] * v.x + bind[5] * v.y + bind[9] * v.z,
                    bind[2] * v.x + bind[6] * v.y + bind[10] * v.z};
      };
      const Vec3 forward = direction({0, 0, 1}), up = direction({0, 1, 0});
      Quat check{};
      if (!FacingRotation(forward, up, &check)) continue;
      if (head) {
        head_forward = forward;
        head_up = up;
        head_axes = true;
      }
      if (body) {
        body_forward = forward;
        body_up = up;
        body_axes = true;
      }
    }
  }
}

inline void Apply(void* camera) {
  if (!camera_root || gc_target(camera_root) != camera || captured_frame < 0) return;
  const Values v = ReadValues();
  if (!v.first_person || !Context(v)) return;
  void* frame = Invoke(frame_number, nullptr);
  if (!frame || *static_cast<int*>(object_unbox(frame)) != captured_frame) return;
  try {
    void* head = head_root ? gc_target(head_root) : nullptr;
    if (!movement::Alive(head)) return;
    Vec3 eyes{};
    position_injected(head, &eyes);
    if (!Finite(eyes)) return;
    if (!keep_direction && (v.body_facing || v.head_facing || v.realism) && !tried_axes) ResolveAxes();
    bool full_motion = false;
    Quat offset{0, 0, 0, 1};
    const bool use_head = v.head_facing || v.realism;
    if (!keep_direction && (use_head ? head_axes : v.body_facing && body_axes)) {
      const Quat bone = movement::Value<Quat>(movement::world_rotation, use_head ? head : gc_target(body_root));
      Quat facing{};
      if (Unit(bone) && FacingRotation(Rotate(bone, use_head ? head_forward : body_forward), Rotate(bone, use_head ? head_up : body_up), &facing)) {
        float reference_yaw = movement::held_yaw;
        if (!movement::attached) {
          void* model = Invoke(transform_method, gc_target(model_root));
          const Vec3 forward = Rotate(movement::Value<Quat>(movement::world_rotation, model), {0, 0, 1});
          reference_yaw = std::atan2(forward.x, forward.z) * 57.295779513f;
        }
        facing = AxisAngle({0, 1, 0}, -reference_yaw) * facing;
        const Vec3 forward = Rotate(facing, {0, 0, 1});
        const float yaw = std::atan2(forward.x, forward.z) * 57.295779513f;
        const float pitch = use_head ? -std::atan2(forward.y, std::hypot(forward.x, forward.z)) * 57.295779513f : 0.f;
        full_motion = v.realism;
        offset = BlendRotation({0, 0, 0, 1}, full_motion ? facing : AxisAngle({0, 1, 0}, yaw) * AxisAngle({1, 0, 0}, pitch), v.animation_motion);
      }
    }
    if (keep_direction || !(v.body_facing || v.head_facing || v.realism) || v.animation_motion <= 0.f) {
      animation_offset = {0, 0, 0, 1};
      motion_time = 0;
      motion_frame = -1;
    } else {
      if (motion_frame != captured_frame) {
        const double now = movement::ClockSeconds();
        const float dt = motion_time ? std::clamp(float(now - motion_time), 0.f, .1f) : 0.f;
        frame_start_offset = animation_offset;
        frame_alpha = -std::expm1(-15.f * dt);
        motion_time = now;
        motion_frame = captured_frame;
      }

      animation_offset = BlendRotation(frame_start_offset, offset, frame_alpha);
    }

    const Quat view = input_view * animation_offset;
    const Vec3 forward = Rotate(view, {0, 0, 1});
    const float length = std::hypot(forward.x, forward.z);
    const Vec3 planar = length > .001f ? Vec3{forward.x / length, 0, forward.z / length} : Vec3{0, 0, 1};
    const Vec3 position = eyes + (full_motion ? Rotate(view, {0, v.eye_height, v.eye_forward}) : Vec3{0, v.eye_height, 0} + planar * v.eye_forward) + Vec3{0, v.height, 0}
                          + Rotate(view, {1, 0, 0}) * v.horizontal;
    void* transform = Invoke(movement::component_transform, camera);
    if (!transform || !Unit(view) || !Finite(position)) return;
    set_position(transform, &position);
    set_rotation(transform, &view);
    written_view = view;
    wrote_view = true;
  } catch (...) {
    static bool reported = false;
    if (!reported) {
      reported = true;
      Log(reshade::log::level::warning, "E_E_FPV: animated head camera update rejected a managed call");
    }
  }
}

inline void HookedTailTick(void* manager, float dt, MethodInfo* method) {
  tail_tick(manager, dt, method);
  void* active_manager = nullptr;
  if (!Context(ReadValues(), &active_manager) || active_manager != manager) return;

  if (movement::attached && movement::effects::animator_root.Get()) {
    try {
      movement::Root animator(movement::effects::animator_root.Get());
      movement::Root model(movement::Call(movement::component_transform, animator.Get()));
      movement::effects::Update(animator.Get(), model.Get());
    } catch (...) {
      static bool reported = false;
      if (!reported) {
        reported = true;
        Log(reshade::log::level::warning, "E_E_FPV: late animation attachment refresh rejected");
      }
    }
  }
  if (camera_root && captured_frame >= 0) Apply(gc_target(camera_root));
}

inline void HookedInputTick(void* manager, float dt, MethodInfo* method) {
  freecam::Maintain();

  RestoreControlView();
  movement::RestoreSimulationPose();
  input_tick(manager, dt, method);
}

inline bool Resolve(Il2CppImage unity, Il2CppImage cine, ResolveICall icall) {
  const auto module = GetModuleHandleW(L"GameAssembly.dll");
  output_camera = FindMethod(cine, "Cinemachine", "CinemachineBrain", "get_OutputCamera", 0);
  frame_number = FindMethod(unity, "UnityEngine", "Time", "get_frameCount", 0);
  get_components = FindMethod(unity, "UnityEngine", "GameObject", "GetComponentsInChildren", 2);
  get_bones = FindMethod(unity, "UnityEngine", "SkinnedMeshRenderer", "get_bones", 0);
  get_mesh = FindMethod(unity, "UnityEngine", "SkinnedMeshRenderer", "get_sharedMesh", 0);
  get_bindposes = FindMethod(unity, "UnityEngine", "Mesh", "get_bindposes", 0);
  skin_class = class_from_name(unity, "UnityEngine", "SkinnedMeshRenderer");
  set_position = reinterpret_cast<decltype(set_position)>(icall("UnityEngine.Transform::set_position_Injected(UnityEngine.Vector3&)"));
  set_rotation = reinterpret_cast<decltype(set_rotation)>(icall("UnityEngine.Transform::set_rotation_Injected(UnityEngine.Quaternion&)"));
  return output_camera && frame_number && get_components && get_bones && get_mesh && get_bindposes && skin_class
         && set_position && set_rotation
         && ResolveExport(module, "il2cpp_class_get_type", &class_type)
         && ResolveExport(module, "il2cpp_type_get_object", &type_object)
         && ResolveExport(module, "il2cpp_array_length", &array_length);
}
}
