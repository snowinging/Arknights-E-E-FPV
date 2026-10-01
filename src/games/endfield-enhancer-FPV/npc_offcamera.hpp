#pragma once
#include "./npc_distance.hpp"
#include "./npc_loading.hpp"
#include <cstdio>
#include <limits>
#include <unordered_set>
#include "./native_hooks.hpp"
#include <unordered_map>

namespace endfield::npc_offcamera {
inline float enabled = 0.f, delay = 2.f, margin = 25.f, protection = 15.f;
inline float npcs_enabled = 0.f, refresh_interval = 0.1f;
inline float closest_first = 1.f;
inline bool unavailable = false;
namespace detail {
using namespace enhancer::detail;
struct Vec3 {
  float x, y, z;
};
using Check = bool (*)(void*, void*);
inline Check blocked = nullptr, in_dialog = nullptr, can_load = nullptr, can_unload = nullptr;
using Cache = bool (*)(void*, const void*, float, void*);
inline Cache rebuild_cache = nullptr;
inline thread_local uint64_t last_reevaluate = 0;
inline thread_local void* reevaluate_manager = nullptr;
inline void* (*main_camera)() = nullptr;
inline void (*project)(void*, const Vec3*, int, Vec3*) = nullptr;
inline size_t lod_offset = 0;
inline bool installed = false;
inline std::atomic_bool active{false}, npc_active{false}, failed{false};
inline std::atomic<float> requested_refresh{0.1f};
inline std::atomic<float> requested_delay{2}, requested_margin{25}, requested_protection{15};
inline std::atomic_uint64_t generation{1};
inline std::atomic_uint64_t recovery_until{0};
struct Entry {
  uintptr_t agent = 0, data = 0, camera = 0, lod = 0;
  int id = 0;
  uint64_t seen = 0, since = 0, checked = 0;
  bool outside = false, denying = false, model_hidden = false;
  uint64_t transition = 0, budget_checked = 0;
};
inline thread_local std::unordered_map<uintptr_t, Entry> entries;
inline thread_local std::unordered_map<uintptr_t, uintptr_t> lod_agents;
inline thread_local uint64_t local_generation = 0;
inline thread_local std::unordered_map<uintptr_t, Entry> model_entries;
using LodTick = void (*)(void*, float, void*, int, void*);
inline LodTick lod_tick = nullptr, downgrade_lod_tick = nullptr;
using ModelTransition = void (*)(void*, void*);
inline ModelTransition hide_model = nullptr, show_model = nullptr;
inline void* (*get_avatar)(void*, void*) = nullptr;
inline Check avatar_visible = nullptr;

struct QueueEnumerator {
  void* dictionary;
  int version, index, key, padding;
  void* value;
  int kind, tail;
};
static_assert(sizeof(QueueEnumerator) == 40 && offsetof(QueueEnumerator, value) == 24);
using QueueTick = void (*)(void*, void*);
using MoveNext = bool (*)(QueueEnumerator*, void*);
using Fade = void (*)(void*, int, float, void*);
inline QueueTick process_queue = nullptr;
inline MoveNext move_next = nullptr;
inline Fade avatar_fade = nullptr;
inline std::atomic_bool nearest{true};
inline thread_local void* pending_queue = nullptr;
inline thread_local void* creating_queue = nullptr;
inline thread_local bool restoring_model = false;
struct QueueSelection {
  void* dictionary = nullptr;
  QueueEnumerator* enumerator = nullptr;
  int version = 0;
  std::unordered_set<int> visited;
};
inline thread_local QueueSelection queue_selection;

inline thread_local std::unordered_map<int, uintptr_t> serviced_jobs;
inline thread_local void* serviced_controller = nullptr;
inline thread_local uint64_t serviced_generation = 0;

inline bool SelectQueueNext(QueueEnumerator* iterator, void* method) {
  if (!iterator || !iterator->dictionary || (iterator->dictionary != pending_queue && iterator->dictionary != creating_queue))
    return move_next(iterator, method);

  const int count = *reinterpret_cast<int*>(static_cast<uint8_t*>(iterator->dictionary) + 0x20);
  if (count < 0 || count > 4096) return move_next(iterator, method);
  auto& selection = queue_selection;
  if (!iterator->index || selection.dictionary != iterator->dictionary || selection.enumerator != iterator || selection.version != iterator->version) {
    selection.dictionary = iterator->dictionary;
    selection.enumerator = iterator;
    selection.version = iterator->version;
    selection.visited.clear();
  }

  QueueEnumerator scan = *iterator, best{};
  scan.index = 0;
  bool found = false, best_serviced = true;
  float best_distance = std::numeric_limits<float>::infinity();
  while (move_next(&scan, method)) {
    if (selection.visited.contains(scan.key)) continue;
    auto* agent = static_cast<uint8_t*>(scan.value);
    const bool creating = iterator->dictionary == creating_queue;
    if (creating && agent) agent = *reinterpret_cast<uint8_t**>(agent + 0x10);
    float distance = agent ? *reinterpret_cast<float*>(agent + 0x58) : std::numeric_limits<float>::infinity();
    if (!std::isfinite(distance) || distance < 0) distance = std::numeric_limits<float>::infinity();
    const auto serviced_entry = serviced_jobs.find(scan.key);
    const bool serviced = creating && serviced_entry != serviced_jobs.end()
                          && serviced_entry->second == reinterpret_cast<uintptr_t>(scan.value);
    if (!found || serviced < best_serviced
        || (serviced == best_serviced && nearest.load() && distance < best_distance)) {
      best = scan;
      best_distance = distance;
      best_serviced = serviced;
      found = true;
    }
  }
  if (!found) {
    *iterator = scan;
    return false;
  }
  if (iterator->dictionary == creating_queue) {
    if (best_serviced || serviced_jobs.size() >= 4096) serviced_jobs.clear();
    serviced_jobs.insert_or_assign(best.key, reinterpret_cast<uintptr_t>(best.value));
  }
  selection.visited.insert(best.key);
  *iterator = best;
  return true;
}
inline bool HookedMoveNext(QueueEnumerator* iterator, void* method) {
  if (failed.load() || (!pending_queue && !creating_queue)) return move_next(iterator, method);
  bool result = false;
  __try {
    result = SelectQueueNext(iterator, method);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    failed = true;
    return move_next(iterator, method);
  }
  return result;
}
inline void HookedProcessQueue(void* self, void* method) {
  if (pending_queue || creating_queue) {
    process_queue(self, method);
    return;
  }
  if (!active.load() || failed.load() || !self) {
    serviced_jobs.clear();
    serviced_controller = nullptr;
    process_queue(self, method);
    return;
  }
  if (serviced_controller != self || serviced_generation != generation.load()) {
    serviced_jobs.clear();
    serviced_controller = self;
    serviced_generation = generation.load();
  }
  __try {
    pending_queue = *reinterpret_cast<void**>(static_cast<uint8_t*>(self) + 0x148);
    creating_queue = *reinterpret_cast<void**>(static_cast<uint8_t*>(self) + 0x188);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    failed = true;
  }
  queue_selection.visited.clear();
  queue_selection.enumerator = nullptr;
  __try {
    process_queue(self, method);
  } __finally {
    pending_queue = nullptr;
    creating_queue = nullptr;
    queue_selection.visited.clear();
  }
}
inline void HookedAvatarFade(void* self, int visible, float seconds, void* method) {
  if (restoring_model && visible > 0) seconds = 0.f;
  avatar_fade(self, visible, seconds, method);
}
inline void SyncGeneration() {
  const auto current = generation.load();
  if (local_generation == current) return;
  entries.clear();
  lod_agents.clear();

  for (auto& [key, entry] : model_entries) {
    entry.since = 0;
    entry.checked = 0;
    entry.outside = false;
    entry.denying = false;
  }
  local_generation = current;
}

inline Entry* IdentityEntry(std::unordered_map<uintptr_t, Entry>* map, void* object, void* data, int id, uint64_t now) {
  const auto key = reinterpret_cast<uintptr_t>(object);
  if (map->size() >= 4096 && !map->contains(key)) {
    std::erase_if(*map, [now](const auto& item) { return now - item.second.seen > 2000 && !item.second.model_hidden; });
    if (map->size() >= 4096) return nullptr;
  }
  auto& entry = (*map)[key];
  if (entry.agent != key || entry.data != reinterpret_cast<uintptr_t>(data) || entry.id != id
      || (entry.seen && now - entry.seen > 2000 && !entry.model_hidden)) {
    entry = {};
    entry.agent = key;
    entry.data = reinterpret_cast<uintptr_t>(data);
    entry.id = id;
  }
  entry.seen = now;
  return &entry;
}

inline Entry* FindEntry(uintptr_t address, bool create, uint64_t now) {
  auto found = entries.find(address);
  if (found != entries.end()) return &found->second;
  if (!create) return nullptr;
  if (entries.size() >= 4096) {
    std::erase_if(entries, [now](const auto& item) { return now - item.second.seen > 2000; });
    if (entries.size() >= 4096) return nullptr;
  }
  return &entries.try_emplace(address).first->second;
}

inline bool Outside(const std::array<Vec3, 8>& points, float padding) {
  bool behind = true, left = true, right = true, below = true, above = true;
  for (const auto& p : points) {
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return false;
    behind &= p.z < -0.1f;
    left &= p.x < -padding;
    right &= p.x > 1.f + padding;
    below &= p.y < -padding;
    above &= p.y > 1.f + padding;
  }
  if (behind) return true;
  for (const auto& p : points)
    if (p.z <= 0.1f) return false;
  return left || right || below || above;
}
inline bool Advance(Entry* entry, bool outside, uint64_t now, uint64_t delay_ms) {
  if (!outside) {
    entry->since = 0;
    entry->denying = false;
    return false;
  }
  if (!entry->since) entry->since = now;
  entry->denying = now - entry->since >= delay_ms;
  return entry->denying;
}
inline bool ProjectEntry(Entry* entry, const Vec3& position, float extent, uint64_t now) {
  void* camera = main_camera();
  if (!camera) return Advance(entry, false, now, 0);
  if (entry->camera != reinterpret_cast<uintptr_t>(camera)) {
    entry->camera = reinterpret_cast<uintptr_t>(camera);
    entry->checked = 0;
    entry->since = 0;
  }
  if (!entry->checked || now - entry->checked >= static_cast<uint64_t>(requested_refresh.load() * 1000.f)) {
    if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))
      return Advance(entry, false, now, 0);
    std::array<Vec3, 8> viewport;
    for (size_t i = 0; i < viewport.size(); ++i) {
      const Vec3 corner{position.x + (i & 1 ? extent : -extent), position.y + (i & 2 ? extent * 2.f : -0.5f), position.z + (i & 4 ? extent : -extent)};
      project(camera, &corner, 2, &viewport[i]);
    }

