#pragma once
// Adapted from ArknightsEnhancer's window_enhancements.cpp.
// Copyright (c) 2026 ItsTheSewerRat. SPDX-License-Identifier: MIT
#include <windows.h>
#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
namespace endfield::window_enhancements {
void uninstall_window_enhancements() noexcept;
}

#include <audioclient.h>
#include <audiopolicy.h>
#include <dwmapi.h>
#include <mmdeviceapi.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <new>
#include <vector>
#include "./cursor_guard.hpp"

namespace endfield::window_enhancements {
constexpr wchar_t k_audio_overlay_class[] =
    L"EndfieldAudioTitleBarControl";
constexpr DWORD k_overlay_fallback_interval_ms = 16;
constexpr DWORD k_audio_refresh_interval_ms = 3000;
constexpr DWORD k_audio_poll_interval_ms = 100;
constexpr int k_overlay_width_dip = 181;
constexpr int k_minimum_overlay_width_dip = 139;
constexpr int k_overlay_height_dip = 24;
constexpr int k_button_width_dip = 31;
constexpr int k_overlay_margin_dip = 5;
constexpr int k_minimum_resize_height = 480;
constexpr UINT_PTR k_background_cursor_timer = 0x45464355;
constexpr UINT_PTR k_window_subclass_id = 0x45465743;
UINT k_install_window_message = 0;
UINT k_remove_window_message = 0;
constexpr int k_audio_supersample_scale = 4;
constexpr COLORREF k_default_light_caption_color = RGB(243, 243, 243);
constexpr COLORREF k_default_dark_caption_color = RGB(32, 32, 32);
constexpr COLORREF k_endfield_cyan = RGB(0, 194, 222);
constexpr COLORREF k_endfield_yellow = RGB(247, 181, 0);
constexpr DWMWINDOWATTRIBUTE k_immersive_dark_mode_attribute =
    static_cast<DWMWINDOWATTRIBUTE>(20);
constexpr DWMWINDOWATTRIBUTE k_caption_color_attribute =
    static_cast<DWMWINDOWATTRIBUTE>(35);
constexpr COLORREF k_dwm_default_color = static_cast<COLORREF>(0xFFFFFFFFu);

struct overlay_thread_context {
  HWND game_window = nullptr;
  HMODULE module = nullptr;
  HANDLE stop_event = nullptr;
  HANDLE present_event = nullptr;
  std::atomic<HWND> overlay_window = nullptr;
};

struct enhancement_lifecycle {
  HWND game_window = nullptr;
  HANDLE thread = nullptr;
  DWORD thread_id = 0;
  overlay_thread_context* context = nullptr;
};

struct native_resize_state {
  HWND window = nullptr;
  bool installed = false;
  LONG_PTR original_style = 0;
  LONG client_width = 0;
  LONG client_height = 0;
  LONG frame_width = 0;
  LONG frame_height = 0;
  bool cursor_override_active = false;
  bool resize_cursor = false;
  bool background_cursor = false;
};

native_resize_state g_native_resize;

enhancement_lifecycle g_enhancements;

[[nodiscard]] int scale_for_dpi(int value, UINT dpi) noexcept {
  return MulDiv(value, static_cast<int>(dpi), 96);
}

[[nodiscard]] bool read_window_style(HWND window, LONG_PTR& style) noexcept {
  SetLastError(ERROR_SUCCESS);
  style = GetWindowLongPtrW(window, GWL_STYLE);
  return style != 0 || GetLastError() == ERROR_SUCCESS;
}

void capture_native_resize_metrics(HWND window) noexcept {
  RECT window_rect = {};
  RECT client_rect = {};
  if (!GetWindowRect(window, &window_rect) || !GetClientRect(window, &client_rect)) {
    return;
  }

  const LONG client_width = client_rect.right - client_rect.left;
  const LONG client_height = client_rect.bottom - client_rect.top;
  const LONG frame_width =
      (window_rect.right - window_rect.left) - client_width;
  const LONG frame_height =
      (window_rect.bottom - window_rect.top) - client_height;
  if (client_width <= 0 || client_height <= 0 || frame_width < 0 || frame_height < 0) {
    return;
  }

  g_native_resize.client_width = client_width;
  g_native_resize.client_height = client_height;
  g_native_resize.frame_width = frame_width;
  g_native_resize.frame_height = frame_height;
}

[[nodiscard]] bool constrain_native_resize(
    WPARAM edge,
    RECT& rect) noexcept {
  if (g_native_resize.client_width <= 0 || g_native_resize.client_height <= 0) {
    capture_native_resize_metrics(g_native_resize.window);
  }
  if (g_native_resize.client_width <= 0 || g_native_resize.client_height <= 0) {
    return false;
  }

  const bool move_left =
      edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT;
  const bool move_right =
      edge == WMSZ_RIGHT || edge == WMSZ_TOPRIGHT || edge == WMSZ_BOTTOMRIGHT;
  const bool move_top =
      edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT;
  const bool move_bottom =
      edge == WMSZ_BOTTOM || edge == WMSZ_BOTTOMLEFT || edge == WMSZ_BOTTOMRIGHT;
  if (!move_left && !move_right && !move_top && !move_bottom)
    return false;

  const LONG proposed_client_width =
      rect.right - rect.left - g_native_resize.frame_width;
  const LONG proposed_client_height =
      rect.bottom - rect.top - g_native_resize.frame_height;
  const bool horizontal_only =
      (move_left || move_right) && !move_top && !move_bottom;
  const bool vertical_only =
      (move_top || move_bottom) && !move_left && !move_right;
  const LONG horizontal_change = std::abs(
      proposed_client_width - g_native_resize.client_width);
  const LONG vertical_change_as_width = std::abs(MulDiv(
      proposed_client_height - g_native_resize.client_height,
      g_native_resize.client_width,
      g_native_resize.client_height));
  const bool width_drives = horizontal_only || (!vertical_only && horizontal_change >= vertical_change_as_width);
  const LONG minimum_client_width = MulDiv(
      k_minimum_resize_height,
      g_native_resize.client_width,
      g_native_resize.client_height);
  LONG client_width = width_drives
                          ? proposed_client_width
                          : MulDiv(
                                proposed_client_height,
                                g_native_resize.client_width,
                                g_native_resize.client_height);
  client_width = std::max(minimum_client_width, client_width);
  const LONG client_height = MulDiv(
      client_width,
      g_native_resize.client_height,
      g_native_resize.client_width);
  const LONG outer_width = client_width + g_native_resize.frame_width;
  const LONG outer_height = client_height + g_native_resize.frame_height;

  if (move_left)
    rect.left = rect.right - outer_width;
  else if (move_right)
    rect.right = rect.left + outer_width;
  else {
    const LONG center_x = (rect.left + rect.right) / 2;
    rect.left = center_x - outer_width / 2;
    rect.right = rect.left + outer_width;
  }
  if (move_top)
    rect.top = rect.bottom - outer_height;
  else if (move_bottom)
    rect.bottom = rect.top + outer_height;
  else {
    const LONG center_y = (rect.top + rect.bottom) / 2;
    rect.top = center_y - outer_height / 2;
    rect.bottom = rect.top + outer_height;
  }
  return true;
}

[[nodiscard]] LRESULT native_resize_hit_test(
    HWND window,
    LPARAM position) noexcept {
  LONG_PTR style = 0;
  RECT rect = {};
  if (!read_window_style(window, style) || (style & WS_THICKFRAME) == 0 || (style & WS_CAPTION) != WS_CAPTION || IsZoomed(window) || !GetWindowRect(window, &rect)) {
    return HTNOWHERE;
  }

  UINT dpi = GetDpiForWindow(window);
  if (dpi == 0)
    dpi = 96;
  const int border_x = std::max(
      1,
      GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi));
  const int border_y = std::max(
      1,
      GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi));
  const POINT cursor = {
      GET_X_LPARAM(position),
      GET_Y_LPARAM(position),
  };
  if (!PtInRect(&rect, cursor)) return HTNOWHERE;
  const bool left = cursor.x < rect.left + border_x;
  const bool right = cursor.x >= rect.right - border_x;
  const bool top = cursor.y < rect.top + border_y;
  const bool bottom = cursor.y >= rect.bottom - border_y;

  if (left && top)
    return HTTOPLEFT;
  if (right && top)
    return HTTOPRIGHT;
  if (left && bottom)
    return HTBOTTOMLEFT;
  if (right && bottom)
    return HTBOTTOMRIGHT;
  if (left)
    return HTLEFT;
  if (right)
    return HTRIGHT;
  if (top)
    return HTTOP;
  if (bottom)
    return HTBOTTOM;
  return HTNOWHERE;
}

