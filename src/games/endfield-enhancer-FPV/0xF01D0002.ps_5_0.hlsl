#include "fpv_hud.hlsli"

// DX11路径: 注入数据在b13(框架约定)
cbuffer FpvHudData : register(b13) { FpvHudData gHud; }
Texture2D t0 : register(t0);

float4 main(float4 vpos : SV_POSITION) : SV_TARGET {
  // 1:1取回本帧画面: 代理pass的SRV与本屏同尺寸, 直接Load不走采样器,
  // 避免依赖屏幕尺寸字段(首帧未填充时会把UV放大成越界采样)
  float4 base = t0.Load(int3(int2(vpos.xy), 0));
  return DrawFpvHud(vpos.xy, base, gHud);
}
