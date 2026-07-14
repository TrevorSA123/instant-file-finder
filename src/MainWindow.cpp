#include "MainWindow.h"
#include "Resource.h"
#include "AppMessages.h"
#include "QueryParser.h"
#include "StringUtil.h"
#include "FormatUtil.h"
#include "FileTimeUtil.h"
#include "Clipboard.h"
#include "ShellHelpers.h"
#include "AboutDialog.h"
#include "PreferencesDialog.h"
#include "IndexDrivesDialog.h"
#include "SearchOptionsDialog.h"
#include "DriveEnumerator.h"
#include "DpiUtil.h"

#include <commctrl.h>
#include <algorithm>
#include <cwctype>
#include <set>
#include <sstream>
#include <string_view>
#include <unordered_set>

namespace {

constexpr wchar_t kClassName[] = L"InstantFileFinderMainWindow";
constexpr int kColumnCount = 10;

const wchar_t* kColumnHeaders[kColumnCount] = {
    L"Name", L"Path", L"Type", L"Extension", L"Size", L"Modified", L"Created", L"Drive", L"Attributes", L"Source",
};
const int kColumnWidths[kColumnCount] = { 180, 320, 60, 70, 90, 130, 130, 50, 90, 90 };

const wchar_t* kModeLabels[] = { L"Contains", L"Starts with", L"Ends with", L"Exact", L"Wildcard", L"Regex" };
const wchar_t* kTypeLabels[] = { L"Files and Folders", L"Files only", L"Folders only" };

constexpr UINT_PTR kSearchEditSubclassId = 1;

// Plain child EDIT controls don't do anything with Enter on their own (that's normally handled
// by a dialog's default-button routing, which this hand-built main window doesn't have). This
// subclass catches Enter directly so pressing it in the search box runs the search immediately.
LRESULT CALLBACK SearchEditSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                         UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
    if (msg == WM_KEYDOWN && wParam == VK_RETURN) {
        auto* self = reinterpret_cast<MainWindow*>(dwRefData);
        if (self != nullptr) self->OnSearchEnterPressed();
        return 0;
    }
    if (msg == WM_CHAR && wParam == VK_RETURN) {
        return 0; // swallow so the edit control doesn't beep
    }
    if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, SearchEditSubclassProc, uIdSubclass);
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

} // namespace

MainWindow::MainWindow() = default;

MainWindow::~MainWindow() {
    if (m_enricher) m_enricher->Shutdown();
    if (m_folderSizeCalculator) m_folderSizeCalculator->Shutdown();
}

LRESULT CALLBACK MainWindow::StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    MainWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<MainWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->m_hwnd = hwnd;
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self != nullptr) {
        return self->HandleMessage(hwnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT MainWindow::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            OnCreate(hwnd);
            return 0;
        case WM_DESTROY:
            OnDestroy();
            PostQuitMessage(0);
            return 0;
        case WM_SIZE:
            OnSize(LOWORD(lParam), HIWORD(lParam));
            return 0;
        case WM_COMMAND:
            OnCommand(wParam, lParam);
            return 0;
        case WM_NOTIFY:
            return OnNotify(lParam);
        case WM_CONTEXTMENU: {
            int x = static_cast<int>(static_cast<short>(LOWORD(lParam)));
            int y = static_cast<int>(static_cast<short>(HIWORD(lParam)));
            OnContextMenu(reinterpret_cast<HWND>(wParam), x, y);
            return 0;
        }
        case WM_DPICHANGED:
            OnDpiChanged(HIWORD(wParam), reinterpret_cast<const RECT*>(lParam));
            return 0;
        case WM_TIMER:
            OnTimer(wParam);
            return 0;
        case WM_APP_DRIVE_STATUS_CHANGED:
            OnDriveStatusChanged(static_cast<wchar_t>(wParam));
            return 0;
        case WM_APP_INDEXING_PROGRESS:
            OnIndexingProgress(static_cast<wchar_t>(wParam), static_cast<uint64_t>(lParam));
            return 0;
        case WM_APP_INDEXING_COMPLETE:
            OnIndexingComplete();
            return 0;
        case WM_APP_METADATA_ENRICHED:
            OnMetadataEnriched(reinterpret_cast<MetadataEnricher::EnrichedResult*>(lParam));
            return 0;
        case WM_APP_FOLDER_SIZE_COMPUTED:
            OnFolderSizeComputed(reinterpret_cast<FolderSizeCalculator::ComputedResult*>(lParam));
            return 0;
        case WM_APP_LIVE_SEARCH_PROGRESS:
            OnLiveSearchUpdate(/*complete=*/false);
            return 0;
        case WM_APP_LIVE_SEARCH_COMPLETE:
            OnLiveSearchUpdate(/*complete=*/true);
            return 0;
        case WM_APP_INDEXED_SEARCH_COMPLETE:
            OnIndexedSearchComplete(reinterpret_cast<IndexedSearchWorker::CompletedSearch*>(lParam));
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

bool MainWindow::Create(HINSTANCE instance, int showCmd) {
    m_instance = instance;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = StaticWndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kClassName;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;

    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    // Authored at 96 DPI; scaled by the primary monitor's DPI here to avoid an initial
    // wrong-then-resized flash for the common single-monitor case. OnCreate() re-queries the
    // window's actual DPI once it exists (correct even if it lands on a different monitor), and
    // WM_DPICHANGED handles the window being dragged to a differently-scaled monitor afterwards.
    int initialDpi = DpiUtil::GetDpiForWindowSafe(nullptr);
    int initialWidth = DpiUtil::Scale(1100, initialDpi);
    int initialHeight = DpiUtil::Scale(700, initialDpi);

    HWND hwnd = CreateWindowExW(
        WS_EX_APPWINDOW, kClassName, L"Instant File Finder",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, initialWidth, initialHeight,
        nullptr, nullptr, instance, this);

    if (hwnd == nullptr) {
        return false;
    }

    ShowWindow(hwnd, showCmd);
    UpdateWindow(hwnd);
    return true;
}

void MainWindow::OnSearchEnterPressed() {
    KillTimer(m_hwnd, IDT_SEARCH_DEBOUNCE);
    RunSearch();
}

void MainWindow::OnCreate(HWND hwnd) {
    m_dpi = DpiUtil::GetDpiForWindowSafe(hwnd);
    m_uiFont = DpiUtil::CreateMessageFont(m_dpi);

    m_settings = SettingsService::Load();
    ApplySettingsToOptions();

    CreateMenus();
    CreateControls();

    m_indexManager.Initialize(hwnd);
    m_liveSearchManager.Initialize(hwnd);
    m_indexedSearchWorker.Initialize(hwnd);
    m_enricher = std::make_unique<MetadataEnricher>(hwnd);
    m_folderSizeCalculator = std::make_unique<FolderSizeCalculator>(hwnd);
    m_folderSizeCalculator->SetIndexSizeLookup(
        [this](const std::vector<std::wstring>& folderPaths) { return LookupFolderSizesFromIndex(folderPaths); });

    PopulateDriveFilterCombo();

    if (m_settings.alwaysOnTop) {
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        CheckMenuItem(m_menu, ID_OPTIONS_ALWAYS_ON_TOP, MF_BYCOMMAND | MF_CHECKED);
    }

    LayoutControls();
    UpdateStatusBar();

    // Indexing is opt-in (see AppSettings::enableIndexing): by default, nothing is scanned on
    // startup and search runs live against the filesystem instead.
    if (m_settings.enableIndexing) {
        StartInitialIndexingIfConfigured();
    }
}

void MainWindow::OnDestroy() {
    SettingsService::Save(m_settings);
    m_indexManager.CancelIndexing();
    m_liveSearchManager.CancelSearch();
    m_indexedSearchWorker.CancelSearch();
    if (m_enricher) m_enricher->Shutdown();
    if (m_folderSizeCalculator) m_folderSizeCalculator->Shutdown();
}

void MainWindow::CreateMenus() {
    m_menu = CreateMenu();

    HMENU fileMenu = CreatePopupMenu();
    AppendMenuW(fileMenu, MF_STRING, ID_FILE_OPEN_SELECTED, L"Open Selected");
    AppendMenuW(fileMenu, MF_STRING, ID_FILE_OPEN_FOLDER, L"Open Containing Folder");
    AppendMenuW(fileMenu, MF_STRING, ID_FILE_COPY_PATH, L"Copy Selected Path");
    AppendMenuW(fileMenu, MF_STRING, ID_FILE_COPY_RESULTS, L"Copy Results");
    AppendMenuW(fileMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(fileMenu, MF_STRING, ID_FILE_EXIT, L"Exit");
    AppendMenuW(m_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(fileMenu), L"File");

    HMENU indexMenu = CreatePopupMenu();
    AppendMenuW(indexMenu, MF_STRING, ID_INDEX_DRIVES, L"Index Drives...");
    AppendMenuW(indexMenu, MF_STRING, ID_INDEX_REFRESH, L"Refresh Index");
    AppendMenuW(indexMenu, MF_STRING, ID_INDEX_REBUILD, L"Rebuild Index");
    AppendMenuW(indexMenu, MF_STRING, ID_INDEX_CANCEL, L"Cancel Indexing");
    AppendMenuW(indexMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(indexMenu, MF_STRING, ID_INDEX_CLEAR_CACHE, L"Clear Index Cache");
    AppendMenuW(indexMenu, MF_STRING, ID_INDEX_VIEW_DETAILS, L"View Index Details...");
    AppendMenuW(indexMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(indexMenu, MF_STRING, ID_INDEX_RUN_AS_ADMIN, L"Run as Administrator...");
    AppendMenuW(m_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(indexMenu), L"Index");

    // Elevation is fixed for the life of the process (there's no de-elevate), so this only needs
    // to be set once here rather than refreshed on any later state change.
    if (ShellHelpers::IsProcessElevated()) {
        EnableMenuItem(m_menu, ID_INDEX_RUN_AS_ADMIN, MF_BYCOMMAND | MF_GRAYED);
    }

    HMENU searchMenu = CreatePopupMenu();
    AppendMenuW(searchMenu, MF_STRING, ID_SEARCH_CLEAR, L"Clear Search");
    AppendMenuW(searchMenu, MF_STRING, ID_SEARCH_OPTIONS, L"Search Options...");
    AppendMenuW(m_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(searchMenu), L"Search");

    HMENU optionsMenu = CreatePopupMenu();
    AppendMenuW(optionsMenu, MF_STRING, ID_OPTIONS_PREFERENCES, L"Preferences...");
    AppendMenuW(optionsMenu, MF_STRING, ID_OPTIONS_ALWAYS_ON_TOP, L"Always On Top");
    AppendMenuW(m_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(optionsMenu), L"Options");

    HMENU helpMenu = CreatePopupMenu();
    AppendMenuW(helpMenu, MF_STRING, ID_HELP_ABOUT, L"About");
    AppendMenuW(m_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(helpMenu), L"Help");

    SetMenu(m_hwnd, m_menu);
}

void MainWindow::ApplyFont(HWND h) {
    if (h != nullptr) {
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(m_uiFont.Get()), TRUE);
    }
}

void MainWindow::ApplyFontToAllControls() {
    HWND controls[] = { m_editSearch, m_comboMode,  m_comboDrive, m_comboType, m_btnSearch,
                         m_btnClear,  m_btnRefresh, m_listView,   m_statusBar };
    for (HWND h : controls) {
        ApplyFont(h);
    }
}

void MainWindow::RescaleListViewColumns() {
    for (int i = 0; i < kColumnCount; ++i) {
        ListView_SetColumnWidth(m_listView, i, DpiUtil::Scale(kColumnWidths[i], m_dpi));
    }
}

void MainWindow::CreateControls() {
    int dropdownHeight = DpiUtil::Scale(200, m_dpi);

    m_editSearch = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 0, 0, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EDIT_SEARCH)), m_instance, nullptr);
    SetWindowSubclass(m_editSearch, SearchEditSubclassProc, kSearchEditSubclassId, reinterpret_cast<DWORD_PTR>(this));

    m_comboMode = CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        0, 0, 0, dropdownHeight, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_COMBO_MODE)), m_instance, nullptr);
    for (const wchar_t* label : kModeLabels) {
        SendMessageW(m_comboMode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
    }
    SendMessageW(m_comboMode, CB_SETCURSEL, 0, 0);

    m_comboDrive = CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        0, 0, 0, dropdownHeight, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_COMBO_DRIVE)), m_instance, nullptr);

    m_comboType = CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        0, 0, 0, dropdownHeight, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_COMBO_TYPE)), m_instance, nullptr);
    for (const wchar_t* label : kTypeLabels) {
        SendMessageW(m_comboType, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
    }
    SendMessageW(m_comboType, CB_SETCURSEL, 0, 0);

    m_btnSearch = CreateWindowExW(0, L"BUTTON", L"Search", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        0, 0, 0, 0, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BUTTON_SEARCH)), m_instance, nullptr);

    m_btnClear = CreateWindowExW(0, L"BUTTON", L"Clear", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        0, 0, 0, 0, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BUTTON_CLEAR)), m_instance, nullptr);

    m_btnRefresh = CreateWindowExW(0, L"BUTTON", L"Refresh Index", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        0, 0, 0, 0, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BUTTON_REFRESH)), m_instance, nullptr);

    m_listView = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS,
        0, 0, 0, 0, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LISTVIEW_RESULTS)), m_instance, nullptr);
    ListView_SetExtendedListViewStyle(m_listView, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    for (int i = 0; i < kColumnCount; ++i) {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        col.pszText = const_cast<LPWSTR>(kColumnHeaders[i]);
        col.cx = DpiUtil::Scale(kColumnWidths[i], m_dpi);
        col.iSubItem = i;
        ListView_InsertColumn(m_listView, i, &col);
    }

    m_statusBar = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
        0, 0, 0, 0, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STATUSBAR)), m_instance, nullptr);

    ApplyFontToAllControls();
}

