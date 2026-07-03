#include "App.h"
#include "MainWindow.h"
#include "SettingsService.h"
#include "ShellHelpers.h"
#include "StartupElevationDialog.h"

#include <commctrl.h>

namespace {

// DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2, resolved dynamically because it's only present
// on Windows 10 1703+ and this app also needs to run on plain Windows 10 RTM/Windows 11.
using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);

} // namespace

void App::EnableDpiAwareness() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 != nullptr) {
        auto setContext = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (setContext != nullptr &&
            setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
            return;
        }
    }
    // Fall back gracefully on older systems where the per-monitor-v2 context isn't available.
    SetProcessDPIAware();
}

void App::InitCommonControls() {
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES | ICC_DATE_CLASSES;
    InitCommonControlsEx(&icc);
}

bool App::HandleStartupElevation(HINSTANCE instance) {
    if (ShellHelpers::IsProcessElevated()) {
        return false; // nothing to offer; Raw MFT (if enabled) will just work
    }

    AppSettings settings = SettingsService::Load();

    if (settings.startupElevationChoice == StartupElevationChoice::AlwaysContinueNormal) {
        return false; // user already said not to ask again, and to stay non-elevated
    }

    bool wantElevate = (settings.startupElevationChoice == StartupElevationChoice::AlwaysElevate);
    if (settings.startupElevationChoice == StartupElevationChoice::AlwaysAsk) {
        // No owner yet: this runs before the main window exists, so the dialog is shown
        // ownerless (centered on the primary monitor's work area - see StartupElevationDialog).
        StartupElevationDialog::Result result = StartupElevationDialog::Show(nullptr, instance);
        wantElevate = (result.choice == StartupElevationDialog::Choice::RestartElevated);
        if (result.remember) {
            settings.startupElevationChoice = wantElevate ? StartupElevationChoice::AlwaysElevate
                                                            : StartupElevationChoice::AlwaysContinueNormal;
        }
    }

    // Either choice means the user wants indexed search rather than the live-scan default;
    // Raw MFT specifically only makes sense paired with an actual elevated relaunch.
    settings.enableIndexing = true;
    settings.autoIndexFixedNtfsDrives = true;
    if (wantElevate) {
        settings.useRawMftScan = true;
    }
    SettingsService::Save(settings);

    if (!wantElevate) {
        return false;
    }

    if (ShellHelpers::RelaunchElevated(nullptr)) {
        return true;
    }

    MessageBoxW(nullptr, L"Could not restart as Administrator. Continuing in normal mode.",
                L"Instant File Finder", MB_OK | MB_ICONWARNING);
    return false;
}

int App::Run(HINSTANCE instance, int showCmd) {
    EnableDpiAwareness();
    InitCommonControls();

    if (HandleStartupElevation(instance)) {
        return 0; // an elevated instance is starting up; this one exits without showing a window
    }

    MainWindow mainWindow;
    if (!mainWindow.Create(instance, showCmd)) {
        return -1;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return static_cast<int>(msg.wParam);
}
