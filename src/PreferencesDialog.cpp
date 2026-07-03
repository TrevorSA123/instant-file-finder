#include "PreferencesDialog.h"
#include "ModalLoop.h"
#include "DpiUtil.h"

#include <string>

namespace {

constexpr wchar_t kClassName[] = L"InstantFileFinder_PreferencesDialog";
constexpr int kCheckboxCount = 12;

enum ControlId : int {
    IDC_CHK_ENABLE_INDEXING = 300,
    IDC_CHK_AUTO_INDEX_FIXED,
    IDC_CHK_INCLUDE_REMOVABLE,
    IDC_CHK_INCLUDE_NETWORK,
    IDC_CHK_FAST_USN_SCAN,
    IDC_CHK_RAW_MFT_SCAN,
    IDC_CHK_PERSIST_CACHE,
    IDC_CHK_INCREMENTAL_UPDATES,
    IDC_CHK_INCLUDE_HIDDEN,
    IDC_CHK_INCLUDE_SYSTEM,
    IDC_CHK_AVOID_REPARSE,
    IDC_CHK_ALWAYS_ON_TOP,
    IDC_EDIT_MAX_RESULTS,
    IDC_EDIT_DEBOUNCE,
    IDC_EDIT_STALE_DAYS,
    IDC_BTN_OK,
    IDC_BTN_CANCEL,
};

struct Controls {
    HWND checkboxes[kCheckboxCount]{};
    HWND editMaxResults = nullptr;
    HWND editDebounce = nullptr;
    HWND editStaleDays = nullptr;
    AppSettings* settings = nullptr;
    bool done = false;
    bool accepted = false;
};

const ControlId kCheckboxIds[kCheckboxCount] = {
    IDC_CHK_ENABLE_INDEXING, IDC_CHK_AUTO_INDEX_FIXED, IDC_CHK_INCLUDE_REMOVABLE, IDC_CHK_INCLUDE_NETWORK,
    IDC_CHK_FAST_USN_SCAN, IDC_CHK_RAW_MFT_SCAN, IDC_CHK_PERSIST_CACHE, IDC_CHK_INCREMENTAL_UPDATES,
    IDC_CHK_INCLUDE_HIDDEN, IDC_CHK_INCLUDE_SYSTEM, IDC_CHK_AVOID_REPARSE, IDC_CHK_ALWAYS_ON_TOP,
};

const wchar_t* kCheckboxLabels[kCheckboxCount] = {
    L"Enable indexing (builds a fast search index; search works without this too)",
    L"Index fixed NTFS drives automatically at startup",
    L"Include removable drives",
    L"Include network drives",
    L"Use fast NTFS USN scan where available",
    L"Use raw MFT parsing where available (fastest; usually needs Administrator)",
    L"Persist index cache to disk",
    L"Incrementally update index from USN journal where available",
    L"Include hidden files",
    L"Include system files",
    L"Avoid reparse points",
    L"Always on top",
};

void ApplyToSettings(Controls& c) {
    bool* fields[kCheckboxCount] = {
        &c.settings->enableIndexing, &c.settings->autoIndexFixedNtfsDrives, &c.settings->includeRemovableDrives,
        &c.settings->includeNetworkDrives, &c.settings->useFastNtfsUsnScan, &c.settings->useRawMftScan,
        &c.settings->persistIndexCache, &c.settings->useIncrementalUsnUpdates,
        &c.settings->includeHiddenFiles, &c.settings->includeSystemFiles,
        &c.settings->avoidReparsePoints, &c.settings->alwaysOnTop,
    };
    for (int i = 0; i < kCheckboxCount; ++i) {
        *fields[i] = (SendMessageW(c.checkboxes[i], BM_GETCHECK, 0, 0) == BST_CHECKED);
    }

    wchar_t buf[16];
    GetWindowTextW(c.editMaxResults, buf, ARRAYSIZE(buf));
    uint32_t maxResults = static_cast<uint32_t>(_wtoi(buf));
    if (maxResults > 0) c.settings->maxDisplayedResults = maxResults;

    GetWindowTextW(c.editDebounce, buf, ARRAYSIZE(buf));
    uint32_t debounce = static_cast<uint32_t>(_wtoi(buf));
    if (debounce > 0) c.settings->searchDebounceMs = debounce;

    GetWindowTextW(c.editStaleDays, buf, ARRAYSIZE(buf));
    c.settings->indexStalenessWarningDays = static_cast<uint32_t>(_wtoi(buf));
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* c = reinterpret_cast<Controls*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_COMMAND:
            if (LOWORD(wParam) == IDC_BTN_OK) {
                ApplyToSettings(*c);
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

HWND CreateCheck(HWND parent, HINSTANCE instance, int id, const wchar_t* label, int y, bool checked, HFONT font, int dpi) {
    HWND h = CreateWindowExW(0, L"BUTTON", label, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        DpiUtil::Scale(20, dpi), y, DpiUtil::Scale(410, dpi), DpiUtil::Scale(20, dpi),
        parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SendMessageW(h, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    return h;
}

} // namespace

namespace PreferencesDialog {

bool Show(HWND owner, HINSTANCE instance, AppSettings& settings) {
    RegisterClassOnce(instance);

    Controls controls;
    controls.settings = &settings;

    int dpi = DpiUtil::GetDpiForWindowSafe(owner);
    const int width = DpiUtil::Scale(480, dpi), height = DpiUtil::Scale(630, dpi);
    RECT ownerRect{};
    GetWindowRect(owner, &ownerRect);
    int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
    int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;

    HWND hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME, kClassName, L"Preferences",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        x, y, width, height, owner, nullptr, instance, nullptr);
    if (hwnd == nullptr) return false;

    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&controls));

    UniqueGdiObject<HFONT> fontOwner = DpiUtil::CreateMessageFont(dpi);
    HFONT font = fontOwner.Get();

    bool checkedValues[kCheckboxCount] = {
        settings.enableIndexing, settings.autoIndexFixedNtfsDrives, settings.includeRemovableDrives,
        settings.includeNetworkDrives, settings.useFastNtfsUsnScan, settings.useRawMftScan, settings.persistIndexCache,
        settings.useIncrementalUsnUpdates, settings.includeHiddenFiles, settings.includeSystemFiles,
        settings.avoidReparsePoints, settings.alwaysOnTop,
    };

    int curY = DpiUtil::Scale(15, dpi);
    for (int i = 0; i < kCheckboxCount; ++i) {
        controls.checkboxes[i] = CreateCheck(hwnd, instance, kCheckboxIds[i], kCheckboxLabels[i], curY, checkedValues[i], font, dpi);
        curY += DpiUtil::Scale(26, dpi);
        if (i == 0) curY += DpiUtil::Scale(8, dpi); // small gap after the master "enable indexing" switch
    }

    curY += DpiUtil::Scale(10, dpi);
    HWND labelMax = CreateWindowExW(0, L"STATIC", L"Maximum displayed results:", WS_CHILD | WS_VISIBLE,
        DpiUtil::Scale(20, dpi), curY + DpiUtil::Scale(3, dpi), DpiUtil::Scale(220, dpi), DpiUtil::Scale(20, dpi),
        hwnd, nullptr, instance, nullptr);
    SendMessageW(labelMax, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    controls.editMaxResults = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", std::to_wstring(settings.maxDisplayedResults).c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER, DpiUtil::Scale(250, dpi), curY, DpiUtil::Scale(100, dpi), DpiUtil::Scale(22, dpi),
        hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EDIT_MAX_RESULTS)), instance, nullptr);
    SendMessageW(controls.editMaxResults, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

    curY += DpiUtil::Scale(32, dpi);
    HWND labelDebounce = CreateWindowExW(0, L"STATIC", L"Search debounce (ms):", WS_CHILD | WS_VISIBLE,
        DpiUtil::Scale(20, dpi), curY + DpiUtil::Scale(3, dpi), DpiUtil::Scale(220, dpi), DpiUtil::Scale(20, dpi),
        hwnd, nullptr, instance, nullptr);
    SendMessageW(labelDebounce, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    controls.editDebounce = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", std::to_wstring(settings.searchDebounceMs).c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER, DpiUtil::Scale(250, dpi), curY, DpiUtil::Scale(100, dpi), DpiUtil::Scale(22, dpi),
        hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EDIT_DEBOUNCE)), instance, nullptr);
    SendMessageW(controls.editDebounce, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

    curY += DpiUtil::Scale(32, dpi);
    HWND labelStale = CreateWindowExW(0, L"STATIC", L"Warn if index older than (days, 0=off):", WS_CHILD | WS_VISIBLE,
        DpiUtil::Scale(20, dpi), curY + DpiUtil::Scale(3, dpi), DpiUtil::Scale(220, dpi), DpiUtil::Scale(20, dpi),
        hwnd, nullptr, instance, nullptr);
    SendMessageW(labelStale, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    controls.editStaleDays = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", std::to_wstring(settings.indexStalenessWarningDays).c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER, DpiUtil::Scale(250, dpi), curY, DpiUtil::Scale(100, dpi), DpiUtil::Scale(22, dpi),
        hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EDIT_STALE_DAYS)), instance, nullptr);
    SendMessageW(controls.editStaleDays, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

    curY += DpiUtil::Scale(45, dpi);
    int btnWidth = DpiUtil::Scale(85, dpi), btnHeight = DpiUtil::Scale(28, dpi);
    HWND okButton = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        width - DpiUtil::Scale(200, dpi), curY, btnWidth, btnHeight, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_OK)), instance, nullptr);
    SendMessageW(okButton, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

    HWND cancelButton = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        width - DpiUtil::Scale(105, dpi), curY, btnWidth, btnHeight, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_CANCEL)), instance, nullptr);
    SendMessageW(cancelButton, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

    SetFocus(controls.checkboxes[0]);

    ModalLoop::Run(hwnd, owner, controls.done);

    return controls.accepted;
}

} // namespace PreferencesDialog
