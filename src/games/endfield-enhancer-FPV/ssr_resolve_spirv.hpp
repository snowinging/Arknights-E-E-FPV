#pragma once

#include <cstdint>
#include <array>
#include <algorithm>
#include <cstring>
#include <span>
#include <vector>

namespace endfield::ssr_resolve {

struct SpirvPatch {
  std::vector<uint32_t> words;
  std::array<uint32_t, 300> ids{};
  bool valid = false;

  explicit SpirvPatch(std::span<const uint8_t> original) {
    if (original.size() < 20 || original.size() % 4 != 0) return;
    words.resize(original.size() / 4);
    std::memcpy(words.data(), original.data(), original.size());
    if (words[0] != 0x07230203u || words[3] == 0 || words[3] > 0x3FFFFFu) return;
    for (size_t i = 5; i < words.size();) {
      const auto count = words[i] >> 16;
      if (!count || count > words.size() - i) return;
      i += count;
    }
    valid = true;
  }

  static uint32_t IdMask(uint32_t opcode) {
    switch (opcode) {
      case 11:
      case 20:
      case 21:
      case 22:
      case 71:
      case 72:
      case 247:
      case 248: return 0x2;
      case 23:
      case 25:  return 0x6;
      case 32:  return 0xA;
      case 43:
      case 46:
      case 59:  return 0x6;
      case 44:  return 0xFFFFFFFEu;
      case 12:  return 0xFFFFFFEEu;
      case 61:
      case 104:
      case 112:
      case 155: return 0xE;
      case 79:  return 0x1E;
      case 81:  return 0xE;
      case 88:  return 0x5E;
      case 99:  return 0xE;
      case 65:
      case 80:
      case 86:
      case 103:
      case 129:
      case 130:
      case 131:
      case 132:
      case 133:
      case 134:
      case 137:
      case 167:
      case 169:
      case 170:
      case 174:
      case 180:
      case 184:
      case 190:
      case 250: return 0xFFFFFFFEu;
      default:  return 0;
    }
  }

  bool Match(std::initializer_list<uint32_t> pattern) {
    if (!valid) return false;
    unsigned matches = 0;
    auto found = ids;
    for (size_t start = 5; start < words.size(); start += words[start] >> 16) {
      auto candidate = ids;
      size_t at = start, p = 0;
      bool match = true;
      while (p < pattern.size() && match) {
        const auto* expected = pattern.begin() + p;
        const auto count = expected[0] >> 16;
        if (!count || p + count > pattern.size() || at + count > words.size() || words[at] != expected[0]) break;
        const auto mask = IdMask(expected[0] & 0xFFFF);
        for (uint32_t j = 1; j < count; ++j) {
          if ((mask >> j) & 1u) {
            if (expected[j] >= candidate.size() || words[at + j] == 0 || words[at + j] >= words[3]) {
              match = false;
              break;
            }
            auto& id = candidate[expected[j]];
            if (id && id != words[at + j]) {
              match = false;
              break;
            }
            id = words[at + j];
          } else if (expected[j] != words[at + j]) {
            match = false;
            break;
          }
        }
        p += count;
        at += count;
      }
      if (match && p == pattern.size()) {
        found = candidate;
        ++matches;
      }
    }
    if (matches != 1) return false;
    ids = found;
    return true;
  }

  bool Resource(uint32_t symbol, uint32_t set, uint32_t binding) {
    uint32_t found = 0;
    for (size_t i = 5; i < words.size(); i += words[i] >> 16) {
      if (words[i] != 0x00040047 || words[i + 2] != 33 || words[i + 3] != binding) continue;
      for (size_t j = 5; j < words.size(); j += words[j] >> 16) {
        if (words[j] != 0x00040047 || words[j + 1] != words[i + 1] || words[j + 2] != 34 || words[j + 3] != set) continue;
        if (found || (ids[symbol] && ids[symbol] != words[i + 1])) return false;
        found = words[i + 1];
      }
    }
    if (!found) return false;
    ids[symbol] = found;
    return true;
  }

