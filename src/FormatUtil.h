#pragma once
#include <windows.h>
#include <string>
#include <cstdint>

namespace FormatUtil {

// "1.5 MB", "unknown" if !known.
std::wstring FormatSize(uint64_t bytes, bool known);

// Compact attribute string, e.g. "D H R" for Directory/Hidden/ReadOnly.
std::wstring FormatAttributes(uint32_t attributes);

// Friendly message for a Win32 GetLastError() code.
std::wstring FormatWin32Error(DWORD errorCode);

// Parses a human size like "1GB", "500MB", "100" (bytes) into bytes. Returns false on parse failure.
bool ParseSize(const std::wstring& text, uint64_t& outBytes);

} // namespace FormatUtil
