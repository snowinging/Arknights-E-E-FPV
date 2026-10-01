float4 main(uint id : SV_VERTEXID, out float2 uv : TEXCOORD0) : SV_POSITION {
  uv.x = (id == 1) ? 2.0 : 0.0;
  uv.y = (id == 2) ? 2.0 : 0.0;
  return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
