Texture2D<float4> t2 : register(t2);

Texture2D<float4> t1 : register(t1);

Texture2D<float4> t0 : register(t0);

SamplerState s1_s : register(s1);

SamplerState s0_s : register(s0);

cbuffer cb1 : register(b1) {
  float4 cb1[6];
}

cbuffer cb0 : register(b0) {
  float4 cb0[3];
}

#define cmp -

RWTexture2D<float4> u0 : register(u0);

[numthreads(8, 8, 1)] void main(uint3 vThreadID : SV_DispatchThreadID) {
  const float4 icb[] = {{0.015625, 0, 0, 0},
                        {0.046875, 0, 0, 0},
                        {0.046875, 0, 0, 0},
                        {0.015625, 0, 0, 0},
                        {0.046875, 0, 0, 0},
                        {0.140625, 0, 0, 0},
                        {0.140625, 0, 0, 0},
                        {0.046875, 0, 0, 0},
                        {0.046875, 0, 0, 0},
                        {0.140625, 0, 0, 0},
                        {0.140625, 0, 0, 0},
                        {0.046875, 0, 0, 0},
                        {0.015625, 0, 0, 0},
                        {0.046875, 0, 0, 0},
                        {0.046875, 0, 0, 0},
                        {0.015625, 0, 0, 0}};
  float4 r0, r1, r2, r3, r4, r5, r6, r7, r8;

  r0.xy = (int2)vThreadID.xy;
  r0.zw = float2(0.5, 0.5) + r0.xy;
  r0.zw = cb1[0].zw * r0.zw;
  r1.x = t1.SampleLevel(s0_s, r0.zw, 0).y;
  float physical_mip = cb1[5].y * r1.x;

  if (all(cb0[0].xy == cb1[0].xy) && cb1[5].y == 6.f) {
    physical_mip = min(physical_mip, max(1.f,
                                         physical_mip - min(1.f / 3.f, max(cb1[5].y - physical_mip, 0.f))));
  }
  r1.y = physical_mip;
  r1.z = cmp(r1.y < 0.25);
  if (r1.z != 0) {
    r2.xyz = t2.SampleLevel(s0_s, r0.zw, 0).xyz;
    r1.w = max(r2.x, r2.y);
    r1.w = max(r1.w, r2.z);
    r1.w = 1 + -r1.w;
    r1.w = 1 / r1.w;
    r2.xyz = r2.xyz * r1.www;
  } else {
    r0.z = t0.SampleLevel(s0_s, r0.zw, 0).x;
    r0.z = cb0[2].z * r0.z + cb0[2].w;
    r0.z = 1 / r0.z;
    r0.w = floor(r1.y);
    r0.xy = cb1[0].zw * r0.xy;
    r1.yz = cb1[0].zw * float2(1.5, 1.5);
    r1.w = exp2(r0.w);
    r3.xy = -r1.yz * r1.ww + r0.xy;
    r3.zw = cb1[0].zw * r1.ww;
    r4.xyz = float3(0, 0, 0);
    r1.w = 0;
    r2.w = 0;
    while (true) {
      r4.w = cmp((int)r2.w >= 4);
      if (r4.w != 0) break;
      r5.x = (int)r2.w;
      r4.w = (uint)r2.w << 2;
      r6.xyz = r4.xyz;
      r5.z = r1.w;
      r5.w = 0;
      while (true) {
        r6.w = cmp((int)r5.w >= 4);
        if (r6.w != 0) break;
        r5.y = (int)r5.w;
        r7.xy = r5.xy * r3.zw + r3.xy;
        r5.y = (int)r4.w + (int)r5.w;
        r6.w = t0.SampleLevel(s0_s, r7.xy, r0.w).x;
        r6.w = cb0[2].z * r6.w + cb0[2].w;
        r6.w = 1 / r6.w;
        r6.w = r6.w + -r0.z;
        r6.w = -0.0144269504 * abs(r6.w);
        r6.w = exp2(r6.w);
        r7.z = icb[r5.y + 0].x * r6.w;
        r7.xyw = t2.SampleLevel(s1_s, r7.xy, r0.w).xyz;
        r7.xyz = r7.xyw * r7.zzz + r6.xyz;
        r5.y = icb[r5.y + 0].x * r6.w + r5.z;
        r6.w = (int)r5.w + 1;
        r6.xyz = r7.xyz;
        r5.z = r5.y;
        r5.w = r6.w;
      }
      r4.xyz = r6.xyz;
      r1.w = r5.z;
      r2.w = (int)r2.w + 1;
    }
    r2.w = 1 + r0.w;
    r2.w = max(0, r2.w);
    r2.w = min(cb1[5].y, r2.w);
    r3.x = exp2(r2.w);
    r0.xy = -r1.yz * r3.xx + r0.xy;
    r1.yz = cb1[0].zw * r3.xx;
    r3.xyzw = float4(0, 0, 0, 0);
    r4.w = 0;
    while (true) {
      r5.x = cmp((int)r4.w >= 4);
      if (r5.x != 0) break;
      r5.x = (int)r4.w;
      r5.z = (uint)r4.w << 2;
      r6.xyz = r3.xyz;
      r5.w = r3.w;
      r6.w = 0;
      while (true) {
        r7.x = cmp((int)r6.w >= 4);
        if (r7.x != 0) break;
        r5.y = (int)r6.w;
        r7.xy = r5.xy * r1.yz + r0.xy;
        r5.y = (int)r5.z + (int)r6.w;
        r7.z = t0.SampleLevel(s0_s, r7.xy, r2.w).x;
        r7.z = cb0[2].z * r7.z + cb0[2].w;
        r7.z = 1 / r7.z;
        r7.z = r7.z + -r0.z;
        r7.z = -0.0144269504 * abs(r7.z);
        r7.z = exp2(r7.z);
        r7.w = icb[r5.y + 0].x * r7.z;
        r8.xyz = t2.SampleLevel(s1_s, r7.xy, r2.w).xyz;
        r7.xyw = r8.xyz * r7.www + r6.xyz;
        r5.y = icb[r5.y + 0].x * r7.z + r5.w;
        r7.z = (int)r6.w + 1;
        r5.w = r5.y;
        r6.xyzw = r7.xywz;
      }
      r3.xyz = r6.xyz;
      r3.w = r5.w;
      r4.w = (int)r4.w + 1;
    }
    r0.x = max(0.00999999978, r1.w);
    r0.xyz = r4.xyz / r0.xxx;
    r1.y = max(0.00999999978, r3.w);
    r1.yzw = r3.xyz / r1.yyy;
    r0.w = physical_mip + -r0.w;
    r1.xyz = r1.yzw + -r0.xyz;
    r0.xyz = r0.www * r1.xyz + r0.xyz;
    r0.w = max(r0.x, r0.y);
    r0.w = max(r0.w, r0.z);
    r0.w = 1 + -r0.w;
    r0.w = 1 / r0.w;
    r2.xyz = r0.xyz * r0.www;
  }
  u0[vThreadID.xy] = r2.xyzx;
}
