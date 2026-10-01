#pragma once

// AEEF.ini —— 本项目自己的配置存储
//
// 为什么不直接用 ReShade.ini:
//   那份文件位于游戏目录, Windows 下可能因为目录权限或反作弊的写入保护而写不进去,
//   而 ReShade 把【所有】配置都压在那一个文件里, 一写不进去就全丢 —— 插件设置、
//   窗口布局、遥控器标定, 一个不剩。这里自己维护一份, 落在当前用户肯定可写的位置,
//   与 ReShade 各存各的, 互不干扰。
//
// 路径策略: 先试加载器所在目录(便于查看和迁移), 不可写就退到 %APPDATA%\E_E_FPV。
// 实际生效的路径会打进日志。

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>

#include <Windows.h>
#include <include/reshade.hpp>

namespace endfield::config_store {

inline std::unordered_map<std::string, std::string> entries;  // "section/key" -> value
inline std::filesystem::path path;
inline bool loaded = false;
inline bool writable = false;
inline bool dirty = false;   // 内存里有未落盘的改动

inline std::string Trim(const std::string& text) {
  const auto begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return {};
  const auto end = text.find_last_not_of(" \t\r\n");
  return text.substr(begin, end - begin + 1);
}

inline std::string Slot(const char* section, const char* key) {
  return std::string(section) + "/" + key;
}

// 试着往目标目录写一个字节, 判断能不能落盘
inline bool ProbeWritable(const std::filesystem::path& candidate) {
  const HANDLE handle = CreateFileW(candidate.c_str(), GENERIC_WRITE, 0, nullptr,
                                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  CloseHandle(handle);
  return true;
}

inline void ResolvePath(HMODULE module) {
  std::filesystem::path module_dir;
  wchar_t buffer[MAX_PATH] = {};
  if (GetModuleFileNameW(module, buffer, MAX_PATH) != 0) {
    module_dir = std::filesystem::path(buffer).parent_path();
  }

  if (!module_dir.empty()) {
    const auto beside_loader = module_dir / L"AEEF.ini";
    if (ProbeWritable(beside_loader)) {
      path = beside_loader;
      writable = true;
      return;
    }
  }

  wchar_t app_data[MAX_PATH] = {};
  const DWORD length = GetEnvironmentVariableW(L"APPDATA", app_data, MAX_PATH);
  std::filesystem::path fallback;
  if (length != 0 && length < MAX_PATH) {
    fallback = std::filesystem::path(app_data) / L"E_E_FPV";
  } else {
    fallback = module_dir.empty() ? std::filesystem::path(L".") : module_dir;
  }

  std::error_code ec;
  std::filesystem::create_directories(fallback, ec);
  path = fallback / L"AEEF.ini";
  writable = ProbeWritable(path);
}

inline void Load(HMODULE module) {
  ResolvePath(module);

  std::ifstream input(path);
  std::string section;
  std::string line;
  while (std::getline(input, line)) {
    line = Trim(line);
    if (line.empty() || line[0] == ';' || line[0] == '#') continue;
    if (line.front() == '[' && line.back() == ']') {
      section = line.substr(1, line.size() - 2);
      continue;
    }
    const auto separator = line.find('=');
    if (separator == std::string::npos || section.empty()) continue;
    const std::string key = Trim(line.substr(0, separator));
    const std::string value = Trim(line.substr(separator + 1));
    if (!key.empty()) entries[section + "/" + key] = value;
  }
  loaded = true;

  char report[512];
  std::snprintf(report, sizeof(report), "E_E_FPV: config store ready (%s, %s, %zu entries)",
                path.string().c_str(), writable ? "writable" : "read-only", entries.size());
  reshade::log::message(reshade::log::level::info, report);
}

inline void Save() {
  if (!loaded || !writable) return;

  std::ofstream output(path, std::ios::trunc);
  if (!output) return;
  output << "; Arknights_E_E_FPV configuration\n"
            "; 由插件自动维护, 可自由编辑; 插件运行时改动会在下一次落盘时覆盖\n\n";

  std::string current_section;
  for (const auto& [slot, value] : entries) {
    const auto separator = slot.find('/');
    if (separator == std::string::npos) continue;
    const std::string section = slot.substr(0, separator);
    const std::string key = slot.substr(separator + 1);
    if (section != current_section) {
      output << (current_section.empty() ? "" : "\n") << "[" << section << "]\n";
      current_section = section;
    }
    output << key << "=" << value << "\n";
  }
}

inline void Set(const char* section, const char* key, const std::string& value) {
  entries[Slot(section, key)] = value;
  dirty = true;
}

inline void SetFloat(const char* section, const char* key, float value) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.6g", value);
  Set(section, key, buffer);
}

inline void SetInt(const char* section, const char* key, int value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%d", value);
  Set(section, key, buffer);
}

inline const std::string* Find(const char* section, const char* key) {
  const auto found = entries.find(Slot(section, key));
  return found == entries.end() ? nullptr : &found->second;
}

inline bool GetFloat(const char* section, const char* key, float& out) {
  const auto* text = Find(section, key);
  if (text == nullptr) return false;
  char* end = nullptr;
  const float parsed = std::strtof(text->c_str(), &end);
  if (end == text->c_str()) return false;
  out = parsed;
  return true;
}

inline bool GetInt(const char* section, const char* key, int& out) {
  float value = 0.f;
  if (!GetFloat(section, key, value)) return false;
  out = static_cast<int>(value);
  return true;
}

}  // namespace endfield::config_store
