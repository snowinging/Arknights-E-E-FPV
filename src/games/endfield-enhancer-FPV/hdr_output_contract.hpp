#pragma once

#include <glad/vulkan.h>

namespace endfield::hdr_output {

template <typename Barrier>
constexpr bool IsOutputBoundary(const Barrier& barrier) {
  return barrier.oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
         && barrier.newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
         && barrier.srcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED
         && barrier.dstQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED
         && barrier.subresourceRange.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT
         && barrier.subresourceRange.baseMipLevel == 0u
         && (barrier.subresourceRange.levelCount == 1u
             || barrier.subresourceRange.levelCount == VK_REMAINING_MIP_LEVELS)
         && barrier.subresourceRange.baseArrayLayer == 0u
         && (barrier.subresourceRange.layerCount == 1u
             || barrier.subresourceRange.layerCount == VK_REMAINING_ARRAY_LAYERS);
}

constexpr VkImageLayout WorkingLayout(VkImageLayout layout) {
  return layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
             ? VK_IMAGE_LAYOUT_GENERAL
             : layout;
}

}
