#include "fpv_hud.hlsli"

// DX11路径: 注入数据在b13(框架约定)
cbuffer FpvHudData : register(b13) { FpvHudData gHud; }

float4 main(float4 vpos : SV_POSITION) : SV_TARGET {
  // 独立叠加 pass: 不读画面, 只输出 HUD 的颜色与 alpha, 由混合状态与画面合成
  return DrawFpvHud(vpos.xy, float4(0.0, 0.0, 0.0, 0.0), gHud);
}