    entry->outside = Outside(viewport, requested_margin.load() * 0.01f);
    entry->checked = now;
  }
  return Advance(entry, entry->outside, now, static_cast<uint64_t>(requested_delay.load() * 1000.f));
}
inline bool Reject(void* agent) {
  SyncGeneration();
  if (!agent) return false;
  auto* bytes = static_cast<uint8_t*>(agent);
  auto* data = *reinterpret_cast<uint8_t**>(bytes + 0x28);
  if (!data || bytes[0x51]) return false;
  const auto address = reinterpret_cast<uintptr_t>(agent);
  const uint64_t now = GetTickCount64();
  auto* timer = FindEntry(address, true, now);
  if (!timer) return false;
  auto& entry = *timer;
  const bool loaded = *reinterpret_cast<void**>(bytes + 0x30) != nullptr;
  const int id = *reinterpret_cast<int*>(bytes + 0x18);
  if (entry.agent != address || entry.data != reinterpret_cast<uintptr_t>(data) || entry.id != id
      || (entry.seen && now - entry.seen > 2000)) {
    entry = {};
    entry.agent = address;
    entry.data = reinterpret_cast<uintptr_t>(data);
    entry.id = id;
  }
  entry.seen = now;

  if (!loaded && (*reinterpret_cast<void**>(bytes + 0x40) || *reinterpret_cast<void**>(bytes + 0x68))) {
    return Advance(&entry, false, now, 0);
  }
  const float distance_sq = *reinterpret_cast<float*>(bytes + 0x58);
  const float near_distance = requested_protection.load();
  if (!std::isfinite(distance_sq) || distance_sq < 0 || distance_sq <= near_distance * near_distance) {
    return Advance(&entry, false, now, 0);
  }
  if (loaded) {
    auto* crowd = *reinterpret_cast<uint8_t**>(bytes + 0x48);
    if (!crowd) crowd = *reinterpret_cast<uint8_t**>(*reinterpret_cast<uint8_t**>(bytes + 0x30) + 0x218);
    if (!crowd) return Advance(&entry, false, now, 0);
    auto* lod = *reinterpret_cast<uint8_t**>(crowd + lod_offset);
    if (!lod) return Advance(&entry, false, now, 0);
    if (entry.lod && entry.lod != reinterpret_cast<uintptr_t>(lod)) lod_agents.erase(entry.lod);
    entry.lod = reinterpret_cast<uintptr_t>(lod);
    if (lod_agents.size() >= 4096 && !lod_agents.contains(entry.lod)) lod_agents.clear();
    lod_agents.insert_or_assign(entry.lod, address);
    if (lod[0x38]) return Advance(&entry, false, now, 0);
    if (lod[0x4f]) return Advance(&entry, false, now, 0);
    if (in_dialog(lod, nullptr)) return Advance(&entry, false, now, 0);
  }
  Vec3 position;
  std::memcpy(&position, data + 0x3c, sizeof(position));
  return ProjectEntry(&entry, position, 1.5f, now);
}
inline bool HookedBlocked(void* agent, void* method) {
  const bool original = blocked(agent, method);
  if (original || !active.load() || failed.load()) return original;
  bool result = false;
  __try {
    result = Reject(agent);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    failed = true;
  }
  return original || result;
}
inline bool HookedCache(void* self, const void* position, float radius, void* method) {
  const bool original = rebuild_cache(self, position, radius, method);
  const uint64_t now = GetTickCount64();
  if (failed.load() || (!active.load() && now >= recovery_until.load())) return original;
  if (original || self != reevaluate_manager || now - last_reevaluate >= static_cast<uint64_t>(requested_refresh.load() * 1000.f)) {
    last_reevaluate = now;
    reevaluate_manager = self;

    return true;
  }
  return false;
}
inline bool RejectModel(void* self) {
  SyncGeneration();
  if (!self) return false;
  auto* bytes = static_cast<uint8_t*>(self);
  const bool atmospheric = bytes[0x4c] != 0;
  if (!(atmospheric ? active.load() : npc_active.load())) return false;
  auto* comp = *reinterpret_cast<uint8_t**>(bytes + 0x20);
  if (!comp) return false;
  const uint64_t now = GetTickCount64();
  auto* entry = IdentityEntry(&model_entries, self, comp, 0, now);
  if (!entry) return false;
  const float distance = *reinterpret_cast<float*>(bytes + 0x2c);
  if (bytes[0x38] || bytes[0x4f] || in_dialog(self, nullptr)
      || !std::isfinite(distance) || distance <= requested_protection.load())
    return Advance(entry, false, now, 0);

  auto* entity = *reinterpret_cast<uint8_t**>(comp + 0x50);
  auto* transform = entity ? *reinterpret_cast<uint8_t**>(entity + 0xb0) : nullptr;
  if (!transform) return Advance(entry, false, now, 0);
  Vec3 position;
  std::memcpy(&position, transform + 0x90, sizeof(position));
  return ProjectEntry(entry, position, 1.5f, now);
}
inline bool CameraRejectsLod(void* self) {
  if (failed.load() || (!active.load() && !npc_active.load())) return false;
  __try {
    if (active.load() && local_generation == generation.load()) {
      const auto found = lod_agents.find(reinterpret_cast<uintptr_t>(self));
      if (found != lod_agents.end()) {
        const auto* entry = FindEntry(found->second, false, GetTickCount64());
        if (entry && entry->lod == reinterpret_cast<uintptr_t>(self) && GetTickCount64() - entry->seen < 2000) {
          auto* bytes = static_cast<uint8_t*>(self);
          return entry->denying && !bytes[0x38] && !bytes[0x4f] && !in_dialog(self, nullptr);
        }
      }
    }
    return RejectModel(self);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    failed = true;
    return false;
  }
}
inline void ApplyModelTransition(void* self) {
  SyncGeneration();
  auto* bytes = static_cast<uint8_t*>(self);
  if (!bytes) return;
  if (bytes[0x4c]) return;
  const auto pending = model_entries.find(reinterpret_cast<uintptr_t>(self));
  if (pending != model_entries.end() && pending->second.model_hidden) pending->second.checked = 0;
  const bool reject = !failed.load() && npc_active.load() && RejectModel(self);
  const auto found = model_entries.find(reinterpret_cast<uintptr_t>(self));
  if (found == model_entries.end()) return;
  auto& entry = found->second;
  if (entry.data != reinterpret_cast<uintptr_t>(*reinterpret_cast<void**>(bytes + 0x20))) return;
  void* avatar = get_avatar(self, nullptr);
  if (!avatar) return;
  const uint64_t now = GetTickCount64();
  if (reject) {
    if ((!entry.model_hidden || bytes[0x3d] || avatar_visible(avatar, nullptr))
        && (!entry.transition || now - entry.transition >= 100)) {
      hide_model(self, nullptr);
      entry.model_hidden = true;
      entry.transition = now;
    }
  } else if (entry.model_hidden) {
    if (can_load(self, nullptr) || !can_unload(self, nullptr)) {
      const bool previous = restoring_model;
      restoring_model = true;
      __try {
        show_model(self, nullptr);
      } __finally {
        restoring_model = previous;
      }

      if (bytes[0x3d]) {
        entry.model_hidden = false;
        entry.transition = 0;
      }
    }
  }
}