void MainWindow::LayoutControls() {
    if (m_hwnd == nullptr) return;

    RECT client{};
    GetClientRect(m_hwnd, &client);
    int width = client.right - client.left;
    int height = client.bottom - client.top;

    SendMessageW(m_statusBar, WM_SIZE, 0, 0);

    RECT statusRect{};
    GetWindowRect(m_statusBar, &statusRect);
    int statusHeight = statusRect.bottom - statusRect.top;

    int statusParts[7];
    for (int i = 0; i < 7; ++i) {
        statusParts[i] = width * (i + 1) / 7;
    }
    statusParts[6] = -1;
    SendMessageW(m_statusBar, SB_SETPARTS, 7, reinterpret_cast<LPARAM>(statusParts));

    const int margin = DpiUtil::Scale(8, m_dpi);
    const int rowHeight = DpiUtil::Scale(24, m_dpi);
    const int gap = DpiUtil::Scale(6, m_dpi);
    int y = margin;
    int x = margin;

    int comboW = DpiUtil::Scale(130, m_dpi);
    int btnW = DpiUtil::Scale(100, m_dpi);
    int refreshBtnW = DpiUtil::Scale(120, m_dpi);

    int fixedWidth = (comboW * 3) + (btnW * 2) + refreshBtnW + gap * 6;
    int editW = std::max(DpiUtil::Scale(150, m_dpi), width - fixedWidth - margin * 2);

    MoveWindow(m_editSearch, x, y, editW, rowHeight, TRUE);
    x += editW + gap;
    MoveWindow(m_comboMode, x, y, comboW, 200, TRUE);
    x += comboW + gap;
    MoveWindow(m_comboDrive, x, y, comboW, 200, TRUE);
    x += comboW + gap;
    MoveWindow(m_comboType, x, y, comboW, 200, TRUE);
    x += comboW + gap;
    MoveWindow(m_btnSearch, x, y, btnW, rowHeight, TRUE);
    x += btnW + gap;
    MoveWindow(m_btnClear, x, y, btnW, rowHeight, TRUE);
    x += btnW + gap;
    MoveWindow(m_btnRefresh, x, y, refreshBtnW, rowHeight, TRUE);

    int listY = y + rowHeight + margin;
    int listHeight = height - listY - statusHeight - margin;
    if (listHeight < 0) listHeight = 0;
    MoveWindow(m_listView, margin, listY, width - margin * 2, listHeight, TRUE);
}

void MainWindow::OnSize(int width, int height) {
    if (width == 0 || height == 0) return;
    LayoutControls();
}