void restore_cursor_visibility() noexcept {
  if (!g_native_resize.cursor_override_active) return;
  KillTimer(g_native_resize.window, k_background_cursor_timer);
  cursor_guard::End(cursor_guard::GameFocused());
  g_native_resize.cursor_override_active = false;
  g_native_resize.background_cursor = false;
  g_native_resize.resize_cursor = false;
}

void show_temporary_cursor(HCURSOR cursor) noexcept {
  if (!cursor_guard::enabled.load()) return;
  cursor_guard::Begin(cursor);
  g_native_resize.cursor_override_active = true;
}

void release_background_cursor() noexcept {
  if (g_native_resize.resize_cursor) restore_cursor_visibility();
  if (!g_native_resize.background_cursor) {
    g_native_resize.background_cursor = true;
    ClipCursor(nullptr);
    SetTimer(g_native_resize.window, k_background_cursor_timer, 100, nullptr);
    show_temporary_cursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)));
  }
}
LRESULT CALLBACK native_resize_window_proc(
    HWND window,
    UINT message,
    WPARAM wparam,
    LPARAM lparam,
    UINT_PTR,
    DWORD_PTR) {
  if (message == k_remove_window_message) {
    restore_cursor_visibility();
    RemoveWindowSubclass(window, native_resize_window_proc, k_window_subclass_id);
    cursor_guard::Uninstall();
    const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    const LONG_PTR restored = (style & ~WS_THICKFRAME) | (g_native_resize.original_style & WS_THICKFRAME);
    g_native_resize = {};
    if (style != restored) {
      SetWindowLongPtrW(window, GWL_STYLE, restored);
      SetWindowPos(window, nullptr, 0, 0, 0, 0,
                   SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
    }
    return 0;
  }
  switch (message) {
    case WM_APP + 0x5F2:
      restore_cursor_visibility();
      return 0;
    case WM_TIMER:
      if (wparam == k_background_cursor_timer) {
        if (cursor_guard::GameFocused())
          restore_cursor_visibility();
        else
          release_background_cursor();
        return 0;
      }
      break;
    case WM_ACTIVATEAPP:
    case WM_ACTIVATE:    {
      const bool active = message == WM_ACTIVATEAPP ? wparam != FALSE : LOWORD(wparam) != WA_INACTIVE;
      if (active) restore_cursor_visibility();
      const LRESULT result = DefSubclassProc(window, message, wparam, lparam);
      if (!active) release_background_cursor();
      return result;
    }
    case WM_SETCURSOR: {
      int cursor_resource = 0;
      switch (LOWORD(lparam)) {
        case HTLEFT:
        case HTRIGHT:
          cursor_resource = 32644;
          break;
        case HTTOP:
        case HTBOTTOM:
          cursor_resource = 32645;
          break;
        case HTTOPLEFT:
        case HTBOTTOMRIGHT:
          cursor_resource = 32642;
          break;
        case HTTOPRIGHT:
        case HTBOTTOMLEFT:
          cursor_resource = 32643;
          break;
      }
      if (cursor_resource != 0) {
        if (g_native_resize.background_cursor) restore_cursor_visibility();
        g_native_resize.resize_cursor = true;
        show_temporary_cursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(cursor_resource)));
        TRACKMOUSEEVENT tracking = {sizeof(TRACKMOUSEEVENT), TME_LEAVE | TME_NONCLIENT, window, 0};
        TrackMouseEvent(&tracking);
        return TRUE;
      }
      if (!cursor_guard::GameFocused()) {
        release_background_cursor();
        return TRUE;
      }
      restore_cursor_visibility();
      break;
    }
    case WM_NCMOUSELEAVE:
    case WM_MOUSEMOVE:
      if (g_native_resize.resize_cursor) restore_cursor_visibility();
      break;
    case WM_NCLBUTTONDOWN:
      if (wparam >= HTLEFT && wparam <= HTBOTTOMRIGHT)
        return DefWindowProcW(window, message, wparam, lparam);
      break;
    case WM_NCHITTEST: {
      const LRESULT result =
          DefSubclassProc(window, message, wparam, lparam);
      if (result == HTMINBUTTON || result == HTMAXBUTTON || result == HTCLOSE || result == HTSYSMENU)
        return result;
      const LRESULT resize_result =
          native_resize_hit_test(window, lparam);
      return resize_result == HTNOWHERE ? result : resize_result;
    }

    case WM_STYLECHANGING: {
      const LRESULT result = DefSubclassProc(window, message, wparam, lparam);
      if (wparam == static_cast<WPARAM>(GWL_STYLE) && lparam != 0) {
        auto* styles = reinterpret_cast<STYLESTRUCT*>(lparam);

        if ((styles->styleNew & WS_CAPTION) == WS_CAPTION)
          styles->styleNew = (styles->styleNew & ~WS_POPUP) | WS_THICKFRAME;
      }
      return result;
    }

    case WM_ENTERSIZEMOVE:
      capture_native_resize_metrics(window);
      break;

    case WM_SIZING:
      if (lparam != 0) {
        auto* const proposed = reinterpret_cast<RECT*>(lparam);
        if (constrain_native_resize(wparam, *proposed)) {
          return TRUE;
        }
      }
      break;

    case WM_GETMINMAXINFO:
      if (lparam != 0) {
        const LRESULT result =
            DefSubclassProc(window, message, wparam, lparam);
        capture_native_resize_metrics(window);
        if (g_native_resize.client_width <= 0 || g_native_resize.client_height <= 0) {
          return result;
        }
        auto* const limits = reinterpret_cast<MINMAXINFO*>(lparam);
        const LONG minimum_client_width = MulDiv(
            k_minimum_resize_height,
            g_native_resize.client_width,
            g_native_resize.client_height);
        limits->ptMinTrackSize.x = std::max<LONG>(
            limits->ptMinTrackSize.x,
            minimum_client_width + g_native_resize.frame_width);
        limits->ptMinTrackSize.y = std::max<LONG>(
            limits->ptMinTrackSize.y,
            k_minimum_resize_height + g_native_resize.frame_height);
        return result;
      }
      break;

    case WM_NCDESTROY: {
      restore_cursor_visibility();
      cursor_guard::Uninstall();
      RemoveWindowSubclass(window, native_resize_window_proc, k_window_subclass_id);
      const LRESULT result =
          DefSubclassProc(window, message, wparam, lparam);
      g_native_resize = {};
      return result;
    }

    default:
      break;
  }

  return DefSubclassProc(window, message, wparam, lparam);
}

