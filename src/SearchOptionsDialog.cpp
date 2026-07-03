#include "SearchOptionsDialog.h"
#include "ModalLoop.h"
#include "FormatUtil.h"
#include "DpiUtil.h"

#include <commctrl.h>
#include <string>

namespace {

constexpr wchar_t kClassName[] = L"InstantFileFinder_SearchOptionsDialog";

enum ControlId : int {
    IDC_CHK_CASE_SENSITIVE = 500,
    IDC_CHK_INCLUDE_HIDDEN,
    IDC_CHK_INCLUDE_SYSTEM,
    IDC_CHK_SIZE_FILTER,
    IDC_EDIT_MIN_SIZE,
    IDC_EDIT_MAX_SIZE,
    IDC_CHK_DATE_FILTER,
    IDC_DTP_AFTER,
    IDC_DTP_BEFORE,
    IDC_BTN_OK,
    IDC_BTN_CANCEL,
};

struct Controls {
    HWND chkCaseSensitive = nullptr;
    HWND chkIncludeHidden = nullptr;
    HWND chkIncludeSystem = nullptr;
    HWND chkSizeFilter = nullptr;
    HWND editMinSize = nullptr;
    HWND editMaxSize = nullptr;
    HWND chkDateFilter = nullptr;
    HWND dtpAfter = nullptr;
    HWND dtpBefore = nullptr;

    SearchOptions* options = nullptr;
    bool done = false;
    bool accepted = false;
};

FILETIME LocalSystemTimeToFileTime(const SYSTEMTIME& local) {
    SYSTEMTIME utc{};
    TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc);
    FILETIME ft{};
    SystemTimeToFileTime(&utc, &ft);
    return ft;
}

SYSTEMTIME FileTimeToLocalSystemTime(const FILETIME& ft) {
    SYSTEMTIME utc{};
    FileTimeToSystemTime(&ft, &utc);
    SYSTEMTIME local{};
    SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);
    return local;
}

