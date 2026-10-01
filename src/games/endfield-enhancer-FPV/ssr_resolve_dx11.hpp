#pragma once

#include <array>
#include <cstring>
#include <span>
#include <vector>
#include <embed/0x18BD6E91.h>
#include <embed/0xDA42CB07.h>
#include <embed/0x4ED659BE.h>

namespace endfield::ssr_resolve {

inline std::vector<uint32_t> PrepareDx11Shader(size_t index, std::span<const uint8_t> source) {
  if (index >= 3 || source.size() < 32 || source.size() % 4 != 0) return {};
  std::vector<uint32_t> words(source.size() / 4);
  std::memcpy(words.data(), source.data(), source.size());
  if (words[0] != 0x43425844 || words[6] != source.size()
      || words[7] > words.size() - 8) return {};
  bool compute = false, improved_setting = false;
  for (size_t chunk = 0; chunk < words[7]; ++chunk) {
    const size_t offset = words[8 + chunk];
    if (offset % 4 != 0 || offset > source.size() - 8) return {};
    const size_t begin = offset / 4;
    const size_t bytes = words[begin + 1];
    if (bytes % 4 != 0 || bytes > source.size() - offset - 8) return {};
    if (words[begin] != 0x52444853 && words[begin] != 0x58454853) continue;
    if (compute || bytes < 8) return {};
    const auto code = std::span(words).subspan(begin + 2, bytes / 4);
    if (code[0] != 0x50050 || code[1] != code.size()) return {};
    compute = true;
    for (size_t i = 2; i < code.size();) {
      const uint32_t opcode = code[i] & 0x7FF;
      const size_t count = opcode == 53 && i + 1 < code.size()
                               ? code[i + 1]
                               : (code[i] >> 24) & 0x7F;
      if (count == 0 || count > code.size() - i) return {};

      if (opcode == 29 && count == 8 && code[i + 1] == 0x00100012
          && code[i + 3] == 0x0020803A && code[i + 4] == 13
          && code[i + 5] == 13 && code[i + 6] == 0x00004001
          && code[i + 7] == 0x3F000000) improved_setting = true;
      i += count;
    }
  }
  if (!compute || (index == 2 && !improved_setting)) return {};
  const auto payload = std::array{__0x18BD6E91, __0xDA42CB07, __0x4ED659BE}[index];
  std::vector<uint32_t> result(payload.size() / 4);
  std::memcpy(result.data(), payload.data(), payload.size());
  return result;
}

inline constexpr std::array<uint32_t, 3> kVulkanHashes = {0x562EDD85, 0xC465A053, 0x4187AEA7};
inline constexpr std::array<uint32_t, 3> kDx11Hashes = {0x18BD6E91, 0xDA42CB07, 0x4ED659BE};

}