bool install_resize_on_window_thread(HWND window) noexcept {
  if (g_native_resize.installed) return g_native_resize.window == window;
  LONG_PTR style = 0;
  if (!read_window_style(window, style)) return false;
  g_native_resize.window = window;
  g_native_resize.original_style = style;
  if (!SetWindowSubclass(window, native_resize_window_proc, k_window_subclass_id, 0)) {
    g_native_resize = {};
    return false;
  }
  g_native_resize.installed = true;
  const bool cursor_interception_installed = cursor_guard::Install(window);
  if (cursor_guard::status_log) cursor_guard::status_log(cursor_interception_installed);
  if (!cursor_interception_installed)
    OutputDebugStringW(L"E_E_FPV: Unity cursor interception refused (build/import check or transaction failure).\n");
  if ((style & WS_CAPTION) == WS_CAPTION) {
    const LONG_PTR resizable = (style & ~WS_POPUP) | WS_THICKFRAME;
    if (style != resizable) {
      SetWindowLongPtrW(window, GWL_STYLE, resizable);
      SetWindowPos(window, nullptr, 0, 0, 0, 0,
                   SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
    }
  }
  capture_native_resize_metrics(window);
  return true;
}

LRESULT CALLBACK install_window_hook(int code, WPARAM wparam, LPARAM lparam) {
  if (code >= 0) {
    const auto* message = reinterpret_cast<const CWPSTRUCT*>(lparam);
    if (message->message == k_install_window_message)
      install_resize_on_window_thread(message->hwnd);
  }
  return CallNextHookEx(nullptr, code, wparam, lparam);
}

bool enable_native_resize(HWND window) noexcept {
  if (!k_install_window_message) k_install_window_message = RegisterWindowMessageW(L"EndfieldEnhancer.InstallWindowControls.v2");
  if (!k_remove_window_message) k_remove_window_message = RegisterWindowMessageW(L"EndfieldEnhancer.RemoveWindowControls.v2");
  if (!k_install_window_message || !k_remove_window_message) return false;
  const DWORD thread = GetWindowThreadProcessId(window, nullptr);
  if (!thread) return false;
  if (thread == GetCurrentThreadId()) return install_resize_on_window_thread(window);

  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&install_window_hook), &module)) return false;
  HHOOK hook = SetWindowsHookExW(WH_CALLWNDPROC, install_window_hook, module, thread);
  if (!hook) return false;
  SendMessageW(window, k_install_window_message, 0, 0);
  UnhookWindowsHookEx(hook);
  return g_native_resize.installed && g_native_resize.window == window;
}

void disable_native_resize() noexcept {
  if (IsWindow(g_native_resize.window))
    SendMessageW(g_native_resize.window, k_remove_window_message, 0, 0);
}

template <typename T>
void release_interface(T*& value) noexcept {
  if (value != nullptr) {
    value->Release();
    value = nullptr;
  }
}

class audio_session_controller {
 public:
  ~audio_session_controller() {
    clear();
  }

  audio_session_controller(const audio_session_controller&) = delete;
  audio_session_controller& operator=(const audio_session_controller&) = delete;

  audio_session_controller() = default;

  void clear() noexcept {
    for (ISimpleAudioVolume* volume : _sessions) {
      if (volume != nullptr)
        volume->Release();
    }
    _sessions.clear();
  }

  [[nodiscard]] bool refresh(DWORD process_id) noexcept {
    clear();

    IMMDeviceEnumerator* device_enumerator = nullptr;
    HRESULT result = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_INPROC_SERVER,
        __uuidof(IMMDeviceEnumerator),
        reinterpret_cast<void**>(&device_enumerator));
    if (FAILED(result) || device_enumerator == nullptr)
      return false;

    IMMDeviceCollection* devices = nullptr;
    result = device_enumerator->EnumAudioEndpoints(
        eRender,
        DEVICE_STATE_ACTIVE,
        &devices);
    release_interface(device_enumerator);
    if (FAILED(result) || devices == nullptr)
      return false;

    UINT device_count = 0;
    if (SUCCEEDED(devices->GetCount(&device_count))) {
      for (UINT device_index = 0; device_index < device_count; ++device_index) {
        IMMDevice* device = nullptr;
        if (FAILED(devices->Item(device_index, &device)) || device == nullptr)
          continue;

        IAudioSessionManager2* session_manager = nullptr;
        result = device->Activate(
            __uuidof(IAudioSessionManager2),
            CLSCTX_INPROC_SERVER,
            nullptr,
            reinterpret_cast<void**>(&session_manager));
        release_interface(device);
        if (FAILED(result) || session_manager == nullptr)
          continue;

        IAudioSessionEnumerator* session_enumerator = nullptr;
        result = session_manager->GetSessionEnumerator(&session_enumerator);
        release_interface(session_manager);
        if (FAILED(result) || session_enumerator == nullptr)
          continue;

        int session_count = 0;
        if (SUCCEEDED(session_enumerator->GetCount(&session_count))) {
          for (int session_index = 0;
               session_index < session_count;
               ++session_index) {
            IAudioSessionControl* session = nullptr;
            if (FAILED(session_enumerator->GetSession(
                    session_index,
                    &session))
                || session == nullptr) {
              continue;
            }

            IAudioSessionControl2* session2 = nullptr;
            result = session->QueryInterface(
                __uuidof(IAudioSessionControl2),
                reinterpret_cast<void**>(&session2));
            release_interface(session);
            if (FAILED(result) || session2 == nullptr)
              continue;

            DWORD session_process_id = 0;
            result = session2->GetProcessId(&session_process_id);
            if (SUCCEEDED(result) && session_process_id == process_id) {
              ISimpleAudioVolume* volume = nullptr;
              if (SUCCEEDED(session2->QueryInterface(
                      __uuidof(ISimpleAudioVolume),
                      reinterpret_cast<void**>(&volume)))
                  && volume != nullptr) {
                _sessions.push_back(volume);
              }
            }

            release_interface(session2);
          }
        }

        release_interface(session_enumerator);
      }
    }

    release_interface(devices);
    return !_sessions.empty();
  }

  [[nodiscard]] bool query(float& volume, bool& muted) const noexcept {
    bool found = false;
    bool all_muted = true;
    float highest_volume = 0.0F;

    for (ISimpleAudioVolume* session : _sessions) {
      float session_volume = 0.0F;
      BOOL session_muted = FALSE;
      if (session == nullptr || FAILED(session->GetMasterVolume(&session_volume)) || FAILED(session->GetMute(&session_muted))) {
        continue;
      }

      found = true;
      highest_volume = std::max(highest_volume, session_volume);
      all_muted = all_muted && session_muted != FALSE;
    }

    if (!found)
      return false;

    volume = std::clamp(highest_volume, 0.0F, 1.0F);
    muted = all_muted;
    return true;
  }

  [[nodiscard]] bool set_volume(float volume) noexcept {
    const float clamped = std::clamp(volume, 0.0F, 1.0F);
    bool changed = false;
    for (ISimpleAudioVolume* session : _sessions) {
      if (session != nullptr && SUCCEEDED(session->SetMasterVolume(clamped, nullptr))) {
        changed = true;
      }
    }
    return changed;
  }

  [[nodiscard]] bool set_muted(bool muted) noexcept {
    bool changed = false;
    for (ISimpleAudioVolume* session : _sessions) {
      if (session != nullptr && SUCCEEDED(session->SetMute(muted ? TRUE : FALSE, nullptr))) {
        changed = true;
      }
    }
    return changed;
  }

 private:
  std::vector<ISimpleAudioVolume*> _sessions;
};

struct audio_overlay_state {
  HWND window = nullptr;
  HWND game_window = nullptr;
  audio_session_controller audio;
  UINT dpi = 96;
  float volume = 1.0F;
  bool muted = false;
  bool audio_available = false;
  bool mouse_tracking = false;
  bool button_hovered = false;
  bool slider_hovered = false;
  bool button_pressed = false;
  bool slider_dragging = false;
  bool fullscreen_pressed = false;
  bool fullscreen_hovered = false;
  bool fullscreen = false;
  bool saved_window = false;
  LONG_PTR saved_style = 0;
  WINDOWPLACEMENT saved_placement = {};
  bool visible = false;
  COLORREF caption_color = k_default_light_caption_color;
  RECT last_window_rect = {};
  ULONGLONG last_audio_refresh = 0;
  ULONGLONG last_audio_poll = 0;
};