  void Allocate(uint32_t first, uint32_t end) {
    for (auto id = first; id < end; ++id)
      if (!ids[id]) ids[id] = words[3]++;
  }

  void Append(std::vector<uint32_t>* output, std::initializer_list<uint32_t> instructions) const {
    for (size_t i = 0; i < instructions.size();) {
      const auto* instruction = instructions.begin() + i;
      const auto count = instruction[0] >> 16;
      const auto mask = IdMask(instruction[0] & 0xFFFF);

      bool existing_type = false;
      if ((instruction[0] & 0xFFFF) == 20 || (instruction[0] & 0xFFFF) == 23) {
        for (size_t j = 5; j < words.size(); j += words[j] >> 16) {
          if (words[j] == instruction[0] && words[j + 1] == ids[instruction[1]]) {
            existing_type = true;
            break;
          }
        }
      }
      if (existing_type) {
        i += count;
        continue;
      }
      output->push_back(instruction[0]);
      for (uint32_t j = 1; j < count; ++j) output->push_back((mask >> j) & 1u ? ids[instruction[j]] : instruction[j]);
      i += count;
    }
  }
};

inline std::vector<uint32_t> PatchFullResolutionResolve(std::span<const uint8_t> original) {
  SpirvPatch patch(original);
  if (!patch.Match({0x0007004F, 26, 60, 59, 59, 0, 1,
                    0x00050086, 31, 61, 49, 32,
                    0x00050084, 31, 62, 61, 32,
                    0x00050082, 31, 63, 49, 62,
                    0x00040070, 26, 64, 63,
                    0x00050085, 26, 65, 64, 54,
                    0x00050081, 26, 66, 60, 65})
      || !patch.Match({0x00070058, 28, 59, 58, 55, 2, 21})
      || !patch.Match({0x00050056, 14, 58, 56, 57})
      || !patch.Match({0x0004003D, 9, 56, 11})
      || !patch.Resource(11, 0, 2) || !patch.Resource(13, 0, 0)
      || !patch.Match({0x0004003D, 12, 80, 13})
      || !patch.Match({0x00090019, 9, 19, 1, 2, 0, 0, 1, 0})
      || !patch.Match({0x00090019, 12, 19, 1, 2, 0, 0, 2, 8})
      || !patch.Match({0x00030016, 19, 32})
      || !patch.Match({0x00040017, 26, 19, 2})
      || !patch.Match({0x00040017, 31, 24, 2})
      || !patch.Match({0x00040015, 24, 32, 0})
      || !patch.Match({0x0005002C, 31, 32, 25, 25})
      || !patch.Match({0x0004002B, 24, 25, 2})
      || !patch.Match({0x00040015, 29, 32, 1})
      || !patch.Match({0x0004002B, 29, 30, 0})) return {};
  patch.Match({0x00020014, 81});
  patch.Match({0x00040017, 82, 81, 2});
  patch.Allocate(81, 91);
  const auto& words = patch.words;

  std::vector<uint32_t> patched(words.begin(), words.begin() + 5);
  patched.insert(patched.end(), {0x00020011, 50});
  bool replaced = false;
  bool declarations = false;
  for (size_t i = 5; i < words.size();) {
    const uint32_t count = words[i] >> 16;
    if (count == 0 || i + count > words.size()) return {};
    if ((words[i] & 0xFFFF) == 54 && !declarations) {
      declarations = true;
      patch.Append(&patched, {0x00020014, 81,
                              0x00040017, 82, 81, 2,
                              0x0003002E, 26, 83});
    }
    if (words[i] == 0x00050081 && words[i + 2] == patch.ids[66]) {
      patch.Append(&patched, {0x00050067, 31, 84, 56, 30,
                              0x0004003D, 12, 85, 13,
                              0x00040068, 31, 86, 85,
                              0x000500AA, 82, 87, 84, 86,
                              0x0004009B, 81, 88, 87,
                              0x00050050, 82, 90, 88, 88,
                              0x000600A9, 26, 89, 90, 83, 65,
                              0x00050081, 26, 66, 60, 89});
      replaced = true;
    } else {
      patched.insert(patched.end(), words.begin() + i, words.begin() + i + count);
    }
    i += count;
  }
  return replaced ? patched : std::vector<uint32_t>{};
}

inline std::vector<uint32_t> PatchImprovedBlur(std::span<const uint8_t> original) {
  SpirvPatch patch(original);
  if (!patch.Match({0x00070058, 37, 103, 102, 87, 2, 25,
                    0x00050051, 23, 104, 103, 1,
                    0x00060041, 69, 105, 11, 44, 41,
                    0x0004003D, 23, 106, 105,
                    0x00050085, 23, 107, 104, 106,
                    0x000500B8, 70, 108, 107, 32})
      || !patch.Match({0x00050041, 68, 84, 11, 40,
                       0x0004003D, 37, 85, 84,
                       0x0007004F, 31, 86, 85, 85, 2, 3,
                       0x00050085, 31, 87, 83, 86})
      || !patch.Resource(5, 1, 0) || !patch.Resource(11, 1, 1)
      || !patch.Match({0x0004003B, 64, 5, 2})
      || !patch.Match({0x00040020, 64, 2, 4})
      || !patch.Match({0x00050048, 4, 0, 35, 0})
      || !patch.Match({0x00040020, 68, 2, 37})
      || !patch.Match({0x00040017, 37, 23, 4})
      || !patch.Match({0x00040017, 31, 23, 2})
      || !patch.Match({0x00030016, 23, 32})
      || !patch.Match({0x00020014, 70})
      || !patch.Match({0x0004002B, 23, 24, 0x3F800000})
      || !patch.Match({0x0004002B, 23, 25, 0})
      || !patch.Match({0x00040015, 38, 32, 1})
      || !patch.Match({0x0004002B, 38, 40, 0})
      || !patch.Match({0x0006000B, 1, 0x4C534C47, 0x6474732E, 0x3035342E, 0})) return {};
  patch.Match({0x00040017, 246, 70, 2});
  patch.Allocate(246, 264);
  const auto& words = patch.words;
  std::vector<uint32_t> patched(words.begin(), words.begin() + 5);
  unsigned replaced = 0;
  bool declarations = false;
  for (size_t i = 5; i < words.size();) {
    const uint32_t count = words[i] >> 16;
    if (count == 0 || i + count > words.size()) return {};
    if ((words[i] & 0xFFFF) == 54 && !declarations) {
      declarations = true;
      patch.Append(&patched, {0x00040017, 246, 70, 2,
                              0x0004002B, 23, 247, 0x40C00000,
                              0x0004002B, 23, 248, 0x3EAAAAAB});
    }
    if (words[i] == 0x00050085 && words[i + 2] == patch.ids[107]) {
      patch.Append(&patched, {0x00050085, 23, 249, 104, 106,
                              0x00050041, 68, 250, 5, 40,
                              0x0004003D, 37, 251, 250,
                              0x0007004F, 31, 252, 251, 251, 0, 1,
                              0x0007004F, 31, 253, 85, 85, 0, 1,
                              0x000500B4, 246, 254, 252, 253,
                              0x0004009B, 70, 255, 254,
                              0x000500B4, 70, 256, 106, 247,
                              0x000500A7, 70, 257, 255, 256,
                              0x00050083, 23, 258, 106, 249,
                              0x0007000C, 23, 259, 1, 80, 258, 25,
                              0x0007000C, 23, 260, 1, 79, 248, 259,
                              0x00050083, 23, 261, 249, 260,
                              0x0007000C, 23, 262, 1, 80, 24, 261,
                              0x0007000C, 23, 263, 1, 79, 249, 262,
                              0x000600A9, 23, 107, 257, 263, 249});
      ++replaced;
    } else {
      patched.insert(patched.end(), words.begin() + i, words.begin() + i + count);
    }
    i += count;
  }
  return replaced == 1 ? patched : std::vector<uint32_t>{};
}

inline std::vector<uint32_t> PatchImprovedBlend(std::span<const uint8_t> original) {
  SpirvPatch patch(original);
  if (!patch.Match({0x00050085, 28, 113, 112, 109,
                    0x00060041, 55, 114, 9, 36, 54,
                    0x0004003D, 28, 115, 114,
                    0x000500BE, 56, 116, 115, 38,
                    0x000300F7, 117, 0,
                    0x000400FA, 116, 118, 119,
                    0x000200F8, 118,
                    0x000500B8, 56, 120, 113, 57})
      || !patch.Match({0x0006000C, 28, 124, 1, 8, 113,
                       0x00050083, 28, 125, 113, 124})
      || !patch.Match({0x0004003D, 43, 106, 6,
                       0x0004003D, 45, 107, 7,
                       0x00050056, 47, 108, 106, 107,
                       0x00070058, 29, 16, 108, 105, 2, 48,
                       0x00050051, 28, 109, 16, 1})
      || !patch.Match({0x00050051, 28, 112, 111, 1})
      || !patch.Resource(6, 0, 1) || !patch.Resource(11, 0, 0)
      || !patch.Match({0x0004003D, 60, 161, 11})
      || !patch.Match({0x00090019, 43, 28, 1, 2, 0, 0, 1, 0})
      || !patch.Match({0x00090019, 60, 28, 1, 2, 0, 0, 2, 8})
      || !patch.Match({0x00030016, 28, 32})
      || !patch.Match({0x00020014, 56})
      || !patch.Match({0x00040015, 30, 32, 0})
      || !patch.Match({0x00040017, 33, 30, 2})
      || !patch.Match({0x00040015, 35, 32, 1})
      || !patch.Match({0x0004002B, 35, 36, 0})
      || !patch.Match({0x0004002B, 28, 48, 0})
      || !patch.Match({0x0004002B, 28, 38, 0x3F000000})
      || !patch.Match({0x0004002B, 28, 57, 0x3E800000})
      || !patch.Match({0x0006000B, 1, 0x4C534C47, 0x6474732E, 0x3035342E, 0})) return {};
  patch.Match({0x00040017, 166, 56, 2});
  patch.Allocate(166, 179);
  const auto& words = patch.words;
  std::vector<uint32_t> patched(words.begin(), words.begin() + 5);
  patched.insert(patched.end(), {0x00020011, 50});
  unsigned replaced = 0;
  bool declarations = false;
  for (size_t i = 5; i < words.size();) {
    const uint32_t count = words[i] >> 16;
    if (count == 0 || i + count > words.size()) return {};
    if ((words[i] & 0xFFFF) == 54 && !declarations) {
      declarations = true;
      patch.Append(&patched, {0x00040017, 166, 56, 2,
                              0x0004002B, 28, 167, 0x40C00000,
                              0x0004002B, 28, 168, 0x3FAAAAAB});
    }
    const auto start = patched.size();
    patched.insert(patched.end(), words.begin() + i, words.begin() + i + count);
    if (words[i] == 0x000200F8 && words[i + 1] == patch.ids[118]) {
      patch.Append(&patched, {0x00050067, 33, 169, 106, 36,
                              0x0004003D, 60, 170, 11,
                              0x00040068, 33, 171, 170,
                              0x000500AA, 166, 172, 169, 171,
                              0x0004009B, 56, 173, 172,
                              0x000500B4, 56, 174, 112, 167,
                              0x000500A7, 56, 175, 173, 174,
                              0x00050083, 28, 176, 113, 168,
                              0x0007000C, 28, 177, 1, 40, 176, 48,
                              0x000600A9, 28, 178, 175, 177, 113});
      ++replaced;
    } else if ((words[i] == 0x000500B8 && words[i + 2] == patch.ids[120])
               || (words[i] == 0x00050083 && words[i + 2] == patch.ids[125])) {
      patched[start + 3] = patch.ids[178];
      ++replaced;
    } else if (words[i] == 0x0006000C && words[i + 2] == patch.ids[124]) {
      patched.back() = patch.ids[178];
      ++replaced;
    }
    i += count;
  }
  return replaced == 4 ? patched : std::vector<uint32_t>{};
}

}
