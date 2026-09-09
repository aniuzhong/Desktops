#pragma once

#include <windows.h>

#include <string_view>

namespace desktops {
namespace log {

void Init();
void Debug(std::wstring_view message);
void Info(std::wstring_view message);
void Warn(std::wstring_view message);
void Err(std::wstring_view message);
void Err(std::wstring_view message, DWORD last_error);

}  // namespace log
}  // namespace desktops
