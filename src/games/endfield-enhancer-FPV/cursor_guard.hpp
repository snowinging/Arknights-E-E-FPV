#pragma once

#include <windows.h>
#include <atomic>
#include <cstring>
#include <iterator>
#include "./game_build.hpp"

namespace endfield::cursor_guard {
inline decltype(&SetCursor) set_cursor = &SetCursor;
inline decltype(&ShowCursor) show_cursor = &ShowCursor;
inline decltype(&ClipCursor) clip_cursor = &ClipCursor;
inline decltype(&SetCursorPos) set_cursor_pos = &SetCursorPos;
inline std::atomic<HWND> window = nullptr;
inline std::atomic<bool> enabled = false;
inline std::atomic<bool> overriding = false;
inline std::atomic<HCURSOR> requested_cursor = nullptr;
inline void (*status_log)(bool) = nullptr;

inline thread_local bool virtual_count_active = false;
inline thread_local int requested_count = 0;
inline thread_local int initial_count = 0;
inline thread_local int visibility_adjustment = 0;

inline bool GameFocused() {
  DWORD process = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &process);
  return process != 0 && process == GetCurrentProcessId();
}

inline bool ShouldOverride(bool focused, bool on_frame, bool resize_override) {
  return !focused || (on_frame && resize_override);
}

inline void End(bool restore_shape = true);

inline bool OwnsCursor() {
  HWND target = window.load(std::memory_order_acquire);
  if (!enabled.load(std::memory_order_acquire) || !target || !IsWindow(target)) return false;
  if (!GameFocused()) return true;

  POINT cursor = {}, client_origin = {};
  RECT client = {}, outer = {};
  if (!GetCursorPos(&cursor) || !GetClientRect(target, &client)
      || !ClientToScreen(target, &client_origin) || !GetWindowRect(target, &outer)) return false;
  OffsetRect(&client, client_origin.x, client_origin.y);
  return ShouldOverride(true, PtInRect(&outer, cursor) && !PtInRect(&client, cursor),
                        overriding.load(std::memory_order_acquire));
}

inline HCURSOR WINAPI SetCursorHook(HCURSOR cursor) {
  if (OwnsCursor()) return requested_cursor.exchange(cursor, std::memory_order_acq_rel);
  End(false);
  return set_cursor(cursor);
}

inline int WINAPI ShowCursorHook(BOOL show) {
  if (!OwnsCursor()) End();
  if (enabled.load(std::memory_order_acquire) && virtual_count_active)
    return requested_count += show ? 1 : -1;
  return show_cursor(show);
}

inline BOOL WINAPI ClipCursorHook(const RECT* rect) {
  if (rect && OwnsCursor()) return TRUE;
  if (!OwnsCursor()) End();
  return clip_cursor(rect);
}

inline BOOL WINAPI SetCursorPosHook(int x, int y) {
  if (OwnsCursor()) return TRUE;
  End();
  return set_cursor_pos(x, y);
}

inline void Begin(HCURSOR cursor) {
  if (!virtual_count_active) {
    requested_cursor.store(GetCursor(), std::memory_order_release);
    requested_count = show_cursor(TRUE) - 1;
    show_cursor(FALSE);
    initial_count = requested_count;
    visibility_adjustment = 0;
    virtual_count_active = true;
    for (int actual = initial_count; actual < 0;) {
      actual = show_cursor(TRUE);
      ++visibility_adjustment;
    }
  }
  overriding.store(true, std::memory_order_release);
  set_cursor(cursor);
}

inline void End(bool restore_shape) {
  if (!virtual_count_active) return;
  overriding.store(false, std::memory_order_release);

  int adjustment = requested_count - initial_count - visibility_adjustment;
  while (adjustment > 0) {
    show_cursor(TRUE);
    --adjustment;
  }
  while (adjustment < 0) {
    show_cursor(FALSE);
    ++adjustment;
  }
  visibility_adjustment = 0;
  virtual_count_active = false;
  if (restore_shape) set_cursor(requested_cursor.load(std::memory_order_acquire));
}

struct Slot {
  const char* name;
  void* replacement;
  void** address = nullptr;
  void* original = nullptr;
};
inline Slot slots[] = {
    {"SetCursor", reinterpret_cast<void*>(&SetCursorHook)},
    {"ShowCursor", reinterpret_cast<void*>(&ShowCursorHook)},
    {"ClipCursor", reinterpret_cast<void*>(&ClipCursorHook)},
    {"SetCursorPos", reinterpret_cast<void*>(&SetCursorPosHook)}};