void ApplyToOptions(Controls& c) {
    SearchOptions& o = *c.options;
    o.caseSensitive = (SendMessageW(c.chkCaseSensitive, BM_GETCHECK, 0, 0) == BST_CHECKED);
    o.includeHidden = (SendMessageW(c.chkIncludeHidden, BM_GETCHECK, 0, 0) == BST_CHECKED);
    o.includeSystem = (SendMessageW(c.chkIncludeSystem, BM_GETCHECK, 0, 0) == BST_CHECKED);

    o.sizeFilterEnabled = (SendMessageW(c.chkSizeFilter, BM_GETCHECK, 0, 0) == BST_CHECKED);
    wchar_t buf[64];
    GetWindowTextW(c.editMinSize, buf, ARRAYSIZE(buf));
    uint64_t minSize = 0;
    o.minSize = FormatUtil::ParseSize(buf, minSize) ? minSize : 0;
    GetWindowTextW(c.editMaxSize, buf, ARRAYSIZE(buf));
    uint64_t maxSize = 0;
    o.maxSize = FormatUtil::ParseSize(buf, maxSize) ? maxSize : 0;

    o.modifiedDateFilterEnabled = (SendMessageW(c.chkDateFilter, BM_GETCHECK, 0, 0) == BST_CHECKED);
    SYSTEMTIME st{};
    DateTime_GetSystemtime(c.dtpAfter, &st);
    o.modifiedAfter = LocalSystemTimeToFileTime(st);
    DateTime_GetSystemtime(c.dtpBefore, &st);
    o.modifiedBefore = LocalSystemTimeToFileTime(st);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* c = reinterpret_cast<Controls*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_COMMAND:
            if (LOWORD(wParam) == IDC_BTN_OK) {
                ApplyToOptions(*c);
                c->accepted = true;
                DestroyWindow(hwnd);
            } else if (LOWORD(wParam) == IDC_BTN_CANCEL) {
                DestroyWindow(hwnd);
            }
            return 0;
        case WM_CLOSE:
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

namespace SearchOptionsDialog {

bool Show(HWND owner, HINSTANCE instance, SearchOptions& options) {
    RegisterClassOnce(instance);

    Controls controls;
    controls.options = &options;

    int dpi = DpiUtil::GetDpiForWindowSafe(owner);
    auto S = [dpi](int v) { return DpiUtil::Scale(v, dpi); };

    const int width = S(420), height = S(380);
    RECT ownerRect{};
    GetWindowRect(owner, &ownerRect);
    int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
    int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;

    HWND hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME, kClassName, L"Search Options",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        x, y, width, height, owner, nullptr, instance, nullptr);
    if (hwnd == nullptr) return false;

    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&controls));

    UniqueGdiObject<HFONT> fontOwner = DpiUtil::CreateMessageFont(dpi);
    HFONT font = fontOwner.Get();
    auto setFont = [&](HWND h) { SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE); };

    int curY = S(15);
    controls.chkCaseSensitive = CreateWindowExW(0, L"BUTTON", L"Case sensitive matching",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, S(20), curY, S(300), S(20), hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CHK_CASE_SENSITIVE)), instance, nullptr);
    setFont(controls.chkCaseSensitive);
    SendMessageW(controls.chkCaseSensitive, BM_SETCHECK, options.caseSensitive ? BST_CHECKED : BST_UNCHECKED, 0);

    curY += S(26);
    controls.chkIncludeHidden = CreateWindowExW(0, L"BUTTON", L"Include hidden files",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, S(20), curY, S(300), S(20), hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CHK_INCLUDE_HIDDEN)), instance, nullptr);
    setFont(controls.chkIncludeHidden);
    SendMessageW(controls.chkIncludeHidden, BM_SETCHECK, options.includeHidden ? BST_CHECKED : BST_UNCHECKED, 0);

    curY += S(26);
    controls.chkIncludeSystem = CreateWindowExW(0, L"BUTTON", L"Include system files",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, S(20), curY, S(300), S(20), hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CHK_INCLUDE_SYSTEM)), instance, nullptr);
    setFont(controls.chkIncludeSystem);
    SendMessageW(controls.chkIncludeSystem, BM_SETCHECK, options.includeSystem ? BST_CHECKED : BST_UNCHECKED, 0);

    curY += S(36);
    controls.chkSizeFilter = CreateWindowExW(0, L"BUTTON", L"Filter by size (e.g. 500KB, 1GB)",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, S(20), curY, S(300), S(20), hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CHK_SIZE_FILTER)), instance, nullptr);
    setFont(controls.chkSizeFilter);
    SendMessageW(controls.chkSizeFilter, BM_SETCHECK, options.sizeFilterEnabled ? BST_CHECKED : BST_UNCHECKED, 0);

    curY += S(26);
    HWND minLabel = CreateWindowExW(0, L"STATIC", L"Min:", WS_CHILD | WS_VISIBLE, S(40), curY + S(3), S(40), S(20), hwnd, nullptr, instance, nullptr);
    setFont(minLabel);
    controls.editMinSize = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
        options.minSize > 0 ? FormatUtil::FormatSize(options.minSize, true).c_str() : L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP, S(85), curY, S(100), S(22), hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EDIT_MIN_SIZE)), instance, nullptr);
    setFont(controls.editMinSize);

    HWND maxLabel = CreateWindowExW(0, L"STATIC", L"Max:", WS_CHILD | WS_VISIBLE, S(200), curY + S(3), S(40), S(20), hwnd, nullptr, instance, nullptr);
    setFont(maxLabel);
    controls.editMaxSize = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
        options.maxSize > 0 ? FormatUtil::FormatSize(options.maxSize, true).c_str() : L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP, S(245), curY, S(100), S(22), hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EDIT_MAX_SIZE)), instance, nullptr);
    setFont(controls.editMaxSize);

    curY += S(36);
    controls.chkDateFilter = CreateWindowExW(0, L"BUTTON", L"Filter by modified date",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, S(20), curY, S(300), S(20), hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CHK_DATE_FILTER)), instance, nullptr);
    setFont(controls.chkDateFilter);
    SendMessageW(controls.chkDateFilter, BM_SETCHECK, options.modifiedDateFilterEnabled ? BST_CHECKED : BST_UNCHECKED, 0);

    curY += S(26);
    HWND afterLabel = CreateWindowExW(0, L"STATIC", L"After:", WS_CHILD | WS_VISIBLE, S(40), curY + S(3), S(45), S(20), hwnd, nullptr, instance, nullptr);
    setFont(afterLabel);
    controls.dtpAfter = CreateWindowExW(0, DATETIMEPICK_CLASSW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        S(90), curY, S(150), S(24), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DTP_AFTER)), instance, nullptr);
    setFont(controls.dtpAfter);

    curY += S(32);
    HWND beforeLabel = CreateWindowExW(0, L"STATIC", L"Before:", WS_CHILD | WS_VISIBLE, S(40), curY + S(3), S(45), S(20), hwnd, nullptr, instance, nullptr);
    setFont(beforeLabel);
    controls.dtpBefore = CreateWindowExW(0, DATETIMEPICK_CLASSW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        S(90), curY, S(150), S(24), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DTP_BEFORE)), instance, nullptr);
    setFont(controls.dtpBefore);

    SYSTEMTIME afterSt = options.modifiedDateFilterEnabled ? FileTimeToLocalSystemTime(options.modifiedAfter) : SYSTEMTIME{};
    SYSTEMTIME beforeSt = options.modifiedDateFilterEnabled ? FileTimeToLocalSystemTime(options.modifiedBefore) : SYSTEMTIME{};
    if (afterSt.wYear != 0) DateTime_SetSystemtime(controls.dtpAfter, GDT_VALID, &afterSt);
    if (beforeSt.wYear != 0) DateTime_SetSystemtime(controls.dtpBefore, GDT_VALID, &beforeSt);

    curY += S(45);
    int btnWidth = S(85), btnHeight = S(28);
    HWND okButton = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        width - S(200), curY, btnWidth, btnHeight, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_OK)), instance, nullptr);
    setFont(okButton);

    HWND cancelButton = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        width - S(105), curY, btnWidth, btnHeight, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_CANCEL)), instance, nullptr);
    setFont(cancelButton);

    ModalLoop::Run(hwnd, owner, controls.done);

    return controls.accepted;
}

} // namespace SearchOptionsDialog
