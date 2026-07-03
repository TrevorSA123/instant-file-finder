#pragma once
// Small helper for hand-rolled modal windows (used by AboutDialog, PreferencesDialog,
// IndexDrivesDialog, SearchOptionsDialog). These are plain WS_POPUP windows built entirely
// with CreateWindowExW rather than DIALOGEX resource templates, per the "no visual designer"
// requirement; this helper gives them standard modal behavior (disabled owner, Tab/Enter/Esc
// navigation via IsDialogMessageW, restored focus on close).

#include <windows.h>

namespace ModalLoop {

// Runs a modal message loop for 'dlg' owned by 'owner'. Returns once 'done' becomes true,
// which the dialog's own WM_DESTROY handler is responsible for setting just before/while the
// window is destroyed (see the Show() implementations for the pattern).
inline void Run(HWND dlg, HWND owner, const bool& done) {
    if (owner != nullptr) {
        EnableWindow(owner, FALSE);
    }
    ShowWindow(dlg, SW_SHOW);
    UpdateWindow(dlg);
    SetForegroundWindow(dlg);

    MSG msg;
    while (!done) {
        BOOL result = GetMessageW(&msg, nullptr, 0, 0);
        if (result <= 0) break; // WM_QUIT or error: let the outer app loop handle shutdown
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (owner != nullptr) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }
}

} // namespace ModalLoop