[[nodiscard]] RECT get_button_rect(const audio_overlay_state& state) noexcept {
  RECT client = {};
  GetClientRect(state.window, &client);
  client.right = std::min<LONG>(
      client.right,
      static_cast<LONG>(scale_for_dpi(k_button_width_dip, state.dpi)));
  return client;
}

[[nodiscard]] RECT get_slider_rect(const audio_overlay_state& state) noexcept {
  RECT client = {};
  GetClientRect(state.window, &client);

  const int button_width = scale_for_dpi(k_button_width_dip, state.dpi);
  const int horizontal_padding = scale_for_dpi(9, state.dpi);
  const int half_track_height = std::max(1, scale_for_dpi(1, state.dpi));
  const int center_y = (client.top + client.bottom) / 2;

  return RECT{
      button_width + horizontal_padding,
      center_y - half_track_height,
      std::max<LONG>(
          static_cast<LONG>(button_width + horizontal_padding + 1),
          client.right - horizontal_padding - button_width),
      center_y + half_track_height + 1,
  };
}

[[nodiscard]] bool point_in_rect(const RECT& rect, POINT point) noexcept {
  return point.x >= rect.left && point.x < rect.right && point.y >= rect.top && point.y < rect.bottom;
}

[[nodiscard]] RECT get_fullscreen_rect(const audio_overlay_state& state) noexcept {
  RECT client = {};
  GetClientRect(state.window, &client);
  client.left = std::max<LONG>(0, client.right - scale_for_dpi(k_button_width_dip, state.dpi));
  return client;
}

