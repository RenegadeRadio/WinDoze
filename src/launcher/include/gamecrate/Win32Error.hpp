#pragma once

#include <Windows.h>

#include <string>

namespace gamecrate {

std::wstring FormatWin32Error(DWORD error);
std::wstring QuoteCommandLineArgument(const std::wstring& value);

}  // namespace gamecrate