void MainWindow::OnDpiChanged(int newDpi, const RECT* suggestedRect) {
    if (newDpi <= 0) return;
    m_dpi = newDpi;
    m_uiFont = DpiUtil::CreateMessageFont(m_dpi);

    ApplyFontToAllControls();
    RescaleListViewColumns();

    if (suggestedRect != nullptr) {
        // Windows already computed the right position/size for the new monitor; adopt it
        // directly rather than guessing. The resulting WM_SIZE re-runs LayoutControls().
        SetWindowPos(m_hwnd, nullptr, suggestedRect->left, suggestedRect->top,
                     suggestedRect->right - suggestedRect->left, suggestedRect->bottom - suggestedRect->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    } else {
        LayoutControls();
    }
}

void MainWindow::PopulateDriveFilterCombo() {
    SendMessageW(m_comboDrive, CB_RESETCONTENT, 0, 0);
    SendMessageW(m_comboDrive, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"All Indexed Drives"));

    for (const auto& d : m_indexManager.GetDriveStatuses()) {
        if (d.driveRoot.empty()) continue;
        std::wstring label = std::wstring(1, d.driveRoot[0]) + L":";
        SendMessageW(m_comboDrive, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
    SendMessageW(m_comboDrive, CB_SETCURSEL, 0, 0);
}

void MainWindow::ApplySettingsToOptions() {
    m_persistentSearchOptions.includeHidden = m_settings.includeHiddenFiles;
    m_persistentSearchOptions.includeSystem = m_settings.includeSystemFiles;
    m_persistentSearchOptions.maxResults = m_settings.maxDisplayedResults;
}

IndexManager::IndexOptions MainWindow::BuildIndexOptions() const {
    IndexManager::IndexOptions opts;
    opts.avoidReparsePoints = m_settings.avoidReparsePoints;
    opts.persistIndexCache = m_settings.persistIndexCache;
    opts.useIncrementalUsnUpdates = m_settings.useIncrementalUsnUpdates;
    opts.useFastNtfsUsnScan = m_settings.useFastNtfsUsnScan;
    opts.useRawMftScan = m_settings.useRawMftScan;
    return opts;
}

void MainWindow::StartInitialIndexingIfConfigured() {
    auto drives = m_indexManager.GetDriveStatuses();
    std::vector<wchar_t> letters;

    for (const auto& d : drives) {
        if (d.driveRoot.empty()) continue;
        bool include = false;
        if (d.driveType == DRIVE_FIXED && !d.fileSystem.empty()) {
            include = m_settings.autoIndexFixedNtfsDrives;
        } else if (d.driveType == DRIVE_REMOVABLE) {
            include = m_settings.includeRemovableDrives;
        } else if (d.driveType == DRIVE_REMOTE) {
            include = m_settings.includeNetworkDrives;
        }
        m_indexManager.SetDriveSelection(d.driveRoot[0], include);
        if (include) letters.push_back(d.driveRoot[0]);
    }

    if (!letters.empty()) {
        m_indexManager.StartIndexing(letters, /*forceRebuild=*/false, BuildIndexOptions());
        UpdateStatusBar(); // reflect "indexing" and grey the index actions right away
    }
}

void MainWindow::EnableIndexingIfNeeded() {
    // Manually starting/refreshing/rebuilding an index only makes sense combined with search
    // actually consulting that index, so treat it as implicitly opting into indexed search.
    if (!m_settings.enableIndexing) {
        m_settings.enableIndexing = true;
        SettingsService::Save(m_settings);
    }
}

void MainWindow::OnCommand(WPARAM wParam, LPARAM lParam) {
    int id = LOWORD(wParam);
    int code = HIWORD(wParam);
    (void)lParam;

    switch (id) {
        case IDC_EDIT_SEARCH:
            if (code == EN_CHANGE) ScheduleDebouncedSearch();
            return;
        case IDC_COMBO_MODE:
        case IDC_COMBO_DRIVE:
        case IDC_COMBO_TYPE:
            if (code == CBN_SELCHANGE) RunSearch();
            return;
        case IDC_BUTTON_SEARCH:
            RunSearch();
            return;
        case IDC_BUTTON_CLEAR:
            ActionClearSearch();
            return;
        case IDC_BUTTON_REFRESH:
            ActionRefreshIndex();
            return;
        case ID_FILE_OPEN_SELECTED:
            ActionOpenSelected();
            return;
        case ID_FILE_OPEN_FOLDER:
            ActionOpenContainingFolder();
            return;
        case ID_FILE_COPY_PATH:
            ActionCopySelectedPath();
            return;
        case ID_FILE_COPY_RESULTS:
            ActionCopyResults();
            return;
        case ID_CONTEXT_DELETE:
            ActionDeleteSelected();
            return;
        case ID_FILE_EXIT:
            DestroyWindow(m_hwnd);
            return;
        case ID_INDEX_DRIVES:
            ActionIndexDrives();
            return;
        case ID_INDEX_REFRESH:
            ActionRefreshIndex();
            return;
        case ID_INDEX_REBUILD:
            ActionRebuildIndex();
            return;
        case ID_INDEX_CANCEL:
            ActionCancelIndexing();
            return;
        case ID_INDEX_CLEAR_CACHE:
            ActionClearCache();
            return;
        case ID_INDEX_VIEW_DETAILS:
            ActionShowIndexDetails();
            return;
        case ID_INDEX_RUN_AS_ADMIN:
            ActionRunAsAdministrator();
            return;
        case ID_SEARCH_CLEAR:
            ActionClearSearch();
            return;
        case ID_SEARCH_OPTIONS:
            ActionSearchOptions();
            return;
        case ID_OPTIONS_PREFERENCES:
            ActionPreferences();
            return;
        case ID_OPTIONS_ALWAYS_ON_TOP:
            ActionToggleAlwaysOnTop();
            return;
        case ID_HELP_ABOUT:
            ActionAbout();
            return;
        default:
            return;
    }
}

LRESULT MainWindow::OnNotify(LPARAM lParam) {
    auto* hdr = reinterpret_cast<LPNMHDR>(lParam);

    if (hdr->hwndFrom == m_statusBar) {
        if (hdr->code == NM_CLICK) {
            ActionShowIndexDetails();
        }
        return 0;
    }

    if (hdr->hwndFrom != m_listView) return 0;

    if (hdr->code == LVN_GETDISPINFOW) {
        auto* di = reinterpret_cast<NMLVDISPINFOW*>(lParam);
        int row = di->item.iItem;
        if ((di->item.mask & LVIF_TEXT) && row >= 0 && row < static_cast<int>(m_currentResults.size())) {
            std::wstring text = FormatCell(m_currentResults[static_cast<size_t>(row)], di->item.iSubItem);
            wcsncpy_s(di->item.pszText, di->item.cchTextMax, text.c_str(), _TRUNCATE);
        }
        return 0;
    }

    if (hdr->code == LVN_COLUMNCLICK) {
        auto* nmv = reinterpret_cast<LPNMLISTVIEW>(lParam);
        SortResultsBy(nmv->iSubItem);
        return 0;
    }

    if (hdr->code == LVN_ODCACHEHINT) {
        RequestEnrichmentForVisibleRows();
        return 0;
    }

    if (hdr->code == NM_DBLCLK) {
        ActionOpenSelected();
        return 0;
    }

    return 0;
}

void MainWindow::OnContextMenu(HWND source, int screenX, int screenY) {
    if (source != m_listView) return;

    POINT pt{ screenX, screenY };
    if (screenX == -1 && screenY == -1) {
        // Invoked from the keyboard (Shift+F10 / Menu key): anchor near the focused row instead
        // of using the (-1, -1) sentinel as a literal screen position.
        int idx = ListView_GetNextItem(m_listView, -1, LVNI_FOCUSED);
        if (idx < 0) return;
        RECT rc{};
        ListView_GetItemRect(m_listView, idx, &rc, LVIR_BOUNDS);
        pt.x = rc.left + 10;
        pt.y = rc.bottom;
        ClientToScreen(m_listView, &pt);
    } else {
        POINT clientPt = pt;
        ScreenToClient(m_listView, &clientPt);
        LVHITTESTINFO hit{};
        hit.pt = clientPt;
        int hitIndex = ListView_HitTest(m_listView, &hit);
        if (hitIndex >= 0) {
            ListView_SetItemState(m_listView, -1, 0, LVIS_SELECTED);
            ListView_SetItemState(m_listView, hitIndex, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        }
    }

    if (GetSelectedResultIndex() < 0) return; // right-clicked empty space: nothing to act on

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_FILE_OPEN_SELECTED, L"Open");
    AppendMenuW(menu, MF_STRING, ID_FILE_OPEN_FOLDER, L"Open Containing Folder");
    AppendMenuW(menu, MF_STRING, ID_FILE_COPY_PATH, L"Copy Path");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_CONTEXT_DELETE, L"Delete (to Recycle Bin)");

    // Standard workaround so the popup dismisses correctly if the user clicks away from it.
    SetForegroundWindow(m_hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, m_hwnd, nullptr);
    PostMessageW(m_hwnd, WM_NULL, 0, 0);

    DestroyMenu(menu);
}

void MainWindow::OnTimer(WPARAM id) {
    if (id == IDT_SEARCH_DEBOUNCE) {
        KillTimer(m_hwnd, IDT_SEARCH_DEBOUNCE);
        RunSearch();
    } else if (id == IDT_ENRICH_REPAINT) {
        KillTimer(m_hwnd, IDT_ENRICH_REPAINT);
        m_enrichRepaintPending = false;
        if (m_sortColumn == 4 || m_sortColumn == 5 || m_sortColumn == 6) {
            // Newly-arrived sizes/dates (file metadata or folder totals) can change the sort
            // order; re-sort so the list reflects it instead of just repainting stale positions.
            ApplyCurrentSort();
            UpdateResultsListView();
        } else {
            InvalidateRect(m_listView, nullptr, FALSE);
        }
    }
}

void MainWindow::ScheduleDebouncedSearch() {
    UINT ms = m_settings.searchDebounceMs > 0 ? m_settings.searchDebounceMs : 500;
    SetTimer(m_hwnd, IDT_SEARCH_DEBOUNCE, ms, nullptr);
}

void MainWindow::RunSearch() {
    wchar_t buf[2048];
    GetWindowTextW(m_editSearch, buf, ARRAYSIZE(buf));
    std::wstring raw = buf;

    SearchOptions base = m_persistentSearchOptions;

    int modeIdx = static_cast<int>(SendMessageW(m_comboMode, CB_GETCURSEL, 0, 0));
    if (modeIdx >= 0) base.mode = static_cast<SearchMode>(modeIdx);

    int driveIdx = static_cast<int>(SendMessageW(m_comboDrive, CB_GETCURSEL, 0, 0));
    if (driveIdx > 0) {
        wchar_t driveBuf[16]{};
        SendMessageW(m_comboDrive, CB_GETLBTEXT, driveIdx, reinterpret_cast<LPARAM>(driveBuf));
        base.driveFilter = driveBuf;
    } else {
        base.driveFilter.clear();
    }

    int typeIdx = static_cast<int>(SendMessageW(m_comboType, CB_GETCURSEL, 0, 0));
    base.filesOnly = (typeIdx == 1);
    base.foldersOnly = (typeIdx == 2);
    base.maxResults = m_settings.maxDisplayedResults;

    SearchOptions parsed = QueryParser::Parse(raw, base);
    m_lastSearchQuery = raw;

    if (m_settings.enableIndexing) {
        RunIndexedSearch(parsed);
    } else {
        RunLiveSearch(parsed);
    }
}

void MainWindow::RunIndexedSearch(const SearchOptions& parsed) {
    m_liveSearchManager.CancelSearch();

    // Runs on a background thread rather than inline: when the size filter is on, PassesFilters
    // may need to stat files whose size wasn't captured at index time (NTFS USN records don't
    // carry it), which is real disk I/O and can take a while across a large index. See
    // IndexedSearchWorker.
    m_indexedSearchWorker.StartSearch(
        parsed, [this](const std::function<void(const IndexedItem&)>& fn) { m_indexManager.ForEachItem(fn); });
    UpdateStatusBar();
}

void MainWindow::OnIndexedSearchComplete(IndexedSearchWorker::CompletedSearch* result) {
    std::unique_ptr<IndexedSearchWorker::CompletedSearch> owned(result);

    m_lastSearchDurationMs = owned->durationMs;
    m_lastSearchTruncated = owned->outcome.truncated;
    m_lastSearchTotalMatched = owned->outcome.totalMatched;
    m_lastStatusOverride.clear();

    if (owned->outcome.queryError) {
        ShowFriendlyMessage(owned->outcome.errorMessage, MB_ICONWARNING);
    }

    m_currentResults = std::move(owned->outcome.results);
    ApplyCurrentSort();
    UpdateResultsListView();

    RequestEnrichmentForVisibleRows();
    RequestFolderSizesForResults();
    UpdateStatusBar();
}

void MainWindow::RunLiveSearch(const SearchOptions& parsed) {
    bool hasCriteria = !parsed.query.empty() || !parsed.extensionFilter.empty() || !parsed.pathSubstring.empty() ||
                        parsed.sizeFilterEnabled || parsed.modifiedDateFilterEnabled;
    if (!hasCriteria) {
        m_liveSearchManager.CancelSearch();
        m_currentResults.clear();
        RebuildPathIndex();
        UpdateResultsListView();
        m_lastSearchTruncated = false;
        m_lastSearchTotalMatched = 0;
        m_lastSearchDurationMs = 0;
        m_lastStatusOverride = L"Type a search term to search (indexing is disabled - enable it in "
                                L"Options > Preferences for instant indexed search).";
        UpdateStatusBar();
        return;
    }

    if (parsed.mode == SearchMode::Regex && !parsed.query.empty()) {
        std::wregex testRegex;
        std::wstring err;
        if (!SearchEngine::TryCompileRegex(parsed, testRegex, err)) {
            ShowFriendlyMessage(err, MB_ICONWARNING);
            return;
        }
    }

    std::vector<std::wstring> roots = GetLiveSearchRootPaths(parsed);
    if (roots.empty()) {
        ShowFriendlyMessage(L"No eligible drives to search. Check Preferences or the drive filter.");
        return;
    }

    m_lastStatusOverride.clear();
    m_liveSearchStartTick = GetTickCount();
    m_liveSearchFirstUpdatePending = true;
    m_liveSearchManager.StartSearch(parsed, roots, m_settings.avoidReparsePoints);
    UpdateStatusBar();
}

std::vector<std::wstring> MainWindow::GetLiveSearchRootPaths(const SearchOptions& parsed) const {
    if (!parsed.driveFilter.empty()) {
        std::wstring root = parsed.driveFilter;
        if (!root.empty() && root.back() != L'\\') root += L'\\';
        return { root };
    }

    std::vector<std::wstring> roots;
    for (const auto& d : DriveEnumerator::EnumerateDrives()) {
        if (d.driveRoot.empty()) continue;
        bool eligible = false;
        if (d.driveType == DRIVE_FIXED && !d.fileSystem.empty()) {
            eligible = true;
        } else if (d.driveType == DRIVE_REMOVABLE) {
            eligible = m_settings.includeRemovableDrives;
        } else if (d.driveType == DRIVE_REMOTE) {
            eligible = m_settings.includeNetworkDrives;
        }
        if (eligible) roots.push_back(d.driveRoot);
    }
    return roots;
}

void MainWindow::OnLiveSearchUpdate(bool complete) {
    auto snapshot = m_liveSearchManager.GetSnapshot();
    size_t previousCount = m_currentResults.size();
    m_currentResults = std::move(snapshot.results);
    RebuildPathIndex();

    m_lastSearchTruncated = snapshot.truncated;
    m_lastSearchTotalMatched = snapshot.totalMatched;

    // A live search streams matches in arrival order, growing the result list one batch at a
    // time. As long as this batch is a pure continuation of the previous one (same search, more
    // rows appended), just tell the list view its new count and let it paint only the newly
    // revealed rows (AppendResultsToListView) instead of re-sorting and repainting everything on
    // every ~200ms tick, which is what caused the visible flicker while a search was running.
    // Sorting is deferred to the final update so mid-search reordering doesn't force a full
    // repaint on every batch either.
    bool isContinuation = !m_liveSearchFirstUpdatePending && m_currentResults.size() >= previousCount;
    m_liveSearchFirstUpdatePending = false;

    if (complete) {
        ApplyCurrentSort();
        UpdateResultsListView();
        m_lastSearchDurationMs = GetTickCount() - m_liveSearchStartTick;
        RequestFolderSizesForResults();
    } else if (isContinuation) {
        AppendResultsToListView(previousCount);
    } else {
        UpdateResultsListView();
    }

    RequestEnrichmentForVisibleRows();
    UpdateStatusBar();
}

void MainWindow::UpdateResultsListView() {
    // Suppress intermediate repaints while the item count changes, then do exactly one clean
    // paint. Calling InvalidateRect with bErase=TRUE here (as a naive implementation would)
    // forces an extra erase+paint cycle on top of the one ListView_SetItemCountEx already
    // triggers, which is what caused the results to visibly flicker.
    SendMessageW(m_listView, WM_SETREDRAW, FALSE, 0);
    ListView_SetItemCountEx(m_listView, m_currentResults.size(), LVSICF_NOSCROLL);
    SendMessageW(m_listView, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(m_listView, nullptr, FALSE);
}

void MainWindow::AppendResultsToListView(size_t previousCount) {
    if (m_currentResults.size() == previousCount) return;
    // LVSICF_NOINVALIDATEALL: the rows already on screen keep their existing (still-valid)
    // content and are not repainted; only the newly added range needs to be drawn, which is
    // what makes streaming results in feel smooth instead of flickering on every batch.
    ListView_SetItemCountEx(m_listView, m_currentResults.size(), LVSICF_NOSCROLL | LVSICF_NOINVALIDATEALL);
}

void MainWindow::ApplyCurrentSort() {
    if (m_currentResults.empty()) {
        RebuildPathIndex();
        return;
    }

    // The fast NTFS USN scan method (the common non-elevated path) never captures size or
    // creation time - those are left unknown and normally only filled in lazily for visible
    // rows. Sorting on an unknown value compares as "equal" for every row, so sorting by one of
    // these columns silently did nothing after a USN-indexed search. Queue whatever's missing
    // for the whole result set for background enrichment so the sort reflects real values once
    // it arrives (see RequestMetadataForSort) - the sort below still runs immediately against
    // whatever's already known/cached, and re-runs as results stream in (OnTimer).
    if (m_sortColumn == 4 || m_sortColumn == 5 || m_sortColumn == 6) {
        RequestMetadataForSort();
    }

    int col = m_sortColumn;
    bool asc = m_sortAscending;

    auto cmp = [col, asc](const IndexedItem& a, const IndexedItem& b) -> bool {
        int result = 0;
        switch (col) {
            case 0: result = _wcsicmp(a.name.c_str(), b.name.c_str()); break;
            case 1: result = _wcsicmp(a.fullPath.c_str(), b.fullPath.c_str()); break;
            case 2: result = static_cast<int>(a.isDirectory) - static_cast<int>(b.isDirectory); break;
            case 3: result = _wcsicmp(a.extension.c_str(), b.extension.c_str()); break;
            case 4: {
                if (a.sizeKnown && b.sizeKnown) result = (a.size < b.size) ? -1 : (a.size > b.size ? 1 : 0);
                else result = static_cast<int>(a.sizeKnown) - static_cast<int>(b.sizeKnown);
                break;
            }
            case 5: {
                if (a.modifiedTimeKnown && b.modifiedTimeKnown) result = FileTimeUtil::Compare(a.modifiedTime, b.modifiedTime);
                else result = static_cast<int>(a.modifiedTimeKnown) - static_cast<int>(b.modifiedTimeKnown);
                break;
            }
            case 6: {
                if (a.createdTimeKnown && b.createdTimeKnown) result = FileTimeUtil::Compare(a.createdTime, b.createdTime);
                else result = static_cast<int>(a.createdTimeKnown) - static_cast<int>(b.createdTimeKnown);
                break;
            }
            case 7: result = static_cast<int>(a.driveLetter) - static_cast<int>(b.driveLetter); break;
            case 8: result = static_cast<int>(a.attributes) - static_cast<int>(b.attributes); break;
            case 9: result = _wcsicmp(a.source.c_str(), b.source.c_str()); break;
            default: result = 0; break;
        }
        return asc ? (result < 0) : (result > 0);
    };

    std::stable_sort(m_currentResults.begin(), m_currentResults.end(), cmp);
    RebuildPathIndex();
}

void MainWindow::RequestMetadataForSort() {
    if (!m_enricher) return;

    std::vector<std::wstring> paths;
    for (auto& item : m_currentResults) {
        if (item.isDirectory) continue;
        if (item.sizeKnown && item.modifiedTimeKnown && item.createdTimeKnown) continue;

        // Already resolved (by an earlier search or the visible-rows queue) - apply from cache
        // immediately instead of waiting on a round trip through the background worker.
        MetadataEnricher::EnrichedResult cached;
        if (m_enricher->TryGetCached(item.fullPath, cached) && cached.found) {
            if (!item.sizeKnown && cached.sizeKnown) { item.size = cached.size; item.sizeKnown = true; }
            if (!item.modifiedTimeKnown && cached.modifiedTimeKnown) {
                item.modifiedTime = cached.modifiedTime;
                item.modifiedTimeKnown = true;
            }
            if (!item.createdTimeKnown && cached.createdTimeKnown) {
                item.createdTime = cached.createdTime;
                item.createdTimeKnown = true;
            }
            continue;
        }

        paths.push_back(item.fullPath);
    }
    if (!paths.empty()) {
        m_enricher->RequestBulkEnrichment(std::move(paths));
    }
}

void MainWindow::RebuildPathIndex() {
    m_pathIndex.clear();
    m_pathIndex.reserve(m_currentResults.size());
    for (size_t i = 0; i < m_currentResults.size(); ++i) {
        m_pathIndex[m_currentResults[i].fullPath] = i;
    }
}

void MainWindow::SortResultsBy(int column) {
    if (column == m_sortColumn) {
        m_sortAscending = !m_sortAscending;
    } else {
        m_sortColumn = column;
        m_sortAscending = true;
    }
    ApplyCurrentSort();
    InvalidateRect(m_listView, nullptr, FALSE);
}

void MainWindow::RequestEnrichmentForVisibleRows() {
    if (!m_enricher) return;

    int top = ListView_GetTopIndex(m_listView);
    int perPage = ListView_GetCountPerPage(m_listView);
    int last = std::min(static_cast<int>(m_currentResults.size()), top + perPage + 5);

    std::vector<std::wstring> paths;
    for (int i = std::max(0, top); i < last; ++i) {
        const auto& item = m_currentResults[static_cast<size_t>(i)];
        // Directories never get a known size (they don't have one), so excluding the size
        // check for them avoids re-queuing every visible folder for enrichment on every
        // call. Without this, folders were perpetually "not yet enriched", which kept
        // re-triggering OnMetadataEnriched -> repaint on every live search tick, flickering
        // the results while a search was running.
        bool needsSize = !item.isDirectory && !item.sizeKnown;
        if (needsSize || !item.modifiedTimeKnown || !item.createdTimeKnown) {
            paths.push_back(item.fullPath);
        }
    }
    if (!paths.empty()) {
        m_enricher->RequestEnrichment(std::move(paths));
    }
}

void MainWindow::RequestFolderSizesForResults() {
    if (!m_folderSizeCalculator || !m_settings.computeFolderSizes) return;

    std::vector<std::wstring> toCompute;
    for (auto& item : m_currentResults) {
        if (!item.isDirectory || item.sizeKnown) continue;

        // Folders sized by an earlier search are already cached - fill those in immediately
        // instead of waiting for a round trip through the background worker.
        uint64_t cachedSize = 0;
        if (m_folderSizeCalculator->TryGetCached(item.fullPath, cachedSize)) {
            item.size = cachedSize;
            item.sizeKnown = true;
        } else {
            item.sizePending = true;
            toCompute.push_back(item.fullPath);
        }
    }

    // Always call this, even with an empty list: it cancels any still-running computation left
    // over from a previous search whose folders are no longer part of the current results.
    m_folderSizeCalculator->RequestFolderSizes(std::move(toCompute), m_settings.avoidReparsePoints);
}

std::unordered_map<std::wstring, uint64_t> MainWindow::LookupFolderSizesFromIndex(
    const std::vector<std::wstring>& folderPaths) const {
    std::unordered_map<std::wstring, uint64_t> resolved;
    if (folderPaths.empty()) return resolved;

    // Only Raw MFT indexing captures real file sizes (see RawMftIndexer vs NtfsUsnIndexer), and
    // only for drives currently marked as indexed - anything else falls straight through to the
    // disk-walk fallback. GetDriveStatuses() takes its own lock, so this (and ForEachItem below)
    // is safe to call from FolderSizeCalculator's background resolver thread.
    std::set<wchar_t> eligibleDrives;
    for (const auto& status : m_indexManager.GetDriveStatuses()) {
        if (status.indexed && status.scanMethod == ScanMethod::RawMft && !status.driveRoot.empty()) {
            eligibleDrives.insert(static_cast<wchar_t>(towupper(status.driveRoot[0])));
        }
    }
    if (eligibleDrives.empty()) return resolved;

    // Normalize each requested folder to a trailing-backslash prefix so "C:\Foo" doesn't wrongly
    // match "C:\FooBar\file.txt", while keeping a way back to the caller's original path string
    // for the result map's keys.
    std::unordered_set<std::wstring> prefixSet;
    std::unordered_map<std::wstring, std::wstring> normalizedToOriginal;
    prefixSet.reserve(folderPaths.size());
    for (const auto& p : folderPaths) {
        if (p.empty() || !eligibleDrives.count(static_cast<wchar_t>(towupper(p[0])))) continue;
        std::wstring prefix = p;
        if (prefix.back() != L'\\') prefix += L'\\';
        prefixSet.insert(prefix);
        normalizedToOriginal[prefix] = p;
    }
    if (prefixSet.empty()) return resolved;

    // Single pass over the whole index: for every indexed file, walk up its own ancestor chain
    // checking each level against the requested folder set, rather than checking every file
    // against every requested folder. A folder only ends up in 'sums' (and stays out of
    // 'incomplete') if every file found under it has a known size - see the header comment on
    // why that's required before trusting an index-derived total.
    std::unordered_map<std::wstring, uint64_t> sums;
    std::unordered_set<std::wstring> incomplete;

    m_indexManager.ForEachItem([&](const IndexedItem& item) {
        if (item.isDirectory) return;

        std::wstring_view path = item.fullPath;
        size_t pos = path.rfind(L'\\');
        while (pos != std::wstring_view::npos) {
            std::wstring dir(path.substr(0, pos + 1));
            if (prefixSet.count(dir)) {
                if (item.sizeKnown) {
                    sums[dir] += item.size;
                } else {
                    incomplete.insert(dir);
                }
            }
            if (pos == 0) break;
            pos = path.rfind(L'\\', pos - 1);
        }
    });

    for (const auto& kv : normalizedToOriginal) {
        const std::wstring& prefix = kv.first;
        if (incomplete.count(prefix)) continue;
        auto it = sums.find(prefix);
        if (it == sums.end()) continue; // nothing found under it: unindexed or genuinely unknown - fall back to walk
        resolved[kv.second] = it->second;
    }
    return resolved;
}

void MainWindow::OnDriveStatusChanged(wchar_t) {
    UpdateStatusBar();
}

void MainWindow::OnIndexingProgress(wchar_t, uint64_t) {
    UpdateStatusBar();
}

void MainWindow::OnIndexingComplete() {
    PopulateDriveFilterCombo();
    UpdateStatusBar();
    if (GetWindowTextLengthW(m_editSearch) > 0) {
        RunSearch();
    }

    std::vector<std::wstring> newlyStale;
    for (const auto& drive : ComputeStaleDrives()) {
        if (m_warnedStaleDrives.insert(drive).second) {
            newlyStale.push_back(drive);
        }
    }
    if (!newlyStale.empty()) {
        std::wstring msg = L"The index for the following drive(s) is older than " +
            std::to_wstring(m_settings.indexStalenessWarningDays) +
            L" day(s) and may be out of date:\n\n";
        for (const auto& drive : newlyStale) msg += drive + L"\n";
        msg += L"\nUse Index > Refresh Index to update it.";
        ShowFriendlyMessage(msg, MB_ICONWARNING);
    }

    // Deliberately no automatic elevation popup here: OnIndexingComplete already refreshes and
    // repaints search results just above, and a blocking MessageBox fired right after that made
    // it look like results appeared and *then* an unrelated warning interrupted - confusing,
    // since the results shown are actually from the fallback method the warning is about. The
    // status bar's Method segment (see ComputeSearchMethodLabel) already shows the downgrade
    // reason continuously; "Index > Run as Administrator..." lets the user act on it when ready.
}

void MainWindow::ActionRunAsAdministrator() {
    if (ShellHelpers::IsProcessElevated()) {
        ShowFriendlyMessage(L"Instant File Finder is already running as Administrator.");
        return;
    }

    int choice = MessageBoxW(m_hwnd,
        L"Relaunch Instant File Finder as Administrator?\n\n"
        L"This is only needed for Raw MFT parsing (the fastest indexing method) and, "
        L"occasionally, NTFS USN scanning. Everything else in the app - including live search "
        L"and recursive/USN indexing - works the same without it.",
        L"Run as Administrator", MB_YESNO | MB_ICONQUESTION);
    if (choice != IDYES) return;

    if (ShellHelpers::RelaunchElevated(m_hwnd)) {
        DestroyWindow(m_hwnd); // hand off to the elevated instance and close this one
    } else {
        ShowFriendlyMessage(L"Could not relaunch as Administrator.", MB_ICONWARNING);
    }
}

std::vector<std::wstring> MainWindow::ComputeStaleDrives() const {
    std::vector<std::wstring> stale;
    if (m_settings.indexStalenessWarningDays == 0) return stale;

    FILETIME now = FileTimeUtil::Now();
    ULARGE_INTEGER nowLarge{};
    nowLarge.LowPart = now.dwLowDateTime;
    nowLarge.HighPart = now.dwHighDateTime;

    const uint64_t ticksPerDay = 10000000ULL * 60ULL * 60ULL * 24ULL;

    for (const auto& d : m_indexManager.GetDriveStatuses()) {
        if (!d.indexed || !d.lastIndexedTimeKnown) continue;

        ULARGE_INTEGER indexedLarge{};
        indexedLarge.LowPart = d.lastIndexedTime.dwLowDateTime;
        indexedLarge.HighPart = d.lastIndexedTime.dwHighDateTime;
        if (nowLarge.QuadPart <= indexedLarge.QuadPart) continue;

        uint64_t daysElapsed = (nowLarge.QuadPart - indexedLarge.QuadPart) / ticksPerDay;
        if (daysElapsed >= m_settings.indexStalenessWarningDays) {
            stale.push_back(d.driveRoot);
        }
    }
    return stale;
}

void MainWindow::OnMetadataEnriched(MetadataEnricher::EnrichedResult* resultPtr) {
    std::unique_ptr<MetadataEnricher::EnrichedResult> result(resultPtr);
    if (!result->found) return;

    // Looked up via m_pathIndex (O(1)) rather than a linear scan: this handler can now fire once
    // per row for an entire large result set (RequestMetadataForSort's bulk queue), and a linear
    // scan per arrival would turn that into an O(n^2) sweep of the UI thread.
    bool changed = false;
    auto it = m_pathIndex.find(result->fullPath);
    if (it != m_pathIndex.end() && it->second < m_currentResults.size()) {
        auto& item = m_currentResults[it->second];
        if (item.fullPath == result->fullPath) {
            if (result->sizeKnown) { item.size = result->size; item.sizeKnown = true; }
            if (result->createdTimeKnown) { item.createdTime = result->createdTime; item.createdTimeKnown = true; }
            if (result->modifiedTimeKnown) { item.modifiedTime = result->modifiedTime; item.modifiedTimeKnown = true; }
            item.attributes = result->attributes;
            changed = true;
        }
    }
    // Coalesce many rapid enrichment arrivals (one per visible row) into a single repaint a
    // short moment later instead of invalidating the list view on every individual result,
    // which is what caused the results to visibly flicker as metadata filled in.
    if (changed && !m_enrichRepaintPending) {
        m_enrichRepaintPending = true;
        SetTimer(m_hwnd, IDT_ENRICH_REPAINT, 60, nullptr);
    }
}

void MainWindow::OnFolderSizeComputed(FolderSizeCalculator::ComputedResult* resultPtr) {
    std::unique_ptr<FolderSizeCalculator::ComputedResult> result(resultPtr);

    // See OnMetadataEnriched for why this uses m_pathIndex instead of a linear scan: folder
    // sizes are requested for every folder in the result set, not just visible ones.
    bool changed = false;
    auto it = m_pathIndex.find(result->folderPath);
    if (it != m_pathIndex.end() && it->second < m_currentResults.size()) {
        auto& item = m_currentResults[it->second];
        if (item.isDirectory && item.fullPath == result->folderPath) {
            item.size = result->size;
            item.sizeKnown = true;
            item.sizePending = false;
            changed = true;
        }
    }
    // Reuse the same coalesced-repaint timer as metadata enrichment: folder sizes can trickle in
    // for a while after a search completes, and batching those into one repaint (with a resort
    // if currently sorted by size) avoids the same per-item flicker fixed for file metadata.
    if (changed && !m_enrichRepaintPending) {
        m_enrichRepaintPending = true;
        SetTimer(m_hwnd, IDT_ENRICH_REPAINT, 60, nullptr);
    }
}

void MainWindow::UpdateIndexingControls() {
    bool indexing = m_indexManager.IsIndexing();

    // Grey the actions that would start a second scan while one is running (the Refresh button and
    // its menu equivalents), and enable Cancel only while there's actually something to cancel.
    EnableWindow(m_btnRefresh, indexing ? FALSE : TRUE);
    UINT startFlags = MF_BYCOMMAND | (indexing ? MF_GRAYED : MF_ENABLED);
    EnableMenuItem(m_menu, ID_INDEX_DRIVES, startFlags);
    EnableMenuItem(m_menu, ID_INDEX_REFRESH, startFlags);
    EnableMenuItem(m_menu, ID_INDEX_REBUILD, startFlags);
    EnableMenuItem(m_menu, ID_INDEX_CANCEL, MF_BYCOMMAND | (indexing ? MF_ENABLED : MF_GRAYED));
}

void MainWindow::UpdateStatusBar(const wchar_t* extraMessage) {
    UpdateIndexingControls();

    auto drives = m_indexManager.GetDriveStatuses();
    bool anyIndexing = m_indexManager.IsIndexing();
    bool liveSearching = !m_settings.enableIndexing && m_liveSearchManager.IsSearching();
    bool indexedSearching = m_settings.enableIndexing && m_indexedSearchWorker.IsSearching();
    std::wstring currentDrive;
    std::wstring currentPhase;
    uint64_t totalWarnings = 0;
    for (const auto& d : drives) {
        if (d.indexing && currentDrive.empty()) {
            currentDrive = d.driveRoot;
            currentPhase = d.statusMessage;
        }
        totalWarnings += d.errors.size();
    }

    // While a drive is being indexed, its statusMessage names the actual phase (loading the
    // cache, replaying journal changes, scanning); saying just "Indexing" during a warm-start
    // cache load misled users into thinking a full scan was running.
    std::wstring phaseLabel = L"Indexing";
    if (!currentPhase.empty() && currentPhase != L"Indexing...") {
        phaseLabel = currentPhase;
        while (!phaseLabel.empty() && phaseLabel.back() == L'.') phaseLabel.pop_back();
    }

    std::wstring part0 = anyIndexing ? (phaseLabel + (currentDrive.empty() ? std::wstring() : L" " + currentDrive))
                          : (liveSearching || indexedSearching) ? L"Searching..."
                          : (m_settings.enableIndexing ? L"Ready" : L"Ready (live search, no index)");
    std::wstring part1 = L"Indexed items: " + std::to_wstring(m_indexManager.TotalIndexedItemCount());

    std::wstring part2;
    if (extraMessage != nullptr) {
        part2 = extraMessage;
    } else if (!m_lastStatusOverride.empty()) {
        part2 = m_lastStatusOverride;
    } else {
        part2 = L"Results: " + std::to_wstring(m_currentResults.size());
        if (m_lastSearchTruncated) {
            if (m_lastSearchTotalMatched > m_currentResults.size()) {
                part2 += L" (showing first " + std::to_wstring(m_currentResults.size()) +
                         L" of " + std::to_wstring(m_lastSearchTotalMatched) + L" - refine your search)";
            } else {
                part2 += L" (more results may exist - refine your search to narrow results)";
            }
        }
    }

    std::wstring part3 = L"Search time: " + std::to_wstring(m_lastSearchDurationMs) + L" ms";
    std::wstring part4 = currentDrive.empty() ? L"" : (L"Current drive: " + currentDrive);
    std::wstring part5 = ComputeSearchMethodLabel(drives);
    std::wstring part6 = L"Warnings: " + std::to_wstring(totalWarnings);

    const std::wstring* parts[7] = { &part0, &part1, &part2, &part3, &part4, &part5, &part6 };
    for (int i = 0; i < 7; ++i) {
        SendMessageW(m_statusBar, SB_SETTEXTW, i, reinterpret_cast<LPARAM>(parts[i]->c_str()));
    }
}

std::wstring MainWindow::ComputeSearchMethodLabel(const std::vector<DriveIndexStatus>& drives) const {
    if (!m_settings.enableIndexing) {
        return L"Method: Live scan (no index)";
    }

    // Narrow to whichever drive(s) the current search filter actually covers, so this reflects
    // what's really being searched rather than every indexed drive regardless of relevance.
    wchar_t filterLetter = L'\0';
    int driveIdx = static_cast<int>(SendMessageW(m_comboDrive, CB_GETCURSEL, 0, 0));
    if (driveIdx > 0) {
        wchar_t buf[16]{};
        SendMessageW(m_comboDrive, CB_GETLBTEXT, driveIdx, reinterpret_cast<LPARAM>(buf));
        if (buf[0] != L'\0') filterLetter = static_cast<wchar_t>(towupper(buf[0]));
    }

    std::wstring commonMethod;
    std::wstring downgradeNote;
    bool mixed = false;
    bool any = false;

    for (const auto& d : drives) {
        if (!d.selected || d.driveRoot.empty() || d.indexingMethod.empty()) continue;
        if (filterLetter != L'\0' && towupper(d.driveRoot[0]) != filterLetter) continue;

        any = true;
        if (commonMethod.empty()) {
            commonMethod = d.indexingMethod;
        } else if (commonMethod != d.indexingMethod) {
            mixed = true;
        }
        if (downgradeNote.empty() && !d.downgradeReason.empty()) {
            downgradeNote = d.downgradeReason;
        }
    }

    if (!any) return L"Method: (not indexed yet)";

    std::wstring label = L"Method: " + (mixed ? L"Mixed" : commonMethod);
    if (!downgradeNote.empty()) {
        label += L" - " + downgradeNote;
    }
    return label;
}

int MainWindow::GetSelectedResultIndex() const {
    int idx = ListView_GetNextItem(m_listView, -1, LVNI_SELECTED);
    if (idx < 0 || idx >= static_cast<int>(m_currentResults.size())) return -1;
    return idx;
}

std::wstring MainWindow::FormatCell(const IndexedItem& item, int column) const {
    switch (column) {
        case 0: return item.name;
        case 1: return item.fullPath;
        case 2: return item.isDirectory ? L"Folder" : L"File";
        case 3: return item.extension;
        case 4:
            if (!item.sizeKnown && item.sizePending) return L"Calculating...";
            return FormatUtil::FormatSize(item.size, item.sizeKnown);
        case 5: return FileTimeUtil::Format(item.modifiedTime, item.modifiedTimeKnown);
        case 6: return FileTimeUtil::Format(item.createdTime, item.createdTimeKnown);
        case 7: return std::wstring(1, item.driveLetter) + L":";
        case 8: return FormatUtil::FormatAttributes(item.attributes);
        case 9: return item.source;
        default: return L"";
    }
}

void MainWindow::ShowFriendlyMessage(const std::wstring& message, UINT iconFlags) const {
    MessageBoxW(m_hwnd, message.c_str(), L"Instant File Finder", MB_OK | iconFlags);
}

void MainWindow::ActionOpenSelected() {
    int idx = GetSelectedResultIndex();
    if (idx < 0) {
        ShowFriendlyMessage(L"Select a result first.");
        return;
    }
    auto result = ShellHelpers::OpenPath(m_hwnd, m_currentResults[static_cast<size_t>(idx)].fullPath);
    if (!result.success) {
        ShowFriendlyMessage(result.message, MB_ICONWARNING);
    }
}

void MainWindow::ActionOpenContainingFolder() {
    int idx = GetSelectedResultIndex();
    if (idx < 0) {
        ShowFriendlyMessage(L"Select a result first.");
        return;
    }
    const auto& item = m_currentResults[static_cast<size_t>(idx)];
    auto result = ShellHelpers::OpenContainingFolder(m_hwnd, item.fullPath, item.isDirectory);
    if (!result.success) {
        ShowFriendlyMessage(result.message, MB_ICONWARNING);
    }
}

void MainWindow::ActionCopySelectedPath() {
    int idx = GetSelectedResultIndex();
    if (idx < 0) {
        ShowFriendlyMessage(L"Select a result first.");
        return;
    }
    Clipboard::CopyText(m_hwnd, m_currentResults[static_cast<size_t>(idx)].fullPath);
}

void MainWindow::ActionDeleteSelected() {
    int idx = GetSelectedResultIndex();
    if (idx < 0) {
        ShowFriendlyMessage(L"Select a result first.");
        return;
    }

    const IndexedItem item = m_currentResults[static_cast<size_t>(idx)]; // copy: erased below
    std::wstring kind = item.isDirectory ? L"folder" : L"file";
    std::wstring prompt = L"Move this " + kind + L" to the Recycle Bin?\n\n" + item.fullPath;
    if (item.isDirectory) {
        prompt += L"\n\nThis deletes the folder and everything inside it.";
    }

    int choice = MessageBoxW(m_hwnd, prompt.c_str(), L"Delete", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    if (choice != IDYES) return;

    auto result = ShellHelpers::DeleteToRecycleBin(m_hwnd, item.fullPath);
    if (!result.success) {
        ShowFriendlyMessage(result.message, MB_ICONWARNING);
        return;
    }

    m_currentResults.erase(m_currentResults.begin() + idx);
    RebuildPathIndex();
    UpdateResultsListView();
    UpdateStatusBar();
}

void MainWindow::ActionCopyResults() {
    if (m_currentResults.empty()) {
        ShowFriendlyMessage(L"There are no results to copy.");
        return;
    }

    std::wstringstream ss;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t timeBuf[64];
    swprintf_s(timeBuf, L"%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    ss << L"Instant File Finder - Search Results\r\n";
    ss << L"Query: " << m_lastSearchQuery << L"\r\n";
    ss << L"Generated: " << timeBuf << L"\r\n\r\n";

    for (int i = 0; i < kColumnCount; ++i) {
        ss << kColumnHeaders[i] << (i < kColumnCount - 1 ? L"\t" : L"\r\n");
    }
    for (const auto& item : m_currentResults) {
        for (int col = 0; col < kColumnCount; ++col) {
            ss << FormatCell(item, col) << (col < kColumnCount - 1 ? L"\t" : L"\r\n");
        }
    }

    Clipboard::CopyText(m_hwnd, ss.str());
    ShowFriendlyMessage(L"Copied " + std::to_wstring(m_currentResults.size()) + L" result(s) to the clipboard.");
}

void MainWindow::ActionClearSearch() {
    SetWindowTextW(m_editSearch, L"");
    RunSearch();
    SetFocus(m_editSearch);
}

void MainWindow::ActionIndexDrives() {
    if (m_indexManager.IsIndexing()) {
        ShowFriendlyMessage(L"Indexing is already in progress. Cancel it first if you want to change drive selection.");
        return;
    }

    if (IndexDrivesDialog::Show(m_hwnd, m_instance, m_indexManager)) {
        std::vector<wchar_t> letters;
        bool settingsChanged = false;
        for (const auto& d : m_indexManager.GetDriveStatuses()) {
            if (!d.selected || d.driveRoot.empty()) continue;
            letters.push_back(d.driveRoot[0]);

            // Manually picking a tier for a drive here should actually try that tier, not
            // silently no-op because a separate Preferences checkbox happens to still be off -
            // that mismatch was confusing (the drive would just fall back with no clear reason).
            if (d.scanMethod == ScanMethod::RawMft && !m_settings.useRawMftScan) {
                m_settings.useRawMftScan = true;
                settingsChanged = true;
            } else if (d.scanMethod == ScanMethod::FastNtfsUsn && !m_settings.useFastNtfsUsnScan) {
                m_settings.useFastNtfsUsnScan = true;
                settingsChanged = true;
            }
        }
        PopulateDriveFilterCombo();
        if (!letters.empty()) {
            EnableIndexingIfNeeded();
            if (settingsChanged) {
                SettingsService::Save(m_settings);
            }
            m_indexManager.StartIndexing(letters, /*forceRebuild=*/false, BuildIndexOptions());
            UpdateStatusBar(); // reflect "indexing" and grey the index actions right away
        }
    }
}

void MainWindow::ActionRefreshIndex() {
    if (m_indexManager.IsIndexing()) {
        ShowFriendlyMessage(L"Indexing is already in progress. Wait for it to finish, or use Index > Cancel Indexing first.");
        return;
    }

    std::vector<wchar_t> letters;
    for (const auto& d : m_indexManager.GetDriveStatuses()) {
        if (d.selected && !d.driveRoot.empty()) letters.push_back(d.driveRoot[0]);
    }
    if (letters.empty()) {
        ShowFriendlyMessage(L"No drives are selected. Use Index > Index Drives... to choose drives first.");
        return;
    }
    EnableIndexingIfNeeded();
    m_indexManager.StartIndexing(letters, /*forceRebuild=*/false, BuildIndexOptions());
    UpdateStatusBar(); // reflect "indexing" and grey the index actions right away
}

void MainWindow::ActionRebuildIndex() {
    if (m_indexManager.IsIndexing()) {
        ShowFriendlyMessage(L"Indexing is already in progress. Wait for it to finish, or use Index > Cancel Indexing first.");
        return;
    }

    std::vector<wchar_t> letters;
    for (const auto& d : m_indexManager.GetDriveStatuses()) {
        if (d.selected && !d.driveRoot.empty()) letters.push_back(d.driveRoot[0]);
    }
    if (letters.empty()) {
        ShowFriendlyMessage(L"No drives are selected. Use Index > Index Drives... to choose drives first.");
        return;
    }
    EnableIndexingIfNeeded();
    m_indexManager.RebuildDrives(letters, BuildIndexOptions());
    UpdateStatusBar(); // reflect "indexing" and grey the index actions right away
}

void MainWindow::ActionCancelIndexing() {
    m_indexManager.CancelIndexing();
}

void MainWindow::ActionClearCache() {
    int choice = MessageBoxW(m_hwnd,
        L"This clears the on-disk index cache and the in-memory index for every drive. "
        L"You will need to re-index before searching again. Continue?",
        L"Clear Index Cache", MB_YESNO | MB_ICONQUESTION);
    if (choice != IDYES) return;

    m_indexManager.ClearAllCaches();
    m_warnedStaleDrives.clear();
    m_currentResults.clear();
    RebuildPathIndex();
    UpdateResultsListView();
    UpdateStatusBar();
}

void MainWindow::ActionShowIndexDetails() {
    auto drives = m_indexManager.GetDriveStatuses();

    // Staleness is judged against the same threshold the post-indexing warning uses (see
    // ComputeStaleDrives), so this dialog and that warning never disagree about what "old" means.
    FILETIME now = FileTimeUtil::Now();
    ULARGE_INTEGER nowLarge{};
    nowLarge.LowPart = now.dwLowDateTime;
    nowLarge.HighPart = now.dwHighDateTime;
    const uint64_t ticksPerDay = 10000000ULL * 60ULL * 60ULL * 24ULL;

    std::wstring msg;
    bool any = false;
    for (const auto& d : drives) {
        if (d.driveRoot.empty()) continue;
        // Only report on drives the user actually cares about: selected for indexing, still
        // holding a searchable index, or carrying leftover warnings from a previous attempt
        // even if since deselected.
        if (!d.selected && !d.indexed && d.errors.empty()) continue;

        any = true;
        msg += d.driveRoot;
        if (!d.label.empty()) msg += L" \"" + d.label + L"\"";
        if (!d.fileSystem.empty()) msg += L" (" + d.fileSystem + L")";
        msg += L"\r\n";

        msg += L"    Method: " + (d.indexingMethod.empty() ? L"(not indexed yet)" : d.indexingMethod) + L"\r\n";

        std::wstring statusText = !d.statusMessage.empty() ? d.statusMessage
                                   : d.indexing ? L"Indexing..."
                                   : d.indexed ? L"Indexed" : L"Not indexed";
        msg += L"    Status: " + statusText + L"\r\n";

        if (d.indexed || d.itemCount > 0) {
            msg += L"    Items: " + std::to_wstring(d.itemCount) + L"\r\n";
        }

        if (d.lastIndexedTimeKnown) {
            msg += L"    Last indexed: " + FileTimeUtil::Format(d.lastIndexedTime, true);
            std::wstring age = FileTimeUtil::FormatAge(d.lastIndexedTime, true);
            if (!age.empty()) msg += L" (" + age + L")";

            if (m_settings.indexStalenessWarningDays > 0) {
                ULARGE_INTEGER indexedLarge{};
                indexedLarge.LowPart = d.lastIndexedTime.dwLowDateTime;
                indexedLarge.HighPart = d.lastIndexedTime.dwHighDateTime;
                if (nowLarge.QuadPart > indexedLarge.QuadPart &&
                    (nowLarge.QuadPart - indexedLarge.QuadPart) / ticksPerDay >= m_settings.indexStalenessWarningDays) {
                    msg += L" - STALE, older than " + std::to_wstring(m_settings.indexStalenessWarningDays) +
                           L" day(s); use Index > Refresh Index";
                }
            }
            msg += L"\r\n";
        } else if (d.indexed) {
            msg += L"    Last indexed: unknown\r\n";
        }

        if (d.indexed) {
            msg += d.hasUsnJournal
                ? L"    Refresh: incremental (USN journal available)\r\n"
                : L"    Refresh: full rescan (no USN journal for this index)\r\n";
        }

        if (!d.downgradeReason.empty()) {
            msg += L"    " + d.downgradeReason + L"\r\n";
        }
        for (const auto& err : d.errors) {
            msg += L"    - " + err + L"\r\n";
        }
        msg += L"\r\n";
    }

    if (!any) {
        msg = L"Nothing is indexed and no drives are selected for indexing.\r\n"
              L"Use Index > Index Drives... to choose drives.";
    } else {
        msg += L"Total indexed items: " + std::to_wstring(m_indexManager.TotalIndexedItemCount()) + L"\r\n";
    }

    bool anyNeedsElevation = false;
    for (const auto& d : drives) {
        if (d.requiresElevation) { anyNeedsElevation = true; break; }
    }
    if (anyNeedsElevation && !ShellHelpers::IsProcessElevated()) {
        msg += L"Tip: use Index > Run as Administrator... to enable the methods above that need it.\r\n";
    }

    MessageBoxW(m_hwnd, msg.c_str(), L"Index Details", MB_OK | MB_ICONINFORMATION);
}

void MainWindow::ActionPreferences() {
    AppSettings edited = m_settings;
    if (PreferencesDialog::Show(m_hwnd, m_instance, edited)) {
        bool indexingWasEnabled = m_settings.enableIndexing;
        m_settings = edited;

        // Checking a specific scan-method box clearly signals the user wants indexing to run,
        // even if they didn't separately check "Enable indexing" above it - without this,
        // turning on e.g. "Use raw MFT parsing" alone would silently do nothing, since search
        // would still be running live with no index to use that method against.
        if (!m_settings.enableIndexing &&
            (m_settings.useRawMftScan || m_settings.useFastNtfsUsnScan || m_settings.autoIndexFixedNtfsDrives)) {
            m_settings.enableIndexing = true;
        }

        SettingsService::Save(m_settings);
        ApplySettingsToOptions();

        SetWindowPos(m_hwnd, m_settings.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        CheckMenuItem(m_menu, ID_OPTIONS_ALWAYS_ON_TOP, MF_BYCOMMAND | (m_settings.alwaysOnTop ? MF_CHECKED : MF_UNCHECKED));

        if (m_settings.enableIndexing && !indexingWasEnabled) {
            // The user just turned indexing on: kick off the initial auto-index now, same as
            // startup would have done had it already been enabled.
            StartInitialIndexingIfConfigured();
        } else if (!m_settings.enableIndexing && indexingWasEnabled) {
            // Turned off: stop any in-progress scan. The on-disk cache/in-memory index (if any)
            // is left alone, but search now runs live instead of consulting it.
            m_indexManager.CancelIndexing();
        }

        RunSearch();
    }
}

void MainWindow::ActionSearchOptions() {
    if (SearchOptionsDialog::Show(m_hwnd, m_instance, m_persistentSearchOptions)) {
        RunSearch();
    }
}

void MainWindow::ActionAbout() {
    AboutDialog::Show(m_hwnd, m_instance);
}

void MainWindow::ActionToggleAlwaysOnTop() {
    m_settings.alwaysOnTop = !m_settings.alwaysOnTop;
    SetWindowPos(m_hwnd, m_settings.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    CheckMenuItem(m_menu, ID_OPTIONS_ALWAYS_ON_TOP, MF_BYCOMMAND | (m_settings.alwaysOnTop ? MF_CHECKED : MF_UNCHECKED));
    SettingsService::Save(m_settings);
}