void toggle_fullscreen(audio_overlay_state& state) noexcept {
  LONG_PTR style = 0;
  if (!read_window_style(state.game_window, style)) return;
  MONITORINFO monitor = {};
  monitor.cbSize = sizeof(MONITORINFO);
  if (!GetMonitorInfoW(MonitorFromWindow(state.game_window, MONITOR_DEFAULTTONEAREST), &monitor)) return;
  const bool windowed = (style & WS_CAPTION) == WS_CAPTION && (style & WS_POPUP) == 0;
  if (windowed) {
    state.saved_placement.length = sizeof(WINDOWPLACEMENT);
    if (!GetWindowPlacement(state.game_window, &state.saved_placement)) return;
    state.saved_style = style;
    SetLastError(ERROR_SUCCESS);
    if (!SetWindowLongPtrW(state.game_window, GWL_STYLE,
                           (style & ~(WS_OVERLAPPEDWINDOW | WS_MAXIMIZE | WS_MINIMIZE)) | WS_POPUP)
        && GetLastError() != ERROR_SUCCESS) return;
    if (!SetWindowPos(state.game_window, nullptr,
                      monitor.rcMonitor.left, monitor.rcMonitor.top,
                      monitor.rcMonitor.right - monitor.rcMonitor.left,
                      monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                      SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_NOZORDER)) {
      SetWindowLongPtrW(state.game_window, GWL_STYLE, style);
      SetWindowPlacement(state.game_window, &state.saved_placement);
      SetWindowPos(state.game_window, nullptr, 0, 0, 0, 0,
                   SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
      return;
    }
    state.saved_window = true;
  } else {
    SetLastError(ERROR_SUCCESS);
    if (!SetWindowLongPtrW(state.game_window, GWL_STYLE, state.saved_window ? state.saved_style : (style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW)
        && GetLastError() != ERROR_SUCCESS) return;
    if (state.saved_window) {
      SetWindowPlacement(state.game_window, &state.saved_placement);
      if (!(state.saved_style & WS_VISIBLE)) ShowWindow(state.game_window, SW_HIDE);
    } else {
      const int width = (monitor.rcWork.right - monitor.rcWork.left) * 4 / 5;
      const int height = (monitor.rcWork.bottom - monitor.rcWork.top) * 4 / 5;
      SetWindowPos(state.game_window, nullptr,
                   monitor.rcWork.left + width / 8, monitor.rcWork.top + height / 8,
                   width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    SetWindowPos(state.game_window, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  }
}

struct titlebar_palette {
  COLORREF background = 0;
  COLORREF foreground = 0;
  COLORREF disabled_foreground = 0;
  COLORREF hover = 0;
  COLORREF border = 0;
  COLORREF track = 0;
};

[[nodiscard]] COLORREF blend_color(
    COLORREF first,
    COLORREF second,
    unsigned int second_weight) noexcept {
  second_weight = std::min(second_weight, 255u);
  const unsigned int first_weight = 255u - second_weight;
  const auto blend_channel =
      [first_weight, second_weight](BYTE first_channel, BYTE second_channel) {
        return static_cast<BYTE>(
            (static_cast<unsigned int>(first_channel) * first_weight + static_cast<unsigned int>(second_channel) * second_weight + 127u) / 255u);
      };

  return RGB(
      blend_channel(GetRValue(first), GetRValue(second)),
      blend_channel(GetGValue(first), GetGValue(second)),
      blend_channel(GetBValue(first), GetBValue(second)));
}

[[nodiscard]] bool is_dark_color(COLORREF color) noexcept {
  const unsigned int luminance =
      static_cast<unsigned int>(GetRValue(color)) * 299u + static_cast<unsigned int>(GetGValue(color)) * 587u + static_cast<unsigned int>(GetBValue(color)) * 114u;
  return luminance < 150000u;
}

[[nodiscard]] titlebar_palette make_titlebar_palette(
    COLORREF caption_color) noexcept {
  const COLORREF foreground = is_dark_color(caption_color)
                                  ? RGB(238, 238, 238)
                                  : RGB(28, 28, 28);
  return {
      caption_color,
      foreground,
      blend_color(foreground, caption_color, 140u),
      blend_color(caption_color, foreground, 30u),
      blend_color(caption_color, foreground, 72u),
      blend_color(caption_color, foreground, 100u),
  };
}

[[nodiscard]] COLORREF read_caption_color(HWND window) noexcept {
  COLORREF caption_color = k_dwm_default_color;
  if (SUCCEEDED(DwmGetWindowAttribute(
          window,
          k_caption_color_attribute,
          &caption_color,
          sizeof(caption_color)))
      && caption_color != k_dwm_default_color) {
    return caption_color;
  }

  BOOL dark_mode = FALSE;
  if (SUCCEEDED(DwmGetWindowAttribute(
          window,
          k_immersive_dark_mode_attribute,
          &dark_mode,
          sizeof(dark_mode)))
      && dark_mode != FALSE) {
    return k_default_dark_caption_color;
  }

  return k_default_light_caption_color;
}

[[nodiscard]] HPEN create_smooth_pen(
    COLORREF color,
    int width) noexcept {
  const LOGBRUSH brush = {
      BS_SOLID,
      color,
      0,
  };
  HPEN pen = ExtCreatePen(
      PS_GEOMETRIC | PS_SOLID | PS_JOIN_ROUND | PS_ENDCAP_ROUND,
      static_cast<DWORD>(std::max(1, width)),
      &brush,
      0,
      nullptr);
  if (pen == nullptr)
    pen = CreatePen(PS_SOLID, std::max(1, width), color);
  return pen;
}

void fill_capsule(
    HDC device_context,
    const RECT& rect,
    COLORREF color) noexcept {
  const int width = rect.right - rect.left;
  const int height = rect.bottom - rect.top;
  if (width <= 0 || height <= 0)
    return;

  HBRUSH brush = CreateSolidBrush(color);
  const HGDIOBJ previous_brush = SelectObject(device_context, brush);
  const HGDIOBJ previous_pen =
      SelectObject(device_context, GetStockObject(NULL_PEN));
  if (width <= height) {
    Ellipse(
        device_context,
        rect.left,
        rect.top,
        rect.right,
        rect.bottom);
  } else {
    RoundRect(
        device_context,
        rect.left,
        rect.top,
        rect.right,
        rect.bottom,
        height,
        height);
  }
  SelectObject(device_context, previous_pen);
  SelectObject(device_context, previous_brush);
  DeleteObject(brush);
}

void draw_speaker_icon(
    HDC device_context,
    const audio_overlay_state& state,
    const RECT& button,
    const titlebar_palette& palette) noexcept {
  const COLORREF color = !state.audio_available
                             ? palette.disabled_foreground
                             : (state.muted ? k_endfield_yellow : palette.foreground);
  const int center_y = (button.top + button.bottom) / 2;
  const int origin_x = button.left + scale_for_dpi(7, state.dpi);
  const int body_width = std::max(2, scale_for_dpi(3, state.dpi));
  const int body_half_height = std::max(2, scale_for_dpi(2, state.dpi));
  const int cone_width = std::max(3, scale_for_dpi(5, state.dpi));
  const int cone_half_height = std::max(3, scale_for_dpi(5, state.dpi));

  HBRUSH brush = CreateSolidBrush(color);
  const HGDIOBJ previous_brush = SelectObject(device_context, brush);
  const HGDIOBJ previous_pen = SelectObject(device_context, GetStockObject(NULL_PEN));

  const RECT body = {
      origin_x,
      center_y - body_half_height,
      origin_x + body_width,
      center_y + body_half_height + 1,
  };
  FillRect(device_context, &body, brush);

  POINT cone[4] = {
      {origin_x + body_width, center_y - body_half_height},
      {origin_x + body_width + cone_width, center_y - cone_half_height},
      {origin_x + body_width + cone_width, center_y + cone_half_height},
      {origin_x + body_width, center_y + body_half_height},
  };
  Polygon(device_context, cone, static_cast<int>(std::size(cone)));

  SelectObject(device_context, previous_pen);
  SelectObject(device_context, previous_brush);
  DeleteObject(brush);

  const int line_width = std::max(1, scale_for_dpi(1, state.dpi));
  HPEN pen = create_smooth_pen(color, line_width);
  const HGDIOBJ old_pen = SelectObject(device_context, pen);
  const int wave_x = origin_x + body_width + cone_width + scale_for_dpi(2, state.dpi);

  if (state.muted || state.volume <= 0.001F) {
    const int radius = std::max(2, scale_for_dpi(3, state.dpi));
    MoveToEx(device_context, wave_x, center_y - radius, nullptr);
    LineTo(device_context, wave_x + radius * 2, center_y + radius);
    MoveToEx(device_context, wave_x + radius * 2, center_y - radius, nullptr);
    LineTo(device_context, wave_x, center_y + radius);
  } else {
    const int small_wave = std::max(2, scale_for_dpi(3, state.dpi));
    MoveToEx(device_context, wave_x, center_y - small_wave, nullptr);
    LineTo(device_context, wave_x + small_wave, center_y);
    LineTo(device_context, wave_x, center_y + small_wave);

    if (state.volume >= 0.5F) {
      const int large_wave = std::max(3, scale_for_dpi(4, state.dpi));
      const int second_x = wave_x + scale_for_dpi(3, state.dpi);
      MoveToEx(device_context, second_x, center_y - large_wave, nullptr);
      LineTo(device_context, second_x + large_wave, center_y);
      LineTo(device_context, second_x, center_y + large_wave);
    }
  }

  SelectObject(device_context, old_pen);
  DeleteObject(pen);
}

void draw_audio_overlay(
    HDC device_context,
    audio_overlay_state& state) noexcept {
  RECT client = {};
  GetClientRect(state.window, &client);
  const titlebar_palette palette =
      make_titlebar_palette(state.caption_color);

  HBRUSH background = CreateSolidBrush(state.caption_color);
  FillRect(device_context, &client, background);
  DeleteObject(background);

  const int cut = std::max(3, scale_for_dpi(5, state.dpi));
  const int center_y = (client.top + client.bottom) / 2;
  POINT panel_points[] = {
      {client.left + cut, client.top},
      {client.right - cut, client.top},
      {client.right, center_y},
      {client.right - cut, client.bottom - 1},
      {client.left + cut, client.bottom - 1},
      {client.left, center_y},
  };
  HBRUSH panel_brush = CreateSolidBrush(palette.background);
  const int panel_outline_width =
      std::max(1, scale_for_dpi(1, state.dpi));
  HPEN panel_pen =
      create_smooth_pen(palette.border, panel_outline_width);
  const HGDIOBJ old_panel_brush = SelectObject(device_context, panel_brush);
  const HGDIOBJ old_panel_pen = SelectObject(device_context, panel_pen);
  Polygon(device_context, panel_points, static_cast<int>(std::size(panel_points)));
  SelectObject(device_context, old_panel_pen);
  SelectObject(device_context, old_panel_brush);
  DeleteObject(panel_pen);
  DeleteObject(panel_brush);

  const RECT button = get_button_rect(state);
  if (state.button_hovered && state.audio_available) {
    POINT hover_points[] = {
        {button.left + cut, button.top + 1},
        {button.right - 1, button.top + 1},
        {button.right - 1, button.bottom - 1},
        {button.left + cut, button.bottom - 1},
        {button.left + 1, center_y},
    };
    HBRUSH hover_brush = CreateSolidBrush(palette.hover);
    const HGDIOBJ old_hover_brush =
        SelectObject(device_context, hover_brush);
    const HGDIOBJ old_hover_pen =
        SelectObject(device_context, GetStockObject(NULL_PEN));
    Polygon(
        device_context,
        hover_points,
        static_cast<int>(std::size(hover_points)));
    SelectObject(device_context, old_hover_pen);
    SelectObject(device_context, old_hover_brush);
    DeleteObject(hover_brush);
  }

  HPEN divider_pen = create_smooth_pen(
      palette.border,
      std::max(1, scale_for_dpi(1, state.dpi)));
  const HGDIOBJ old_divider_pen = SelectObject(device_context, divider_pen);
  MoveToEx(device_context, button.right + scale_for_dpi(2, state.dpi), button.top + 4, nullptr);
  LineTo(device_context, button.right - scale_for_dpi(2, state.dpi), button.bottom - 4);
  SelectObject(device_context, old_divider_pen);
  DeleteObject(divider_pen);

  draw_speaker_icon(device_context, state, button, palette);

  RECT fullscreen_button = get_fullscreen_rect(state);
  if (state.fullscreen_hovered) {
    InflateRect(&fullscreen_button, -scale_for_dpi(3, state.dpi), -scale_for_dpi(2, state.dpi));
    HBRUSH hover = CreateSolidBrush(palette.hover);
    FillRect(device_context, &fullscreen_button, hover);
    DeleteObject(hover);
    fullscreen_button = get_fullscreen_rect(state);
  }
  InflateRect(&fullscreen_button, -scale_for_dpi(9, state.dpi), -scale_for_dpi(7, state.dpi));
  HPEN fullscreen_pen = create_smooth_pen(palette.foreground, std::max(1, scale_for_dpi(1, state.dpi)));
  const HGDIOBJ old_fullscreen_pen = SelectObject(device_context, fullscreen_pen);
  const HGDIOBJ old_fullscreen_brush = SelectObject(device_context, GetStockObject(NULL_BRUSH));
  Rectangle(device_context, fullscreen_button.left, fullscreen_button.top, fullscreen_button.right, fullscreen_button.bottom);
  if (state.fullscreen) {
    OffsetRect(&fullscreen_button, -scale_for_dpi(3, state.dpi), scale_for_dpi(3, state.dpi));
    Rectangle(device_context, fullscreen_button.left, fullscreen_button.top, fullscreen_button.right, fullscreen_button.bottom);
  }
  SelectObject(device_context, old_fullscreen_pen);
  SelectObject(device_context, old_fullscreen_brush);
  DeleteObject(fullscreen_pen);

  const RECT slider = get_slider_rect(state);
  RECT slider_outline = slider;
  const int slider_outline_width =
      std::max(1, scale_for_dpi(1, state.dpi));
  InflateRect(
      &slider_outline,
      slider_outline_width,
      slider_outline_width);
  fill_capsule(device_context, slider_outline, palette.border);
  fill_capsule(device_context, slider, palette.track);

  if (state.audio_available) {
    RECT filled = slider;
    const int track_width = std::max(
        1,
        static_cast<int>(slider.right - slider.left));
    filled.right = slider.left + static_cast<int>(static_cast<float>(track_width) * state.volume + 0.5F);
    const COLORREF active_color = state.muted
                                      ? k_endfield_yellow
                                      : k_endfield_cyan;
    fill_capsule(device_context, filled, active_color);

    const int knob_radius = std::max(
        scale_for_dpi(state.slider_hovered || state.slider_dragging ? 4 : 3, state.dpi),
        2);
    const int knob_x = std::clamp(filled.right, slider.left, slider.right);
    const int slider_center_y = (slider.top + slider.bottom) / 2;
    POINT knob_points[] = {
        {knob_x, slider_center_y - knob_radius},
        {knob_x + knob_radius, slider_center_y},
        {knob_x, slider_center_y + knob_radius},
        {knob_x - knob_radius, slider_center_y},
    };
    HBRUSH knob_brush = CreateSolidBrush(palette.foreground);
    const HGDIOBJ old_brush = SelectObject(device_context, knob_brush);
    const HGDIOBJ old_pen = SelectObject(device_context, GetStockObject(NULL_PEN));
    Polygon(
        device_context,
        knob_points,
        static_cast<int>(std::size(knob_points)));
    SelectObject(device_context, old_pen);
    SelectObject(device_context, old_brush);
    DeleteObject(knob_brush);
  }
}

void paint_audio_overlay(audio_overlay_state& state) noexcept {
  PAINTSTRUCT paint = {};
  HDC device_context = BeginPaint(state.window, &paint);
  if (device_context == nullptr)
    return;

  RECT client = {};
  GetClientRect(state.window, &client);
  const int client_width = client.right - client.left;
  const int client_height = client.bottom - client.top;
  if (client_width <= 0 || client_height <= 0) {
    EndPaint(state.window, &paint);
    return;
  }

  const int render_width =
      client_width * k_audio_supersample_scale;
  const int render_height =
      client_height * k_audio_supersample_scale;
  HDC render_context = CreateCompatibleDC(device_context);
  HBITMAP render_bitmap = render_context != nullptr
                              ? CreateCompatibleBitmap(
                                    device_context,
                                    render_width,
                                    render_height)
                              : nullptr;
  if (render_context == nullptr || render_bitmap == nullptr) {
    if (render_bitmap != nullptr)
      DeleteObject(render_bitmap);
    if (render_context != nullptr)
      DeleteDC(render_context);
    draw_audio_overlay(device_context, state);
    EndPaint(state.window, &paint);
    return;
  }

  const HGDIOBJ previous_bitmap =
      SelectObject(render_context, render_bitmap);
  SetMapMode(render_context, MM_ANISOTROPIC);
  SetWindowExtEx(
      render_context,
      client_width,
      client_height,
      nullptr);
  SetViewportExtEx(
      render_context,
      render_width,
      render_height,
      nullptr);
  draw_audio_overlay(render_context, state);
  SetMapMode(render_context, MM_TEXT);

  const int previous_stretch_mode =
      SetStretchBltMode(device_context, HALFTONE);
  POINT previous_brush_origin = {};
  SetBrushOrgEx(device_context, 0, 0, &previous_brush_origin);
  StretchBlt(
      device_context,
      client.left,
      client.top,
      client_width,
      client_height,
      render_context,
      0,
      0,
      render_width,
      render_height,
      SRCCOPY);
  SetBrushOrgEx(
      device_context,
      previous_brush_origin.x,
      previous_brush_origin.y,
      nullptr);
  SetStretchBltMode(device_context, previous_stretch_mode);

  SelectObject(render_context, previous_bitmap);
  DeleteObject(render_bitmap);
  DeleteDC(render_context);
  EndPaint(state.window, &paint);
}

[[nodiscard]] bool refresh_audio_sessions(audio_overlay_state& state) noexcept {
  state.last_audio_refresh = GetTickCount64();
  if (!state.audio.refresh(GetCurrentProcessId())) {
    state.audio_available = false;
    InvalidateRect(state.window, nullptr, FALSE);
    return false;
  }

  float volume = state.volume;
  bool muted = state.muted;
  state.audio_available = state.audio.query(volume, muted);
  if (state.audio_available) {
    state.volume = volume;
    state.muted = muted;
  }
  InvalidateRect(state.window, nullptr, FALSE);
  return state.audio_available;
}

void poll_audio_state(audio_overlay_state& state) noexcept {
  const ULONGLONG now = GetTickCount64();
  if (state.last_audio_refresh == 0 || now - state.last_audio_refresh >= k_audio_refresh_interval_ms) {
    static_cast<void>(refresh_audio_sessions(state));
  }

  if (now - state.last_audio_poll < k_audio_poll_interval_ms)
    return;
  state.last_audio_poll = now;

  float volume = state.volume;
  bool muted = state.muted;
  const bool available = state.audio.query(volume, muted);
  if (available != state.audio_available || muted != state.muted || std::fabs(volume - state.volume) >= 0.001F) {
    state.audio_available = available;
    if (available) {
      state.volume = volume;
      state.muted = muted;
    }
    InvalidateRect(state.window, nullptr, FALSE);
  }
}

void set_audio_volume_from_point(audio_overlay_state& state, LONG x) noexcept {
  if (!state.audio_available && !refresh_audio_sessions(state))
    return;

  const RECT slider = get_slider_rect(state);
  const int width = std::max(
      1,
      static_cast<int>(slider.right - slider.left));
  const float volume = std::clamp(
      static_cast<float>(x - slider.left) / static_cast<float>(width),
      0.0F,
      1.0F);
  if (state.audio.set_volume(volume)) {
    state.volume = volume;
    InvalidateRect(state.window, nullptr, FALSE);
  }
}

void toggle_audio_mute(audio_overlay_state& state) noexcept {
  if (!state.audio_available && !refresh_audio_sessions(state))
    return;

  if (state.audio.set_muted(!state.muted)) {
    state.muted = !state.muted;
    InvalidateRect(state.window, nullptr, FALSE);
  }
}

LRESULT CALLBACK audio_overlay_window_proc(
    HWND window,
    UINT message,
    WPARAM wparam,
    LPARAM lparam) {
  auto* state = reinterpret_cast<audio_overlay_state*>(
      GetWindowLongPtrW(window, GWLP_USERDATA));

  if (message == WM_NCCREATE) {
    const auto* const create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
    state = static_cast<audio_overlay_state*>(create->lpCreateParams);
    if (state == nullptr)
      return FALSE;
    state->window = window;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
  }

  if (state == nullptr)
    return DefWindowProcW(window, message, wparam, lparam);

  switch (message) {
    case WM_APP + 1:
      toggle_fullscreen(*state);
      return 0;
    case WM_ERASEBKGND:
      return TRUE;

    case WM_PAINT:
      paint_audio_overlay(*state);
      return 0;

    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;

    case WM_SETCURSOR:
      SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32649)));
      return TRUE;

    case WM_MOUSEMOVE: {
      const POINT point = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      const bool button_hovered = point_in_rect(get_button_rect(*state), point);
      RECT client = {};
      GetClientRect(window, &client);
      const bool fullscreen_hovered = point_in_rect(get_fullscreen_rect(*state), point);
      const bool slider_hovered = !button_hovered && !fullscreen_hovered && point_in_rect(client, point);

      if (!state->mouse_tracking) {
        TRACKMOUSEEVENT tracking = {
            sizeof(tracking),
            TME_LEAVE,
            window,
            0,
        };
        state->mouse_tracking = TrackMouseEvent(&tracking) != FALSE;
      }

      if (button_hovered != state->button_hovered || slider_hovered != state->slider_hovered || fullscreen_hovered != state->fullscreen_hovered) {
        state->button_hovered = button_hovered;
        state->slider_hovered = slider_hovered;
        state->fullscreen_hovered = fullscreen_hovered;
        InvalidateRect(window, nullptr, FALSE);
      }

      if (state->slider_dragging)
        set_audio_volume_from_point(*state, point.x);
      return 0;
    }

    case WM_MOUSELEAVE:
      state->mouse_tracking = false;
      state->button_hovered = false;
      state->slider_hovered = false;
      state->fullscreen_hovered = false;
      InvalidateRect(window, nullptr, FALSE);
      return 0;

    case WM_LBUTTONDOWN: {
      const POINT point = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      SetCapture(window);
      if (point_in_rect(get_fullscreen_rect(*state), point)) {
        state->fullscreen_pressed = true;
      } else if (point_in_rect(get_button_rect(*state), point)) {
        state->button_pressed = true;
      } else {
        state->slider_dragging = true;
        set_audio_volume_from_point(*state, point.x);
      }
      return 0;
    }

    case WM_LBUTTONUP: {
      const POINT point = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      const bool activate_button = state->button_pressed && point_in_rect(get_button_rect(*state), point);
      const bool activate_fullscreen = state->fullscreen_pressed && point_in_rect(get_fullscreen_rect(*state), point);
      state->fullscreen_pressed = false;
      state->button_pressed = false;

      if (state->slider_dragging)
        set_audio_volume_from_point(*state, point.x);
      state->slider_dragging = false;

      if (GetCapture() == window)
        ReleaseCapture();
      if (activate_button)
        toggle_audio_mute(*state);
      if (activate_fullscreen)
        toggle_fullscreen(*state);
      InvalidateRect(window, nullptr, FALSE);
      return 0;
    }

    case WM_MOUSEWHEEL:
      if (state->audio_available || refresh_audio_sessions(*state)) {
        const int wheel_steps = GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA;
        const float new_volume = std::clamp(
            state->volume + static_cast<float>(wheel_steps) * 0.05F,
            0.0F,
            1.0F);
        if (state->audio.set_volume(new_volume)) {
          state->volume = new_volume;
          InvalidateRect(window, nullptr, FALSE);
        }
      }
      return 0;

    case WM_CAPTURECHANGED:
      state->fullscreen_pressed = false;
      state->button_pressed = false;
      state->slider_dragging = false;
      return 0;

    case WM_DPICHANGED:
      state->dpi = GetDpiForWindow(window);
      InvalidateRect(window, nullptr, FALSE);
      return 0;

    case WM_CLOSE:
      DestroyWindow(window);
      return 0;

    case WM_DESTROY:
      state->window = nullptr;
      return 0;

    default:
      return DefWindowProcW(window, message, wparam, lparam);
  }
}

void hide_audio_overlay(audio_overlay_state& state) noexcept {
  if (state.visible) {
    ShowWindow(state.window, SW_HIDE);
    state.visible = false;
  }
}

[[nodiscard]] bool caption_button_bounds(
    HWND game_window,
    const RECT& window_rect,
    RECT& bounds) noexcept {
  RECT raw_bounds = {};
  if (FAILED(DwmGetWindowAttribute(
          game_window,
          DWMWA_CAPTION_BUTTON_BOUNDS,
          &raw_bounds,
          sizeof(raw_bounds)))
      || raw_bounds.right <= raw_bounds.left || raw_bounds.bottom <= raw_bounds.top) {
    return false;
  }

  const LONG window_width = window_rect.right - window_rect.left;
  const LONG window_height = window_rect.bottom - window_rect.top;
  if (raw_bounds.left >= 0 && raw_bounds.top >= 0 && raw_bounds.right <= window_width && raw_bounds.bottom <= window_height) {
    OffsetRect(&raw_bounds, window_rect.left, window_rect.top);
  } else if (raw_bounds.left < window_rect.left || raw_bounds.top < window_rect.top || raw_bounds.right > window_rect.right || raw_bounds.bottom > window_rect.bottom) {
    return false;
  }

  bounds = raw_bounds;
  return true;
}

void update_caption_color(audio_overlay_state& state) noexcept {
  const COLORREF caption_color = read_caption_color(state.game_window);
  if (caption_color != state.caption_color) {
    state.caption_color = caption_color;
    InvalidateRect(state.window, nullptr, FALSE);
  }
}

void update_audio_overlay_position(audio_overlay_state& state) noexcept {
  if (!IsWindow(state.game_window) || !IsWindowVisible(state.game_window) || IsIconic(state.game_window)) {
    hide_audio_overlay(state);
    return;
  }

  BOOL cloaked = FALSE;
  if (SUCCEEDED(DwmGetWindowAttribute(
          state.game_window,
          DWMWA_CLOAKED,
          &cloaked,
          sizeof(cloaked)))
      && cloaked != FALSE) {
    hide_audio_overlay(state);
    return;
  }

  LONG_PTR style = 0;
  if (!read_window_style(state.game_window, style)) {
    hide_audio_overlay(state);
    return;
  }

  RECT window_rect = {};
  if (!GetWindowRect(state.game_window, &window_rect)) {
    hide_audio_overlay(state);
    return;
  }

  state.dpi = GetDpiForWindow(state.game_window);
  if (state.dpi == 0)
    state.dpi = 96;

  const bool fullscreen = (style & WS_CAPTION) != WS_CAPTION || (style & WS_POPUP) != 0;
  if (state.fullscreen != fullscreen) {
    state.fullscreen = fullscreen;
    InvalidateRect(state.window, nullptr, FALSE);
  }
  if (fullscreen) {
    const int width = scale_for_dpi(k_overlay_width_dip, state.dpi);
    const int height = scale_for_dpi(k_overlay_height_dip, state.dpi);
    const RECT target = {window_rect.right - width - scale_for_dpi(5, state.dpi),
                         window_rect.top, window_rect.right - scale_for_dpi(5, state.dpi), window_rect.top + height};
    POINT cursor = {};
    RECT reveal = target;
    InflateRect(&reveal, scale_for_dpi(5, state.dpi), scale_for_dpi(8, state.dpi));
    if (GetForegroundWindow() != state.game_window || (!state.slider_dragging && !state.fullscreen_pressed && (!GetCursorPos(&cursor) || !point_in_rect(reveal, cursor)))) {
      hide_audio_overlay(state);
      return;
    }
    if (!state.visible || !EqualRect(&target, &state.last_window_rect)) {
      SetWindowPos(state.window, HWND_TOP, target.left, target.top, width, height,
                   SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOSENDCHANGING);
      state.last_window_rect = target;
      state.visible = true;
      InvalidateRect(state.window, nullptr, FALSE);
    }
    update_caption_color(state);
    return;
  }

  RECT buttons = {};
  int button_left = 0;
  int overlay_top = 0;
  int overlay_height = 0;
  if (caption_button_bounds(state.game_window, window_rect, buttons)) {
    button_left = buttons.left;
    overlay_top = buttons.top;
    overlay_height = buttons.bottom - buttons.top;
  } else {
    const int frame_x = GetSystemMetricsForDpi(SM_CXSIZEFRAME, state.dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, state.dpi);
    const int frame_y = GetSystemMetricsForDpi(SM_CYSIZEFRAME, state.dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, state.dpi);
    const int caption_button_width =
        GetSystemMetricsForDpi(SM_CXSIZE, state.dpi);
    overlay_height = GetSystemMetricsForDpi(SM_CYSIZE, state.dpi);
    button_left = window_rect.right - frame_x - caption_button_width * 3;
    overlay_top = window_rect.top + frame_y;
  }

  if (overlay_height <= 0) {
    hide_audio_overlay(state);
    return;
  }

  const int compact_height = std::min(
      overlay_height,
      scale_for_dpi(k_overlay_height_dip, state.dpi));
  overlay_top += (overlay_height - compact_height) / 2;
  overlay_height = compact_height;

  const int margin = scale_for_dpi(k_overlay_margin_dip, state.dpi);
  const int desired_width = scale_for_dpi(k_overlay_width_dip, state.dpi);
  const int minimum_width = scale_for_dpi(k_minimum_overlay_width_dip, state.dpi);
  const int reserved_title_width = scale_for_dpi(100, state.dpi);
  const int available_width = button_left - margin - (window_rect.left + reserved_title_width);
  const int overlay_width = std::min(desired_width, available_width);
  if (overlay_width < minimum_width) {
    hide_audio_overlay(state);
    return;
  }

  const RECT target = {
      button_left - margin - overlay_width,
      overlay_top,
      button_left - margin,
      overlay_top + overlay_height,
  };

  update_caption_color(state);
  if (!state.visible || !EqualRect(&target, &state.last_window_rect)) {
    SetWindowPos(
        state.window,
        HWND_TOP,
        target.left,
        target.top,
        target.right - target.left,
        target.bottom - target.top,
        SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOSENDCHANGING);
    state.last_window_rect = target;
    state.visible = true;
    InvalidateRect(state.window, nullptr, FALSE);
  }
}

DWORD run_audio_overlay(void* parameter) noexcept {
  auto* const context = static_cast<overlay_thread_context*>(parameter);
  if (context == nullptr)
    return 0;

  static_cast<void>(enable_native_resize(context->game_window));

  const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const bool uninitialize_com = SUCCEEDED(com_result);
  const bool com_available = uninitialize_com || com_result == RPC_E_CHANGED_MODE;

  WNDCLASSEXW window_class = {};
  window_class.cbSize = sizeof(window_class);
  window_class.style = CS_DBLCLKS;
  window_class.lpfnWndProc = audio_overlay_window_proc;
  window_class.hInstance = context->module;
  window_class.lpszClassName = k_audio_overlay_class;

  bool class_registered = RegisterClassExW(&window_class) != 0;
  if (!class_registered && GetLastError() == ERROR_CLASS_ALREADY_EXISTS)
    class_registered = true;

  audio_overlay_state state;
  state.game_window = context->game_window;
  state.dpi = GetDpiForWindow(context->game_window);
  if (state.dpi == 0)
    state.dpi = 96;

  if (class_registered) {
    state.window = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        k_audio_overlay_class,
        L"Endfield volume",
        WS_POPUP,
        0,
        0,
        1,
        1,
        context->game_window,
        nullptr,
        context->module,
        &state);
  }

  context->overlay_window.store(state.window, std::memory_order_release);

  if (state.window != nullptr && com_available)
    static_cast<void>(refresh_audio_sessions(state));

  bool running = state.window != nullptr;
  while (running) {
    const HANDLE wait_handles[] = {
        context->stop_event,
        context->present_event,
    };
    const DWORD wait_result = MsgWaitForMultipleObjects(
        2,
        wait_handles,
        FALSE,
        k_overlay_fallback_interval_ms,
        QS_ALLINPUT);
    if (wait_result == WAIT_OBJECT_0 || wait_result == WAIT_FAILED)
      break;

    MSG message = {};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      if (message.message == WM_QUIT) {
        running = false;
        break;
      }
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }

    if (!running || !IsWindow(context->game_window))
      break;

    if (state.window != nullptr) update_audio_overlay_position(state);
    if (state.window != nullptr && com_available)
      poll_audio_state(state);
  }

  context->overlay_window.store(nullptr, std::memory_order_release);
  if (state.window != nullptr)
    DestroyWindow(state.window);
  if (class_registered)
    UnregisterClassW(k_audio_overlay_class, context->module);
  disable_native_resize();
  state.audio.clear();
  if (uninitialize_com)
    CoUninitialize();
  return 0;
}

