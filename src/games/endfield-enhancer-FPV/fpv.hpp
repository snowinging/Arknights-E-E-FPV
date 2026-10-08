#pragma once

// FPV 穿越机模块: 在 freecam 相机控制之上叠加无人机物理
// 姿态 = 四元数, 摇杆命令角度(自稳)/速率(手动), 油门沿机头向上推力, 重力+阻力
// 输入: winmm HID 遥控器轮询线程 (Windows/Wine 通吃) 或 键鼠虚拟摇杆(测试用)

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#ifndef DIRECTINPUT_VERSION
#define DIRECTINPUT_VERSION 0x0800
#endif
#include <windows.h>
#include <joystickapi.h>
#include <dinput.h>

#include <include/reshade.hpp>

#include "./config_store.hpp"

// camera_math.hpp 只提供了加法和标量乘, 本模块自备减法
inline Vec3 Sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }

namespace fpv {
// ---- 可调参数(overlay 绑定) ----
inline float mode = 0.f;              // 0=Angle 1=Horizon 2=Acro
inline float source = 0.f;            // 0=Auto 1=Radio 2=Keyboard/Mouse
inline float gravity = 9.81f;         // m/s^2
inline float thrust = 24.f;           // 满油门加速度 m/s^2 (悬停油门 = gravity/thrust)
inline float drag_linear = 0.02f;     // 线性阻力 1/s
inline float drag_quad = 0.02f;       // 二次阻力 1/m
inline float rate_roll = 360.f;       // acro 最大角速度 deg/s
inline float rate_pitch = 360.f;
inline float rate_yaw = 270.f;
inline float expo = 0.35f;            // 手动模式指数曲线
inline float tilt_max = 60.f;         // 自稳最大倾角 deg
inline float angle_kp = 6.f;          // 自稳 P 增益 (1/s)
inline float anchor_enabled = 0.f;    // 锚点迁移开关(实验): 流式/LOD中心跟随FPV相机
inline float horizon_delay = 0.3f;    // 半自稳: 死区需持续驻留的时长(秒), 0=立即
inline float horizon_hold = 0.f;      // 死区驻留累计
inline float cam_pitch = 0.f;
inline float hud_crosshair = 1.f;        // 着色器HUD: 中心准星
inline float hud_sticks = 1.f;           // 着色器HUD: 摇杆方框
inline float hud_master = 1.f;          // 着色器HUD总开关
inline float hud_cx = 0.82f;            // HUD锚点(归一化)
inline float hud_cy = 0.55f;
inline float hud_pad = 70.f;             // 摇杆方框边长(px)
inline float hud_gap = 10.f;             // 摇杆方框间距(px)
inline float horizon_fov = 100.f;        // 人工地平线换算用的竖直FOV(deg), 需与实际视场接近
inline float horizon_on = 1.f;          // 着色器HUD: 人工地平线
inline float angle_rate_limit = 360.f;
inline float floor_y = -1.f;          // 备用地板高度(有真碰撞后默认关闭), 负数禁用
inline float kbd_throttle_speed = 1.5f; // 键盘油门变化速度 /s
inline float kbd_mouse_sens = 0.0035f;  // 鼠标计数→虚拟杆量 (每计数)
inline float kbd_recenter = 3.5f;       // 虚拟杆量回中速度 1/s (越大回中越快)
inline float kbd_yaw_smooth = 8.f;      // 键盘偏航平滑速度 1/s
// 键鼠虚拟摇杆状态: 鼠标推杆后靠 kbd_recenter 自动回中, 不再用"每帧位移直接当杆量"
inline float vjoy_pitch = 0.f, vjoy_roll = 0.f, vjoy_yaw = 0.f;
inline bool kbd_boost = false;          // 键鼠模式: 按住 boost 键时油门变化更快(camera_free 每帧写入)
// ---- 键鼠自由飞 (mode 3, 与三种姿态模式并列的一套独立操作) ----
inline float ptr_speed = 12.f;          // 水平最大速度 m/s
inline float ptr_vertical_speed = 8.f;  // 垂直最大速度 m/s
inline float ptr_accel = 25.f;          // 加速度 m/s^2 (加速缓冲)
inline float ptr_decel = 12.f;          // 减速度 m/s^2 (减速缓冲)
inline float ptr_mouse_sens = 0.12f;    // 鼠标灵敏度 deg/计数
inline float ptr_yaw = 0.f, ptr_pitch = 0.f;  // 视角朝向, 约定与自由相机一致
inline float ptr_vertical = 0.f;        // 空格上升 / Ctrl 或 Shift 下降, camera_free 每帧写入
inline float ptr_mode_prev = -1.f;      // 切入该模式时用来对齐视角, 免得镜头跳一下
inline int device_id = 0;             // JOYSTICKID
// HID 轴映射: 0=X 1=Y 2=Z 3=R 4=U 5=V
inline int axis_roll = 0, axis_pitch = 1, axis_yaw = 3, axis_throttle = 2;
inline int invert_pitch = 0, invert_roll = 0, invert_yaw = 0, invert_throttle = 0;
inline float deadzone = 0.02f;

// ---- 运行状态 ----
inline Vec3 position{}, velocity{};
inline Quat attitude{0.f, 0.f, 0.f, 1.f};
inline float throttle = 0.f;
inline std::atomic_bool requested{false};   // 用户开关(与 freecam::requested 联动)
inline std::atomic_bool active{false};      // 物理已初始化

// ---- DirectInput8 后端: 轴/滑条/按键分离, 不受 winmm 6 轴限制 ----
namespace di {
inline std::atomic<void*> target_hwnd{nullptr};
inline IDirectInput8W* d8 = nullptr;
inline IDirectInputDevice8W* device = nullptr;
inline std::atomic_bool ready{false};

inline void Shutdown() {
  ready.store(false, std::memory_order_relaxed);
  if (device) { device->Unacquire(); device->Release(); device = nullptr; }
  if (d8) { d8->Release(); d8 = nullptr; }
}

inline bool Init() {
  Shutdown();
  HINSTANCE inst = static_cast<HINSTANCE>(GetModuleHandleW(nullptr));
  if (FAILED(DirectInput8Create(inst, DIRECTINPUT_VERSION, IID_IDirectInput8W,
                                reinterpret_cast<void**>(&d8), nullptr)))
    return false;
  struct Ctx { IDirectInput8W* d8; IDirectInputDevice8W* dev; } ctx{d8, nullptr};
  d8->EnumDevices(DI8DEVCLASS_GAMECTRL,
                  [](LPCDIDEVICEINSTANCEW, LPVOID pv) -> BOOL {
                    auto* c = static_cast<Ctx*>(pv);
                    if (c->dev) return DIENUM_CONTINUE;
                    return SUCCEEDED(c->d8->CreateDevice(GUID_Joystick, &c->dev, nullptr)) ? DIENUM_STOP : DIENUM_CONTINUE;
                  },
                  &ctx, DIEDFL_ATTACHEDONLY);
  if (!ctx.dev) { Shutdown(); return false; }
  device = ctx.dev;
  if (FAILED(device->SetDataFormat(&c_dfDIJoystick2))) { Shutdown(); return false; }
  if (FAILED(device->SetCooperativeLevel(static_cast<HWND>(target_hwnd.load()),
                                         DISCL_NONEXCLUSIVE | DISCL_BACKGROUND))) { Shutdown(); return false; }
  if (FAILED(device->Acquire())) { Shutdown(); return false; }
  ready.store(true, std::memory_order_relaxed);
  return true;
}

inline bool Read(DIJOYSTATE2* out) {
  if (!ready.load(std::memory_order_relaxed) || !device) return false;
  if (FAILED(device->Poll())) device->Acquire();
  return SUCCEEDED(device->GetDeviceState(sizeof(DIJOYSTATE2), out));
}

// 轴索引 0..7: lX lY lZ lRx lRy lRz 滑条0 滑条1
inline float AxisNorm(const DIJOYSTATE2& js, int ax) {
  LONG v = 0;
  switch (ax) {
    case 0: v = js.lX; break;  case 1: v = js.lY; break;
    case 2: v = js.lZ; break;  case 3: v = js.lRx; break;
    case 4: v = js.lRy; break; case 5: v = js.lRz; break;
    case 6: v = js.rglSlider[0]; break; case 7: v = js.rglSlider[1]; break;
    default: return 0.f;
  }
  const float out = ax >= 6 ? (v - 32768.f) / 32768.f : v / 32768.f;
  return std::clamp(out, -1.f, 1.f);
}
} // namespace di

// ---- 遥控器轮询线程 ----
// 注: 原先这里有一层「隐身」——用 IAT 钩子把本进程里的手柄整个藏起来。
// Linux 与 Windows 实测游戏都不会把 HID 遥控器当成手柄, 这层没用, 还会连累玩家自己的手柄, 已整体删除。

inline float Expo(float command, float amount) {
  return amount * command * command * command + (1.f - amount) * command;
}

// 逐轴逼近目标值, 加速和减速用不同斜率(键鼠自由飞的速度缓冲)
inline float Approach1(float current, float target, float accel, float decel, float dt) {
  const bool same_direction = target == 0.f || current == 0.f || ((target > 0.f) == (current > 0.f));
  const bool speeding_up = same_direction && std::abs(target) > std::abs(current);
  const float rate = std::max(speeding_up ? accel : decel, 0.001f);
  return current + std::clamp(target - current, -rate * dt, rate * dt);
}

namespace radio {
inline std::thread worker;
inline std::atomic_bool running{false};
inline std::atomic_bool device_ok{false};
inline std::atomic_bool has_signal{false};  // 本帧轮询到数据
inline std::atomic<float> thr{0.f}, yaw_in{0.f}, pitch_in{0.f}, roll_in{0.f};
inline std::atomic_uint32_t buttons{0};
inline std::atomic_uint32_t raw[8] = {};    // 原始轴值(DI含滑条), overlay 监视用
inline std::atomic_int active_device{-1};   // 实际响应的 JOYSTICKID
inline std::atomic_bool cal_travel{false};  // 行程校准进行中
inline std::atomic_uint64_t travel_until{0};
// 行程校准的扫描缓冲: 单独立一份, 扫的过程里不碰 cal[], 免得标定中途杆量乱跳
inline float cal_scan_lo[8] = {};
inline float cal_scan_hi[8] = {};
inline float cal[8][3] = {};                // 每轴 {最小, 中心, 最大}
inline std::atomic<float> vis_thr{0.f}, vis_yaw{0.f}, vis_pitch{0.f}, vis_roll{0.f};
inline wchar_t device_name[128] = L"";

// ---- 标定与映射的配置持久化 ----
// section 由调用方传入(与 renodx settings 用的段名一致, 否则会和设置项分家)
inline std::atomic_bool cal_loaded{false};  // 已从配置读过标定, Poll 就不要再填默认值

inline void SaveToConfig() {
  using endfield::config_store::SetFloat;
  using endfield::config_store::SetInt;
  char key[32];
  for (int i = 0; i < 8; ++i) {
    std::snprintf(key, sizeof(key), "Cal%dLo", i);
    SetFloat("FPV", key, cal[i][0]);
    std::snprintf(key, sizeof(key), "Cal%dMid", i);
    SetFloat("FPV", key, cal[i][1]);
    std::snprintf(key, sizeof(key), "Cal%dHi", i);
    SetFloat("FPV", key, cal[i][2]);
  }
  SetInt("FPV", "DeviceId", device_id);
  SetInt("FPV", "AxisRoll", axis_roll);
  SetInt("FPV", "AxisPitch", axis_pitch);
  SetInt("FPV", "AxisYaw", axis_yaw);
  SetInt("FPV", "AxisThrottle", axis_throttle);
  SetInt("FPV", "InvertPitch", invert_pitch);
  SetInt("FPV", "InvertRoll", invert_roll);
  SetInt("FPV", "InvertYaw", invert_yaw);
  SetInt("FPV", "InvertThrottle", invert_throttle);
}

inline void LoadFromConfig() {
  using endfield::config_store::GetFloat;
  using endfield::config_store::GetInt;
  char key[32];
  for (int i = 0; i < 8; ++i) {
    std::snprintf(key, sizeof(key), "Cal%dLo", i);
    GetFloat("FPV", key, cal[i][0]);
    std::snprintf(key, sizeof(key), "Cal%dMid", i);
    GetFloat("FPV", key, cal[i][1]);
    std::snprintf(key, sizeof(key), "Cal%dHi", i);
    GetFloat("FPV", key, cal[i][2]);
  }
  GetInt("FPV", "DeviceId", device_id);
  GetInt("FPV", "AxisRoll", axis_roll);
  GetInt("FPV", "AxisPitch", axis_pitch);
  GetInt("FPV", "AxisYaw", axis_yaw);
  GetInt("FPV", "AxisThrottle", axis_throttle);
  GetInt("FPV", "InvertPitch", invert_pitch);
  GetInt("FPV", "InvertRoll", invert_roll);
  GetInt("FPV", "InvertYaw", invert_yaw);
  GetInt("FPV", "InvertThrottle", invert_throttle);
  // 夹一下, 免得配置文件被写坏之后数值飞掉
  axis_roll = std::clamp(axis_roll, 0, 7);
  axis_pitch = std::clamp(axis_pitch, 0, 7);
  axis_yaw = std::clamp(axis_yaw, 0, 7);
  axis_throttle = std::clamp(axis_throttle, 0, 7);
  device_id = std::clamp(device_id, -1, 15);
  for (int i = 0; i < 8; ++i) {
    if (!(cal[i][0] < cal[i][1] && cal[i][1] < cal[i][2])) {
      cal[i][0] = 0.f;
      cal[i][1] = 32768.f;
      cal[i][2] = 65535.f;
    }
  }
  cal_loaded.store(true, std::memory_order_relaxed);
}

inline void StartTravelCalibration() {
  // 关键: 初值铺成反的哨兵, 否则扫描值永远落在 [0, 65535] 里, 一个数都改不动
  for (int i = 0; i < 8; ++i) {
    cal_scan_lo[i] = 65536.f;
    cal_scan_hi[i] = -1.f;
  }
  travel_until.store(GetTickCount64() + 5000, std::memory_order_relaxed);
  cal_travel.store(true, std::memory_order_relaxed);
}

inline float TravelCalibrationLeft() {
  const uint64_t end = travel_until.load(std::memory_order_relaxed);
  const uint64_t now = GetTickCount64();
  return end > now ? static_cast<float>(end - now) / 1000.f : 0.f;
}

// 扫完一圈: 只认这一轮真扫到行程的轴, 其余保持原标定不动
inline void FinishTravelCalibration() {
  for (int i = 0; i < 8; ++i) {
    const float lo = cal_scan_lo[i];
    const float hi = cal_scan_hi[i];
    if (hi - lo < 2000.f) continue;  // 没动过 / 一路没信号, 跳过
    cal[i][0] = lo;
    cal[i][2] = hi;
    // 中心必须严格落在新行程里面, 否则下次读配置会被当成坏数据整轴重置
    if (!(lo < cal[i][1] && cal[i][1] < hi)) {
      cal[i][1] = std::clamp((lo + hi) * 0.5f, lo + 1.f, hi - 1.f);
    }
  }
  SaveToConfig();
}

inline void Poll() {
  JOYINFOEX ex{};
  ex.dwSize = sizeof(ex);
  ex.dwFlags = JOY_RETURNALL | JOY_RETURNCENTERED;
  // 只有没从配置里读到标定的时候才铺默认值, 否则会把用户的标定冲掉
  if (!cal_loaded.load(std::memory_order_relaxed)) {
    for (int i = 0; i < 8; ++i) { cal[i][0] = 0.f; cal[i][1] = 32768.f; cal[i][2] = 65535.f; }
  }
  while (running.load(std::memory_order_relaxed)) {
    if (cal_travel.load(std::memory_order_relaxed)
        && GetTickCount64() > travel_until.load(std::memory_order_relaxed)) {
      cal_travel.store(false, std::memory_order_relaxed);
      FinishTravelCalibration();  // 结果落到内存表, 由主线程节流落盘
    }
    DIJOYSTATE2 js{};
    const bool via_di = di::ready.load(std::memory_order_relaxed) && di::Read(&js);
    UINT dev = via_di ? 0u : 0xFFFF;
    const int want = device_id;
    if (!via_di) {
      if (want >= 0 && joyGetPosEx(static_cast<UINT>(want), &ex) == JOYERR_NOERROR)
        dev = static_cast<UINT>(want);
      else
        for (UINT i = 0; i < 16; ++i)
          if (joyGetPosEx(i, &ex) == JOYERR_NOERROR) { dev = i; break; }
    }
    if (dev == 0xFFFF) {
      device_ok.store(false, std::memory_order_relaxed);
      has_signal.store(false, std::memory_order_relaxed);
      active_device.store(-1, std::memory_order_relaxed);
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      continue;
    }
    device_ok.store(true, std::memory_order_relaxed);
    active_device.store(via_di ? -2 : static_cast<int>(dev), std::memory_order_relaxed);
    DWORD values[6] = {0, 0, 0, 0, 0, 0};
    if (via_di) {
      const LONG src[8] = {js.lX, js.lY, js.lZ, js.lRx, js.lRy, js.lRz, js.rglSlider[0], js.rglSlider[1]};
      DWORD mask = 0;
      for (int b = 0; b < 32; ++b) if (js.rgbButtons[b] & 0x80) mask |= 1u << b;
      buttons.store(mask, std::memory_order_relaxed);
      wcsncpy(device_name, L"DirectInput", 127), device_name[127] = 0;
      for (int i = 0; i < 8; ++i) {
        const LONG scaled = i >= 6 ? src[i] : src[i] + 32768;
        raw[i].store(static_cast<DWORD>(std::clamp(scaled, 0L, 65535L)), std::memory_order_relaxed);
      }
    } else {
      values[0] = ex.dwXpos, values[1] = ex.dwYpos, values[2] = ex.dwZpos;
      values[3] = ex.dwRpos, values[4] = ex.dwUpos, values[5] = ex.dwVpos;
      buttons.store(ex.dwButtons, std::memory_order_relaxed);
      JOYCAPSW caps{};
      if (joyGetDevCapsW(dev, &caps, sizeof(caps)) == JOYERR_NOERROR)
        wcsncpy(device_name, caps.szPname, 127), device_name[127] = 0;
      for (int i = 0; i < 6; ++i) raw[i].store(values[i], std::memory_order_relaxed);
    }
    if (cal_travel.load(std::memory_order_relaxed)) {
      for (int i = 0; i < 8; ++i) {
        const float v = static_cast<float>(raw[i].load(std::memory_order_relaxed));
        if (v < cal_scan_lo[i]) cal_scan_lo[i] = v;
        if (v > cal_scan_hi[i]) cal_scan_hi[i] = v;
      }
    }
    auto norm = [&](int axis) {
      const float lo = cal[axis][0], mid = cal[axis][1], hi = cal[axis][2];
      // raw[] 是两条输入路径统一后的 0..65535 原始值, values[] 只有 winmm 那条路会填
      const float v = static_cast<float>(raw[axis].load(std::memory_order_relaxed));
      float out = v >= mid ? (v - mid) / (hi - mid + 0.001f) : (v - mid) / (mid - lo + 0.001f);
      out = std::clamp(out, -1.f, 1.f);
      const float m = std::abs(out);
      if (m < deadzone) return 0.f;
      return (out > 0.f ? 1.f : -1.f) * ((m - deadzone) / (1.f - deadzone));
    };
    const float p = norm(axis_pitch) * (invert_pitch ? -1.f : 1.f);
    const float r = norm(axis_roll) * (invert_roll ? -1.f : 1.f);
    const float y = norm(axis_yaw) * (invert_yaw ? -1.f : 1.f);
    pitch_in.store(Expo(p, expo), std::memory_order_relaxed);
    roll_in.store(Expo(r, expo), std::memory_order_relaxed);
    yaw_in.store(Expo(y, expo), std::memory_order_relaxed);
    const float t = (norm(axis_throttle) + 1.f) * 0.5f;
    thr.store(std::clamp(invert_throttle ? 1.f - t : t, 0.f, 1.f), std::memory_order_relaxed);
    vis_pitch.store(pitch_in.load(), std::memory_order_relaxed);
    vis_roll.store(roll_in.load(), std::memory_order_relaxed);
    vis_yaw.store(yaw_in.load(), std::memory_order_relaxed);
    vis_thr.store(thr.load(), std::memory_order_relaxed);
    has_signal.store(true, std::memory_order_relaxed);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

inline void Start() {
  if (running.load()) return;
  running.store(true);
  worker = std::thread(Poll);
}

inline void Stop() {
  running.store(false);
  if (worker.joinable()) worker.join();
  device_ok.store(false);
  has_signal.store(false);
}
} // namespace radio



// 初始化/重置物理状态 (freecam 激活时从相机姿态带入)
// ---- 碰撞/crash参数与状态 ----
inline float collision_enabled = 1.f;   // 碰撞检测总开关
inline float crash_enabled = 0.f;       // crash开关(前置: 碰撞)
inline float crash_threshold = 8.f;     // 撞击速度阈值 m/s
inline float land_speed = 0.5f;         // 着陆判定: 水平速度低于此值落地静止, 高于则滑行
inline float ground_friction = 3.f;     // 地面摩擦减速度 m/s^2 (滑行时水平速度衰减)
inline float drone_radius = 0.15f;     // 机体碰撞半径 m (0=纯射线, 可在面板拉条)
inline bool airborne = false;          // 锚点自动模式: 起飞后置位, 碰撞全关
inline float takeoff_lock = 3.f;       // 起飞判定锁死倒计时(激活后3秒内不判起飞)
inline float last_floor = 0.f;         // 最后已知地面高度(自动模式用)
inline float crash_timer = 0.f;         // >0 = 失控翻滚中
inline Vec3 spawn_pos{0.f, 2.f, 0.f};
inline float spawn_heading = 0.f;

inline void StartCrash() {
  crash_timer = 1.2f;
  // 弹回: 沿法线反射后衰减
  velocity = velocity * -0.35f;
}

inline void Reset(const Vec3& origin, float heading) {
  position = origin;
  velocity = {0.f, 0.f, 0.f};
  attitude = AxisAngle({0.f, 1.f, 0.f}, heading);
  throttle = 0.f;
  vjoy_pitch = vjoy_roll = vjoy_yaw = 0.f;
  ptr_yaw = heading;
  ptr_pitch = 0.f;
  ptr_vertical = 0.f;
  ptr_mode_prev = -1.f;
  spawn_pos = origin;
  spawn_heading = heading;
  airborne = false;
  last_floor = origin.y;
  takeoff_lock = 3.f;
  horizon_hold = 0.f;
  active.store(true, std::memory_order_relaxed);
}

// 每帧推进物理。kb = freecam 键鼠输入; 返回新的姿态与位置
// ---- 物理查询: Unity SphereCast + 地面检测 + 碰撞/crash状态机 ----
namespace phys {
inline bool resolved = false;
inline void* sphere_cast = nullptr;
inline void* ray_cast = nullptr;
inline bool sphere_cast_qti = false; // 7参版本带QueryTriggerInteraction.Ignore
inline size_t hit_point_off = 0, hit_normal_off = 0, hit_dist_off = 0, hit_collider_off = 0;

inline float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Normalized(Vec3 v) {
  const float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
  return l > 0.0001f ? v * (1.f / l) : Vec3{0.f, 0.f, 0.f};
}

inline bool Resolve() {
  if (resolved) return sphere_cast != nullptr;
  resolved = true;
  const auto image = FindImage("UnityEngine.PhysicsModule.dll");
  if (!image) {
    Log(reshade::log::level::warning, "E_E_FPV phys: PhysicsModule image not found");
    return false;
  }
  HMODULE ga = GetModuleHandleW(L"GameAssembly.dll");
  using GetMethodFn = void* (*)(void*, const char*, int);
  using GetMethodsFn = void* (*)(void*, void**);
  using MethodNameFn = const char* (*)(void*);
  using ParamCountFn = int (*)(void*);
  using GetParamFn = void* (*)(void*, unsigned);
  using ClassFromTypeFn = void* (*)(void*);
  using ClassGetFieldsFn = void* (*)(void*, void**);
  using FieldNameFn = const char* (*)(void*);
  using FieldOffsetFn = int (*)(void*);
  using ClassValueSizeFn = int (*)(void*);
  GetMethodFn get_method = nullptr;
  GetMethodsFn get_methods = nullptr;
  MethodNameFn method_name = nullptr;
  ParamCountFn param_count = nullptr;
  GetParamFn get_param = nullptr;
  ClassFromTypeFn class_from_type = nullptr;
  ClassGetFieldsFn get_fields = nullptr;
  FieldNameFn field_name = nullptr;
  FieldOffsetFn field_offset = nullptr;
  ClassValueSizeFn value_size = nullptr;
  if (!ga || !ResolveExport(ga, "il2cpp_class_get_method_from_name", &get_method)
      || !ResolveExport(ga, "il2cpp_class_get_methods", &get_methods)
      || !ResolveExport(ga, "il2cpp_method_get_name", &method_name)
      || !ResolveExport(ga, "il2cpp_method_get_param_count", &param_count)
      || !ResolveExport(ga, "il2cpp_method_get_param", &get_param)
      || !ResolveExport(ga, "il2cpp_class_from_type", &class_from_type)
      || !ResolveExport(ga, "il2cpp_class_get_fields", &get_fields)
      || !ResolveExport(ga, "il2cpp_field_get_name", &field_name)
      || !ResolveExport(ga, "il2cpp_field_get_offset", &field_offset)
      || !ResolveExport(ga, "il2cpp_class_value_size", &value_size))
    return false;
  void* klass = class_from_name(image, "UnityEngine", "Physics");
  if (!klass) return false;
  const auto mscorlib = FindImage("mscorlib.dll");
  const auto core = FindImage("UnityEngine.CoreModule.dll");
  void* single_class = mscorlib ? class_from_name(mscorlib, "System", "Single") : nullptr;
  void* vector3_class = core ? class_from_name(core, "UnityEngine", "Vector3") : nullptr;
  void* int32_class = mscorlib ? class_from_name(mscorlib, "System", "Int32") : nullptr;
  void* hit_class = class_from_name(image, "UnityEngine", "RaycastHit");
  void* iter = nullptr;
  void* m = nullptr;
  while ((m = get_methods(klass, &iter)) != nullptr) {
    const char* name = method_name(m);
    if (!name) continue;
    const int pc = param_count(m);
    void* p1 = get_param(m, 1);
    void* p2 = get_param(m, 2);
    void* p3 = get_param(m, 3);
    void* p4 = get_param(m, 4);
    if (!single_class || !vector3_class || !hit_class || !p1 || !p2 || !p3 || !p4) continue;
    if (strcmp(name, "SphereCast") == 0 && !sphere_cast) {
      void* p1v = get_param(m, 1);
      void* p2v = get_param(m, 2);
      void* p3v = get_param(m, 3);
      void* p4v = get_param(m, 4);
      if (!p1v || !p2v || !p3v || !p4v) continue;
      const int rc = param_count(m);
      // (origin, radius, direction, out hit, maxDistance[, layerMask[, QTI]])
      if (rc != 5 && rc != 7) continue;
      if (class_from_type(p1v) != single_class || class_from_type(p2v) != vector3_class) continue;
      if (class_from_type(p3v) != hit_class || class_from_type(p4v) != single_class) continue;
      if (rc == 7) {
        void* p5 = get_param(m, 5);
        if (!p5 || class_from_type(p5) != int32_class) continue;
        sphere_cast_qti = true;
      }
      sphere_cast = m;
    } else if (strcmp(name, "Raycast") == 0 && pc == 5 && !ray_cast) {
      // (Vector3 origin, Vector3 direction, out RaycastHit hit, float maxDistance, int layerMask)
      void* p0 = get_param(m, 0);
      void* p0t = p0 ? class_from_type(p0) : nullptr;
      if (!p0t || p0t != vector3_class) continue;
      if (class_from_type(p1) != vector3_class || class_from_type(p2) != hit_class) continue;
      if (class_from_type(p3) != single_class || class_from_type(p4) != int32_class) continue;
      ray_cast = m;
    }
    if (sphere_cast && ray_cast) break;
  }
  if (!sphere_cast) {
    Log(reshade::log::level::warning, "E_E_FPV phys: Physics.SphereCast(5) overload not found");
    return false;
  }
  void* iter2 = nullptr;
  void* f = nullptr;
  while ((f = get_fields(hit_class, &iter2)) != nullptr) {
    const char* n = field_name(f);
    if (!n) continue;
    // il2cpp字段偏移带16字节对象头偏置, runtime_invoke写出的裸结构体没有头, 全部减16
    const int off = field_offset(f) - 16;
    if (off < 0) continue;
    if (!strcmp(n, "m_Point")) hit_point_off = static_cast<size_t>(off);
    else if (!strcmp(n, "m_Normal")) hit_normal_off = static_cast<size_t>(off);
    else if (!strcmp(n, "m_Distance")) hit_dist_off = static_cast<size_t>(off);
    else if (!strcmp(n, "m_Collider")) hit_collider_off = static_cast<size_t>(off);
  }
  char buf[128];
  snprintf(buf, sizeof(buf), "E_E_FPV phys: SphereCast resolved, RaycastHit point=%zu normal=%zu dist=%zu collider=%zu",
           hit_point_off, hit_normal_off, hit_dist_off, hit_collider_off);
  Log(reshade::log::level::info, buf);
  return sphere_cast != nullptr;
}

// 返回是否命中; 命中时输出命中点/法线/距离
inline bool SphereCast(const Vec3& origin, float radius, const Vec3& dir, float max_dist,
                       Vec3* out_point, Vec3* out_normal, float* out_dist) {
  if (!sphere_cast) return false;
  alignas(16) uint8_t hit[160] = {};
  Vec3 o = origin, d = dir;
  float r = radius, md = max_dist;
  int mask = -1, qti_ignore = 1; // QueryTriggerInteraction.Ignore
  void* args[7] = {&o, &r, &d, hit, &md, &mask, &qti_ignore};
  void* exception = nullptr;
  runtime_invoke(sphere_cast, nullptr, args, &exception);
  if (exception) {
    static std::atomic_uint exc_logs{0};
    if (exc_logs.fetch_add(1, std::memory_order_relaxed) < 3)
      Log(reshade::log::level::warning, "E_E_FPV phys: SphereCast threw a managed exception");
    return false;
  }
  void* collider = nullptr;
  std::memcpy(&collider, hit + hit_collider_off, sizeof(void*));
  if (!collider) return false;
  static std::atomic_uint hit_logs{0};
  if (hit_logs.fetch_add(1, std::memory_order_relaxed) < 3) {
    char buf[128];
    float dist = 0.f;
    std::memcpy(&dist, hit + hit_dist_off, sizeof(float));
    snprintf(buf, sizeof(buf), "E_E_FPV phys: first hit dist=%.2f max=%.2f", dist, max_dist);
    Log(reshade::log::level::info, buf);
  }
  if (out_point) std::memcpy(out_point, hit + hit_point_off, sizeof(Vec3));
  if (out_normal) std::memcpy(out_normal, hit + hit_normal_off, sizeof(Vec3));
  if (out_dist) std::memcpy(out_dist, hit + hit_dist_off, sizeof(float));
  return true;
}
// 纯射线查询: 不受球体是否嵌入碰撞体影响
inline bool Ray(const Vec3& origin, const Vec3& dir, float max_dist,
                Vec3* out_point, Vec3* out_normal, float* out_dist) {
  if (!ray_cast) return false;
  alignas(16) uint8_t hit[160] = {};
  Vec3 o = origin, d = dir;
  float md = max_dist;
  int mask = -1;
  void* args[5] = {&o, &d, hit, &md, &mask};
  void* exception = nullptr;
  runtime_invoke(ray_cast, nullptr, args, &exception);
  if (exception) return false;
  void* collider = nullptr;
  std::memcpy(&collider, hit + hit_collider_off, sizeof(void*));
  if (!collider) return false;
  if (out_point) std::memcpy(out_point, hit + hit_point_off, sizeof(Vec3));
  if (out_normal) std::memcpy(out_normal, hit + hit_normal_off, sizeof(Vec3));
  if (out_dist) std::memcpy(out_dist, hit + hit_dist_off, sizeof(float));
  return true;
}
} // namespace phys

inline void Tick(float dt, const Vec3& move, float mouse_yaw_deg, float mouse_pitch_deg) {
  if (dt <= 0.f || !active.load(std::memory_order_relaxed)) return;
  dt = std::clamp(dt, 0.001f, 0.05f);

  // ---- crash失控状态: 无视输入翻滚坠落, 计时结束送回起飞点 ----
  if (crash_timer > 0.f) {
    crash_timer -= dt;
    attitude = attitude * AxisAngle({1.f, 0.f, 0.f}, 230.f * dt) * AxisAngle({0.f, 0.f, 1.f}, -280.f * dt);
    velocity = velocity + Vec3{0.f, -gravity, 0.f} * dt;
    position = position + velocity * dt;
    if (crash_timer <= 0.f) {
      position = spawn_pos;
      velocity = {0.f, 0.f, 0.f};
      attitude = AxisAngle({0.f, 1.f, 0.f}, spawn_heading);
      throttle = 0.f;
      vjoy_pitch = vjoy_roll = vjoy_yaw = 0.f;
    }
    if (!Finite(position)) position = {0.f, 2.f, 0.f};
    if (!Finite(velocity)) velocity = {0.f, 0.f, 0.f};
    return;
  }

  // ---- 键鼠自由飞 (mode 3): WASD 水平平移, 鼠标管视角, 空格升 / Ctrl·Shift 降 ----
  // 这一段只负责姿态与速度, 位置积分和碰撞交给下面共用的碰撞段, 与三种姿态模式完全同一套
  const bool pointer_mode = mode >= 2.5f;
  if (pointer_mode) {
    // 刚切进来先把视角对齐当前机头, 免得镜头突然跳一下
    if (ptr_mode_prev < 2.5f) {
      const Vec3 f = Rotate(attitude, {0.f, 0.f, 1.f});
      ptr_yaw = std::atan2(f.x, f.z) * 57.295779513f;
      ptr_pitch = -std::asin(std::clamp(f.y, -1.f, 1.f)) * 57.295779513f;
      velocity = {0.f, 0.f, 0.f};
    }
    ptr_mode_prev = mode;

    // 视角: 鼠标位移直接累积, 鼠标停下朝向就保持(不是杆量, 不回中)
    ptr_yaw += mouse_yaw_deg * ptr_mouse_sens;
    if (ptr_yaw > 180.f) ptr_yaw -= 360.f;
    else if (ptr_yaw < -180.f) ptr_yaw += 360.f;
    ptr_pitch = std::clamp(ptr_pitch + mouse_pitch_deg * ptr_mouse_sens, -89.f, 89.f);
    attitude = AxisAngle({0.f, 1.f, 0.f}, ptr_yaw) * AxisAngle({1.f, 0.f, 0.f}, ptr_pitch);

    // 取水平朝向: 去掉俯仰分量, 这样 W/S 是水平平移, 低头时不会往地里扎
    const Vec3 view_fwd = Rotate(attitude, {0.f, 0.f, 1.f});
    const Vec3 view_rgt = Rotate(attitude, {1.f, 0.f, 0.f});
    const float fwd_flat = std::hypot(view_fwd.x, view_fwd.z);
    const float rgt_flat = std::hypot(view_rgt.x, view_rgt.z);
    const Vec3 flat_fwd = fwd_flat > 0.001f ? Vec3{view_fwd.x / fwd_flat, 0.f, view_fwd.z / fwd_flat} : Vec3{0.f, 0.f, 1.f};
    const Vec3 flat_rgt = rgt_flat > 0.001f ? Vec3{view_rgt.x / rgt_flat, 0.f, view_rgt.z / rgt_flat} : Vec3{1.f, 0.f, 0.f};

    const Vec3 target = flat_fwd * (std::clamp(move.z, -1.f, 1.f) * ptr_speed)
                        + flat_rgt * (std::clamp(move.x, -1.f, 1.f) * ptr_speed)
                        + Vec3{0.f, std::clamp(ptr_vertical, -1.f, 1.f) * ptr_vertical_speed, 0.f};
    velocity.x = Approach1(velocity.x, target.x, ptr_accel, ptr_decel, dt);
    velocity.y = Approach1(velocity.y, target.y, ptr_accel, ptr_decel, dt);
    velocity.z = Approach1(velocity.z, target.z, ptr_accel, ptr_decel, dt);
    if (!Finite(velocity)) velocity = {0.f, 0.f, 0.f};
  } else {
  ptr_mode_prev = mode;

  // ---- 输入合成: 遥控器优先 ----
  float in_thr, in_yaw, in_pitch, in_roll;
  const bool use_radio = source < 1.5f && radio::device_ok.load(std::memory_order_relaxed)
                         && radio::has_signal.load(std::memory_order_relaxed);
  if (use_radio) {
    in_thr = radio::thr.load(std::memory_order_relaxed);
    in_yaw = radio::yaw_in.load(std::memory_order_relaxed);
    in_pitch = radio::pitch_in.load(std::memory_order_relaxed);
    in_roll = radio::roll_in.load(std::memory_order_relaxed);
  } else {
    // ---- 键鼠虚拟摇杆 ----
    // 鼠标位移推杆, 松手按时间常数自动回中; 这样小幅移动=小幅杆量, 不会一碰就满舵
    const float decay = std::exp(-std::max(kbd_recenter, 0.01f) * dt);
    // 鼠标上移 = 推杆前 = 低头, 鼠标右移 = 右横滚
    vjoy_pitch = std::clamp(vjoy_pitch * decay - mouse_pitch_deg * kbd_mouse_sens, -1.f, 1.f);
    vjoy_roll = std::clamp(vjoy_roll * decay + mouse_yaw_deg * kbd_mouse_sens, -1.f, 1.f);
    // 指数衰减永远到不了 0, 这里收个尾, 否则半自稳的"双杆回中"判定永远不成立
    if (std::abs(vjoy_pitch) < deadzone * 0.5f) vjoy_pitch = 0.f;
    if (std::abs(vjoy_roll) < deadzone * 0.5f) vjoy_roll = 0.f;
    // 键盘偏航: 一阶平滑, 免得 A/D 一按下就是满偏航
    const float yaw_target = std::clamp(move.x, -1.f, 1.f);
    vjoy_yaw += (yaw_target - vjoy_yaw) * std::min(1.f, std::max(kbd_yaw_smooth, 0.1f) * dt);
    if (yaw_target == 0.f && std::abs(vjoy_yaw) < 0.005f) vjoy_yaw = 0.f;
    // 油门: W/S 增量, 按住 Shift(boost) 时快三倍
    throttle = std::clamp(throttle + move.z * kbd_throttle_speed * (kbd_boost ? 3.f : 1.f) * dt, 0.f, 1.f);
    in_thr = throttle;
    in_yaw = std::clamp(vjoy_yaw, -1.f, 1.f);
    in_pitch = vjoy_pitch;
    in_roll = vjoy_roll;
  }

  // ---- 角速度命令 ----
  // 半自稳: 双杆在死区内持续驻留 horizon_delay 秒后启动自稳, 出死区立即切回速率
  const bool sticks_centered = std::abs(in_pitch) <= deadzone && std::abs(in_roll) <= deadzone;
  horizon_hold = sticks_centered ? std::min(horizon_hold + dt, horizon_delay + 0.5f) : 0.f;
  const float horizon_engaged = sticks_centered && horizon_hold >= horizon_delay ? 1.f : 0.f;
  float rate_pitch_cmd = 0.f, rate_roll_cmd = 0.f, rate_yaw_cmd = 0.f;

  const Vec3 forward = Rotate(attitude, {0.f, 0.f, 1.f});
  const Vec3 right = Rotate(attitude, {1.f, 0.f, 0.f});
  const float pitch_current = -std::asin(std::clamp(forward.y, -1.f, 1.f)) * 57.295779513f; // 正=低头
  const float roll_current = std::atan2(right.y, std::hypot(right.x, right.z)) * 57.295779513f;

  const float angle_pitch = angle_kp * (in_pitch * tilt_max - pitch_current);
  const float angle_roll = angle_kp * (in_roll * tilt_max + roll_current);
  const float acro_pitch = Expo(in_pitch, expo) * rate_pitch;
  const float acro_roll = Expo(in_roll, expo) * rate_roll;

  if (mode < 0.5f) { // Angle
    rate_pitch_cmd = std::clamp(angle_pitch, -angle_rate_limit, angle_rate_limit);
    rate_roll_cmd = std::clamp(angle_roll, -angle_rate_limit, angle_rate_limit);
  } else if (mode < 1.5f) { // Horizon
    rate_pitch_cmd = horizon_engaged * angle_pitch + (1.f - horizon_engaged) * acro_pitch;
    rate_roll_cmd = horizon_engaged * angle_roll + (1.f - horizon_engaged) * acro_roll;
  } else { // Acro
    rate_pitch_cmd = acro_pitch;
    rate_roll_cmd = acro_roll;
  }
  rate_yaw_cmd = Expo(in_yaw, expo) * rate_yaw;

  // ---- 姿态积分 (体轴系, RH 四元数) ----
  // pitch: +X = 低头; yaw: +Y = 右偏; roll: -Z = 右滚
  attitude = attitude * AxisAngle({1.f, 0.f, 0.f}, rate_pitch_cmd * dt)
                       * AxisAngle({0.f, 1.f, 0.f}, rate_yaw_cmd * dt)
                       * AxisAngle({0.f, 0.f, 1.f}, -rate_roll_cmd * dt);
  {
    const float n = std::sqrt(attitude.x * attitude.x + attitude.y * attitude.y
                              + attitude.z * attitude.z + attitude.w * attitude.w);
    if (!std::isfinite(n) || n < 0.0001f) attitude = {0.f, 0.f, 0.f, 1.f};
    else attitude = {attitude.x / n, attitude.y / n, attitude.z / n, attitude.w / n};
  }

  // ---- 力学: 沿机头向上的推力 + 重力 + 阻力 ----
  const float thr_in = use_radio ? in_thr : in_thr;
  const Vec3 up_body = Rotate(attitude, {0.f, 1.f, 0.f});
  Vec3 acceleration = up_body * (thr_in * thrust) + Vec3{0.f, -gravity, 0.f};
  const float speed = std::sqrt(velocity.x * velocity.x + velocity.y * velocity.y + velocity.z * velocity.z);
  acceleration = Sub(acceleration, velocity * (drag_linear + drag_quad * speed));
  velocity = velocity + acceleration * dt;
  } // ---- 三种姿态模式到此为止(块内未额外缩进, 保持原缩进便于对照) ----

  // ---- 碰撞与地面检测 ----
  bool ground_hit_now = false;
  const bool anchor_auto = collision_enabled >= 1.5f; // 起飞后关闭模式
  if (takeoff_lock > 0.f) takeoff_lock -= dt;
  const bool col_active = collision_enabled >= .5f && !(anchor_auto && airborne);
  if (col_active && phys::Resolve()) {
    const float move_speed = std::sqrt(velocity.x * velocity.x + velocity.y * velocity.y + velocity.z * velocity.z);
    bool moved = false;
    if (move_speed > 0.05f) {
      const Vec3 dir = phys::Normalized(velocity);
      Vec3 hp{}, hn{};
      float hd = 0.f;
      if (phys::SphereCast(position, drone_radius, dir, move_speed * dt + 0.35f, &hp, &hn, &hd)) {
        const float into = phys::Dot(velocity, hn);
        const float impact = -into;
        if (hd < 0.05f) {
          // 初始重叠: 仅在高速压入时沿法线顶出; 静止贴合保持原位, 防止悬挂抖动
          if (into < -1.f) position = position + hn * 0.06f;
        } else {
          position = position + dir * std::max(0.f, hd - std::max(0.02f, drone_radius * 0.3f));
        }
        velocity = Sub(velocity, hn * into); // 沿碰撞面滑动
        if (crash_enabled >= .5f && impact >= crash_threshold && crash_timer <= 0.f) StartCrash();
        moved = true;
      }
    }
    if (!moved) position = position + velocity * dt;
    Vec3 hp{}, gn{};
    float hd = 0.f;
    const float probe_radius = drone_radius;
    // 从机体上方1米垂直打射线: 与球体是否嵌入碰撞体无关, 改半径/嵌地都钳得住
    // 高空平飞时命中率极低(实测约13%), 而这是跨语言物理查询, 离地远且不在下落就跳过
    const bool probe_ground = (position.y - last_floor) < 3.f || velocity.y < -0.5f;
    const bool ground_hit = probe_ground
                            && phys::Ray(position + Vec3{0.f, 0.1f, 0.f}, Vec3{0.f, -1.f, 0.f}, 0.1f + probe_radius + 0.5f, &hp, &gn, &hd);
    if (ground_hit) {
      ground_hit_now = true;
      last_floor = hp.y;
      const float floor = hp.y + probe_radius;
      if (anchor_auto && takeoff_lock <= 0.f && position.y > last_floor + 1.5f) airborne = true; // 升空过高, 碰撞关闭
      if (position.y < floor) {
        const float into = phys::Dot(velocity, gn);
        if (crash_enabled >= .5f && -into >= crash_threshold && crash_timer <= 0.f) {
          StartCrash();
        } else if (into < 0.f) {
          // 下落触地: 沿地面滑行 + 摩擦衰减
          velocity = Sub(velocity, gn * into);
          const float fr = ground_friction * dt;
          const float hs = std::sqrt(velocity.x * velocity.x + velocity.z * velocity.z);
          if (fr > 0.f && hs > 0.001f) {
            const float k = std::max(0.f, hs - fr) / hs;
            velocity.x *= k;
            velocity.z *= k;
          }
        } else if (into <= 0.2f) {
          // 贴地静止: 爬升杆量未生效(沿法线速度不足)时才落地静止, 给杆即起飞
          const float h_speed = std::sqrt(velocity.x * velocity.x + velocity.z * velocity.z);
          if (h_speed < land_speed) velocity = {0.f, 0.f, 0.f};
        }
        // into > 0.2: 爬升中, 不干预
        position.y = floor;
      }
    } else if (anchor_auto && takeoff_lock <= 0.f && !airborne) {
      airborne = true; // 脚下探不到地面, 已起飞, 碰撞关闭
    }
  } else {
    position = position + velocity * dt;
  }
  // 虚空保底: 地面射线探不到地面(流送边界外)时的绝对高度保险
  if (!ground_hit_now && floor_y >= 0.f && position.y < floor_y) {
    position.y = floor_y;
    if (velocity.y < 0.f) velocity.y = 0.f;
  }
  if (!Finite(position)) position = {0.f, 2.f, 0.f};
  if (!Finite(velocity)) velocity = {0.f, 0.f, 0.f};
}

// 修饰键重同步: FPV期间游戏输入被屏蔽, 退出时对已松开的修饰键补发抬起事件, 防止卡键
inline void ResyncModifiers() {
  INPUT events[3]{};
  int n = 0;
  const int keys[3] = {VK_SHIFT, VK_CONTROL, VK_MENU};
  for (int i = 0; i < 3; ++i) {
    if (!(GetAsyncKeyState(keys[i]) & 0x8000)) { // 物理上已松开的才补发
      events[n].type = INPUT_KEYBOARD;
      events[n].ki.wVk = static_cast<WORD>(keys[i]);
      events[n].ki.dwFlags = KEYEVENTF_KEYUP;
      ++n;
    }
  }
  if (n > 0) SendInput(static_cast<UINT>(n), events, sizeof(INPUT));
}


// ---- 锚点迁移: 调引擎自带的 SetOverrideStreamingCenterByCamera, 让流式/LOD中心跟随FPV相机 ----
namespace anchor {
// 光照体积(IV)流送中心跟随相机。
//
// 定位过程留个记录, 免得以后重新踩一遍:
//   class  HG.Rendering.Runtime.HGIrradianceVolumeManager @ HG.RenderPipelines.Runtime.dll
//   method SetOverrideStreamingCenterByCamera(Camera)  —— 实例方法, 置一个模式标志
//   field  m_overrideStreamingCenterByCamera : Boolean —— 就是那个标志
//   实例   HGManagerContext.get_currentManagerContext().ivManager
// 另外查过: 远处地形粗模由 HGTerrainGroundLayer(Clipmap).SetPlayerCenter 与
// DynamicSceneGridDealer 的 streaming centers 决定, 而且都是"每帧被喂一次"的数据,
// 想改必须钩游戏函数(动代码页)。评估后不纳入, 本项目不再新增钩子。
inline bool api_ready = false;
inline bool resolved = false;
inline bool logged = false;
inline std::atomic_bool engaged{false};            // 覆盖是否已经设上
inline std::atomic_bool pending_reset{false};      // 复位只能回游戏线程做, 见 ConsumeReset
inline Il2CppMethod set_override_by_camera = nullptr;
inline int override_argc = 1;
inline void* manager_class = nullptr;
inline void* manager_instance = nullptr;

using FnFieldStaticGet = void (*)(void*, void*);
using FnFieldGetValue = void (*)(void*, void*, void*);
using FnFieldSetValue = void (*)(void*, void*, void*);
using FnClassGetName = const char* (*)(void*);
using FnClassGetMethodFromName = void* (*)(void*, const char*, int);
using FnImageGetClassCount = size_t (*)(Il2CppImage);
using FnImageGetClass = void* (*)(Il2CppImage, size_t);
using FnDomainGet = void* (*)();
using FnDomainGetAssemblies = void** (*)(const void*, size_t*);
using FnAssemblyGetImage = Il2CppImage (*)(const void*);

inline FnFieldStaticGet field_static_get_value = nullptr;
inline FnFieldGetValue field_get_value = nullptr;
inline FnFieldSetValue field_set_value = nullptr;
inline FnClassGetName class_get_name = nullptr;
inline FnClassGetMethodFromName class_get_method_from_name = nullptr;
inline FnImageGetClassCount image_get_class_count = nullptr;
inline FnImageGetClass image_get_class = nullptr;
inline FnDomainGet domain_get = nullptr;
inline FnDomainGetAssemblies domain_get_assemblies = nullptr;
inline FnAssemblyGetImage assembly_get_image = nullptr;

inline void Warn(const char* what) {
  char buf[192];
  snprintf(buf, sizeof(buf), "E_E_FPV anchor: %s", what);
  Log(reshade::log::level::warning, buf);
}

inline void ResolveApi() {
  if (api_ready) return;
  api_ready = true;
  HMODULE ga = GetModuleHandleW(L"GameAssembly.dll");
  if (!ga) { Warn("no GameAssembly"); return; }
  ResolveExport(ga, "il2cpp_field_static_get_value", &field_static_get_value);
  ResolveExport(ga, "il2cpp_field_get_value", &field_get_value);
  ResolveExport(ga, "il2cpp_field_set_value", &field_set_value);
  ResolveExport(ga, "il2cpp_class_get_name", &class_get_name);
  ResolveExport(ga, "il2cpp_class_get_method_from_name", &class_get_method_from_name);
  ResolveExport(ga, "il2cpp_image_get_class_count", &image_get_class_count);
  ResolveExport(ga, "il2cpp_image_get_class", &image_get_class);
  ResolveExport(ga, "il2cpp_domain_get", &domain_get);
  ResolveExport(ga, "il2cpp_domain_get_assemblies", &domain_get_assemblies);
  ResolveExport(ga, "il2cpp_assembly_get_image", &assembly_get_image);
}

// 按类名(不带命名空间)遍历全部程序集找类
inline void* FindClassByName(const char* wanted) {
  if (!domain_get || !domain_get_assemblies || !assembly_get_image
      || !image_get_class_count || !image_get_class || !class_get_name) return nullptr;
  void* domain = domain_get();
  if (domain == nullptr) return nullptr;
  size_t count = 0;
  void** assemblies = domain_get_assemblies(domain, &count);
  if (assemblies == nullptr) return nullptr;
  for (size_t a = 0; a < count; ++a) {
    Il2CppImage image = assembly_get_image(assemblies[a]);
    if (image == nullptr) continue;
    const size_t classes = image_get_class_count(image);
    for (size_t i = 0; i < classes; ++i) {
      void* type = image_get_class(image, i);
      if (type == nullptr) continue;
      const char* name = class_get_name(type);
      if (name != nullptr && strcmp(name, wanted) == 0) return type;
    }
  }
  return nullptr;
}

// 现取一份实例: 问 HGManagerContext.get_currentManagerContext() 要当前上下文, 再读它的 ivManager。
// 不能缓太久 —— 管理器可能随场景重建, 拿旧指针写内存就是当场崩。
inline void RefreshInstance() {
  ResolveApi();
  if (manager_class == nullptr) {
    manager_class = FindClassByName("HGIrradianceVolumeManager");
    if (manager_class == nullptr) return;
  }
  void* context_class = FindClassByName("HGManagerContext");
  if (context_class == nullptr || !class_get_method_from_name
      || !field_get_value || !field_set_value) return;
  void* getter = class_get_method_from_name(context_class, "get_currentManagerContext", 0);
  if (getter == nullptr) return;
  void* exception = nullptr;
  void* context = runtime_invoke(getter, nullptr, nullptr, &exception);
  if (exception != nullptr || context == nullptr) return;
  // ivManager 是 HGManagerContext 的私有字段, 偏移 88(实测); 按名字取一次更稳
  static void* iv_field = nullptr;
  if (iv_field == nullptr) {
    void* field = nullptr;
    void* parent = context_class;
    // 字段可能声明在基类上, 顺着往上找
    for (int depth = 0; parent != nullptr && depth < 6 && field == nullptr; ++depth) {
      field = class_get_field_from_name(parent, "ivManager");
      if (field == nullptr) {
        HMODULE ga = GetModuleHandleW(L"GameAssembly.dll");
        using Parent = void* (*)(void*);
        Parent parent_of = nullptr;
        if (ga && ResolveExport(ga, "il2cpp_class_get_parent", &parent_of) && parent_of) {
          parent = parent_of(parent);
        } else {
          break;
        }
      }
    }
    iv_field = field;
  }
  if (iv_field == nullptr) return;
  void* value = nullptr;
  field_get_value(context, iv_field, &value);
  if (value == nullptr) return;
  manager_instance = value;
}

inline void Resolve() {
  if (resolved) return;
  resolved = true;
  ResolveApi();
  if (manager_class == nullptr) manager_class = FindClassByName("HGIrradianceVolumeManager");
  if (manager_class == nullptr) { Warn("class HGIrradianceVolumeManager not found"); return; }
  if (class_get_method_from_name != nullptr) {
    for (int argc = 1; argc >= 0; --argc) {
      set_override_by_camera = class_get_method_from_name(manager_class, "SetOverrideStreamingCenterByCamera", argc);
      if (set_override_by_camera != nullptr) { override_argc = argc; break; }
    }
  }
  if (set_override_by_camera == nullptr) { Warn("method SetOverrideStreamingCenterByCamera not found"); return; }
  RefreshInstance();
  if (manager_instance == nullptr) { Warn("no instance (get_currentManagerContext)"); return; }
  if (!logged) {
    logged = true;
    char buf[160];
    snprintf(buf, sizeof(buf), "E_E_FPV anchor: ready — instance=%p (get_currentManagerContext().ivManager)", manager_instance);
    Log(reshade::log::level::info, buf);
  }
}

// 只做一件事: 把模式标志置真, 让 IV 流送中心跟着相机
inline void Update(void* camera) {
  Resolve();
  if (set_override_by_camera == nullptr || camera == nullptr) return;
  if (engaged.load(std::memory_order_relaxed)) return;
  RefreshInstance();
  if (manager_instance == nullptr) return;
  void* args[2] = {camera, nullptr};
  void* exception = nullptr;
  if (override_argc == 1) {
    runtime_invoke(set_override_by_camera, manager_instance, args, &exception);
  } else {
    runtime_invoke(set_override_by_camera, manager_instance, nullptr, &exception);
  }
  if (exception == nullptr) {
    engaged.store(true, std::memory_order_relaxed);
    Log(reshade::log::level::info, "E_E_FPV anchor: streaming center override engaged");
  }
}

// 撤销: m_overrideStreamingCenterByCamera 是 Boolean, 直接把这一位写 0。
// 不调游戏代码, 就没有"传 null 会不会被解引用"的问题。
// 必须在游戏线程上被调用(见 ConsumeReset) —— 从渲染线程摸游戏对象会崩。
inline void Reset() {
  if (!engaged.load(std::memory_order_relaxed)) return;
  Resolve();
  RefreshInstance();
  if (manager_instance == nullptr) { Warn("reset skipped: no instance"); return; }
  void* field = class_get_field_from_name(manager_class, "m_overrideStreamingCenterByCamera");
  bool cleared = false;
  if (field != nullptr && field_set_value != nullptr) {
    uint8_t before = 0xFF;
    if (field_get_value != nullptr) field_get_value(manager_instance, field, &before);
    uint8_t off = 0;
    field_set_value(manager_instance, field, &off);
    cleared = true;
    char buf[160];
    snprintf(buf, sizeof(buf), "E_E_FPV anchor: reset — flag 0 (was %u)", static_cast<unsigned>(before));
    Log(reshade::log::level::info, buf);
  } else if (set_override_by_camera != nullptr && override_argc == 1) {
    void* args[1] = {nullptr};
    void* exception = nullptr;
    runtime_invoke(set_override_by_camera, manager_instance, args, &exception);
    cleared = exception == nullptr;
  }
  if (!cleared) Warn("reset FAILED");
  engaged.store(false, std::memory_order_relaxed);
}

// 游戏线程每帧调用: 真正执行复位
inline void ConsumeReset() {
  if (pending_reset.exchange(false, std::memory_order_relaxed)) Reset();
}

// 每帧看一次武装状态(锚点迁移开着 + FPV 开着 + 自由相机开着), 只在边沿上动作。
// 复位只挂待办, 由游戏线程的 ConsumeReset 执行。
inline void Poll(bool armed) {
  static bool armed_last = false;
  if (armed && !armed_last) Resolve();
  if (armed_last && !armed) pending_reset.store(true, std::memory_order_relaxed);
  armed_last = armed;
}
} // namespace anchor

} // namespace fpv
