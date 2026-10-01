#pragma once

#include <atomic>
#include <cstdint>

#include <Windows.h>

#include "./render_quality.hpp"
#include "./ssr_depth_guard.hpp"

namespace endfield::enhancer::ssr_depth {

struct DepthSource {
  void* graph = nullptr;
  uint64_t epoch = 0;
  uint64_t pyramid = 0;
  uint64_t source = 0;
  int32_t width = 0;
  int32_t height = 0;
};
struct Context {
  void* graph;
  int32_t width;
  int32_t height;
};
using BuildPyramid = void* (*)(void*, void*, int32_t, void*);
using RegisterRay = void (*)(void*, int32_t, void*);
using AddRead = void (*)(void*, int32_t, void*, const int32_t*, const uint64_t*);
inline BuildPyramid build_pyramid = nullptr;
inline RegisterRay register_full = nullptr;
inline RegisterRay register_low = nullptr;
inline AddRead add_read = nullptr;
inline std::atomic_uint64_t epoch = 0;
inline std::atomic_bool failed = false;
inline thread_local DepthSource depth = {};
inline thread_local const Context* active = nullptr;

inline void* HookedBuildPyramid(void* output, void* graph, int32_t pass, void* input) {
  depth = {};
  DepthSource candidate = {};
  __try {
    if (input != nullptr && output != nullptr && !failed.load(std::memory_order_relaxed)) {
      auto* data = static_cast<const uint8_t*>(input);
      candidate = {graph, epoch.load(std::memory_order_relaxed), 0,
                   *reinterpret_cast<const uint64_t*>(data + 8),
                   *reinterpret_cast<const int32_t*>(data),
                   *reinterpret_cast<const int32_t*>(data + 4)};
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    failed.store(true, std::memory_order_relaxed);
  }

  void* result = build_pyramid(output, graph, pass, input);
  __try {
    if (candidate.source != 0 && candidate.width > 0 && candidate.width <= 16384
        && candidate.height > 0 && candidate.height <= 16384
        && candidate.epoch == epoch.load(std::memory_order_relaxed)) {
      candidate.pyramid = *static_cast<const uint64_t*>(output);
      if (static_cast<uint32_t>(candidate.pyramid) != 0
          && static_cast<uint32_t>(candidate.source) != 0) depth = candidate;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    failed.store(true, std::memory_order_relaxed);
  }
  return result;
}

inline void RegisterWithFullDepth(RegisterRay original, void* graph, int32_t pass, void* input) {
  RenderQualityOverrides overrides;
  uint64_t source = 0;
  __try {
    if (active != nullptr && active->graph == graph && depth.graph == graph
        && active->width == depth.width && active->height == depth.height
        && depth.epoch == epoch.load(std::memory_order_relaxed)
        && depth.source != 0 && depth.pyramid != 0 && input != nullptr
        && add_read != nullptr && !failed.load(std::memory_order_relaxed)

        && static_cast<const uint8_t*>(input)[0x90] == 1
        && static_cast<const uint8_t*>(input)[0xB8] == 0
        && *reinterpret_cast<const uint64_t*>(static_cast<const uint8_t*>(input) + 0x50)
               == depth.pyramid) {
      source = depth.source;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    failed.store(true, std::memory_order_relaxed);
  }
  __try {
    if (source != 0) {
      const int32_t mip = -1;
      add_read(graph, pass, nullptr, &mip, &source);
      overrides.Set<uint64_t>(input, 0x50, source);
    }
    original(graph, pass, input);
  } __finally {
    if (!overrides.Restore()) failed.store(true, std::memory_order_relaxed);
  }
}

inline void HookedRegisterFull(void* graph, int32_t pass, void* input) {
  RegisterWithFullDepth(register_full, graph, pass, input);
}
inline void HookedRegisterLow(void* graph, int32_t pass, void* input) {
  RegisterWithFullDepth(register_low, graph, pass, input);
}

}
