Texture2D<float4> t1 : register(t1);
Texture2D<float4> t0 : register(t0);
SamplerState s1_s : register(s1);
SamplerState s0_s : register(s0);
cbuffer cb0 : register(b0) { float4 cb0[1]; }
RWTexture2D<float4> u0 : register(u0);

[numthreads(8, 8, 1)] void main(uint3 pixel : SV_DispatchThreadID) {
  float2 uv = t1.SampleLevel(s0_s, (float2(pixel.xy) + 0.5f) * cb0[0].zw, 0).xy;
  float2 offset = float2(pixel.xy - (pixel.xy & 0xffffe)) * cb0[0].zw;
  uint width, height, out_width, out_height;
  t1.GetDimensions(width, height);
  u0.GetDimensions(out_width, out_height);
  if (width == out_width && height == out_height) offset = 0.f;
  float3 color = t0.SampleLevel(s1_s, uv + offset, 0).xyz;
  u0[pixel.xy] = (color / (1.f + max(color.x, max(color.y, color.z)))).xyzx;
}
