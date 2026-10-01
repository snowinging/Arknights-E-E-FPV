[[vk::binding(0, 0)]] Texture2D<float4> t0;

float4 main(float4 position : SV_POSITION) : SV_TARGET {
  return t0.Load(int3(int2(position.xy), 0));
}
