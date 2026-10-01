#pragma once

#include "./native_hooks.hpp"
#include "./npc_distance.hpp"
#include "./ui_visibility_script.hpp"

namespace endfield::ui_visibility {
inline float hide_all = 0.f, hide_uid = 0.f, hide_bar = 0.f, hide_ping = 0.f;
inline float hide_quest = 0.f, hide_map = 0.f, hide_map_buttons = 0.f, hide_menu_buttons = 0.f, hide_utility = 0.f;
inline std::atomic_bool unavailable = false;

inline std::atomic_int status = 0;
namespace detail {
using namespace enhancer::detail;
using Tick = void (*)(void*, float, MethodInfo*);
inline Tick tick = nullptr;
inline void* entry = nullptr;
inline std::array<uint8_t, 32> patched{};
inline Il2CppMethod do_string = nullptr, get_env = nullptr;
inline Il2CppMethod get_key = nullptr, encrypt = nullptr, decrypt = nullptr;
inline void* (*string_new)(const char*) = nullptr;
inline int (*string_length)(void*) = nullptr;
inline const wchar_t* (*string_chars)(void*) = nullptr;
inline void (*format_exception)(void*, char*, int) = nullptr;
inline uint32_t (*gc_new)(void*, bool) = nullptr;
inline void (*gc_free)(uint32_t) = nullptr;
inline uintptr_t (*array_length)(void*) = nullptr;
inline std::atomic_uint32_t requested = 0;
inline std::atomic_bool stopping = false;
inline bool installed = false;
inline uint32_t last_mask = 0;
inline uint32_t last_attempt = 0;
inline ULONGLONG next_update = 0;

struct Root {
  void* object;
  uint32_t handle;
  explicit Root(void* value) : object(value), handle(value ? gc_new(value, true) : 0) {
    if (!handle) object = nullptr;
  }
  Root(const Root&) = delete;
  ~Root() {
    if (handle) gc_free(handle);
  }
};

inline bool Fail(const char* message) {
  unavailable.store(true);
  status.store(4);
  Log(reshade::log::level::error, message);
  return false;
}

inline void* Call(Il2CppMethod method, void* object = nullptr, void** args = nullptr) {
  if (unavailable.load()) return nullptr;
  void* exception = nullptr;
  void* result = runtime_invoke(method, object, args, &exception);
  if (exception) {
    char message[2048] = "E_E_FPV: UI visibility managed call failed: ";
    format_exception(exception, message + std::strlen(message), static_cast<int>(sizeof(message) - std::strlen(message)));
    message[sizeof(message) - 1] = '\0';
    Fail(message);
    return nullptr;
  }
  return result;
}

inline bool Execute(void* manager, const char* source) {
  Root code(string_new(source));
  Root key(Call(get_key));
  if (!code.object || !key.object) return Fail("E_E_FPV: UI visibility could not prepare its Lua input.");
  void* args[] = {code.object, key.object};

  Root encoded(Call(encrypt, nullptr, args));
  if (!encoded.object) return Fail("E_E_FPV: UI visibility Lua encoding failed.");
  args[0] = encoded.object;
  Root decoded(Call(decrypt, nullptr, args));
  const size_t length = std::strlen(source);
  if (!decoded.object || string_length(decoded.object) != static_cast<int>(length))
    return Fail("E_E_FPV: UI visibility Lua loader round-trip failed.");
  const wchar_t* chars = string_chars(decoded.object);
  for (size_t i = 0; i < length; ++i) {
    if (chars[i] != static_cast<unsigned char>(source[i]))
      return Fail("E_E_FPV: UI visibility Lua loader round-trip mismatch.");
  }
  Root result(Call(do_string, manager, args));
  if (unavailable.load()) return false;

  if (!result.object || array_length(result.object) != 1)
    return Fail("E_E_FPV: UI visibility Lua returned no acknowledgement; refusing silent retries.");
  void* value = *reinterpret_cast<void**>(static_cast<uint8_t*>(result.object) + 0x20);
  return value && *static_cast<bool*>(object_unbox(value));
}

inline void HookedTick(void* manager, float delta, MethodInfo* method) {
  tick(manager, delta, method);
  if (stopping.load() || unavailable.load()) return;
  const uint32_t mask = requested.load(std::memory_order_acquire);
  if (mask == 0 && last_mask == 0) return;
  const ULONGLONG now = GetTickCount64();
  if (mask == last_attempt && now < next_update) return;
  last_attempt = mask;
  next_update = now + 250;
  void* exception = nullptr;
  if (!runtime_invoke(get_env, manager, nullptr, &exception) || exception) {
    status.store(2);
    return;
  }
  char source[192];
  std::snprintf(source, sizeof(source),
                "local s=rawget(_G,'__RenoDXUIVisibility'); if not s then return false end; return s.set(%u)", mask);
  if (!Execute(manager, source)) {
    if (unavailable.load()) return;
    if (mask == 0) {
      last_mask = 0;
      status.store(0);
      return;
    }
    if (!Execute(manager, kScript) || !Execute(manager, source)) {
      if (!unavailable.load()) status.store(2);
      return;
    }
  }
  last_mask = mask;
  status.store(mask ? 3 : 0);
}

inline bool UpdateHook(bool attach) {
  return native_hooks::Update(attach ? "UI visibility install" : "UI visibility removal", [attach]() -> LONG {
    return attach ? DetourAttach(&tick, HookedTick) : DetourDetach(&tick, HookedTick);
  });
}

inline bool Resolve() {
  if (!npc_distance::detail::SupportedBuild()) return false;
  HMODULE module = GetModuleHandleW(L"GameAssembly.dll");
  const auto image = FindImage("Lua.Beyond.dll");
  if (!image || !ResolveExport(module, "il2cpp_string_new", &string_new)
      || !ResolveExport(module, "il2cpp_string_length", &string_length)
      || !ResolveExport(module, "il2cpp_string_chars", &string_chars)
      || !ResolveExport(module, "il2cpp_format_exception", &format_exception)
      || !ResolveExport(module, "il2cpp_gchandle_new", &gc_new)
      || !ResolveExport(module, "il2cpp_gchandle_free", &gc_free)
      || !ResolveExport(module, "il2cpp_array_length", &array_length)) return false;
  auto method = FindMethod(image, "Beyond.Lua", "LuaManager", "Tick", 1);
  do_string = FindMethod(image, "Beyond.Lua", "LuaManager", "DoString", 1);
  get_env = FindMethod(image, "Beyond.Lua", "LuaManager", "get_luaEnv", 0);
  const auto xlua = FindImage("XLua.Runtime.dll");
  if (!method || !do_string || !get_env || !xlua) return false;
  get_key = FindMethod(xlua, "Beyond.Lua", "LuaCypher", "GetKey", 0);
  decrypt = FindMethod(xlua, "Beyond.Lua", "LuaCypher", "DecryptLuaString", 1);
  void* (*class_methods)(void*, void**) = nullptr;
  const char* (*method_name)(void*) = nullptr;
  uint32_t (*param_count)(void*) = nullptr;
  const void* (*param_type)(void*, uint32_t) = nullptr;
  int (*type_kind)(const void*) = nullptr;
  if (!get_key || !decrypt || !ResolveExport(module, "il2cpp_class_get_methods", &class_methods)
      || !ResolveExport(module, "il2cpp_method_get_name", &method_name)
      || !ResolveExport(module, "il2cpp_method_get_param_count", &param_count)
      || !ResolveExport(module, "il2cpp_method_get_param", &param_type)
      || !ResolveExport(module, "il2cpp_type_get_type", &type_kind)) return false;
  const auto crypto = class_from_name(xlua, "Security", "XXTEA");
  if (!crypto) return false;
  void* iterator = nullptr;
  while (void* candidate = class_methods(crypto, &iterator)) {
    if (std::strcmp(method_name(candidate), "EncryptToBase64String") || param_count(candidate) != 2) continue;

    if (type_kind(param_type(candidate, 0)) == 14 && type_kind(param_type(candidate, 1)) == 14) {
      if (encrypt) return false;
      encrypt = candidate;
    }
  }
  if (!encrypt) return false;
  const auto* base = reinterpret_cast<const uint8_t*>(module);
  entry = reinterpret_cast<MethodInfo*>(method)->method_pointer;
  constexpr std::array<uint8_t, 32> expected = {
      0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x50, 0x0f, 0x29, 0x74, 0x24, 0x40, 0x0f,
      0x28, 0xf1, 0x48, 0x8b, 0xd9, 0x48, 0x8b, 0x0d, 0xac, 0xf2, 0xe1, 0x09, 0x83, 0xb9, 0xe0, 0x00};
  if (entry != base + 0x31b59a0 || std::memcmp(entry, expected.data(), expected.size())
      || reinterpret_cast<MethodInfo*>(do_string)->method_pointer != base + 0x9165fdc) return false;
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64
      || (nt->OptionalHeader.SizeOfImage != 0xf7cb000 && nt->OptionalHeader.SizeOfImage != 0xf7cc000)) return false;
  const auto* sections = IMAGE_FIRST_SECTION(nt);
  size_t matches = 0;
  for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
    if (!(sections[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
    const auto* begin = base + sections[i].VirtualAddress;
    const auto* end = begin + sections[i].Misc.VirtualSize;
    while ((begin = std::search(begin, end, expected.begin(), expected.end())) != end) {
      ++matches;
      ++begin;
    }
  }
  if (matches != 1) return false;
  tick = reinterpret_cast<Tick>(entry);
  return true;
}
}

inline void OnPresent() {
  using namespace detail;
  requested.store((hide_all >= .5f ? 1u : 0u) | (hide_uid >= .5f ? 2u : 0u)
                      | (hide_bar >= .5f ? 4u : 0u) | (hide_ping >= .5f ? 8u : 0u)
                      | (hide_quest >= .5f ? 16u : 0u) | (hide_map >= .5f ? 32u : 0u)
                      | (hide_map_buttons >= .5f ? 64u : 0u) | (hide_menu_buttons >= .5f ? 128u : 0u)
                      | (hide_utility >= .5f ? 256u : 0u),
                  std::memory_order_release);
  if (installed || unavailable.load() || stopping.load() || requested.load() == 0) return;
  if (!ResolveApi() || !FindImage("Lua.Beyond.dll")) return;
  const bool resolved = Resolve();
  if (!resolved || !UpdateHook(true)) {
    unavailable.store(true);
    status.store(4);
    Log(reshade::log::level::warning, resolved
                                          ? "E_E_FPV: UI visibility hook installation failed; visibility unchanged."
                                          : "E_E_FPV: UI visibility hook refused for this game build or conflicting patch.");
    return;
  }
  std::memcpy(patched.data(), entry, patched.size());
  installed = true;
  status.store(1);
}

inline void Shutdown() {
  using namespace detail;
  stopping.store(true);
  requested.store(0);

  if (!installed) return;
  if (std::memcmp(entry, patched.data(), patched.size()) || !UpdateHook(false)) {
    Log(reshade::log::level::error, "E_E_FPV: UI visibility hook detach failed or entry was modified.");
    return;
  }
  installed = false;
}
}
