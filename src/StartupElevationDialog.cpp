#include "StartupElevationDialog.h"
#include "ModalLoop.h"
#include "DpiUtil.h"

namespace {

constexpr wchar_t kClassName[] = L"InstantFileFinder_StartupElevationDialog";

enum ControlId : int {
    IDC_CHK_REMEMBER = 600,
    IDC_BTN_ELEVATE,
    IDC_BTN_NORMAL,
};

struct Controls {
    HWND chkRemember = nullptr;
    StartupElevationDialog::Result result;
    bool done = false;
};

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* c = reinterpret_cast<Controls*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_COMMAND:
            if (LOWORD(wParam) == IDC_BTN_ELEVATE || LOWORD(wParam) == IDC_BTN_NORMAL) {
                c->result.choice = (LOWORD(wParam) == IDC_BTN_ELEVATE)
                    ? StartupElevationDialog::Choice::RestartElevated
                    : StartupElevationDialog::Choice::ContinueNormal;
                c->result.remember = (SendMessageW(c->chkRemember, BM_GETCHECK, 0, 0) == BST_CHECKED);
                DestroyWindow(hwnd);
            }
            return 0;
        case WM_CLOSE:
            // Dismissed without an explicit choice: behave like "continue normal" but never
            // remember an inconclusive interaction.
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (c != nullptr) c->done = true;
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

void RegisterClassOnce(HINSTANCE instance) {
    static bool registered = false;
    if (registered) return;
    registered = true;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
}

} // namespace

namespace StartupElevationDialog {

Result Show(HWND owner, HINSTANCE instance) {
    RegisterClassOnce(instance);

    Controls controls;

    int dpi = DpiUtil::GetDpiForWindowSafe(owner);
    auto S = [dpi](int v) { return DpiUtil::Scale(v, dpi); };

    const int width = S(460), height = S(300);

    RECT bounds{};
    if (owner != nullptr) {
        GetWindowRect(owner, &bounds);
    } else {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &bounds, 0);
    }
    int x = bounds.left + ((bounds.right - bounds.left) - width) / 2;
    int y = bounds.top + ((bounds.bottom - bounds.top) - height) / 2;

    HWND hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME, kClassName, L"Faster Search Available",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        x, y, width, height, owner, nullptr, instance, nullptr);
    if (hwnd == nullptr) return controls.result;

    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&controls));

    UniqueGdiObject<HFONT> normalFont = DpiUtil::CreateMessageFont(dpi);
    UniqueGdiObject<HFONT> boldFont(CreateFontW(
        S(-16), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI"));

    const int margin = S(20);
    const int contentWidth = width - margin * 2;

    HWND title = CreateWindowExW(0, L"STATIC", L"Instant File Finder can search even faster",
        WS_CHILD | WS_VISIBLE, margin, S(15), contentWidth, S(24), hwnd, nullptr, instance, nullptr);
    SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(boldFont.Get()), TRUE);

    const wchar_t* body =
        L"Raw MFT parsing is the fastest way to index a drive, but it needs the app to run as "
        L"Administrator. Without it, Instant File Finder still indexes quickly using the NTFS "
        L"USN journal - no elevation required either way for normal use.";
    HWND desc = CreateWindowExW(0, L"STATIC", body, WS_CHILD | WS_VISIBLE | SS_LEFT,
        margin, S(45), contentWidth, S(90), hwnd, nullptr, instance, nullptr);
    SendMessageW(desc, WM_SETFONT, reinterpret_cast<WPARAM>(normalFont.Get()), TRUE);

    controls.chkRemember = CreateWindowExW(0, L"BUTTON", L"Remember this choice and don't ask again",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, margin, S(145), contentWidth, S(20),
        hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CHK_REMEMBER)), instance, nullptr);
    SendMessageW(controls.chkRemember, WM_SETFONT, reinterpret_cast<WPARAM>(normalFont.Get()), TRUE);

    int btnY = S(180);
    int btnHeight = S(32);
    int btnWidth = contentWidth;

    HWND elevateButton = CreateWindowExW(0, L"BUTTON", L"Restart as Administrator (fastest)",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, margin, btnY, btnWidth, btnHeight,
        hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_ELEVATE)), instance, nullptr);
    SendMessageW(elevateButton, WM_SETFONT, reinterpret_cast<WPARAM>(normalFont.Get()), TRUE);

    HWND normalButton = CreateWindowExW(0, L"BUTTON", L"Continue in Normal Mode (NTFS USN)",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP, margin, btnY + btnHeight + S(10), btnWidth, btnHeight,
        hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_NORMAL)), instance, nullptr);
    SendMessageW(normalButton, WM_SETFONT, reinterpret_cast<WPARAM>(normalFont.Get()), TRUE);

    SetFocus(elevateButton);

    ModalLoop::Run(hwnd, owner, controls.done);

    return controls.result;
}

} // namespace StartupElevationDialog