using LodManagerTick = void (*)(void*, void*, float, void*);
inline LodManagerTick lod_manager_tick = nullptr;
inline void* (*get_lod_manager)(void*) = nullptr;
inline int (*cpu_budget)(void*) = nullptr;
inline const int* budget_model_cap = nullptr;
inline const int* budget_crowd_cap = nullptr;
inline const int* budget_moving_reserve = nullptr;
inline void** budget_npc_manager = nullptr;
inline thread_local void* ranking_manager = nullptr;
inline thread_local void* ranking_list = nullptr;
inline thread_local int ranking_version = 0;
inline thread_local std::vector<int> rank_credits, crowd_rank_credits;
inline thread_local void* ranking_crowd_list = nullptr;
inline thread_local int ranking_crowd_version = 0;
inline std::atomic_uint64_t budget_epoch{1};

inline bool ReleasesModelSlot(void* self) {
  if (!self || failed.load()) return false;
  auto* bytes = static_cast<uint8_t*>(self);
  if (bytes[0x3d]) return false;
  if (bytes[0x4c]) {
    const float distance = *reinterpret_cast<float*>(bytes + 0x2c);
    if (!active.load() || !std::isfinite(distance) || distance <= requested_protection.load()
        || !CameraRejectsLod(self)) return false;
  } else {
    if (!npc_active.load()) return false;
    const auto found = model_entries.find(reinterpret_cast<uintptr_t>(self));
    if (found == model_entries.end() || !found->second.model_hidden
        || found->second.data != reinterpret_cast<uintptr_t>(*reinterpret_cast<void**>(bytes + 0x20))) return false;

    const auto epoch = budget_epoch.load();
    if (found->second.budget_checked != epoch) {
      found->second.checked = 0;
      found->second.budget_checked = epoch;
    }
    if (!RejectModel(self)) return false;
  }
  void* avatar = get_avatar(self, nullptr);
  return avatar && !avatar_visible(avatar, nullptr);
}

