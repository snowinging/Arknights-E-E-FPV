// FPV HUD共享绘制库: DX11(cbuffer b13)与Vulkan(push constant)两份入口共用
// 独立叠加 pass 用: 传入的 base 恒为全透明, 函数返回的是 HUD 颜色与覆盖率(alpha),
// 与画面的合成交给硬件混合完成, 因此不读回任何画面内容。
// payload布局与C++侧 endfield::fpv_swapchain::FpvHudPayload.v[16] 一一对应:
//   v[0],v[1],v[6],v[7] = HDR输出契约哨兵(1,1,4,1), hdr_output.hpp ReadOutputContract
//                         会拿这四个值做校验, 任何一侧都不可改写
//   v[2],v[3] = screen_w, screen_h        (addon.cpp OnPresent 每帧写入)
//   v[4],v[5] = hud_cx, hud_cy(归一化)     (camera_free.hpp OnFrame 写入)
//   v[8]=master, v[9..12]=摇杆(yaw/thr/roll/pitch, 已是±1)
//   v[13]=HUD元素开关位掩码(bit0 中心准星, bit1 人工地平线, bit2 摇杆方框)
//   v[14]=地平线竖直偏移(屏幕高度比例, 已按FOV折算), v[15]=横滚角(deg)
//   v[16]=摇杆方框边长(px), v[17]=摇杆方框间距(px), v[18..19]=预留
struct FpvHudData {
  float4 contract; // x,y = HDR契约哨兵; z,w = screen_w, screen_h
  float4 hud;      // x,y = hud_cx, hud_cy; z,w = HDR契约哨兵
  float4 state;    // x=master, y=stick_yaw(±1), z=stick_thr(±1), w=stick_roll(±1)
  float4 misc;     // x=stick_pitch(±1), y=元素开关位掩码, z=地平线竖直偏移(比例), w=横滚角deg
  float4 extra;    // x=摇杆方框边长(px), y=方框间距(px), z,w=预留
};

float hud_falloff(float d, float half_w) {
  return 1.0 - smoothstep(half_w * 0.6, half_w, d);
}

float4 DrawFpvHud(float2 pix, float4 base, FpvHudData d) {
  if (d.state.x < 0.5) return base;
  float3 white = float3(1, 1, 1);
  float3 amber = float3(1.0, 0.72, 0.3);
  float3 cyan = float3(0.35, 0.9, 1.0);
  float3 red = float3(1.0, 0.35, 0.25);
  float2 screen = max(d.contract.zw, float2(1.0, 1.0));
  float2 center = screen * 0.5;
  // v[13] 位掩码: bit0 中心准星, bit1 人工地平线, bit2 摇杆方框
  const int flags = (int)d.misc.y;

  // 中心准星: 圆环 + 中心点
  if ((flags & 1) != 0) {
    float dc = length(pix - center);
    float ring = hud_falloff(abs(dc - 16.0), 2.0);
    float dotc = 1.0 - smoothstep(1.2, 2.4, dc);
    base = lerp(base, float4(white, 1.0), saturate(ring * 0.7 + dotc * 0.9));
  }

  // 人工地平线: 过中心的直线, 随roll旋转, 随pitch上下偏移, 中段留缺口
  if ((flags & 2) != 0) {
    float roll = radians(d.misc.w);
    float2 dir = float2(cos(roll), sin(roll));
    float2 nrm = float2(-dir.y, dir.x);
    // misc.z 是C++侧按竖直FOV折算好的偏移量(屏幕高度比例), 这里只乘高度
    float off = d.misc.z * screen.y;
    float2 lp = center + nrm * off;
    float dist_line = abs(dot(pix - lp, nrm));
    float along = dot(pix - lp, dir);
    float gap = step(16.0, abs(along));
    float span = step(abs(along), 160.0);
    float horizon = hud_falloff(dist_line, 2.4) * gap * span;
    base = lerp(base, float4(red, 1.0), horizon * 0.75);
  }

  // 摇杆双pad: 左=偏航/油门(琥珀), 右=横滚/俯仰(青)
  // 边长/间距由 payload 的 v[16]/v[17] 给, 布局公式与 ImGui 面板预览完全一致, 面板可直接当对齐参照
  if ((flags & 4) != 0) {
    const float pad = clamp(d.extra.x, 16.0, 240.0);
    const float gap = clamp(d.extra.y, 0.0, 80.0);
    const float2 anchor = float2(d.hud.x * screen.x, d.hud.y * screen.y);
    // 第二个框右边缘距锚点 gap, 两框之间再空 gap
    const float right = anchor.x - gap;
    const float2 origins[2] = {float2(right - pad * 2.0 - gap, anchor.y),
                               float2(right - pad, anchor.y)};
    // 摇杆量已经是 ±1 归一化(payload 里就是最终值, 别再换算一次, 否则油门会冲出方框3倍)
    const float2 sticks[2] = {clamp(float2(d.state.y, d.state.z), -1.0, 1.0),
                              clamp(float2(d.state.w, d.misc.x), -1.0, 1.0)};
    const float3 cols[2] = {amber, cyan};
    const float dot_radius = pad * 0.05;
    [unroll]
    for (int i = 0; i < 2; ++i) {
      const float2 p0 = origins[i];
      const float2 p1 = p0 + float2(pad, pad);
      const float2 c = (p0 + p1) * 0.5;
      const float2 dpad = abs(pix - c);
      // pad 内部遮罩: 十字线只画在框里, 否则会贯穿整屏
      const float in_pad = 1.0 - smoothstep(pad * 0.5 - 1.5, pad * 0.5, max(dpad.x, dpad.y));
      // 边框
      const float2 dv = max(dpad - pad * 0.5, 0.0);
      const float border = hud_falloff(length(dv), 1.5);
      base = lerp(base, float4(white, 1.0), border * 0.55);
      // 十字线
      const float cross = (hud_falloff(dpad.y, 0.8) + hud_falloff(dpad.x, 0.8)) * in_pad;
      base = lerp(base, float4(white, 1.0), saturate(cross) * 0.35);
      // 杆量圆点: 行程与半径按边长缩放
      const float2 dot_pos = c + float2(sticks[i].x, -sticks[i].y) * (pad * 0.5 - dot_radius);
      const float dd = length(pix - dot_pos);
      base = lerp(base, float4(cols[i], 1.0), 1.0 - smoothstep(dot_radius * 0.6, dot_radius, dd));
    }
  }
  return base;
}