DWORD WINAPI audio_overlay_thread(void* parameter) noexcept {
  const HMODULE module = static_cast<overlay_thread_context*>(parameter)->module;
  const DWORD result = run_audio_overlay(parameter);
  FreeLibraryAndExitThread(module, result);
}
}

namespace endfield::window_enhancements {
void install_window_enhancements(HWND game_window, HMODULE module) noexcept {
  if (game_window == nullptr || module == nullptr)
    return;

  if (IsWindow(g_enhancements.game_window) && g_enhancements.thread != nullptr && WaitForSingleObject(g_enhancements.thread, 0) == WAIT_TIMEOUT) {
    return;
  }

  uninstall_window_enhancements();

  auto* const context = new (std::nothrow) overlay_thread_context;
  if (context == nullptr)
    return;

  context->game_window = game_window;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                          reinterpret_cast<LPCWSTR>(&audio_overlay_thread), &context->module)) {
    delete context;
    return;
  }
  context->stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  context->present_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (context->stop_event == nullptr || context->present_event == nullptr) {
    if (context->stop_event != nullptr)
      CloseHandle(context->stop_event);
    if (context->present_event != nullptr)
      CloseHandle(context->present_event);
    FreeLibrary(context->module);
    delete context;
    return;
  }

  DWORD thread_id = 0;
  HANDLE thread = CreateThread(
      nullptr,
      0,
      audio_overlay_thread,
      context,
      0,
      &thread_id);
  if (thread == nullptr) {
    CloseHandle(context->stop_event);
    CloseHandle(context->present_event);
    FreeLibrary(context->module);
    delete context;
    return;
  }

  g_enhancements.game_window = game_window;
  g_enhancements.thread = thread;
  g_enhancements.thread_id = thread_id;
  g_enhancements.context = context;
}