inline int BuildRankCredits(void* manager, std::vector<int>* credits, size_t list_offset = 0x20) {
  auto* list = manager ? *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(manager) + list_offset) : nullptr;
  if (!list) return -1;
  const int count = *reinterpret_cast<int*>(list + 0x18);
  auto* items = *reinterpret_cast<uint8_t**>(list + 0x10);
  if (count < 0 || count > 4096 || (count && (!items || *reinterpret_cast<uintptr_t*>(items + 0x18) < static_cast<uintptr_t>(count)))) return -1;
  const int version = *reinterpret_cast<int*>(list + 0x1c);
  credits->clear();
  credits->reserve(count + 1);
  credits->push_back(0);
  for (int i = 0; i < count; ++i) {
    void* lod = *reinterpret_cast<void**>(items + 0x20 + i * sizeof(void*));
    credits->push_back(credits->back() + (ReleasesModelSlot(lod) ? 1 : 0));
  }
  if (*reinterpret_cast<int*>(list + 0x1c) != version || *reinterpret_cast<int*>(list + 0x18) != count) {
    credits->clear();
    return -1;
  }
  return count;
}
inline int EffectiveModelRank(void* self, int rank) {
  if (!ranking_manager || rank < 0 || (!npc_active.load() && !active.load()) || failed.load()) return rank;
  if (rank_credits.empty()) {
    if (BuildRankCredits(ranking_manager, &rank_credits) < 0) return rank;
    ranking_list = *reinterpret_cast<void**>(static_cast<uint8_t*>(ranking_manager) + 0x20);
    ranking_version = *reinterpret_cast<int*>(static_cast<uint8_t*>(ranking_list) + 0x1c);
  }
  auto* list = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(ranking_manager) + 0x20);
  if (list != ranking_list || *reinterpret_cast<int*>(list + 0x1c) != ranking_version
      || *reinterpret_cast<int*>(list + 0x18) != static_cast<int>(rank_credits.size()) - 1) return rank;
  const int count = static_cast<int>(rank_credits.size()) - 1;
  int reclaimed = 0;
  if (rank < count) {
    auto* items = *reinterpret_cast<uint8_t**>(list + 0x10);
    if (*reinterpret_cast<void**>(items + 0x20 + rank * sizeof(void*)) != self) return rank;
    reclaimed = rank_credits[rank];
  } else {
    if (crowd_rank_credits.empty()) {
      if (BuildRankCredits(ranking_manager, &crowd_rank_credits, 0x28) < 0) return rank;
      ranking_crowd_list = *reinterpret_cast<void**>(static_cast<uint8_t*>(ranking_manager) + 0x28);
      ranking_crowd_version = *reinterpret_cast<int*>(static_cast<uint8_t*>(ranking_crowd_list) + 0x1c);
    }
    auto* crowd_list = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(ranking_manager) + 0x28);
    const int index = rank - count;
    if (crowd_list != ranking_crowd_list || *reinterpret_cast<int*>(crowd_list + 0x1c) != ranking_crowd_version
        || *reinterpret_cast<int*>(crowd_list + 0x18) != static_cast<int>(crowd_rank_credits.size()) - 1
        || index >= static_cast<int>(crowd_rank_credits.size()) - 1) return rank;
    auto* items = *reinterpret_cast<uint8_t**>(crowd_list + 0x10);
    if (*reinterpret_cast<void**>(items + 0x20 + index * sizeof(void*)) != self) return rank;
    reclaimed = rank_credits.back() + crowd_rank_credits[index];
  }
  return rank - reclaimed;
}
inline void HookedDowngradeLodTick(void* self, float delta, void* camera, int rank, void* method) {
  int effective_rank = rank;
  __try {
    if (self) effective_rank = EffectiveModelRank(self, rank);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    failed = true;
  }
  downgrade_lod_tick(self, delta, camera, effective_rank, method);
}
inline void HookedLodManagerTick(void* self, void* all_entities, float delta, void* method) {
  if ((!npc_active.load() && !active.load()) || failed.load() || ranking_manager) {
    lod_manager_tick(self, all_entities, delta, method);
    return;
  }
  ranking_manager = self;
  ranking_list = nullptr;
  ranking_crowd_list = nullptr;
  rank_credits.clear();
  crowd_rank_credits.clear();
  __try {
    lod_manager_tick(self, all_entities, delta, method);
  } __finally {
    ranking_manager = nullptr;
    ranking_list = nullptr;
    ranking_crowd_list = nullptr;
    rank_credits.clear();
    crowd_rank_credits.clear();
  }
}
inline int ReclaimCrowdBudget(int original) {
  void* manager = get_lod_manager(nullptr);
  std::vector<int> credits;
  const int regular_count = BuildRankCredits(manager, &credits);
  if (regular_count < 0 || credits.back() == 0) return original;
  auto* npc_manager = static_cast<uint8_t*>(*budget_npc_manager);
  if (!npc_manager) return original;
  const int cap = *budget_model_cap, crowd_cap = *budget_crowd_cap;
  const int moving = *budget_moving_reserve, other = *reinterpret_cast<int*>(npc_manager + 0xb8);
  if (cap < 0 || cap > 10000 || crowd_cap < 0 || crowd_cap > 10000 || moving < 0 || moving > 10000 || other < 0 || other > 10000) return original;
  const int other_capacity = crowd_cap - moving - other;

  if (original != std::max(0, std::min(other_capacity, cap - regular_count))) return original;
  return std::max(0, std::min(other_capacity, cap - (regular_count - credits.back())));
}
inline int HookedCpuBudget(void* method) {
  const int original = cpu_budget(method);
  if (!npc_active.load() || failed.load()) return original;
  __try {
    return ReclaimCrowdBudget(original);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    failed = true;
    return original;
  }
}
inline void HookedLodTick(void* self, float delta, void* camera, int rank, void* method) {
  if (!failed.load() && self) {
    __try {
      auto* bytes = static_cast<uint8_t*>(self);
      const bool selected = bytes[0x4c] ? active.load() : npc_active.load();
      if (selected || GetTickCount64() < recovery_until.load()) {
        auto* countdown = reinterpret_cast<float*>(bytes + 0x14);
        const float interval = requested_refresh.load();
        if (std::isfinite(*countdown) && *countdown > interval) {
          *countdown = interval;
        }
        CameraRejectsLod(self);
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      failed = true;
    }
  }
  int effective_rank = rank;
  __try {
    if (self) effective_rank = EffectiveModelRank(self, rank);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    failed = true;
  }
  lod_tick(self, delta, camera, effective_rank, method);
  __try {
    ApplyModelTransition(self);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    failed = true;
  }
}
inline bool HookedCanLoad(void* self, void* method) {
  const bool original = can_load(self, method);
  return original && !CameraRejectsLod(self);
}
inline bool HookedCanUnload(void* self, void* method) {
  const bool original = can_unload(self, method);
  return original || CameraRejectsLod(self);
}
inline bool UpdateHooks(bool attach) {
  return native_hooks::Update(attach ? "Off-camera entities install" : "Off-camera entities removal", [attach]() -> LONG {
    LONG result = attach ? DetourAttach(&blocked, HookedBlocked) : DetourDetach(&blocked, HookedBlocked);
    if (result == NO_ERROR) result = attach ? DetourAttach(&rebuild_cache, HookedCache) : DetourDetach(&rebuild_cache, HookedCache);
    if (result == NO_ERROR) result = attach ? DetourAttach(&can_load, HookedCanLoad) : DetourDetach(&can_load, HookedCanLoad);
    if (result == NO_ERROR) result = attach ? DetourAttach(&can_unload, HookedCanUnload) : DetourDetach(&can_unload, HookedCanUnload);
    if (result == NO_ERROR) result = attach ? DetourAttach(&lod_tick, HookedLodTick) : DetourDetach(&lod_tick, HookedLodTick);
    if (result == NO_ERROR) result = attach ? DetourAttach(&downgrade_lod_tick, HookedDowngradeLodTick) : DetourDetach(&downgrade_lod_tick, HookedDowngradeLodTick);
    if (result == NO_ERROR) result = attach ? DetourAttach(&lod_manager_tick, HookedLodManagerTick) : DetourDetach(&lod_manager_tick, HookedLodManagerTick);
    if (result == NO_ERROR) result = attach ? DetourAttach(&cpu_budget, HookedCpuBudget) : DetourDetach(&cpu_budget, HookedCpuBudget);
    if (result == NO_ERROR) result = attach ? DetourAttach(&process_queue, HookedProcessQueue) : DetourDetach(&process_queue, HookedProcessQueue);
    if (result == NO_ERROR) result = attach ? DetourAttach(&move_next, HookedMoveNext) : DetourDetach(&move_next, HookedMoveNext);
    if (result == NO_ERROR) result = attach ? DetourAttach(&avatar_fade, HookedAvatarFade) : DetourDetach(&avatar_fade, HookedAvatarFade);
    return result;
  });
}
inline bool Resolve() {
  if (!npc_distance::detail::SupportedBuild()) return false;
  auto module = GetModuleHandleW(L"GameAssembly.dll");
  auto* base = reinterpret_cast<uint8_t*>(module);
  constexpr uint8_t downgrade_lod_tick_bytes[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7c, 0x24, 0x18, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec, 0x70, 0x0f, 0x29, 0x74, 0x24, 0x60, 0x41, 0x8b, 0xf1};
  if (std::memcmp(base + 0x3064930, downgrade_lod_tick_bytes, sizeof(downgrade_lod_tick_bytes))) return false;
  downgrade_lod_tick = reinterpret_cast<LodTick>(base + 0x3064930);
  constexpr uint8_t lod_manager_tick_bytes[] = {0x40, 0x53, 0x57, 0x48, 0x81, 0xec, 0xb8, 0x00, 0x00, 0x00, 0x48, 0x8b, 0xf9, 0x0f, 0x29, 0xb4, 0x24, 0x90, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x0d, 0xe4, 0x88, 0x31, 0x0a, 0x0f, 0x28, 0xf2, 0x48, 0x8b, 0xda};
  if (std::memcmp(base + 0x2cdac90, lod_manager_tick_bytes, sizeof(lod_manager_tick_bytes))) return false;
  lod_manager_tick = reinterpret_cast<decltype(lod_manager_tick)>(base + 0x2cdac90);
  constexpr uint8_t get_lod_manager_bytes[] = {0x48, 0x83, 0xec, 0x28, 0x48, 0x8b, 0x15, 0x15, 0xd4, 0xd6, 0x09, 0x83, 0xba, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x84, 0x8a, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x82, 0xb8, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x08};
  if (std::memcmp(base + 0x3286170, get_lod_manager_bytes, sizeof(get_lod_manager_bytes))) return false;
  get_lod_manager = reinterpret_cast<decltype(get_lod_manager)>(base + 0x3286170);
  constexpr uint8_t cpu_budget_bytes[] = {0x48, 0x83, 0xec, 0x28, 0x80, 0x3d, 0xe2, 0xe5, 0xc1, 0x0a, 0x00, 0x0f, 0x84, 0x8c, 0x01, 0x00, 0x00, 0x48, 0x8b, 0x15, 0xd8, 0xd6, 0xd6, 0x09, 0x83, 0xba, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x84, 0xc0, 0x01, 0x00, 0x00};
  if (std::memcmp(base + 0x3285ea0, cpu_budget_bytes, sizeof(cpu_budget_bytes))) return false;
  cpu_budget = reinterpret_cast<decltype(cpu_budget)>(base + 0x3285ea0);
  constexpr uint8_t process_queue_bytes[] = {0x4c, 0x8b, 0xdc, 0x49, 0x89, 0x4b, 0x08, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xec, 0x40, 0x01, 0x00, 0x00, 0x41, 0x0f, 0x29, 0x73, 0xb8, 0x4c, 0x8b, 0xe9};
  if (std::memcmp(base + 0x3284de0, process_queue_bytes, sizeof(process_queue_bytes))) return false;
  process_queue = reinterpret_cast<decltype(process_queue)>(base + 0x3284de0);
  constexpr uint8_t move_next_bytes[] = {0x48, 0x89, 0x5c, 0x24, 0x18, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b, 0x39, 0x48, 0x8b, 0xd9, 0x8b, 0x71, 0x08, 0x48, 0x85, 0xff, 0x0f, 0x84, 0x5a, 0x01, 0x00, 0x00, 0x3b, 0x77, 0x2c, 0x0f, 0x85, 0x49, 0x01, 0x00, 0x00, 0x48, 0x89, 0x6c, 0x24, 0x58, 0x0f, 0x1f, 0x00, 0x48, 0x8b, 0x03, 0x48, 0x85, 0xc0, 0x0f, 0x84, 0x4f, 0x01, 0x00, 0x00, 0x48, 0x8b, 0xc8, 0x8b, 0x40, 0x20, 0x39, 0x43, 0x0c, 0x0f, 0x83, 0x13, 0x01, 0x00, 0x00, 0x48, 0x63, 0x73, 0x0c, 0x48, 0x8b, 0x79, 0x18, 0x8d, 0x46, 0x01, 0x89, 0x43, 0x0c, 0x48, 0x85, 0xff, 0x0f, 0x84, 0x1d, 0x01, 0x00, 0x00, 0x3b, 0x77, 0x18, 0x0f, 0x83, 0x1a, 0x01, 0x00, 0x00, 0x48, 0x8d, 0x0c, 0x76, 0x83, 0x7c, 0xcf, 0x20, 0x00, 0x48, 0x8d, 0x2c, 0xcf, 0x7c, 0xb6, 0x48, 0x8b, 0x4a, 0x20, 0x8b, 0x7d, 0x28};
  if (std::memcmp(base + 0x2ed1410, move_next_bytes, sizeof(move_next_bytes))) return false;
  move_next = reinterpret_cast<decltype(move_next)>(base + 0x2ed1410);
  constexpr uint8_t avatar_fade_bytes[] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x56, 0x48, 0x83, 0xec, 0x40, 0x8b, 0xf2, 0x0f, 0x29, 0x74, 0x24, 0x30, 0x48, 0x8b, 0xd9, 0x33, 0xd2, 0xb9, 0x9b, 0x08, 0x00, 0x00, 0x0f, 0x28, 0xf2, 0xe8, 0xbd, 0x72, 0x5a, 0xff};
  if (std::memcmp(base + 0x36f0690, avatar_fade_bytes, sizeof(avatar_fade_bytes))) return false;
  avatar_fade = reinterpret_cast<decltype(avatar_fade)>(base + 0x36f0690);
  constexpr uint8_t hide_model_bytes[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0x33, 0xd2, 0xb9, 0x4f, 0x06, 0x00, 0x00, 0xe8, 0x87, 0x1d, 0xc3, 0xff, 0x33, 0xd2, 0x84, 0xc0, 0x0f, 0x85, 0x63, 0x07, 0xc8, 0x01};
  if (std::memcmp(base + 0x3065bd0, hide_model_bytes, sizeof(hide_model_bytes))) return false;
  hide_model = reinterpret_cast<decltype(hide_model)>(base + 0x3065bd0);
  constexpr uint8_t show_model_bytes[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b, 0xd9, 0x0f, 0x29, 0x74, 0x24, 0x20, 0xb9, 0x92, 0x08, 0x00, 0x00, 0x33, 0xd2, 0xe8, 0x02, 0x1f, 0xc3, 0xff, 0x84, 0xc0};
  if (std::memcmp(base + 0x3065a50, show_model_bytes, sizeof(show_model_bytes))) return false;
  show_model = reinterpret_cast<decltype(show_model)>(base + 0x3065a50);
  constexpr uint8_t get_avatar_bytes[] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0x48, 0x8b, 0x0d, 0x70, 0x80, 0x31, 0x0a, 0x83, 0xb9, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x74, 0x32, 0x48, 0x8b, 0x81, 0xb8, 0x00, 0x00, 0x00};
  if (std::memcmp(base + 0x2cdb510, get_avatar_bytes, sizeof(get_avatar_bytes))) return false;
  get_avatar = reinterpret_cast<decltype(get_avatar)>(base + 0x2cdb510);
  constexpr uint8_t avatar_visible_bytes[] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b, 0xd9, 0x48, 0x8b, 0x0d, 0x50, 0x1f, 0x90, 0x09, 0x83, 0xb9, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x84, 0xc9, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x81, 0xb8, 0x00, 0x00, 0x00};
  if (std::memcmp(base + 0x36f1630, avatar_visible_bytes, sizeof(avatar_visible_bytes))) return false;
  avatar_visible = reinterpret_cast<decltype(avatar_visible)>(base + 0x36f1630);
  constexpr uint8_t lod_tick_bytes[] = {0x48, 0x8b, 0xc4, 0x53, 0x57, 0x48, 0x83, 0xec, 0x78, 0x0f, 0x29, 0x70, 0xd8, 0x33, 0xd2, 0x44, 0x0f, 0x29, 0x40, 0xb8, 0x49, 0x8b, 0xf8, 0x45, 0x0f, 0x57, 0xc0, 0xc7, 0x40, 0x20, 0x00, 0x00, 0x00, 0x00};
  if (std::memcmp(base + 0x2cdb7a0, lod_tick_bytes, sizeof(lod_tick_bytes))) return false;
  lod_tick = reinterpret_cast<decltype(lod_tick)>(base + 0x2cdb7a0);
  constexpr uint8_t load_bytes[] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b, 0xd9, 0x33, 0xd2, 0xb9, 0x8f, 0x08, 0x00, 0x00, 0xe8, 0x1b, 0x1a, 0xc3, 0xff, 0x84, 0xc0, 0x0f, 0x85, 0xfd, 0x04, 0xc8, 0x01, 0x38, 0x43, 0x4f};
  constexpr uint8_t unload_bytes[] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x80, 0x3d, 0x87, 0x34, 0xb0, 0x09, 0x00, 0x48, 0x8b, 0xd9, 0x74, 0x67, 0x33, 0xd2, 0xb9, 0xb6, 0x08, 0x00, 0x00, 0xe8, 0x42, 0x3d, 0x8f, 0xfe, 0x33, 0xd2};
  constexpr uint8_t cache_bytes[] = {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x10, 0x48, 0x89, 0x78, 0x18, 0x4c, 0x89, 0x60, 0x20, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xec, 0xe0, 0x00, 0x00, 0x00, 0x0f, 0x29, 0x70, 0xd8, 0x0f, 0x29, 0x78, 0xc8, 0x44, 0x0f, 0x29, 0x40, 0xb8, 0x44, 0x0f, 0x28, 0xc2};
  constexpr uint8_t blocked_bytes[] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x80, 0x3d, 0x1d, 0xd8, 0xc5, 0x0a, 0x00, 0x48, 0x8b, 0xd9, 0x74, 0x4c, 0x33, 0xd2, 0xb9, 0xee, 0xeb, 0x00, 0x00, 0xe8, 0xf2, 0x0c, 0xa5, 0xff, 0x84, 0xc0};
  constexpr uint8_t dialog_bytes[] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x05, 0xe3, 0x81, 0x31, 0x0a, 0x48, 0x8b, 0xd9, 0x83, 0xb8, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x84, 0xd4, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x90, 0xb8, 0x00, 0x00, 0x00};
  if (std::memcmp(base + 0x3065f40, load_bytes, sizeof(load_bytes))
      || std::memcmp(base + 0x43a3c10, unload_bytes, sizeof(unload_bytes))
      || std::memcmp(base + 0x34203c0, cache_bytes, sizeof(cache_bytes))
      || std::memcmp(base + 0x3246c60, blocked_bytes, sizeof(blocked_bytes))
      || std::memcmp(base + 0x2cdb3a0, dialog_bytes, sizeof(dialog_bytes))) return false;
  void* (*resolve_icall)(const char*) = nullptr;
  if (!ResolveExport(module, "il2cpp_resolve_icall", &resolve_icall)) return false;
  main_camera = reinterpret_cast<decltype(main_camera)>(resolve_icall("UnityEngine.Camera::get_main()"));
  project = reinterpret_cast<decltype(project)>(resolve_icall("UnityEngine.Camera::WorldToViewportPoint_Injected(UnityEngine.Vector3&,UnityEngine.Camera/MonoOrStereoscopicEye,UnityEngine.Vector3&)"));
  auto image = FindImage("Gameplay.Beyond.dll");
  if (!image || !main_camera || !project) return false;

  size_t (*class_count)(void*) = nullptr;
  void* (*image_class)(void*, size_t) = nullptr;
  const char* (*class_name)(void*) = nullptr;
  void* (*declaring_class)(void*) = nullptr;
  int (*field_flags)(void*) = nullptr;
  if (!ResolveExport(module, "il2cpp_image_get_class_count", &class_count)
      || !ResolveExport(module, "il2cpp_image_get_class", &image_class)
      || !ResolveExport(module, "il2cpp_class_get_name", &class_name)
      || !ResolveExport(module, "il2cpp_class_get_declaring_type", &declaring_class)
      || !ResolveExport(module, "il2cpp_field_get_flags", &field_flags)) return false;
  void* agent = nullptr;
  void* creation_job = nullptr;
  for (size_t i = 0, count = class_count(image); i < count; ++i) {
    void* candidate = image_class(image, i);
    if (candidate && std::strcmp(class_name(candidate), "AtmosphereNpcCpuCreateJob") == 0) {
      if (creation_job) return false;
      creation_job = candidate;
    }
    if (candidate && std::strcmp(class_name(candidate), "AtmosphereNpcAoiAgent") == 0) {
      void* owner = declaring_class(candidate);
      if (agent || !owner || std::strcmp(class_name(owner), "AtmosphereNpcAoiController") != 0) return false;
      agent = candidate;
    }
  }
  auto lod_type = class_from_name(image, "Beyond.NPC.Lod", "NPCCrowdLOD");
  if (!lod_type) return false;
  for (const auto& field : {std::pair{"<nextRenderLODTick>k__BackingField", 0x14},
                            std::pair{"<npcComp>k__BackingField", 0x20}, std::pair{"<playerDistance>k__BackingField", 0x2c},
                            std::pair{"<isLockLOD>k__BackingField", 0x38}, std::pair{"<isLoadedModel>k__BackingField", 0x3d}, std::pair{"<isAtmospheric>k__BackingField", 0x4c},
                            std::pair{"m_tempCloseUnloadMode", 0x4f}}) {
    auto handle = class_get_field_from_name(lod_type, field.first);
    if (!handle || (field_flags(handle) & 0x10) || field_get_offset(handle) != field.second) return false;
  }
  if (!agent || !creation_job) return false;
  auto job_agent = class_get_field_from_name(creation_job, "agent");
  if (!job_agent || (field_flags(job_agent) & 0x10) || field_get_offset(job_agent) != 0x10) return false;
  void* controller = declaring_class(agent);
  if (!controller) return false;
  for (const auto& field : {std::pair{"m_agentsToCreate", 0x148}, std::pair{"m_agentsCreating", 0x188}}) {
    auto handle = class_get_field_from_name(controller, field.first);
    if (!handle || (field_flags(handle) & 0x10) || field_get_offset(handle) != field.second) return false;
  }
  void* (*static_data)(void*) = nullptr;
  void (*class_init)(void*) = nullptr;
  if (!ResolveExport(module, "il2cpp_class_get_static_field_data", &static_data)
      || !ResolveExport(module, "il2cpp_runtime_class_init", &class_init)) return false;
  auto lod_manager_type = class_from_name(image, "Beyond.NPC.Lod", "NPCCrowdLodManager");
  auto atmosphere_type = class_from_name(image, "Beyond.Gameplay.Core", "AtmosphereNpcMgr");
  auto npc_manager_type = class_from_name(image, "Beyond.Gameplay.Core", "NpcManager");
  auto world_type = class_from_name(image, "Beyond.Gameplay.Core", "GameWorld");
  auto settings_type = class_from_name(image, "Beyond.NPC.Lod", "NPCCrowdLODSetting");
  auto aoi_type = class_from_name(image, "Beyond.Gameplay.Core", "AtmosphereNpcAoiSetting");
  if (!lod_manager_type || !atmosphere_type || !npc_manager_type || !world_type || !settings_type || !aoi_type) return false;
  struct BudgetField {
    void* type;
    const char* name;
    size_t offset;
    bool is_static;
  };
  for (const auto& field : {BudgetField{lod_manager_type, "m_lods", 0x20, false}, BudgetField{lod_manager_type, "m_standOnlyNpcLods", 0x28, false},
                            BudgetField{npc_manager_type, "m_proxyMissionNpcCount", 0xb8, false},
                            BudgetField{atmosphere_type, "s_movingNpcCountLimit", 0x1c, true},
                            BudgetField{world_type, "npcManager", 0x68, true},
                            BudgetField{settings_type, "s_maxNPCRenderNum", 0x1c, true},
                            BudgetField{aoi_type, "s_maxAtmosphereCpuNpcCount", 0x24, true}}) {
    auto handle = class_get_field_from_name(field.type, field.name);
    if (!handle || ((field_flags(handle) & 0x10) != 0) != field.is_static || field_get_offset(handle) != field.offset) {
      char message[256];
      std::snprintf(message, sizeof(message), "E_E_FPV: NPC budget field validation failed: %s.%s expected_offset=%zu actual_offset=%td expected_static=%d actual_static=%d.",
                    class_name(field.type), field.name, field.offset, handle ? static_cast<ptrdiff_t>(field_get_offset(handle)) : -1,
                    static_cast<int>(field.is_static), handle ? static_cast<int>((field_flags(handle) & 0x10) != 0) : -1);
      Log(reshade::log::level::warning, message);
      return false;
    }
  }
  class_init(atmosphere_type);
  class_init(world_type);
  class_init(settings_type);
  class_init(aoi_type);
  auto* atmosphere_data = static_cast<uint8_t*>(static_data(atmosphere_type));
  auto* world_data = static_cast<uint8_t*>(static_data(world_type));
  auto* settings_data = static_cast<uint8_t*>(static_data(settings_type));
  auto* aoi_data = static_cast<uint8_t*>(static_data(aoi_type));
  if (!atmosphere_data || !world_data || !settings_data || !aoi_data) return false;
  budget_model_cap = reinterpret_cast<int*>(settings_data + 0x1c);
  budget_crowd_cap = reinterpret_cast<int*>(aoi_data + 0x24);
  budget_moving_reserve = reinterpret_cast<int*>(atmosphere_data + 0x1c);
  budget_npc_manager = reinterpret_cast<void**>(world_data + 0x68);
  auto crowd = class_from_name(image, "Beyond.NPC", "NPCCrowdEntityComponent");
  if (!agent || !crowd) return false;
  struct Field {
    const char* name;
    size_t offset;
  };
  for (auto field : {Field{"nameHash", 0x18}, Field{"runtimeData", 0x28}, Field{"entity", 0x30}, Field{"m_cachedCrowd", 0x48}, Field{"m_cpuCreateJob", 0x40}, Field{"preloadHandle", 0x68}, Field{"isCreature", 0x51}, Field{"currentDistanceSq", 0x58}}) {
    auto handle = class_get_field_from_name(agent, field.name);
    if (!handle || (field_flags(handle) & 0x10) || field_get_offset(handle) != field.offset) return false;
  }
  auto entity_type = class_from_name(image, "Beyond.Gameplay.Core", "Entity");
  if (!entity_type) return false;
  auto crowd_field = class_get_field_from_name(entity_type, "<npcCrowd>k__BackingField");
  if (!crowd_field || (field_flags(crowd_field) & 0x10) || field_get_offset(crowd_field) != 0x218) return false;
  auto lod_field = class_get_field_from_name(crowd, "lod");
  if (!lod_field) return false;
  lod_offset = field_get_offset(lod_field);
  if (lod_offset < 0x100 || lod_offset > 0x400 || lod_offset % 8) return false;
  rebuild_cache = reinterpret_cast<Cache>(base + 0x34203c0);
  can_load = reinterpret_cast<Check>(base + 0x3065f40);
  can_unload = reinterpret_cast<Check>(base + 0x43a3c10);
  blocked = reinterpret_cast<Check>(base + 0x3246c60);
  in_dialog = reinterpret_cast<Check>(base + 0x2cdb3a0);
  return true;
}
}
inline void OnPresent() {
  using namespace detail;
  ++budget_epoch;
  nearest = closest_first >= 0.5f;
  npc_loading::automatic_loading = false;
  static std::array<float, 6> previous{-1, -1, -1, -1, -1, -1};
  const std::array<float, 6> values{enabled, delay, margin, protection, npcs_enabled, refresh_interval};
  if (values != previous) {
    if ((previous[0] >= 0.5f && enabled < 0.5f) || (previous[4] >= 0.5f && npcs_enabled < 0.5f)) recovery_until = GetTickCount64() + 5000;
    active = false;
    npc_active = false;
    requested_refresh = std::isfinite(refresh_interval) ? std::clamp(refresh_interval, 0.05f, 1.f) : 0.1f;
    requested_delay = std::isfinite(delay) ? std::clamp(delay, 0.5f, 10.f) : 2.f;
    requested_margin = std::isfinite(margin) ? std::clamp(margin, 0.f, 100.f) : 25.f;
    requested_protection = std::isfinite(protection) ? std::clamp(protection, 5.f, 100.f) : 15.f;
    ++generation;
    previous = values;
  }
  if (failed) {
    if (!unavailable) Log(reshade::log::level::warning, "E_E_FPV: off-camera entity override stopped after an invalid runtime read; vanilla eligibility restored.");
    unavailable = true;
    active = false;
    npc_active = false;
    return;
  }
  if (enabled < 0.5f && npcs_enabled < 0.5f) {
    active = false;
    npc_active = false;
    return;
  }
  if (!installed) {
    if (!enhancer::detail::ResolveApi()) return;
    bool ok = false;
    __try {
      ok = Resolve();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    if (!ok || !UpdateHooks(true)) {
      failed = true;
      unavailable = true;
      Log(reshade::log::level::warning, "E_E_FPV: off-camera entity unload refused unsupported build, fields or hooks.");
      return;
    }
    installed = true;
  }
  active = enabled >= 0.5f;
  npc_active = npcs_enabled >= 0.5f;
  npc_loading::automatic_loading = active.load();
}
inline void Shutdown() {
  npc_loading::automatic_loading = false;
  detail::active = false;
  detail::npc_active = false;
  if (detail::installed && !detail::UpdateHooks(false))
    enhancer::detail::Log(reshade::log::level::error, "E_E_FPV: off-camera entity hook detach failed.");
  else
    detail::installed = false;
}
}
