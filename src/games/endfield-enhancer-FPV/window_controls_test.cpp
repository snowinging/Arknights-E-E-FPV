
#define NOMINMAX
#include "./window_enhancements.hpp"
#include <cstdio>
namespace cursor_guard = endfield::cursor_guard;

WNDPROC foreign_previous = nullptr;
int foreign_depth = 0;
int maximum_foreign_depth = 0;
UINT resize_messages[16]{};
LPARAM resize_dimensions[16]{};
int resize_message_count = 0;
bool record_resize = false;
LRESULT CALLBACK TestWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  if (record_resize && (message == WM_SIZE || message == WM_ENTERSIZEMOVE || message == WM_EXITSIZEMOVE)) {
    if (resize_message_count < 16) {
      resize_messages[resize_message_count] = message;
      resize_dimensions[resize_message_count] = lparam;
    }
    ++resize_message_count;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}
LRESULT CALLBACK ForeignWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  maximum_foreign_depth = std::max(maximum_foreign_depth, ++foreign_depth);
  const LRESULT result = foreign_depth < 8 ? CallWindowProcW(foreign_previous, window, message, wparam, lparam) : -1234;
  --foreign_depth;
  return result;
}

int main() {
  using namespace endfield::window_enhancements;
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  WNDCLASSW wc{};
  wc.lpfnWndProc = TestWindowProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"EndfieldWindowControlTest";
  if (!RegisterClassW(&wc)) return 1;
  HWND window = CreateWindowExW(0, wc.lpszClassName, L"Window control test",
                                WS_OVERLAPPEDWINDOW, 100, 100, 800, 600, nullptr, nullptr, wc.hInstance, nullptr);
  if (!window) return 2;
  audio_overlay_state state;
  state.game_window = window;
  state.window = window;
  RECT original{};
  GetWindowRect(window, &original);
  const LONG_PTR original_style = GetWindowLongPtrW(window, GWL_STYLE);
  int failures = 0;
  for (int i = 0; i < 3; ++i) {
    toggle_fullscreen(state);
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(MONITORINFO);
    GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
    RECT actual{};
    GetWindowRect(window, &actual);
    if (!EqualRect(&actual, &monitor.rcMonitor) || (GetWindowLongPtrW(window, GWL_STYLE) & WS_CAPTION)) ++failures;
    toggle_fullscreen(state);
    GetWindowRect(window, &actual);
    if (!EqualRect(&actual, &original) || GetWindowLongPtrW(window, GWL_STYLE) != original_style) {
      ++failures;
      std::printf("restore: %ld,%ld,%ld,%ld expected %ld,%ld,%ld,%ld style %llx expected %llx\n", actual.left, actual.top, actual.right, actual.bottom, original.left, original.top, original.right, original.bottom, GetWindowLongPtrW(window, GWL_STYLE), original_style);
    }
  }
  if (!enable_native_resize(window)) ++failures;
  record_resize = true;
  SendMessageW(window, WM_ENTERSIZEMOVE, 0, 0);
  const LONG aspect_width = g_native_resize.client_width;
  const LONG aspect_height = g_native_resize.client_height;
  for (WPARAM edge = WMSZ_LEFT; edge <= WMSZ_BOTTOMRIGHT; ++edge) {
    RECT proposed = original;
    proposed.right += 200;
    proposed.bottom += 60;
    if (!SendMessageW(window, WM_SIZING, edge, reinterpret_cast<LPARAM>(&proposed))) ++failures;
    const LONG width = proposed.right - proposed.left - g_native_resize.frame_width;
    const LONG height = proposed.bottom - proposed.top - g_native_resize.frame_height;
    if (height < k_minimum_resize_height || std::abs(height - MulDiv(width, aspect_height, aspect_width)) > 1) ++failures;
  }
  SendMessageW(window, WM_SIZE, SIZE_RESTORED, MAKELPARAM(900, 650));
  if (resize_message_count != 2 || resize_messages[1] != WM_SIZE
      || resize_dimensions[1] != MAKELPARAM(900, 650)) ++failures;
  SendMessageW(window, WM_SIZE, SIZE_RESTORED, MAKELPARAM(1100, 750));
  SendMessageW(window, WM_EXITSIZEMOVE, 0, 0);
  record_resize = false;
  if (resize_message_count != 4 || resize_messages[0] != WM_ENTERSIZEMOVE
      || resize_messages[2] != WM_SIZE || resize_dimensions[2] != MAKELPARAM(1100, 750)
      || resize_messages[3] != WM_EXITSIZEMOVE) ++failures;
  RECT bounds{};
  GetWindowRect(window, &bounds);
  if (native_resize_hit_test(window, MAKELPARAM(bounds.left + 1, bounds.top + 1)) != HTTOPLEFT
      || native_resize_hit_test(window, MAKELPARAM(bounds.right - 1, bounds.bottom - 1)) != HTBOTTOMRIGHT) ++failures;
  cursor_guard::enabled.store(true);
  cursor_guard::window.store(window);

  SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)));
  SendMessageW(window, WM_ACTIVATEAPP, TRUE, 0);
  int hides = 0;
  int hidden_count;
  do {
    hidden_count = ShowCursor(FALSE);
    ++hides;
  } while (hidden_count >= 0);
  ClipCursor(&original);
  SendMessageW(window, WM_ACTIVATEAPP, FALSE, 0);
  const int visible_count = ShowCursor(TRUE) - 1;
  ShowCursor(FALSE);
  RECT clip{};
  GetClipCursor(&clip);
  if (visible_count < 0 || EqualRect(&clip, &original)) ++failures;
  const int adjustments = cursor_guard::requested_count;
  SendMessageW(window, WM_ACTIVATEAPP, FALSE, 0);
  if (adjustments != cursor_guard::requested_count) ++failures;
  cursor_guard::ShowCursorHook(FALSE);
  SendMessageW(window, WM_TIMER, k_background_cursor_timer, 0);
  const int maintained_count = ShowCursor(TRUE) - 1;
  ShowCursor(FALSE);
  if (maintained_count < 0) ++failures;
  SendMessageW(window, WM_ACTIVATEAPP, TRUE, 0);
  const int restored_count = ShowCursor(TRUE) - 1;
  ShowCursor(FALSE);
  if (restored_count != hidden_count - 1 || cursor_guard::virtual_count_active) ++failures;
  ShowCursor(TRUE);

  for (int shape : {32644, 32645, 32642, 32643}) {
    SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32514)));
    const HCURSOR game_cursor = GetCursor();
    g_native_resize.resize_cursor = true;
    show_temporary_cursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(shape)));
    const int edge_count = ShowCursor(TRUE) - 1;
    ShowCursor(FALSE);
    if (edge_count < 0 || GetCursor() != LoadCursorW(nullptr, MAKEINTRESOURCEW(shape))) ++failures;
    for (int update = 0; update < 1000; ++update) {
      cursor_guard::SetCursorHook(game_cursor);
      cursor_guard::ShowCursorHook(FALSE);
      cursor_guard::ShowCursorHook(TRUE);
      if (GetCursor() != LoadCursorW(nullptr, MAKEINTRESOURCEW(shape))) ++failures;
    }
    POINT before{}, after{};
    GetCursorPos(&before);
    cursor_guard::SetCursorPosHook(before.x + 100, before.y + 100);
    GetCursorPos(&after);
    if (before.x != after.x || before.y != after.y) ++failures;
    cursor_guard::ShowCursorHook(TRUE);
    cursor_guard::End();
    restore_cursor_visibility();
    const int menu_count = ShowCursor(TRUE) - 1;
    ShowCursor(FALSE);
    if (menu_count != hidden_count + 1 || GetCursor() != game_cursor) ++failures;
    ShowCursor(FALSE);
  }
  while (hides-- > 0) ShowCursor(TRUE);

  for (int native_delta : {-1, 1}) {
    const int baseline = ShowCursor(TRUE) - 1;
    ShowCursor(FALSE);
    cursor_guard::Begin(LoadCursorW(nullptr, MAKEINTRESOURCEW(32644)));
    ShowCursor(native_delta > 0);
    cursor_guard::ShowCursorHook(TRUE);
    cursor_guard::End();
    const int actual = ShowCursor(TRUE) - 1;
    ShowCursor(FALSE);
    if (actual != baseline + native_delta + 1) {
      ++failures;
      std::printf("native cursor delta lost: %d expected %d\n", actual, baseline + native_delta + 1);
    }
    ShowCursor(native_delta < 0);
    ShowCursor(FALSE);

    int balanced = ShowCursor(TRUE);
    while (balanced > baseline) balanced = ShowCursor(FALSE);
    while (balanced < baseline) balanced = ShowCursor(TRUE);
  }

  if (cursor_guard::ShouldOverride(true, false, true)
      || cursor_guard::ShouldOverride(true, true, false)
      || !cursor_guard::ShouldOverride(true, true, true)
      || !cursor_guard::ShouldOverride(false, false, false)) ++failures;
  show_temporary_cursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32644)));
  cursor_guard::window.store(nullptr);
  const int expected_show = cursor_guard::requested_count + 1;
  if (cursor_guard::ShowCursorHook(TRUE) != expected_show || cursor_guard::virtual_count_active) ++failures;
  cursor_guard::ShowCursorHook(FALSE);
  const HCURSOR menu_cursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32514));
  cursor_guard::SetCursorHook(menu_cursor);
  if (GetCursor() != menu_cursor) ++failures;
  static bool clip_forwarded = false;
  static bool position_forwarded = false;
  const auto original_clip = cursor_guard::clip_cursor;
  const auto original_position = cursor_guard::set_cursor_pos;
  cursor_guard::clip_cursor = [](const RECT* rect) -> BOOL { clip_forwarded = rect != nullptr; return TRUE; };
  cursor_guard::set_cursor_pos = [](int x, int y) -> BOOL { position_forwarded = x == 123 && y == 456; return TRUE; };
  cursor_guard::ClipCursorHook(&original);
  cursor_guard::SetCursorPosHook(123, 456);
  if (!clip_forwarded || !position_forwarded) ++failures;
  cursor_guard::clip_cursor = original_clip;
  cursor_guard::set_cursor_pos = original_position;
  cursor_guard::window.store(window);
  restore_cursor_visibility();

  auto* slot_memory = static_cast<void**>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  if (!slot_memory) return 6;
  *slot_memory = reinterpret_cast<void*>(0x1000);
  cursor_guard::Slot slot{"test", reinterpret_cast<void*>(0x2000), slot_memory, *slot_memory};
  DWORD old_protection = 0;
  VirtualProtect(slot_memory, 4096, PAGE_READONLY, &old_protection);
  MEMORY_BASIC_INFORMATION memory{};
  if (!cursor_guard::Replace(slot, true) || *slot_memory != slot.replacement) ++failures;
  VirtualQuery(slot_memory, &memory, sizeof(memory));
  if (memory.Protect != PAGE_READONLY || cursor_guard::Replace(slot, true)) ++failures;
  if (!cursor_guard::Replace(slot, false) || *slot_memory != slot.original) ++failures;
  if (cursor_guard::MatchesBuild(reinterpret_cast<HMODULE>(slot_memory), 1, 4096)) ++failures;
  for (size_t failure = 0; failure <= 4; ++failure) {
    VirtualProtect(slot_memory, 4096, PAGE_READWRITE, &old_protection);
    cursor_guard::Slot transaction[4];
    for (size_t i = 0; i < 4; ++i) {
      slot_memory[i] = reinterpret_cast<void*>(0x1000);
      transaction[i] = {"test", reinterpret_cast<void*>(0x2000), &slot_memory[i],
                        reinterpret_cast<void*>(i == failure ? 0x3000 : 0x1000)};
    }
    VirtualProtect(slot_memory, 4096, PAGE_READONLY, &old_protection);
    size_t applied = 0;
    while (applied < 4 && cursor_guard::Replace(transaction[applied], true)) ++applied;
    if (applied != failure) ++failures;
    while (applied)
      if (!cursor_guard::Replace(transaction[--applied], false)) ++failures;
    for (void* value : {slot_memory[0], slot_memory[1], slot_memory[2], slot_memory[3]})
      if (value != reinterpret_cast<void*>(0x1000)) ++failures;
  }
  VirtualFree(slot_memory, 0, MEM_RELEASE);
  disable_native_resize();
  if (reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window, GWLP_WNDPROC)) != TestWindowProc) ++failures;

  HANDLE installer = CreateThread(nullptr, 0, [](void* value) -> DWORD { return enable_native_resize(static_cast<HWND>(value)) ? 0 : 1; }, window, 0, nullptr);
  if (!installer) return 4;
  while (MsgWaitForMultipleObjects(1, &installer, FALSE, 3000, QS_ALLINPUT) == WAIT_OBJECT_0 + 1) {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }
  DWORD installed = 1;
  GetExitCodeThread(installer, &installed);
  CloseHandle(installer);
  if (installed != 0) return 5;
  foreign_previous = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&ForeignWindowProc)));

  for (int i = 0; i < 5; ++i) {
    if (!enable_native_resize(window)) ++failures;
    SetWindowLongPtrW(window, GWL_STYLE, WS_POPUP | WS_CAPTION | WS_SYSMENU);
    const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    if (!(style & WS_THICKFRAME) || (style & WS_POPUP)) ++failures;
    SendMessageW(window, WM_NCHITTEST, 0, MAKELPARAM(bounds.left + 1, bounds.top + 1));
    if (reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window, GWLP_WNDPROC)) != ForeignWindowProc) ++failures;
  }
  if (maximum_foreign_depth > 1) ++failures;
  disable_native_resize();
  DWORD_PTR ref_data = 0;
  if (GetWindowSubclass(window, native_resize_window_proc, k_window_subclass_id, &ref_data)) ++failures;
  SendMessageW(window, WM_NULL, 0, 0);
  if (maximum_foreign_depth > 2) ++failures;
  SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&TestWindowProc));

  state.saved_window = false;
  SetWindowLongPtrW(window, GWL_STYLE, WS_POPUP);
  toggle_fullscreen(state);
  if ((GetWindowLongPtrW(window, GWL_STYLE) & WS_CAPTION) != WS_CAPTION) ++failures;

  for (UINT dpi : {96u, 144u, 192u}) {
    state.dpi = dpi;
    SetWindowLongPtrW(window, GWL_STYLE, WS_POPUP);
    SetWindowPos(window, nullptr, 0, 0,
                 scale_for_dpi(k_minimum_overlay_width_dip, dpi), scale_for_dpi(k_overlay_height_dip, dpi),
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    const RECT mute = get_button_rect(state);
    const RECT slider = get_slider_rect(state);
    const RECT fullscreen = get_fullscreen_rect(state);
    if (mute.right >= slider.left || slider.right >= fullscreen.left || slider.right <= slider.left) ++failures;
  }
  DestroyWindow(window);
  UnregisterClassW(wc.lpszClassName, wc.hInstance);
  std::printf("Window controls: %d failures\n", failures);
  return failures ? 3 : 0;
}
