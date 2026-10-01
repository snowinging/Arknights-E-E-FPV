inline void (*native_awake)(void*) = nullptr;
inline void* clone_awake_return = nullptr;
struct CloneAwakeScope {
  void* source = nullptr;
  void* deferred = nullptr;
  bool unexpected = false;
  inline static thread_local CloneAwakeScope* current = nullptr;
  explicit CloneAwakeScope(void* native_source) : source(native_source) {
    if (!source || current) throw std::runtime_error("Invalid or nested mesh clone");
    current = this;
  }
  ~CloneAwakeScope() { current = nullptr; }
  CloneAwakeScope(const CloneAwakeScope&) = delete;
  CloneAwakeScope& operator=(const CloneAwakeScope&) = delete;
};
inline void HookedMeshAwake(void* mesh) {
  auto* scope = CloneAwakeScope::current;
  if (scope && _ReturnAddress() == clone_awake_return && mesh != scope->source) {
    if (scope->deferred && scope->deferred != mesh) scope->unexpected = true;
    scope->deferred = mesh;
    return;
  }
  native_awake(mesh);
}
inline bool ResolveCloneAwake(ResolveICall icall) {
  HMODULE module = GetModuleHandleW(L"UnityPlayer.dll");
  if (!endfield::game_build::IsSupportedUnityPlayer(module)) return false;
  const auto* base = reinterpret_cast<const uint8_t*>(module);
  const auto* wrapper = static_cast<const uint8_t*>(icall("UnityEngine.Mesh::UploadMeshDataImpl"));
  constexpr uint8_t wrapper_call[] = {0x40, 0x84, 0xff, 0x48, 0x8b, 0xc8, 0x0f, 0x95, 0xc2, 0xe8, 0x53, 0x00, 0x00, 0x00};
  constexpr uint8_t awake_call[] = {0x38, 0x43, 0x79, 0x48, 0x8b, 0xcb, 0x0f, 0x94, 0xc2, 0xe8, 0xb3, 0x64, 0x0c, 0x00};
  constexpr std::array<uint8_t, 32> signature{0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0x48, 0x81, 0xc1, 0x98, 0x01, 0x00, 0x00,
                                              0xe8, 0x3b, 0xf3, 0xf9, 0xff, 0x48, 0x85, 0xc0, 0x75, 0x15, 0x38, 0x43, 0x7c, 0x75, 0x22, 0x38};
  if (wrapper != base + 0x19bcc0 || std::memcmp(wrapper + 0x5f, wrapper_call, sizeof(wrapper_call))
      || std::memcmp(base + 0xd58bf, awake_call, sizeof(awake_call))) return false;
  if (wrapper + 0x6d + Read<int32_t>(wrapper, 0x69) != base + 0x19bd80) return false;
  const auto* target = base + 0xd58a0;
  constexpr uint8_t dispatcher_call[]{0x41, 0xff, 0xd0};
  if (std::memcmp(base + 0x165f04, dispatcher_call, sizeof(dispatcher_call))
      || std::memcmp(target, signature.data(), signature.size())) return false;
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
  unsigned matches = 0;
  const auto* sections = IMAGE_FIRST_SECTION(nt);
  for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
    if (!(sections[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
    const auto* end = base + sections[i].VirtualAddress + sections[i].Misc.VirtualSize;
    for (const auto* at = base + sections[i].VirtualAddress; (at = std::search(at, end, signature.begin(), signature.end())) != end; ++at) ++matches;
  }
  if (matches != 1) return false;
  const auto core = FindImage("UnityEngine.CoreModule.dll");
  void* field = class_get_field_from_name(class_from_name(core, "UnityEngine", "Object"), "m_CachedPtr");
  if (!field || field_get_offset(field) != 0x10) return false;
  native_awake = reinterpret_cast<decltype(native_awake)>(const_cast<uint8_t*>(target));
  clone_awake_return = const_cast<uint8_t*>(base + 0x165f07);
  return true;
}
