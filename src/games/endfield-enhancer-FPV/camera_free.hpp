#pragma once

#include "./fpv.hpp"

namespace freecam {
inline std::atomic_bool requested{false}, active{false}, available{false};
inline float speed = 5.f;
inline Il2CppMethod player_controller, add_mask, remove_mask, get_mask;
inline uint32_t player_root = 0;
inline uint32_t mask_handle = 0;
inline bool owns_mask = false;
inline Vec3 position{};
inline float heading = 0.f, elevation = 0.f, lens = 60.f;
inline double last_tick = 0;
inline std::atomic_bool read_mouse{false};
inline std::atomic_long mouse_x{0}, mouse_y{0};
inline HHOOK mouse_hook = nullptr;
inline DWORD mouse_thread = 0;
inline SRWLOCK cursor_bounds_lock = SRWLOCK_INIT;
inline RECT cursor_bounds{};
inline HWND cursor_window = nullptr;
inline decltype(&ClipCursor) native_clip_cursor = nullptr;
inline bool clip_owned = false;
inline RECT applied_clip{};

inline void ReleaseClip() {
  AcquireSRWLockExclusive(&cursor_bounds_lock);
  RECT current{};
  if (clip_owned && native_clip_cursor && GetClipCursor(&current)
      && EqualRect(&current, &applied_clip)) native_clip_cursor(nullptr);
  clip_owned = false;
  ReleaseSRWLockExclusive(&cursor_bounds_lock);
}

inline void OnCursorPresent(reshade::api::effect_runtime*) {
  if (!native_clip_cursor)
    native_clip_cursor = reinterpret_cast<decltype(native_clip_cursor)>(GetProcAddress(GetModuleHandleW(L"win32u.dll"), "NtUserClipCursor"));
  AcquireSRWLockShared(&cursor_bounds_lock);
  const RECT bounds = cursor_bounds;
  const HWND window = cursor_window;
  ReleaseSRWLockShared(&cursor_bounds_lock);
  if (!read_mouse.load() || !active.load() || !requested.load() || !window
      || GetForegroundWindow() != window || bounds.right <= bounds.left || bounds.bottom <= bounds.top) {
    ReleaseClip();
    return;
  }
  AcquireSRWLockExclusive(&cursor_bounds_lock);
  RECT actual{};
  const bool success = native_clip_cursor && native_clip_cursor(&bounds)
                       && GetClipCursor(&actual) && EqualRect(&actual, &bounds);
  if (success) {
    applied_clip = bounds;
    clip_owned = true;
  }
  ReleaseSRWLockExclusive(&cursor_bounds_lock);
  static bool reported_failure = false;
  if (!success && !reported_failure)
    Log(reshade::log::level::warning, "E_E_FPV: free camera native cursor clip failed verification");
  reported_failure = !success;
}

inline LRESULT CALLBACK MouseMessages(int code, WPARAM removed, LPARAM message) {
  if (code == HC_ACTION && removed == PM_REMOVE && read_mouse.load(std::memory_order_relaxed)) {
    const auto* msg = reinterpret_cast<const MSG*>(message);
    if (msg->message == WM_INPUT) {
      RAWINPUT raw{};
      UINT size = sizeof(raw);
      if (GetRawInputData(reinterpret_cast<HRAWINPUT>(msg->lParam), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) != UINT(-1)
          && raw.header.dwType == RIM_TYPEMOUSE && !(raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
        mouse_x.fetch_add(raw.data.mouse.lLastX, std::memory_order_relaxed);
        mouse_y.fetch_add(raw.data.mouse.lLastY, std::memory_order_relaxed);
      }
    }
  }
  return CallNextHookEx(nullptr, code, removed, message);
}

inline void StopMouse() {
  read_mouse.store(false);
  ReleaseClip();
  if (mouse_hook) UnhookWindowsHookEx(mouse_hook);
  mouse_hook = nullptr;
  mouse_thread = 0;
  mouse_x.store(0);
  mouse_y.store(0);
  AcquireSRWLockExclusive(&cursor_bounds_lock);
  cursor_window = nullptr;
  ReleaseSRWLockExclusive(&cursor_bounds_lock);
}
struct Input {
  Vec3 move{};
  float yaw = 0.f, pitch = 0.f, speed = 5.f;
  ULONGLONG stamp = 0;
};
inline Input input;
inline SRWLOCK input_lock = SRWLOCK_INIT;

inline void Release() {
  read_mouse.store(false);
  active.store(false);
  last_tick = 0;
  fpv::active.store(false, std::memory_order_relaxed);
  fpv::ResyncModifiers();
  if (owns_mask) {
    try {
      void* args[]{&mask_handle};

      movement::Call(remove_mask, gc_target(player_root), args);
      owns_mask = false;
    } catch (...) {
      return;
    }
  }
  if (player_root) gc_free(player_root);
  player_root = 0;
}

inline void Maintain() {
  if (!requested.load() && !player_root) return;
  movement::game_thread = GetCurrentThreadId();
  void* controller = Context(ReadValues());
  if (!requested.load() || !available.load() || !controller
      || (object_class(controller) != level_class && object_class(controller) != free_class)) {
    requested.store(false);
    Release();
  }
}

inline bool Apply(void* brain, void* state, MethodInfo* method) {
  if (!requested.load() || !available.load()) return false;
  if (owns_mask && !active.load()) {
    requested.store(false);
    Release();
    return false;
  }
  try {
    void* player = movement::Call(player_controller);
    if (!player || (player_root && gc_target(player_root) != player)) {
      requested.store(false);
      Release();
      return false;
    }
    if (!active.load()) {
      position = Read<Vec3>(state, 0x80) + Read<Vec3>(state, 0xac);
      Quat rotation = Read<Quat>(state, 0x8c) * Read<Quat>(state, 0xb8);
      void* camera = Invoke(motion::output_camera, brain);
      void* transform = camera ? Invoke(movement::component_transform, camera) : nullptr;
      if (transform) {
        position_injected(transform, &position);
        rotation = movement::Value<Quat>(movement::world_rotation, transform);
      }
      if (!Finite(position) || !Unit(rotation)) throw std::runtime_error("Invalid camera pose");
      const Vec3 forward = Rotate(rotation, {0, 0, 1});
      heading = std::atan2(forward.x, forward.z) * 57.295779513f;
      elevation = std::clamp(-std::atan2(forward.y, std::hypot(forward.x, forward.z)) * 57.295779513f, -89.f, 89.f);
      const Values settings = ReadValues();
      lens = settings.first_person ? settings.first_person_fov : settings.fov;
      player_root = gc_new(player, false);
      if (!player_root) throw std::runtime_error("Unable to root player controller");
      int32_t mask = 0;
      void* args[]{&mask};
      void* result = movement::Call(add_mask, gc_target(player_root), args);
      if (!result) throw std::runtime_error("Missing input mask handle");
      mask_handle = *static_cast<uint32_t*>(object_unbox(result));
      owns_mask = true;
      if (movement::Value<int32_t>(get_mask, gc_target(player_root)) != 0)
        throw std::runtime_error("Player input mask was not applied");
      movement::Release();
      mesh_runtime::UpdateBinding(false);
      ReleaseHead();
      dialogue::ResetView();
      uncensor::force_body_visible.store(false, std::memory_order_relaxed);
      AcquireSRWLockExclusive(&input_lock);
      input = {};
      ReleaseSRWLockExclusive(&input_lock);
      if (fpv::requested.load(std::memory_order_relaxed)) fpv::Reset(position, heading);
      active.store(true);
    }
    AcquireSRWLockExclusive(&input_lock);
    const Input sample = input;
    input.yaw = input.pitch = 0.f;
    ReleaseSRWLockExclusive(&input_lock);
    const double now = movement::ClockSeconds();
    const float dt = last_tick ? std::clamp(float(now - last_tick), 0.f, .05f) : 0.f;
    last_tick = now;
    if (fpv::requested.load(std::memory_order_relaxed)) {
      if (fpv::anchor_enabled >= .5f) {
        fpv::anchor::Update(Invoke(motion::output_camera, brain));
        fpv::anchor::ResolveGSC();
        fpv::anchor::ResolvePCU();
      }
      fpv::Tick(dt, sample.move, sample.yaw, sample.pitch);
      position = fpv::position;
    } else if (GetTickCount64() - sample.stamp < 250) {
      heading = std::remainder(heading + sample.yaw, 360.f);
      elevation = std::clamp(elevation + sample.pitch, -89.f, 89.f);
      const Quat view = AxisAngle({0, 1, 0}, heading) * AxisAngle({1, 0, 0}, elevation);
      Vec3 direction = Rotate(view, {sample.move.x, 0, sample.move.z}) + Vec3{0, sample.move.y, 0};
      const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
      if (length > 1.f) direction = direction * (1.f / length);
      position = position + direction * (sample.speed * dt);
    }
    alignas(16) std::array<uint8_t, 0x120> copy;
    std::memcpy(copy.data(), state, copy.size());
    Write(copy.data(), 0x80, position);
    Write(copy.data(), 0x8c, fpv::requested.load(std::memory_order_relaxed)
                                  ? fpv::attitude * AxisAngle({1, 0, 0}, -fpv::cam_pitch)
                                  : AxisAngle({0, 1, 0}, heading) * AxisAngle({1, 0, 0}, elevation));
    Write(copy.data(), 0xac, Vec3{});
    Write(copy.data(), 0xb8, Quat{0, 0, 0, 1});
    Write(copy.data(), 0x20, lens);
    Write(copy.data(), 0x30, 0.f);
    push_state(brain, copy.data(), method);
    return true;
  } catch (...) {
    requested.store(false);
    Release();
    Log(reshade::log::level::warning, "E_E_FPV: free camera refused a pose or managed input operation");
    return false;
  }
}

inline bool Resolve(Il2CppImage game, int32_t (*value_size)(void*, uint32_t*),
                    void* (*class_from_type)(const void*), const void* (*return_type)(Il2CppMethod),
                    uint32_t (*method_flags)(Il2CppMethod, uint32_t*)) {
  player_controller = FindMethod(game, "Beyond.Gameplay", "GameInstance", "get_playerController", 0);
  add_mask = FindMethod(game, "Beyond.Gameplay.Core", "PlayerController", "AddActionEnableMask", 1);
  remove_mask = FindMethod(game, "Beyond.Gameplay.Core", "PlayerController", "RemoveActionEnableMask", 1);
  get_mask = FindMethod(game, "Beyond.Gameplay.Core", "PlayerController", "get_playerActionEnableMask", 0);
  const void* (*parameter)(Il2CppMethod, uint32_t) = nullptr;
  if (!player_controller || !add_mask || !remove_mask || !get_mask
      || !ResolveExport(GetModuleHandleW(L"GameAssembly.dll"), "il2cpp_method_get_param", &parameter)) return false;
  auto system = FindImage("mscorlib.dll");
  uint32_t alignment = 0;
  return (method_flags(player_controller, nullptr) & 0x10)
         && !(method_flags(add_mask, nullptr) & 0x10) && !(method_flags(remove_mask, nullptr) & 0x10)
         && class_from_type(return_type(player_controller)) == class_from_name(game, "Beyond.Gameplay.Core", "PlayerController")
         && class_from_type(return_type(add_mask)) == class_from_name(system, "System", "UInt32")
         && class_from_type(parameter(remove_mask, 0)) == class_from_name(system, "System", "UInt32")
         && class_from_type(return_type(remove_mask)) == class_from_name(system, "System", "Boolean")
         && class_from_type(parameter(add_mask, 0)) == class_from_type(return_type(get_mask))
         && value_size(class_from_type(parameter(add_mask, 0)), &alignment) == 4;
}

struct Controls {
  bool exit = false, forward = false, backward = false, left = false, right = false;
  bool down = false, up = false, boost = false;
};
inline void OnFrame(reshade::api::effect_runtime* runtime, const Controls& controls, bool allow_input = true) {
  // FPV HUD遥测 → 交换链payload
  {
    auto& v = endfield::fpv_swapchain::hud_payload.v;
    // HUD 只要"请求了FPV"就显示: active 由游戏线程的相机钩子置位, 钩子还没接管相机时
    // 会让整块 OSD 直接消失, 所以不拿它当可见性条件
    v[8] = fpv::requested.load(std::memory_order_relaxed) ? fpv::hud_master : 0.f;
    v[9] = fpv::radio::vis_yaw.load();
    v[10] = fpv::radio::vis_thr.load() * 2.f - 1.f;
    v[11] = fpv::radio::vis_roll.load();
    v[12] = fpv::radio::vis_pitch.load();
    // 视角要用摄像头姿态: 相机 = 机体姿态 × 绕机体X轴的 -cam_pitch (与写回相机姿态的公式一致)
    // 否则调了摄像头上仰角之后, 地平线会按机体而不是按镜头算, 位置偏掉
    const Quat view_attitude = fpv::attitude * AxisAngle({1.f, 0.f, 0.f}, -fpv::cam_pitch);
    const Vec3 fwd = Rotate(view_attitude, {0.f, 0.f, 1.f});
    const Vec3 rgt = Rotate(view_attitude, {1.f, 0.f, 0.f});
    // v[13] = HUD 元素开关位掩码: bit0 中心准星, bit1 人工地平线, bit2 摇杆方框
    v[13] = (fpv::hud_crosshair >= .5f ? 1.f : 0.f)
            + (fpv::horizon_on >= .5f ? 2.f : 0.f)
            + (fpv::hud_sticks >= .5f ? 4.f : 0.f);
    // v[14] 不再传俯仰角, 这里直接折算成"屏幕高度的比例", 着色器只乘 screen.y
    // off = 0.5 * tan(pitch) / tan(fov_v/2), 与小孔成像的竖直投影一致
    const float pitch_deg = std::clamp(std::asin(std::clamp(fwd.y, -1.f, 1.f)) * 57.2957795f, -75.f, 75.f);
    const float tan_half_fov = std::tan(std::clamp(fpv::horizon_fov, 40.f, 150.f) * 0.5f * 0.01745329252f);
    v[14] = 0.5f * std::tan(pitch_deg * 0.01745329252f) / tan_half_fov;
    v[15] = std::atan2(rgt.y, std::hypot(rgt.x, rgt.z)) * 57.2957795f;
    v[4] = fpv::hud_cx;
    v[5] = fpv::hud_cy;
    v[16] = std::clamp(fpv::hud_pad, 16.f, 240.f);
    v[17] = std::clamp(fpv::hud_gap, 0.f, 80.f);
  }

  const bool focused = runtime->get_hwnd() == GetForegroundWindow();
  if (!focused || !ReadValues().enabled || !available.load()) requested.store(false);
  if (allow_input && !ImGui::GetIO().WantCaptureKeyboard && controls.exit) requested.store(false);
  Input sample;
  sample.stamp = GetTickCount64();
  const bool looking = focused && requested.load() && active.load() && allow_input
                       && !ImGui::GetIO().WantCaptureKeyboard && !ImGui::GetIO().WantCaptureMouse;
  if (looking && (!mouse_hook || mouse_thread != GetWindowThreadProcessId(static_cast<HWND>(runtime->get_hwnd()), nullptr))) {
    StopMouse();
    mouse_thread = GetWindowThreadProcessId(static_cast<HWND>(runtime->get_hwnd()), nullptr);
    if (mouse_thread) mouse_hook = SetWindowsHookExW(WH_GETMESSAGE, MouseMessages, endfield::runtime_status::addon_module, mouse_thread);
    if (!mouse_hook)
      Log(reshade::log::level::warning, "E_E_FPV: free camera relative mouse listener unavailable");
  }
  RECT bounds{};
  HWND window = static_cast<HWND>(runtime->get_hwnd());
  if (looking && GetClientRect(window, &bounds)) {
    POINT top_left{bounds.left, bounds.top}, bottom_right{bounds.right, bounds.bottom};
    if (ClientToScreen(window, &top_left) && ClientToScreen(window, &bottom_right)) {
      LogicalToPhysicalPointForPerMonitorDPI(window, &top_left);
      LogicalToPhysicalPointForPerMonitorDPI(window, &bottom_right);
      bounds = {top_left.x, top_left.y, bottom_right.x, bottom_right.y};
    } else
      window = nullptr;
  } else
    window = nullptr;
  AcquireSRWLockExclusive(&cursor_bounds_lock);
  cursor_bounds = bounds;
  cursor_window = window;
  ReleaseSRWLockExclusive(&cursor_bounds_lock);
  const bool was_looking = read_mouse.exchange(looking && mouse_hook);
  const LONG dx = mouse_x.exchange(0), dy = mouse_y.exchange(0);
  if (focused && requested.load() && active.load()) {
    runtime->block_input_next_frame();
    fpv::kbd_boost = allow_input && controls.boost;  // 键鼠模式下用来加速油门变化
    // 键鼠自由飞的垂直输入: 空格上升, Ctrl 或 Shift 下降
    if (allow_input && !ImGui::GetIO().WantCaptureKeyboard) {
      const float up = runtime->is_key_down(VK_SPACE) ? 1.f : 0.f;
      const float down = (runtime->is_key_down(VK_CONTROL) || runtime->is_key_down(VK_SHIFT)) ? 1.f : 0.f;
      fpv::ptr_vertical = up - down;
    } else {
      fpv::ptr_vertical = 0.f;
    }
    if (allow_input && !ImGui::GetIO().WantCaptureKeyboard && !ImGui::GetIO().WantCaptureMouse) {
      sample.move = {float(controls.right) - float(controls.left),
                     float(controls.up) - float(controls.down),
                     float(controls.forward) - float(controls.backward)};
      sample.speed = std::isfinite(speed) ? std::clamp(speed, .1f, 50.f) : 5.f;
      if (controls.boost) sample.speed *= 4.f;
      if (was_looking) {
        sample.yaw = float(dx) * .15f;
        sample.pitch = float(dy) * .15f;
      }
    }
  }
  AcquireSRWLockExclusive(&input_lock);
  sample.yaw += input.yaw;
  sample.pitch += input.pitch;
  input = sample;
  ReleaseSRWLockExclusive(&input_lock);
  if (fpv::requested.load(std::memory_order_relaxed)
      && !fpv::radio::running.load(std::memory_order_relaxed)) {
    fpv::di::target_hwnd.store(runtime->get_hwnd());
    fpv::radio::Start();
  }
}
}
