#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace todofloat {

inline constexpr wchar_t kDefaultRunSubKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
inline constexpr wchar_t kDefaultAutoStartValueName[] = L"TodoFloat";

// 获取当前可执行文件的完整路径
std::filesystem::path currentExecutablePath();

// 将可执行文件路径格式化为带双引号的开机自启命令行
std::wstring formatStartupCommand(const std::filesystem::path& exePath);

// 判断注册表中记录的启动命令行是否指向指定可执行文件（忽略大小写与外层引号）
bool startupCommandMatches(std::wstring_view registeredValue, const std::filesystem::path& exePath);

// 查询当前用户开机自启是否已启用且指向指定可执行文件
bool isAutoStartEnabled(
    const std::filesystem::path& exePath,
    const wchar_t* subKey = kDefaultRunSubKey,
    const wchar_t* valueName = kDefaultAutoStartValueName);

// 开启或关闭当前用户开机自启（写入或删除 HKCU Run 注册表项）
bool setAutoStartEnabled(
    bool enable,
    const std::filesystem::path& exePath,
    const wchar_t* subKey = kDefaultRunSubKey,
    const wchar_t* valueName = kDefaultAutoStartValueName);

} // namespace todofloat
