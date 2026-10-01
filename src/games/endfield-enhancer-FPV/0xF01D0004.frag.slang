#include "fpv_hud.hlsli"

// Vulkan路径: 注入数据走 push constant, 大小由 FpvHudData 决定
[[vk::push_constant]] cbuffer FpvHudPush { FpvHudData gHud; }

float4 main(float4 vpos : SV_POSITION) : SV_TARGET {
  // 独立叠加 pass: 不读画面, 只输出 HUD 的颜色与 alpha
  return DrawFpvHud(vpos.xy, float4(0.0, 0.0, 0.0, 0.0), gHud);
}
