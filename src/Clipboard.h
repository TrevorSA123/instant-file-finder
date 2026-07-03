#pragma once
#include <windows.h>
#include <string>

namespace Clipboard {

// Copies text to the clipboard as Unicode (CF_UNICODETEXT). Returns false if the clipboard
// could not be opened/written.
bool CopyText(HWND owner, const std::wstring& text);

} // namespace Clipboard
