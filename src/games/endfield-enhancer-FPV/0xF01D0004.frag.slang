#include "fpv_hud.hlsli"

// Vulkan路径: 注入数据走 push constant, 大小直接由 FpvHudData 决定
[[vk::push_constant]] cbuffer FpvHudPush { FpvHudData gHud; }
[[vk::binding(0, 0)]] Texture2D<float4> t0;

float4 main(float4 vpos : SV_POSITION) : SV_TARGET {
  float4 base = t0.Load(int3(int2(vpos.xy), 0));
  return DrawFpvHud(vpos.xy, base, gHud);
}
