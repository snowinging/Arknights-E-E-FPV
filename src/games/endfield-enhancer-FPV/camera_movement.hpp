#pragma once

namespace movement {
inline std::atomic<DWORD> game_thread{0};
inline std::atomic_bool attached{false};
inline Il2CppMethod started, released, paused, cinematic, get_movement;
inline Il2CppMethod animated_move, block_movement, base_controller, script_controlled;
inline Il2CppMethod player_controller, raw_move_axis;
inline Il2CppMethod get_rotator, entity_rotation, set_facing;
struct MoveAxes {
  float x, y;
};
inline void* character_controller_class = nullptr;
inline bool (*assignable)(void*, void*) = nullptr;
inline size_t input_offset = 0;
inline float lateral_yaw = 0.f;
inline float held_yaw = 0.f;
inline bool interaction_alignment = false;
inline float lateral_target = 0.f;
inline float turn_start = 0.f, turn_time = .35f;
inline float turn_velocity = 0.f, turn_start_velocity = 0.f;
inline double facing_time = 0;
inline Il2CppMethod get_component, get_animator, component_transform, find_transform, parent_transform;
inline Il2CppMethod local_rotation, world_rotation, set_local_rotation, set_world_rotation, object_alive;
inline void* (*new_string)(const char*) = nullptr;

inline double ClockSeconds() {
  static const double frequency = [] {
    LARGE_INTEGER value{};
    QueryPerformanceFrequency(&value);
    return static_cast<double>(value.QuadPart);
  }();
  LARGE_INTEGER counter{};
  QueryPerformanceCounter(&counter);
  return static_cast<double>(counter.QuadPart) / frequency;
}

inline void* Call(Il2CppMethod method, void* object = nullptr, void** args = nullptr) {
  if (!method) throw std::runtime_error("Movement presentation API unavailable");
  void* exception = nullptr;
  void* result = runtime_invoke(method, object, args, &exception);
  if (exception) throw std::runtime_error("Movement presentation call rejected");
  return result;
}
template <typename T>
inline T Value(Il2CppMethod method, void* object) {
  void* result = Call(method, object);
  if (!result) throw std::runtime_error("Movement presentation value unavailable");
  return *static_cast<T*>(object_unbox(result));
}
struct Root {
  uint32_t handle = 0;
  Root() = default;
  explicit Root(void* object) : handle(object ? gc_new(object, false) : 0) {
    if (object && !handle) throw std::runtime_error("Movement reference allocation failed");
  }
  Root(const Root&) = delete;
  Root& operator=(const Root&) = delete;
  Root(Root&& other) noexcept : handle(std::exchange(other.handle, 0)) {}
  Root& operator=(Root&& other) noexcept {
    if (handle) gc_free(handle);
    handle = std::exchange(other.handle, 0);
    return *this;
  }
  ~Root() {
    if (handle) gc_free(handle);
  }
  void* Get() const { return handle ? gc_target(handle) : nullptr; }
};
inline Root visual_entity, visual_transform;
#include "./camera_locomotion.hpp"
inline Quat original_visual{0, 0, 0, 1}, written_visual{0, 0, 0, 1};
inline bool Alive(void* object) {
  if (!object) return false;
  void* args[]{object};
  void* result = Call(object_alive, nullptr, args);
  return result && *static_cast<bool*>(object_unbox(result));
}
inline bool SameRotation(Quat a, Quat b) {
  return Unit(a) && Unit(b) && std::abs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w) > .99999f;
}
inline bool RestoreSimulationPose() {
  try {
    if (Alive(visual_transform.Get()) && SameRotation(Value<Quat>(local_rotation, visual_transform.Get()), written_visual)) {
      void* args[]{&original_visual};
      Call(set_local_rotation, visual_transform.Get(), args);
    }
    return true;
  } catch (...) {
    return false;
  }
}
#include "./camera_effects.hpp"
inline bool RestoreVisual() {
  try {
    if (!RestoreSimulationPose()) return false;
    effects::Restore();
    visual_transform = Root();
    visual_entity = Root();
    attached = false;
    lateral_yaw = 0.f;
    lateral_target = 0.f;
    turn_start = 0.f;
    turn_time = .35f;
    turn_velocity = turn_start_velocity = 0.f;
    locomotion::reversed = false;
    locomotion::blend_target = 0;
    held_yaw = 0.f;
    facing_time = 0;
    return true;
  } catch (...) {
    return false;
  }
}
inline void Release() { RestoreVisual(); }
inline void Update(bool active, Vec3 view, float look_limit = 60.f) {
  game_thread = GetCurrentThreadId();
  interaction_alignment = false;
  try {
    Root entity(active ? Call(character_method) : nullptr);
    if (!entity.Get() || !Value<bool>(started, entity.Get()) || Value<bool>(released, entity.Get())
        || Value<bool>(paused, entity.Get()) || Value<bool>(cinematic, entity.Get())
        || !Finite(view) || std::hypot(view.x, view.z) < .001f) {
      Release();
      return;
    }
    Root controller(Call(base_controller, entity.Get()));
    Root movement(Call(get_movement, entity.Get()));
    Root input(movement.Get() ? Read<void*>(movement.Get(), input_offset) : nullptr);

    if (!controller.Get() || !movement.Get() || !input.Get()
        || !assignable(character_controller_class, object_class(controller.Get()))) {
      Release();
      return;
    }
    interaction_alignment = Value<bool>(script_controlled, controller.Get()) || Value<bool>(block_movement, movement.Get());
    if (interaction_alignment || Value<bool>(animated_move, input.Get())) {
      Release();
      return;
    }
    Root component(Call(get_component, entity.Get()));
    Root animator(component.Get() ? Call(get_animator, component.Get()) : nullptr);
    Root animator_transform(animator.Get() ? Call(component_transform, animator.Get()) : nullptr);
    Root path(new_string("Root"));
    void* find_args[]{path.Get()};
    Root skeleton(Alive(animator_transform.Get()) ? Call(find_transform, animator_transform.Get(), find_args) : nullptr);

    if (!Alive(skeleton.Get()) || skeleton.Get() == animator_transform.Get()
        || Call(parent_transform, skeleton.Get()) != animator_transform.Get()) {
      Release();
      return;
    }
    if (visual_entity.Get() != entity.Get() || visual_transform.Get() != skeleton.Get()) {
      if (!RestoreVisual()) return;
      original_visual = Value<Quat>(local_rotation, skeleton.Get());
      if (!Unit(original_visual)) return;
      visual_entity = std::move(entity);
      visual_transform = std::move(skeleton);
      written_visual = original_visual;
    }
    const Quat local = Value<Quat>(local_rotation, visual_transform.Get());
    const Quat world = Value<Quat>(world_rotation, visual_transform.Get());
    if (!Unit(local) || !Unit(world)) {
      Release();
      return;
    }
    if (!SameRotation(local, written_visual)) original_visual = local;
    Root player(Call(player_controller));
    if (!player.Get()) {
      Release();
      return;
    }
    const MoveAxes axes = Value<MoveAxes>(raw_move_axis, player.Get());

    const Vec3 move{axes.x, 0, axes.y};
    const double now = ClockSeconds();
    const float elapsed = facing_time ? std::clamp(float(now - facing_time), 0.f, .1f) : 0.f;
    facing_time = now;
    const float view_yaw = std::atan2(view.x, view.z) * 57.295779513f;
    if (!attached) held_yaw = view_yaw;
    if (Finite(move) && std::hypot(move.x, move.z) > .01f) {
      const float target = LateralFacingYaw(move, {0, 0, 1});
      if (target * lateral_target < 0.f && std::abs(target) > 5.f && std::abs(lateral_target) > 5.f) {
        locomotion::reversed = true;
      }
      if (std::abs(target - lateral_target) > .1f) {
        turn_start = lateral_yaw;
        turn_start_velocity = turn_velocity;
        turn_time = 0.f;
      }
      lateral_target = target;
      if (std::abs(target) < 5.f) {
        locomotion::reversed = false;
        locomotion::blend_target = 0;
      }
      locomotion::InterruptTurn(animator.Get());

      if (turn_time < .35f) {
        turn_time = std::min(turn_time + elapsed, .35f);
        const float t = turn_time / .35f;

        lateral_yaw = turn_start + (target - turn_start) * (t * t * (3.f - 2.f * t))
                      + .35f * turn_start_velocity * t * (1.f - t) * (1.f - t);
        turn_velocity = (target - turn_start) * (6.f * t * (1.f - t) / .35f)
                        + turn_start_velocity * (1.f - 4.f * t + 3.f * t * t);
      } else {
        lateral_yaw += (target - lateral_yaw) * (-std::expm1(-16.f * elapsed));
        turn_velocity = 16.f * (target - lateral_yaw);
      }
      held_yaw = view_yaw + lateral_yaw;
    } else {
      held_yaw = view_yaw - std::clamp(std::remainder(view_yaw - held_yaw, 360.f), -look_limit, look_limit);
      lateral_yaw = std::remainder(held_yaw - view_yaw, 360.f);
      lateral_target = 0.f;
      turn_time = .35f;
      turn_velocity = turn_start_velocity = 0.f;
      locomotion::reversed = false;
      locomotion::blend_target = 0;

      if (!RestoreSimulationPose()) {
        Release();
        return;
      }
      Root rotator(Call(get_rotator, visual_entity.Get()));
      const Quat rotation = Value<Quat>(entity_rotation, visual_entity.Get());
      if (!rotator.Get() || !Unit(rotation)) {
        Release();
        return;
      }
      const Vec3 forward = Rotate(rotation, {0, 0, 1});
      const float delta = std::remainder(held_yaw - std::atan2(forward.x, forward.z) * 57.295779513f, 360.f);
      if (std::abs(delta) > .01f) {
        Quat wanted = AxisAngle({0, 1, 0}, delta) * rotation;
        void* args[]{&wanted};
        Call(set_facing, rotator.Get(), args);
      }
      written_visual = Value<Quat>(local_rotation, visual_transform.Get());
      attached = true;
      effects::Restore();
      return;
    }
    const Vec3 facing = Rotate(world, {0, 0, 1});
    Quat wanted = AxisAngle({0, 1, 0}, std::remainder(held_yaw - std::atan2(facing.x, facing.z) * 57.295779513f, 360.f)) * world;
    void* args[]{&wanted};
    attached = true;
    Call(set_world_rotation, visual_transform.Get(), args);
    written_visual = Value<Quat>(local_rotation, visual_transform.Get());
    if (effects::animator_root.Get() != animator.Get()) {
      effects::Restore();
      effects::animator_root = Root(animator.Get());
    }
  } catch (...) {
    Release();
  }
}
inline bool Resolve(Il2CppImage game, int32_t (*value_size)(void*, uint32_t*), void* (*class_from_type)(const void*)) {
  const HMODULE module = GetModuleHandleW(L"GameAssembly.dll");
  const void* (*parameter)(Il2CppMethod, uint32_t) = nullptr;
  const void* (*return_type)(Il2CppMethod) = nullptr;
  const void* (*field_type)(void*) = nullptr;
  if (!ResolveExport(module, "il2cpp_method_get_param", &parameter)
      || !ResolveExport(module, "il2cpp_method_get_return_type", &return_type)
      || !ResolveExport(module, "il2cpp_field_get_type", &field_type)
      || !ResolveExport(module, "il2cpp_string_new", &new_string)) return false;
  const auto unity = FindImage("UnityEngine.CoreModule.dll"), animation = FindImage("UnityEngine.AnimationModule.dll");
  if (!unity || !animation || !locomotion::Resolve(animation)) return false;
  player_controller = FindMethod(game, "Beyond.Gameplay", "GameInstance", "get_playerController", 0);
  raw_move_axis = FindMethod(game, "Beyond.Gameplay.Core", "PlayerController", "get_rawMoveAxis", 0);
  if (!player_controller || !raw_move_axis) return false;
  uint32_t axes_alignment = 0;
  if (class_from_type(return_type(player_controller)) != class_from_name(game, "Beyond.Gameplay.Core", "PlayerController")
      || class_from_type(return_type(raw_move_axis)) != class_from_name(unity, "UnityEngine", "Vector2")
      || value_size(class_from_type(return_type(raw_move_axis)), &axes_alignment) != sizeof(MoveAxes)) return false;
  get_rotator = FindMethod(game, "Beyond.Gameplay.Core", "Entity", "get_rotateCom", 0);
  entity_rotation = FindMethod(game, "Beyond.Gameplay.Core", "Entity", "get_rotation", 0);
  set_facing = FindMethod(game, "Beyond.Gameplay.Core", "RotatorComponent", "SetRotation", 1);
  if (!get_rotator || !entity_rotation || !set_facing
      || class_from_type(return_type(get_rotator)) != class_from_name(game, "Beyond.Gameplay.Core", "RotatorComponent")
      || class_from_type(return_type(entity_rotation)) != class_from_name(unity, "UnityEngine", "Quaternion")
      || class_from_type(parameter(set_facing, 0)) != class_from_name(unity, "UnityEngine", "Quaternion")
      || class_from_type(return_type(set_facing)) != class_from_name(FindImage("mscorlib.dll"), "System", "Void")) return false;
  started = FindMethod(game, "Beyond.Gameplay.Core", "Entity", "get_markStarted", 0);
  released = FindMethod(game, "Beyond.Gameplay.Core", "Entity", "get_markReleased", 0);
  paused = FindMethod(game, "Beyond.Gameplay.Core", "Entity", "get_isPaused", 0);
  get_movement = FindMethod(game, "Beyond.Gameplay.Core", "Entity", "get_movementComponent", 0);
  animated_move = FindMethod(game, "Beyond.Gameplay.Core", "MoveInput", "get_hasAnimatedMove", 0);
  block_movement = FindMethod(game, "Beyond.Gameplay.Core", "MovementComponent", "get_blockGroundedMove", 0);
  base_controller = FindMethod(game, "Beyond.Gameplay.Core", "Entity", "get_baseController", 0);
  script_controlled = FindMethod(game, "Beyond.Gameplay.Core", "CharacterController", "get_isScriptControlled", 0);
  character_controller_class = class_from_name(game, "Beyond.Gameplay.Core", "CharacterController");
  if (!character_controller_class || !ResolveExport(module, "il2cpp_class_is_assignable_from", &assignable)) return false;
  for (auto method : {animated_move, block_movement, script_controlled})
    if (!method || class_from_type(return_type(method)) != class_from_name(FindImage("mscorlib.dll"), "System", "Boolean")) return false;
  if (!base_controller) return false;
  void* input_field = class_get_field_from_name(class_from_name(game, "Beyond.Gameplay.Core", "MovementComponent"), "input");
  if (!input_field || class_from_type(field_type(input_field)) != class_from_name(game, "Beyond.Gameplay.Core", "MoveInput")) return false;
  input_offset = field_get_offset(input_field);
  if (input_offset < 0x10 || input_offset > 0x400) return false;
  cinematic = FindMethod(game, "Beyond.Gameplay.Core", "Entity", "get_inCinematic", 0);
  get_component = FindMethod(game, "Beyond.Gameplay.Core", "Entity", "get_animatorCom", 0);
  get_animator = FindMethod(game, "Beyond.Gameplay.View", "ComplexAnimatorComponent", "get_animator", 0);
  component_transform = FindMethod(unity, "UnityEngine", "Component", "get_transform", 0);
  find_transform = FindMethod(unity, "UnityEngine", "Transform", "Find", 1);
  parent_transform = FindMethod(unity, "UnityEngine", "Transform", "get_parent", 0);
  object_alive = FindMethod(unity, "UnityEngine", "Object", "op_Implicit", 1);
  local_rotation = FindMethod(unity, "UnityEngine", "Transform", "get_localRotation", 0);
  world_rotation = FindMethod(unity, "UnityEngine", "Transform", "get_rotation", 0);
  set_local_rotation = FindMethod(unity, "UnityEngine", "Transform", "set_localRotation", 1);
  set_world_rotation = FindMethod(unity, "UnityEngine", "Transform", "set_rotation", 1);
  for (auto entry : {started, released, paused, cinematic, get_movement, get_component, get_animator, component_transform, find_transform, parent_transform,
                     object_alive, local_rotation, world_rotation, set_local_rotation, set_world_rotation})
    if (!entry) return false;
  uint32_t alignment = 0;
  return value_size(class_from_type(return_type(entity_rotation)), &alignment) == sizeof(Quat)
         && value_size(class_from_type(parameter(set_facing, 0)), &alignment) == sizeof(Quat)
         && effects::Resolve(game, unity)
         && value_size(class_from_type(return_type(local_rotation)), &alignment) == sizeof(Quat)
         && value_size(class_from_type(return_type(world_rotation)), &alignment) == sizeof(Quat)
         && value_size(class_from_type(parameter(set_local_rotation, 0)), &alignment) == sizeof(Quat)
         && value_size(class_from_type(parameter(set_world_rotation, 0)), &alignment) == sizeof(Quat);
}
}