void notify_window_presented() noexcept {
  overlay_thread_context* const context = g_enhancements.context;
  if (context != nullptr && context->present_event != nullptr)
    SetEvent(context->present_event);
}

void request_fullscreen_toggle() noexcept {
  if (g_enhancements.context != nullptr) {
    if (HWND window = g_enhancements.context->overlay_window.load(std::memory_order_acquire))
      PostMessageW(window, WM_APP + 1, 0, 0);
  }
}

void uninstall_window_enhancements() noexcept {
  overlay_thread_context* const context = g_enhancements.context;
  HANDLE const thread = g_enhancements.thread;
  if (context == nullptr) {
    g_enhancements = {};
    return;
  }

  if (context->stop_event != nullptr)
    SetEvent(context->stop_event);

  if (thread != nullptr && GetCurrentThreadId() != g_enhancements.thread_id) {
    while (MsgWaitForMultipleObjects(1, &thread, FALSE, INFINITE, QS_SENDMESSAGE) == WAIT_OBJECT_0 + 1) {
      MSG message = {};
      PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
    }
  }

  if (thread != nullptr)
    CloseHandle(thread);
  if (context->stop_event != nullptr)
    CloseHandle(context->stop_event);
  if (context->present_event != nullptr)
    CloseHandle(context->present_event);
  delete context;
  g_enhancements = {};
}

void request_window_enhancements_shutdown() noexcept {
  if (g_enhancements.context != nullptr && g_enhancements.context->stop_event != nullptr) {
    SetEvent(g_enhancements.context->stop_event);
  }
}
}
