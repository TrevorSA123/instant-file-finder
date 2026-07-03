#include "AboutDialog.h"
#include "ModalLoop.h"
#include "DpiUtil.h"

namespace {

constexpr int IDC_ABOUT_OK = 101;
constexpr wchar_t kClassName[] = L"InstantFileFinder_AboutDialog";

struct State {
    bool done = false;
};

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_COMMAND:
            if (LOWORD(wParam) == IDC_ABOUT_OK) {
                DestroyWindow(hwnd);
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (state != nullptr) state->done = true;
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

namespace AboutDialog {

void Show(HWND owner, HINSTANCE instance) {
    RegisterClassOnce(instance);

    State state;

    // Scaled from the owner's DPI (not the not-yet-existing dialog's) since the dialog is
    // centered relative to it and will typically land on the same monitor.
    int dpi = DpiUtil::GetDpiForWindowSafe(owner);
    const int width = DpiUtil::Scale(380, dpi), height = DpiUtil::Scale(250, dpi);

    RECT ownerRect{};
    GetWindowRect(owner, &ownerRect);
    int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
    int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;

    HWND hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME, kClassName, L"About Instant File Finder",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        x, y, width, height, owner, nullptr, instance, nullptr);
    if (hwnd == nullptr) return;

    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&state));

    // Lay out from the actual client rect (not the outer window size passed to
    // CreateWindowExW) so the OK button gets a real bottom margin regardless of how big the
    // caption bar and borders end up being at the current DPI.
    RECT client{};
    GetClientRect(hwnd, &client);
    int clientWidth = client.right - client.left;
    int clientHeight = client.bottom - client.top;

    UniqueGdiObject<HFONT> boldFont(CreateFontW(
        DpiUtil::Scale(-18, dpi), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI"));
    UniqueGdiObject<HFONT> normalFont = DpiUtil::CreateMessageFont(dpi);

    const int margin = DpiUtil::Scale(20, dpi);
    const int contentWidth = clientWidth - margin * 2;

    HWND title = CreateWindowExW(0, L"STATIC", L"Instant File Finder",
        WS_CHILD | WS_VISIBLE, margin, DpiUtil::Scale(15, dpi), contentWidth, DpiUtil::Scale(28, dpi),
        hwnd, nullptr, instance, nullptr);
    SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(boldFont.Get()), TRUE);

    HWND author = CreateWindowExW(0, L"STATIC", L"Created by Trevor Woollacott",
        WS_CHILD | WS_VISIBLE, margin, DpiUtil::Scale(48, dpi), contentWidth, DpiUtil::Scale(20, dpi),
        hwnd, nullptr, instance, nullptr);
    SendMessageW(author, WM_SETFONT, reinterpret_cast<WPARAM>(normalFont.Get()), TRUE);

    const wchar_t* description =
        L"A small native Windows utility for finding files very quickly "
        L"by using NTFS metadata indexing where available.";
    HWND desc = CreateWindowExW(0, L"STATIC", description,
        WS_CHILD | WS_VISIBLE | SS_LEFT, margin, DpiUtil::Scale(80, dpi), contentWidth, DpiUtil::Scale(70, dpi),
        hwnd, nullptr, instance, nullptr);
    SendMessageW(desc, WM_SETFONT, reinterpret_cast<WPARAM>(normalFont.Get()), TRUE);

    const int btnWidth = DpiUtil::Scale(90, dpi), btnHeight = DpiUtil::Scale(28, dpi);
    int btnY = clientHeight - btnHeight - margin;
    HWND okButton = CreateWindowExW(0, L"BUTTON", L"OK",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        (clientWidth - btnWidth) / 2, btnY, btnWidth, btnHeight, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_ABOUT_OK)), instance, nullptr);
    SendMessageW(okButton, WM_SETFONT, reinterpret_cast<WPARAM>(normalFont.Get()), TRUE);
    SetFocus(okButton);

    ModalLoop::Run(hwnd, owner, state.done);
}

} // namespace AboutDialog
