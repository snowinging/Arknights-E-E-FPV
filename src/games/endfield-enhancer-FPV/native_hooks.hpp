#pragma once

#include <cstdio>
#include <vector>

#include <Windows.h>
#include <TlHelp32.h>
#include <detours.h>
#include <include/reshade.hpp>

namespace endfield::native_hooks {

inline LONG Begin() {
  const LONG error = DetourTransactionBegin();
  if (error != NO_ERROR && error != ERROR_INVALID_OPERATION) DetourTransactionAbort();
  return error;
}

template <typename Operation>
bool Update(const char* feature, Operation&& operation) {
  struct Thread {
    HANDLE handle;
    DWORD id;
  };
  const char* stage = nullptr;
  LONG error = NO_ERROR;
  DWORD thread_id = 0;
  for (unsigned attempt = 0; attempt < 3; ++attempt) {
    std::vector<Thread> threads;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 entry{sizeof(entry)};
    stage = nullptr;
    error = NO_ERROR;
    thread_id = 0;
    if (snapshot == INVALID_HANDLE_VALUE || !Thread32First(snapshot, &entry)) {
      stage = snapshot == INVALID_HANDLE_VALUE ? "CreateToolhelp32Snapshot" : "Thread32First";
      error = GetLastError();
    } else {
      for (;;) {
        if (entry.th32OwnerProcessID == GetCurrentProcessId() && entry.th32ThreadID != GetCurrentThreadId()) {
          HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT
                                         | THREAD_QUERY_INFORMATION | SYNCHRONIZE,
                                     FALSE, entry.th32ThreadID);
          if (thread) {
            threads.push_back({thread, entry.th32ThreadID});
          } else if (GetLastError() != ERROR_INVALID_PARAMETER) {
            stage = "OpenThread";
            error = GetLastError();
            thread_id = entry.th32ThreadID;
            break;
          }
        }
        if (!Thread32Next(snapshot, &entry)) {
          error = GetLastError();
          if (error != ERROR_NO_MORE_FILES) stage = "Thread32Next";
          break;
        }
      }
    }
    if (snapshot != INVALID_HANDLE_VALUE) CloseHandle(snapshot);
    bool retry = false;
    if (!stage) {
      error = Begin();
      if (error != NO_ERROR) {
        stage = "DetourTransactionBegin";
      } else {
        for (const auto& thread : threads) {
          thread_id = thread.id;
          DWORD state = WaitForSingleObject(thread.handle, 0);
          if (state == WAIT_OBJECT_0) continue;
          if (state != WAIT_TIMEOUT) {
            stage = "WaitForSingleObject";
            error = state == WAIT_FAILED ? GetLastError() : ERROR_INVALID_DATA;
            break;
          }
          error = DetourUpdateThread(thread.handle);
          if (error == NO_ERROR) continue;
          stage = "DetourUpdateThread";
          state = WaitForSingleObject(thread.handle, 0);
          if (state == WAIT_OBJECT_0) {
            retry = true;
          } else if (state != WAIT_TIMEOUT) {
            stage = "WaitForSingleObject after DetourUpdateThread";
            error = state == WAIT_FAILED ? GetLastError() : ERROR_INVALID_DATA;
          }
          break;
        }
        if (!stage) {
          thread_id = 0;
          error = operation();
          if (error != NO_ERROR) stage = "hook operation";
        }
        if (stage) {
          DetourTransactionAbort();
        } else {
          error = DetourTransactionCommit();
          if (error != NO_ERROR) stage = "DetourTransactionCommit";
        }
      }
    }
    for (const auto& thread : threads) CloseHandle(thread.handle);
    if (!stage) return true;
    if (!retry || attempt == 2) break;
  }
  char message[384];
  std::snprintf(message, sizeof(message), "E_E_FPV: %s hook transaction failed at %s (error %ld, thread %lu).",
                feature, stage, error, thread_id);
  reshade::log::message(reshade::log::level::warning, message);
  return false;
}

}