inline bool Replace(const Slot& slot, bool install) {
  void* expected = install ? slot.original : slot.replacement;
  void* desired = install ? slot.replacement : slot.original;
  DWORD protection = 0;
  if (!VirtualProtect(slot.address, sizeof(void*), PAGE_READWRITE, &protection)) return false;
  const bool changed = InterlockedCompareExchangePointer(slot.address, desired, expected) == expected;
  DWORD unused = 0;
  if (!VirtualProtect(slot.address, sizeof(void*), protection, &unused)) {
    if (changed) InterlockedCompareExchangePointer(slot.address, expected, desired);
    VirtualProtect(slot.address, sizeof(void*), protection, &unused);
    return false;
  }
  return changed;
}

inline bool MatchesBuild(HMODULE module, DWORD timestamp, DWORD image_size) {
  if (!module) return false;
  const auto* base = reinterpret_cast<const BYTE*>(module);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < static_cast<LONG>(sizeof(IMAGE_DOS_HEADER)) || dos->e_lfanew > 4096) return false;
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
  return nt->Signature == IMAGE_NT_SIGNATURE && nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64
         && nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC
         && nt->FileHeader.TimeDateStamp == timestamp && nt->OptionalHeader.SizeOfImage == image_size;
}

inline bool Install(HWND target) {
  if (enabled.load()) {
    window.store(target);
    return true;
  }
  HMODULE unity = GetModuleHandleW(L"UnityPlayer.dll");
  if ((!MatchesBuild(unity, 0x6A85914F, 0x208B000) && !MatchesBuild(unity, 0x6A85914F, 0x208A000))
      || !endfield::game_build::IsSupportedUnityPlayer(unity)) return false;
  auto* base = reinterpret_cast<BYTE*>(unity);
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
  const auto imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!imports.VirtualAddress || imports.VirtualAddress + imports.Size > nt->OptionalHeader.SizeOfImage) return false;
  for (auto& slot : slots) {
    slot.address = nullptr;
    slot.original = nullptr;
  }
  for (DWORD offset = 0; offset + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= imports.Size; offset += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress + offset);
    if (!descriptor->Name) break;
    if (descriptor->Name >= nt->OptionalHeader.SizeOfImage || !descriptor->OriginalFirstThunk) return false;
    if (_stricmp(reinterpret_cast<const char*>(base + descriptor->Name), "USER32.dll") != 0) continue;
    for (DWORD index = 0;; ++index) {
      const size_t names_rva = descriptor->OriginalFirstThunk + index * sizeof(IMAGE_THUNK_DATA64);
      const size_t iat_rva = descriptor->FirstThunk + index * sizeof(IMAGE_THUNK_DATA64);
      if (names_rva + 8 > nt->OptionalHeader.SizeOfImage || iat_rva + 8 > nt->OptionalHeader.SizeOfImage) return false;
      const auto name = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + names_rva)->u1.AddressOfData;
      if (!name) break;
      if (IMAGE_SNAP_BY_ORDINAL64(name)) continue;
      if (name + sizeof(IMAGE_IMPORT_BY_NAME) >= nt->OptionalHeader.SizeOfImage) return false;
      for (auto& slot : slots) {
        if (std::strcmp(reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + name)->Name, slot.name)) continue;
        if (slot.address) return false;
        slot.address = reinterpret_cast<void**>(base + iat_rva);
        slot.original = *slot.address;
        if (slot.original != reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), slot.name))) return false;
      }
    }
  }
  for (const auto& slot : slots)
    if (!slot.address || !slot.original) return false;
  set_cursor = reinterpret_cast<decltype(set_cursor)>(slots[0].original);
  show_cursor = reinterpret_cast<decltype(show_cursor)>(slots[1].original);
  clip_cursor = reinterpret_cast<decltype(clip_cursor)>(slots[2].original);
  set_cursor_pos = reinterpret_cast<decltype(set_cursor_pos)>(slots[3].original);

  HMODULE self = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                          reinterpret_cast<LPCWSTR>(&SetCursorHook), &self)) return false;
  size_t applied = 0;
  for (; applied < std::size(slots); ++applied) {
    if (!Replace(slots[applied], true)) {
      while (applied) Replace(slots[--applied], false);
      return false;
    }
  }
  window.store(target);
  enabled.store(true, std::memory_order_release);
  return true;
}

inline void Uninstall() {
  enabled.store(false, std::memory_order_release);
  overriding.store(false);
  window.store(nullptr);
  for (size_t i = std::size(slots); i > 0; --i) {
    const auto& slot = slots[i - 1];
    if (slot.address && *slot.address == slot.replacement) Replace(slot, false);
  }
}
}
