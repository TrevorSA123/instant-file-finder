#pragma once
#include <windows.h>

namespace AboutDialog {

// Shows a simple modal About box (blocks until closed).
void Show(HWND owner, HINSTANCE instance);

} // namespace AboutDialog
