#include "IndexDrivesDialog.h"
#include "ModalLoop.h"
#include "DpiUtil.h"

#include <commctrl.h>
#include <vector>
#include <string>

namespace {

constexpr wchar_t kClassName[] = L"InstantFileFinder_IndexDrivesDialog";

enum ControlId : int {
    IDC_LIST = 400,
    IDC_BTN_CYCLE_METHOD,
    IDC_BTN_OK,
    IDC_BTN_CANCEL,
};

struct Controls {
    HWND listView = nullptr;
    std::vector<DriveIndexStatus> drives;
    bool done = false;
    bool accepted = false;
};

const wchar_t* DriveTypeText(UINT type) {
    switch (type) {
        case DRIVE_FIXED: return L"Fixed";
        case DRIVE_REMOVABLE: return L"Removable";
        case DRIVE_REMOTE: return L"Network";
        case DRIVE_CDROM: return L"Optical";
        case DRIVE_RAMDISK: return L"RAM Disk";
        default: return L"Unknown";
    }
}

const wchar_t* ScanMethodText(ScanMethod method) {
    switch (method) {
        case ScanMethod::RawMft: return L"Raw MFT (fastest)";
        case ScanMethod::FastNtfsUsn: return L"Fast NTFS USN";
        case ScanMethod::Recursive: return L"Recursive fallback";
        default: return L"Disabled";
    }
}

void SetItemText(HWND list, int row, int col, const std::wstring& text) {
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = row;
    item.iSubItem = col;
    item.pszText = const_cast<LPWSTR>(text.c_str());
    if (col == 0) {
        ListView_SetItem(list, &item);
    } else {
        ListView_SetItemText(list, row, col, item.pszText);
    }
}

void PopulateList(Controls& c) {
    ListView_DeleteAllItems(c.listView);
    for (size_t i = 0; i < c.drives.size(); ++i) {
        const auto& d = c.drives[i];
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.iSubItem = 0;
        std::wstring driveText = d.driveRoot;
        item.pszText = const_cast<LPWSTR>(driveText.c_str());
        ListView_InsertItem(c.listView, &item);

        SetItemText(c.listView, static_cast<int>(i), 1, d.label.empty() ? L"(no label)" : d.label);
        SetItemText(c.listView, static_cast<int>(i), 2, d.fileSystem.empty() ? L"(unknown)" : d.fileSystem);
        SetItemText(c.listView, static_cast<int>(i), 3, DriveTypeText(d.driveType));
        SetItemText(c.listView, static_cast<int>(i), 4, d.statusMessage.empty() ? L"Not indexed" : d.statusMessage);
        SetItemText(c.listView, static_cast<int>(i), 5, ScanMethodText(d.scanMethod));

        ListView_SetCheckState(c.listView, static_cast<int>(i), d.selected);
    }
}

void CycleScanMethod(Controls& c, int row) {
    if (row < 0 || row >= static_cast<int>(c.drives.size())) return;
    DriveIndexStatus& d = c.drives[static_cast<size_t>(row)];

    bool ntfsFixed = (d.driveType == DRIVE_FIXED && d.fileSystem == L"NTFS");

    switch (d.scanMethod) {
        case ScanMethod::Disabled:
            d.scanMethod = ScanMethod::Recursive;
            break;
        case ScanMethod::Recursive:
            d.scanMethod = ntfsFixed ? ScanMethod::FastNtfsUsn : ScanMethod::Disabled;
            break;
        case ScanMethod::FastNtfsUsn:
            d.scanMethod = ntfsFixed ? ScanMethod::RawMft : ScanMethod::Disabled;
            break;
        case ScanMethod::RawMft:
        default:
            d.scanMethod = ScanMethod::Disabled;
            break;
    }
    SetItemText(c.listView, row, 5, ScanMethodText(d.scanMethod));
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* c = reinterpret_cast<Controls*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_NOTIFY: {
            auto* hdr = reinterpret_cast<LPNMHDR>(lParam);
            if (c != nullptr && hdr->hwndFrom == c->listView && hdr->code == LVN_ITEMCHANGED) {
                auto* nmv = reinterpret_cast<LPNMLISTVIEW>(lParam);
                if ((nmv->uChanged & LVIF_STATE) && nmv->iItem >= 0 &&
                    nmv->iItem < static_cast<int>(c->drives.size())) {
                    bool checked = ListView_GetCheckState(c->listView, nmv->iItem) != 0;
                    c->drives[static_cast<size_t>(nmv->iItem)].selected = checked;
                }
            } else if (c != nullptr && hdr->hwndFrom == c->listView && hdr->code == NM_DBLCLK) {
                int row = ListView_GetNextItem(c->listView, -1, LVNI_FOCUSED);
                CycleScanMethod(*c, row);
            }
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == IDC_BTN_CYCLE_METHOD) {
                int row = ListView_GetNextItem(c->listView, -1, LVNI_SELECTED);
                CycleScanMethod(*c, row);
            } else if (LOWORD(wParam) == IDC_BTN_OK) {
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

namespace IndexDrivesDialog {

bool Show(HWND owner, HINSTANCE instance, IndexManager& indexManager) {
    RegisterClassOnce(instance);

    Controls controls;
    controls.drives = indexManager.GetDriveStatuses();

    int dpi = DpiUtil::GetDpiForWindowSafe(owner);
    const int width = DpiUtil::Scale(640, dpi), height = DpiUtil::Scale(440, dpi);
    RECT ownerRect{};
    GetWindowRect(owner, &ownerRect);
    int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
    int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;

    HWND hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME, kClassName, L"Index Drives",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        x, y, width, height, owner, nullptr, instance, nullptr);
    if (hwnd == nullptr) return false;

    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&controls));

    UniqueGdiObject<HFONT> fontOwner = DpiUtil::CreateMessageFont(dpi);
    HFONT font = fontOwner.Get();

    HWND info = CreateWindowExW(0, L"STATIC",
        L"Check the drives to index. Double-click a row (or use the button below) to cycle its scan method.\r\n"
        L"Raw MFT is the fastest method but only takes effect if enabled in Preferences, and usually needs Administrator.",
        WS_CHILD | WS_VISIBLE, DpiUtil::Scale(15, dpi), DpiUtil::Scale(12, dpi), width - DpiUtil::Scale(40, dpi),
        DpiUtil::Scale(32, dpi), hwnd, nullptr, instance, nullptr);
    SendMessageW(info, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

    controls.listView = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL,
        DpiUtil::Scale(15, dpi), DpiUtil::Scale(52, dpi), width - DpiUtil::Scale(40, dpi), height - DpiUtil::Scale(144, dpi),
        hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LIST)), instance, nullptr);
    ListView_SetExtendedListViewStyle(controls.listView, LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES | LVS_EX_GRIDLINES);
    SendMessageW(controls.listView, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

    struct { const wchar_t* text; int width; } columns[] = {
        { L"Drive", 60 }, { L"Label", 130 }, { L"File System", 90 },
        { L"Type", 90 }, { L"Status", 130 }, { L"Scan Method", 130 },
    };
    for (int i = 0; i < static_cast<int>(ARRAYSIZE(columns)); ++i) {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.pszText = const_cast<LPWSTR>(columns[i].text);
        col.cx = DpiUtil::Scale(columns[i].width, dpi);
        ListView_InsertColumn(controls.listView, i, &col);
    }

    PopulateList(controls);

    int buttonY = height - DpiUtil::Scale(80, dpi);
    int btnWidth = DpiUtil::Scale(85, dpi), btnHeight = DpiUtil::Scale(28, dpi);
    HWND cycleBtn = CreateWindowExW(0, L"BUTTON", L"Cycle Scan Method", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        DpiUtil::Scale(15, dpi), buttonY, DpiUtil::Scale(160, dpi), DpiUtil::Scale(26, dpi),
        hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_CYCLE_METHOD)), instance, nullptr);
    SendMessageW(cycleBtn, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

    HWND okButton = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        width - DpiUtil::Scale(200, dpi), buttonY, btnWidth, btnHeight, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_OK)), instance, nullptr);
    SendMessageW(okButton, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

    HWND cancelButton = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        width - DpiUtil::Scale(105, dpi), buttonY, btnWidth, btnHeight, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_CANCEL)), instance, nullptr);
    SendMessageW(cancelButton, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

    ModalLoop::Run(hwnd, owner, controls.done);

    if (controls.accepted) {
        for (const auto& d : controls.drives) {
            if (d.driveRoot.empty()) continue;
            wchar_t letter = d.driveRoot[0];
            indexManager.SetDriveSelection(letter, d.selected);
            indexManager.SetDriveScanMethod(letter, d.scanMethod);
        }
    }

    return controls.accepted;
}

} // namespace IndexDrivesDialog
