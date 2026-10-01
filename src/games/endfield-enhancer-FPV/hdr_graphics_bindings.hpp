#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>
#include <glad/vulkan.h>

namespace endfield::hdr_output {

struct GraphicsBindings {
  struct Batch {
    uint32_t first = 0;
    std::vector<VkDescriptorSet> sets;
    std::vector<uint32_t> offsets;
  };
  VkPipelineLayout layout = VK_NULL_HANDLE;
  std::vector<Batch> batches;
  bool valid = true;

  void Record(VkPipelineLayout next_layout, uint32_t first, uint32_t count,
              const VkDescriptorSet* sets, uint32_t offset_count, const uint32_t* offsets) {
    if (layout != next_layout) {
      batches.clear();
      layout = next_layout;
      valid = true;
    }
    if (!valid || count == 0) return;
    if (layout == VK_NULL_HANDLE || sets == nullptr || first >= 64 || count > 64 - first
        || offset_count > 256 || (offset_count != 0 && offsets == nullptr)) {
      valid = false;
      return;
    }
    for (uint32_t i = 0; i < count; ++i) {
      if (sets[i] == VK_NULL_HANDLE) {
        valid = false;
        return;
      }
    }

    std::erase_if(batches, [first, count](const Batch& batch) {
      return first <= batch.first && first + count >= batch.first + batch.sets.size();
    });
    if (batches.size() >= 32) {
      valid = false;
      return;
    }
    auto& batch = batches.emplace_back();
    batch.first = first;
    batch.sets.assign(sets, sets + count);
    if (offset_count != 0) batch.offsets.assign(offsets, offsets + offset_count);
  }

  bool Matches(uint64_t expected_layout, std::span<const uint64_t> expected_sets) const {
    if (!valid || expected_sets.size() > 64) return false;
    if (expected_layout == 0) return layout == VK_NULL_HANDLE && batches.empty() && expected_sets.empty();
    if (reinterpret_cast<uint64_t>(layout) != expected_layout || batches.empty()) return false;
    uint64_t sets[64] = {};
    for (const auto& batch : batches) {
      if (batch.first + batch.sets.size() > expected_sets.size()) return false;
      for (size_t i = 0; i < batch.sets.size(); ++i) sets[batch.first + i] = reinterpret_cast<uint64_t>(batch.sets[i]);
    }
    return std::equal(expected_sets.begin(), expected_sets.end(), sets);
  }

  void Restore(VkCommandBuffer command, PFN_vkCmdBindDescriptorSets bind) const {
    for (const auto& batch : batches) {
      bind(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, batch.first,
           static_cast<uint32_t>(batch.sets.size()), batch.sets.data(),
           static_cast<uint32_t>(batch.offsets.size()), batch.offsets.empty() ? nullptr : batch.offsets.data());
    }
  }
};

}
