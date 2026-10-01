#pragma once

#include <cmath>
#include <algorithm>

namespace endfield::camera {
struct Vec3 {
  float x, y, z;
};
struct Quat {
  float x, y, z, w;
};
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator*(Vec3 a, float b) { return {a.x * b, a.y * b, a.z * b}; }
inline Quat operator*(Quat a, Quat b) {
  return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
          a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
          a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
          a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline Vec3 Rotate(Quat q, Vec3 v) {
  const Quat r = q * Quat{v.x, v.y, v.z, 0.f} * Quat{-q.x, -q.y, -q.z, q.w};
  return {r.x, r.y, r.z};
}
inline Quat AxisAngle(Vec3 axis, float degrees) {
  const float angle = degrees * 0.008726646259971648f;
  return {axis.x * std::sin(angle), axis.y * std::sin(angle), axis.z * std::sin(angle), std::cos(angle)};
}

inline float ExpandLookPitch(Quat view, float up, float down) {
  if (up == 1.f && down == 1.f) return 0.f;
  const Vec3 forward = Rotate(view, {0, 0, 1});
  const float pitch = -std::atan2(forward.y, std::hypot(forward.x, forward.z)) * 57.295779513f;
  const float range = pitch < 0.f ? up : down;
  return range == 1.f ? 0.f : std::clamp(pitch * range, -89.f, 89.f) - pitch;
}
inline bool Finite(Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

inline float LateralFacingYaw(Vec3 move, Vec3 view) {
  const float length = std::hypot(move.x, move.z) * std::hypot(view.x, view.z);
  if (!Finite(move) || !Finite(view) || length < .001f) return 0.f;
  const float side = std::clamp((move.x * view.z - move.z * view.x) / length, -1.f, 1.f);
  if (move.x * view.x + move.z * view.z < -length * .382683432f) {
    return std::abs(side) > .382683432f ? std::copysign(45.f, -side) : 0.f;
  }
  return 45.f * side;
}
inline bool Unit(Quat q) {
  const float n = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
  return std::isfinite(n) && n > 0.98f && n < 1.02f;
}
inline Quat BlendRotation(Quat from, Quat to, float amount) {
  amount = std::clamp(amount, 0.f, 1.f);
  if (from.x * to.x + from.y * to.y + from.z * to.z + from.w * to.w < 0.f)
    to = {-to.x, -to.y, -to.z, -to.w};
  Quat q{from.x + (to.x - from.x) * amount, from.y + (to.y - from.y) * amount,
         from.z + (to.z - from.z) * amount, from.w + (to.w - from.w) * amount};
  const float length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (!std::isfinite(length) || length < .0001f) return from;
  return {q.x / length, q.y / length, q.z / length, q.w / length};
}

inline bool FacingRotation(Vec3 forward, Vec3 up, Quat* result) {
  const auto cross = [](Vec3 a, Vec3 b) { return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; };
  const auto normalize = [](Vec3* v) {
    const float length = std::sqrt(v->x * v->x + v->y * v->y + v->z * v->z);
    if (!Finite(*v) || length < .0001f) return false;
    *v = *v * (1.f / length);
    return true;
  };
  if (!normalize(&forward)) return false;
  Vec3 right = cross(up, forward);
  if (!normalize(&right)) return false;
  up = cross(forward, right);
  const float trace = right.x + up.y + forward.z;
  Quat q{};
  if (trace > 0.f) {
    const float s = 2.f * std::sqrt(trace + 1.f);
    q = {(up.z - forward.y) / s, (forward.x - right.z) / s, (right.y - up.x) / s, s * .25f};
  } else if (right.x > up.y && right.x > forward.z) {
    const float s = 2.f * std::sqrt(1.f + right.x - up.y - forward.z);
    q = {s * .25f, (up.x + right.y) / s, (forward.x + right.z) / s, (up.z - forward.y) / s};
  } else if (up.y > forward.z) {
    const float s = 2.f * std::sqrt(1.f + up.y - right.x - forward.z);
    q = {(up.x + right.y) / s, s * .25f, (forward.y + up.z) / s, (forward.x - right.z) / s};
  } else {
    const float s = 2.f * std::sqrt(1.f + forward.z - right.x - up.y);
    q = {(forward.x + right.z) / s, (forward.y + up.z) / s, s * .25f, (right.y - up.x) / s};
  }
  if (!Unit(q)) return false;
  *result = q;
  return true;
}
}
