cbuffer RenoDxSsr : register(b13) {
  float improved_ssr : packoffset(c13.w);
}

Texture2D<float4> t2 : register(t2);
Texture2D<float4> t1 : register(t1);
Texture2D<float4> t0 : register(t0);

SamplerState s1_s : register(s1);
SamplerState s0_s : register(s0);

cbuffer cb1 : register(b1) {
  float4 cb1[6];
}

cbuffer cb0 : register(b0) {
  float4 cb0[1];
}

RWTexture2D<float4> u0 : register(u0);

#define cmp -

[numthreads(8, 8, 1)] void main(uint3 vThreadID : SV_DispatchThreadID) {
  float4 r0, r1;

  r0.xy = (uint2)vThreadID.xy;
  r0.xy = float2(0.5, 0.5) + r0.xy;
  r0.xy = cb0[0].zw * r0.xy;

  r1.xyz = t1.SampleLevel(s1_s, r0.xy, 0).xyz;
  r0.z = max(r1.x, r1.y);
  r0.z = max(r0.z, r1.z);
  r0.z = 1 + -r0.z;
  r0.z = 1 / r0.z;
  r1.xyzw = r1.xyzx * r0.zzzz;

  r0.z = t0.SampleLevel(s0_s, r0.xy, 0).y;
  r0.z = cb1[5].y * r0.z;

  if (improved_ssr >= 0.5f) {
    uint width, height, out_width, out_height;
    t0.GetDimensions(width, height);
    u0.GetDimensions(out_width, out_height);
    if (width == out_width && height == out_height && cb1[5].y == 6.f) {
      r0.z = max(0.f, r0.z - 4.f / 3.f);
    }

    if (r0.z < 0.25) {
      r0.xyzw = r1.xyzw;
    } else {
      float mip0 = floor(r0.z);
      float fraction = saturate(r0.z - mip0);
      float4 color_blurred = t2.SampleLevel(s1_s, r0.xy, 0).xyzx;

      if (mip0 == 0.0) {
        r0.xyzw = lerp(r1.xyzw, color_blurred, fraction);
      } else {
        r0.xyzw = color_blurred;
      }
    }
  } else {
    r0.xyw = t2.SampleLevel(s1_s, r0.xy, 0).xyz;
    r0.z = cmp(r0.z < 0.25);
    r0.xyzw = r0.zzzz ? r1.xyzw : r0.xywx;
  }

  u0[vThreadID.xy] = r0.xyzw;
}
