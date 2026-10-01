#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <Windows.h>

namespace endfield::enhancer {
struct RenderQualityOverrides {
  struct Write {
    void* address = nullptr;
    uint64_t original = 0;
    uint64_t replacement = 0;
    size_t size = 0;
  };

  std::array<Write, 18> writes = {};
  size_t count = 0;

  template <typename T>
  void Set(void* settings, size_t offset, T value) {
    static_assert(sizeof(T) <= sizeof(uint64_t));
    if (settings == nullptr || count == writes.size()) return;
    auto* address = static_cast<uint8_t*>(settings) + offset;
    if (std::memcmp(address, &value, sizeof(value)) == 0) return;
    auto& write = writes[count];
    write.address = address;
    write.size = sizeof(value);
    std::memcpy(&write.original, address, sizeof(value));
    std::memcpy(&write.replacement, &value, sizeof(value));
    ++count;
    std::memcpy(address, &value, sizeof(value));
  }

  bool Restore() {
    bool restored = true;
    while (count != 0) {
      auto& write = writes[--count];

      __try {
        if (std::memcmp(write.address, &write.replacement, write.size) == 0) {
          std::memcpy(write.address, &write.original, write.size);
        }
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        restored = false;
      }
    }
    return restored;
  }
};
}
