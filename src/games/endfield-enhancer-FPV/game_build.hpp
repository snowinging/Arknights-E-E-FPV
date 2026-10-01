#pragma once

#include <array>
#include <string>
#include <Windows.h>
#include <wincrypt.h>
#pragma comment(lib, "crypt32.lib")

namespace endfield::game_build {

inline std::string ModuleFileSha256(HMODULE module) {
  if (module == nullptr) return {};
  std::array<wchar_t, 32768> path = {};
  const DWORD path_length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
  if (path_length == 0 || path_length >= path.size()) return {};

  const HANDLE file = CreateFileW(path.data(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return {};
  LARGE_INTEGER size = {};
  const HANDLE mapping = GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart <= MAXDWORD
                             ? CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr)
                             : nullptr;
  const auto* bytes = mapping ? static_cast<const BYTE*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0)) : nullptr;
  std::array<BYTE, 32> hash = {};
  DWORD length = static_cast<DWORD>(hash.size());
  const bool hashed = bytes != nullptr && CryptHashCertificate2(L"SHA256", 0, nullptr, bytes, static_cast<DWORD>(size.QuadPart), hash.data(), &length);
  if (bytes != nullptr) UnmapViewOfFile(bytes);
  if (mapping != nullptr) CloseHandle(mapping);
  CloseHandle(file);
  if (!hashed || length != hash.size()) return {};
  constexpr char hex[] = "0123456789abcdef";
  std::string result;
  result.reserve(hash.size() * 2);
  for (const BYTE byte : hash) {
    result += hex[byte >> 4];
    result += hex[byte & 0xF];
  }
  return result;
}

inline bool IsSupportedGameAssembly(HMODULE module) {
  const auto hash = ModuleFileSha256(module);
  return hash == "593d0b905f793e6bebd25ec3432af3f7ff0f4f0c24399ec49c3ddbb129bdc86c"
         || hash == "c24495e51b406f03b03890c4788ee618ae022c991405be5d5b8b787cb775ae89";
}

inline bool IsSupportedUnityPlayer(HMODULE module) {
  const auto hash = ModuleFileSha256(module);
  return hash == "41d8ba3111c5652c777124ee321d53f14f31ee866a6317eefb4ecf20168370b5"
         || hash == "bee7be52370adddd67ba61e4937ca51b7f272656841d187e95e505496da798d1";
}

}
