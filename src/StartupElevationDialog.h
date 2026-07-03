#pragma once
// Shown at startup when the process is not elevated, offering to restart as Administrator for
// Raw MFT parsing (the fastest indexing method) versus continuing with NTFS USN scanning (fast,
// no elevation needed). See App.cpp for how the result is applied.

#include <windows.h>

namespace StartupElevationDialog {

enum class Choice {
    RestartElevated,
    ContinueNormal,
};

struct Result {
    Choice choice = Choice::ContinueNormal;
    bool remember = false;
};

// 'owner' may be nullptr: this can run before the main window exists, in which case the dialog
// is centered on the primary monitor's work area instead of an owner window.
Result Show(HWND owner, HINSTANCE instance);

} // namespace StartupElevationDialog
