#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <span>
#include <vector>
#include <string_view>

namespace endfield::camera::mesh {
inline constexpr uint8_t kHeadBone = 1;
inline constexpr uint8_t kNeckBone = 2;

inline bool IsDedicatedHeadMesh(std::wstring_view role) {
  if (!role.starts_with(L"s_actor_") || role.find(L"_lod") == role.npos
      || role.find(L"shadowproxy") != role.npos) return false;
  for (const auto part : {L"_face_", L"_hair_", L"_brow_", L"_eyebrow_",
                          L"_iris_", L"_eyeshadow_", L"_hairshadow_"})
    if (role.find(part) != role.npos) return true;
  return false;
}

inline bool IsBodyMesh(std::wstring_view role) {
  return role.starts_with(L"s_actor_") && role.find(L"_body_") != role.npos
         && role.find(L"_lod") != role.npos && role.find(L"shadowproxy") == role.npos;
}

struct Vertex {
  std::array<float, 3> position;
  std::array<uint16_t, 4> weights;
  std::array<uint16_t, 4> bones;
};

inline bool FilterHeadComponents(std::span<const Vertex> vertices,
                                 std::span<const uint32_t> indices,
                                 std::span<const uint8_t> head_bones,
                                 std::vector<uint32_t>* visible,
                                 bool dedicated_head = false, bool body_skin = false) {
  if (!visible || vertices.empty() || vertices.size() > 200000
      || indices.empty() || indices.size() % 3 != 0 || head_bones.empty()) return false;
  for (const auto& vertex : vertices) {
    uint32_t total = 0;
    for (float coordinate : vertex.position)
      if (!std::isfinite(coordinate)) return false;
    for (size_t i = 0; i < 4; ++i) {
      total += vertex.weights[i];
      if (vertex.weights[i] && vertex.bones[i] >= head_bones.size()) return false;
    }
    if (total < 65531 || total > 65539) return false;
  }
  for (uint32_t index : indices)
    if (index >= vertices.size()) return false;

  if (dedicated_head) {
    std::vector<uint32_t> result(indices.begin(), indices.end());
    for (size_t i = 0; i < result.size(); i += 3)
      result[i + 1] = result[i + 2] = result[i];
    *visible = std::move(result);
    return true;
  }

  std::vector<uint32_t> parent(vertices.size()), order(vertices.size());
  std::iota(parent.begin(), parent.end(), 0u);
  std::iota(order.begin(), order.end(), 0u);
  const auto root = [&](uint32_t index) {
    while (parent[index] != index) {
      parent[index] = parent[parent[index]];
      index = parent[index];
    }
    return index;
  };
  for (size_t i = 0; i < indices.size(); i += 3) {
    parent[root(indices[i + 1])] = root(indices[i]);
    parent[root(indices[i + 2])] = root(indices[i]);
  }

  const auto less = [&](uint32_t a, uint32_t b) {
    if (vertices[a].position != vertices[b].position) return vertices[a].position < vertices[b].position;
    if (vertices[a].weights != vertices[b].weights) return vertices[a].weights < vertices[b].weights;
    return vertices[a].bones < vertices[b].bones;
  };
  std::sort(order.begin(), order.end(), less);
  for (size_t i = 1; i < order.size(); ++i) {
    if (!less(order[i - 1], order[i])) parent[root(order[i])] = root(order[i - 1]);
  }

  std::vector<uint64_t> head_weight(vertices.size()), total_weight(vertices.size());
  std::vector<uint32_t> skin_head_weight(body_skin ? vertices.size() : 0);
  for (uint32_t i = 0; i < vertices.size(); ++i) {
    const auto component = root(i);
    for (size_t j = 0; j < 4; ++j) {
      const uint16_t weight = vertices[i].weights[j];
      total_weight[component] += weight;
      if (weight && head_bones[vertices[i].bones[j]] == kHeadBone) head_weight[component] += weight;
      if (body_skin && weight && (head_bones[vertices[i].bones[j]] == kHeadBone || head_bones[vertices[i].bones[j]] == kNeckBone))
        skin_head_weight[i] += weight;
    }
  }
  std::vector<uint32_t> result(indices.begin(), indices.end());
  for (size_t i = 0; i < indices.size(); i += 3) {
    const auto component = root(indices[i]);
    const bool head_skin = body_skin && skin_head_weight[indices[i]] > 32767
                           && skin_head_weight[indices[i + 1]] > 32767 && skin_head_weight[indices[i + 2]] > 32767;
    if (head_weight[component] * 2 > total_weight[component] || head_skin) {
      result[i + 1] = result[i];
      result[i + 2] = result[i];
    }
  }
  *visible = std::move(result);
  return true;
}
struct NeckFrame {
  std::array<double, 3> center{}, axis{};
  double length = 0;
};
inline NeckFrame MakeNeckFrame(const std::array<float, 16>& bind,
                               const std::array<float, 3>& neck,
                               const std::array<float, 3>& head) {
  for (float v : bind)
    if (!std::isfinite(v)) return {};
  const double a = bind[0], b = bind[4], c = bind[8], d = bind[1], e = bind[5], f = bind[9], g = bind[2], h = bind[6], i = bind[10];
  const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
  if (std::abs(det) < 1e-12) return {};
  const auto invert = [&](const std::array<float, 3>& p) {
    const double x = p[0] - bind[12], y = p[1] - bind[13], z = p[2] - bind[14];
    return std::array<double, 3>{((e * i - f * h) * x + (c * h - b * i) * y + (b * f - c * e) * z) / det,
                                 ((f * g - d * i) * x + (a * i - c * g) * y + (c * d - a * f) * z) / det, ((d * h - e * g) * x + (b * g - a * h) * y + (a * e - b * d) * z) / det};
  };
  NeckFrame result;
  result.center = invert(neck);
  auto end = invert(head);
  for (int k = 0; k < 3; ++k) {
    if (!std::isfinite(result.center[k]) || !std::isfinite(end[k])) return {};
    result.axis[k] = end[k] - result.center[k];
    result.length += result.axis[k] * result.axis[k];
  }
  result.length = std::sqrt(result.length);
  if (result.length < 1e-6) return {};
  for (auto& v : result.axis) v /= result.length;
  return result;
}
struct NeckCapTriangle {
  std::array<uint32_t, 3> vertices;
  size_t material_triangle;
};

inline std::vector<NeckCapTriangle> BuildNeckCaps(std::span<const Vertex> vertices,
                                                  std::span<const uint32_t> original,
                                                  std::span<const uint8_t> roles,
                                                  std::span<const uint32_t> visible, const NeckFrame& frame) {
  if (original.size() != visible.size() || original.size() % 3 || vertices.empty()) return {};
  const bool framed = std::isfinite(frame.length) && frame.length > 1e-6;
  if (!framed && std::find(roles.begin(), roles.end(), kNeckBone) == roles.end()) return {};
  const auto near_neck = [&](const std::array<float, 3>& p) {
    double height = 0, radius = 0;
    for (int k = 0; k < 3; ++k) height += (p[k] - frame.center[k]) * frame.axis[k];
    for (int k = 0; k < 3; ++k) {
      double d = p[k] - frame.center[k] - height * frame.axis[k];
      radius += d * d;
    }
    return height >= -1.5 * frame.length && height <= 1.5 * frame.length && radius <= 9 * frame.length * frame.length;
  };
  std::vector<uint32_t> order(vertices.size()), canonical(vertices.size());
  std::iota(order.begin(), order.end(), 0u);
  const double tolerance = framed ? frame.length * 1e-4 : 1e-6;
  const auto key_position = [&](uint32_t i) {
    std::array<double, 3> key{};
    for (int k = 0; k < 3; ++k) key[k] = std::round(vertices[i].position[k] / tolerance);
    return key;
  };
  std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return key_position(a) < key_position(b); });
  for (size_t i = 0; i < order.size();) {
    size_t end = i + 1;
    while (end < order.size() && key_position(order[i]) == key_position(order[end])) ++end;
    for (size_t j = i; j < end; ++j) {
      canonical[order[j]] = order[j];
      if (end - i > 64) continue;
      for (size_t k = i; k < j; ++k) {
        const auto& a = vertices[order[j]];
        const auto& b = vertices[canonical[order[k]]];
        std::array<std::pair<uint16_t, int>, 8> weights{};
        for (int q = 0; q < 4; ++q) {
          weights[q] = {a.bones[q], int(a.weights[q])};
          weights[q + 4] = {b.bones[q], -int(b.weights[q])};
        }
        std::sort(weights.begin(), weights.end());
        int distance = 0, total = 0;
        uint16_t bone = weights[0].first;
        for (const auto& weight : weights) {
          if (weight.first != bone) {
            distance += std::abs(total);
            total = 0;
            bone = weight.first;
          }
          total += weight.second;
        }
        distance += std::abs(total);
        if (distance <= 128) {
          canonical[order[j]] = canonical[order[k]];
          break;
        }
      }
    }
    i = end;
  }
  struct Edge {
    uint32_t a, b, vertex;
    size_t triangle;
    bool removed;
  };
  std::vector<Edge> edges;
  for (size_t i = 0; i < original.size(); i += 3) {
    const bool removed = visible[i] == visible[i + 1] && visible[i] == visible[i + 2];
    const uint32_t a = original[i], b = original[i + 1], c = original[i + 2];
    if (a >= vertices.size() || b >= vertices.size() || c >= vertices.size()) return {};
    if (canonical[a] == canonical[b] || canonical[b] == canonical[c] || canonical[c] == canonical[a]) continue;
    for (int j = 0; j < 3; ++j) edges.push_back({canonical[original[i + j]], canonical[original[i + (j + 1) % 3]], original[i + j], i, removed});
  }
  const auto key = [](const Edge& e) { return std::array<uint32_t, 2>{std::min(e.a, e.b), std::max(e.a, e.b)}; };
  std::sort(edges.begin(), edges.end(), [&](const Edge& a, const Edge& b) { return key(a) < key(b); });
  std::vector<Edge> boundary;
  for (size_t i = 0; i < edges.size();) {
    size_t end = i + 1;
    while (end < edges.size() && key(edges[i]) == key(edges[end])) ++end;

    if (end - i == 2 && edges[i].a == edges[i + 1].b && edges[i].b == edges[i + 1].a
        && edges[i].removed != edges[i + 1].removed) {
      auto cut = edges[i].removed ? edges[i] : edges[i + 1];
      cut.triangle = edges[i].removed ? edges[i + 1].triangle : edges[i].triangle;
      boundary.push_back(cut);
    } else if (end - i == 1 && !edges[i].removed && framed
               && near_neck(vertices[edges[i].a].position) && near_neck(vertices[edges[i].b].position)) {
      auto cut = edges[i];
      std::swap(cut.a, cut.b);
      cut.vertex = cut.a;
      boundary.push_back(cut);
    }
    i = end;
  }
  if (boundary.empty() || boundary.size() > 4096) return {};
  std::vector<int> next(vertices.size(), -1), incoming(vertices.size(), 0);
  for (size_t i = 0; i < boundary.size(); ++i) {
    auto& e = boundary[i];
    next[e.a] = next[e.a] == -1 ? static_cast<int>(i) : -2;
    ++incoming[e.b];
  }
  std::vector<bool> visited(boundary.size());
  std::vector<NeckCapTriangle> result;
  for (size_t first = 0; first < boundary.size(); ++first) {
    if (visited[first]) continue;
    std::vector<uint32_t> ring;
    size_t current = first;
    bool closed = false;
    while (!visited[current] && ring.size() < 256) {
      visited[current] = true;
      const auto& e = boundary[current];
      if (next[e.a] < 0 || incoming[e.a] != 1 || next[e.b] < 0 || incoming[e.b] != 1) break;
      ring.push_back(e.vertex);
      current = static_cast<size_t>(next[e.b]);
      if (current == first) {
        closed = true;
        break;
      }
    }
    if (!closed || ring.size() < 3) continue;

    bool neck = true;
    for (auto index : ring) {
      bool bound = false;
      for (size_t j = 0; j < 4; ++j)
        if (vertices[index].weights[j] && vertices[index].bones[j] < roles.size()
            && roles[vertices[index].bones[j]] == kNeckBone) bound = true;
      neck &= bound;
    }
    if (framed) {
      std::array<float, 3> center{};
      bool local = true;
      for (auto index : ring) {
        local &= near_neck(vertices[index].position);
        for (int k = 0; k < 3; ++k) center[k] += vertices[index].position[k] / float(ring.size());
      }
      double height = 0, radius = 0;
      for (int k = 0; k < 3; ++k) height += (center[k] - frame.center[k]) * frame.axis[k];
      for (int k = 0; k < 3; ++k) {
        double d = center[k] - frame.center[k] - height * frame.axis[k];
        radius += d * d;
      }
      if (!local || height < -frame.length || height > 1.25 * frame.length || radius > frame.length * frame.length * .64) continue;
    } else if (!neck)
      continue;
    std::array<double, 3> normal{};
    for (size_t i = 0; i < ring.size(); ++i) {
      const auto& a = vertices[ring[i]].position;
      const auto& b = vertices[ring[(i + 1) % ring.size()]].position;
      normal[0] += (a[1] - b[1]) * (a[2] + b[2]);
      normal[1] += (a[2] - b[2]) * (a[0] + b[0]);
      normal[2] += (a[0] - b[0]) * (a[1] + b[1]);
    }
    int drop = 0;
    for (int i = 1; i < 3; ++i)
      if (std::abs(normal[i]) > std::abs(normal[drop])) drop = i;
    using Point = std::array<double, 2>;
    std::vector<Point> points;
    for (auto index : ring) points.push_back({vertices[index].position[(drop + 1) % 3], vertices[index].position[(drop + 2) % 3]});
    const auto cross = [](const Point& a, const Point& b, const Point& c) { return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]); };
    double area = 0, scale = 0;
    for (size_t i = 0; i < points.size(); ++i) {
      const auto& a = points[i];
      const auto& b = points[(i + 1) % points.size()];
      area += a[0] * b[1] - b[0] * a[1];
      scale = std::max(scale, std::max(std::abs(a[0] - points[0][0]), std::abs(a[1] - points[0][1])));
    }
    const double epsilon = std::max(1e-16, scale * scale * 1e-10);
    if (std::abs(area) <= epsilon) continue;
    bool simple = true;
    for (size_t i = 0; i < points.size() && simple; ++i)
      for (size_t j = i + 1; j < points.size(); ++j) {
        if (j == i + 1 || (i == 0 && j + 1 == points.size())) continue;
        const auto& a = points[i];
        const auto& b = points[(i + 1) % points.size()];
        const auto& c = points[j];
        const auto& d = points[(j + 1) % points.size()];
        const double ab_c = cross(a, b, c), ab_d = cross(a, b, d), cd_a = cross(c, d, a), cd_b = cross(c, d, b);

        const bool overlap = std::max(std::min(a[0], b[0]), std::min(c[0], d[0])) <= std::min(std::max(a[0], b[0]), std::max(c[0], d[0])) + epsilon
                             && std::max(std::min(a[1], b[1]), std::min(c[1], d[1])) <= std::min(std::max(a[1], b[1]), std::max(c[1], d[1])) + epsilon;
        if (overlap && std::min(ab_c, ab_d) <= epsilon && std::max(ab_c, ab_d) >= -epsilon
            && std::min(cd_a, cd_b) <= epsilon && std::max(cd_a, cd_b) >= -epsilon) simple = false;
      }
    if (!simple) continue;
    std::vector<size_t> polygon(ring.size());
    std::iota(polygon.begin(), polygon.end(), 0u);
    std::vector<std::array<uint32_t, 3>> triangles;
    const double sign = area > 0 ? 1 : -1;
    while (polygon.size() > 2) {
      bool clipped = false;
      for (size_t i = 0; i < polygon.size(); ++i) {
        const auto a = polygon[(i + polygon.size() - 1) % polygon.size()], b = polygon[i], c = polygon[(i + 1) % polygon.size()];
        const double turn = sign * cross(points[a], points[b], points[c]);
        if (std::abs(turn) <= epsilon && (points[b][0] - points[a][0]) * (points[b][0] - points[c][0]) + (points[b][1] - points[a][1]) * (points[b][1] - points[c][1]) <= epsilon) {
          triangles.push_back({ring[a], ring[b], ring[c]});
          polygon.erase(polygon.begin() + i);
          clipped = true;
          break;
        }
        if (turn <= epsilon) continue;
        bool inside = false;
        for (auto p : polygon)
          if (p != a && p != b && p != c && sign * cross(points[a], points[b], points[p]) >= -epsilon
              && sign * cross(points[b], points[c], points[p]) >= -epsilon && sign * cross(points[c], points[a], points[p]) >= -epsilon) {
            inside = true;
            break;
          }
        if (inside) continue;
        triangles.push_back({ring[a], ring[b], ring[c]});
        polygon.erase(polygon.begin() + i);
        clipped = true;
        break;
      }
      if (!clipped) break;
    }
    if (triangles.size() != ring.size() - 2) continue;
    for (const auto& triangle : triangles) result.push_back({triangle, boundary[first].triangle});
  }
  return result;
}
}
