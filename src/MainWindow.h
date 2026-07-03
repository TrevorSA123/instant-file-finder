#pragma once
// Owns the main HWND: menu, controls, WndProc, and coordination between IndexManager,
// SearchEngine, LiveSearchManager, and MetadataEnricher. This is the only class that touches UI
// controls; all worker threads communicate with it exclusively through PostMessage (see
// AppMessages.h).

#include "IndexManager.h"
#include "LiveSearchManager.h"
#include "MetadataEnricher.h"
#include "SettingsService.h"
#include "IndexTypes.h"
#include "SearchEngine.h"
#include "DpiUtil.h"
#include "Handle.h"

#include <windows.h>
#include <memory>
#include <set>
#include <string>
#include <vector>

class MainWindow {
public:
    MainWindow();
    ~MainWindow();

    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    bool Create(HINSTANCE instance, int showCmd);
    HWND Handle() const { return m_hwnd; }

    // Called by the search-box subclass procedure when the user presses Enter.
    void OnSearchEnterPressed();

private:
    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void OnCreate(HWND hwnd);
    void OnDestroy();
    void OnSize(int width, int height);
    void OnCommand(WPARAM wParam, LPARAM lParam);
    LRESULT OnNotify(LPARAM lParam);
    void OnTimer(WPARAM id);
    void OnContextMenu(HWND source, int screenX, int screenY);
    void OnDpiChanged(int newDpi, const RECT* suggestedRect);

    void OnDriveStatusChanged(wchar_t letter);
    void OnIndexingProgress(wchar_t letter, uint64_t count);
    void OnIndexingComplete();
    void OnMetadataEnriched(MetadataEnricher::EnrichedResult* result);
    void OnLiveSearchUpdate(bool complete);

    void CreateMenus();
    void CreateControls();
    void LayoutControls();
    void ApplyFont(HWND h);
    void ApplyFontToAllControls();
    void RescaleListViewColumns();
    void PopulateDriveFilterCombo();
    void UpdateStatusBar(const wchar_t* extraMessage = nullptr);
    std::wstring ComputeSearchMethodLabel(const std::vector<DriveIndexStatus>& drives) const;

    void RunSearch();
    void RunIndexedSearch(const SearchOptions& parsed);
    void RunLiveSearch(const SearchOptions& parsed);
    std::vector<std::wstring> GetLiveSearchRootPaths(const SearchOptions& parsed) const;
    void ScheduleDebouncedSearch();
    void RequestEnrichmentForVisibleRows();
    void SortResultsBy(int column);
    void ApplyCurrentSort();
    void UpdateResultsListView();
    void AppendResultsToListView(size_t previousCount);
    IndexManager::IndexOptions BuildIndexOptions() const;
    void EnableIndexingIfNeeded();
    std::vector<std::wstring> ComputeStaleDrives() const;

    void ActionOpenSelected();
    void ActionOpenContainingFolder();
    void ActionCopySelectedPath();
    void ActionCopyResults();
    void ActionDeleteSelected();
    void ActionClearSearch();
    void ActionIndexDrives();
    void ActionRefreshIndex();
    void ActionRebuildIndex();
    void ActionCancelIndexing();
    void ActionClearCache();
    void ActionShowIndexDetails();
    void ActionRunAsAdministrator();
    void ActionPreferences();
    void ActionSearchOptions();
    void ActionAbout();
    void ActionToggleAlwaysOnTop();

    void ApplySettingsToOptions();
    void StartInitialIndexingIfConfigured();
    void ShowFriendlyMessage(const std::wstring& message, UINT iconFlags = MB_ICONINFORMATION) const;

    int GetSelectedResultIndex() const;
    std::wstring FormatCell(const IndexedItem& item, int column) const;

    HWND m_hwnd = nullptr;
    HWND m_editSearch = nullptr;
    HWND m_comboMode = nullptr;
    HWND m_comboDrive = nullptr;
    HWND m_comboType = nullptr;
    HWND m_btnSearch = nullptr;
    HWND m_btnClear = nullptr;
    HWND m_btnRefresh = nullptr;
    HWND m_listView = nullptr;
    HWND m_statusBar = nullptr;

    HINSTANCE m_instance = nullptr;
    HMENU m_menu = nullptr;

    // Manual per-monitor DPI scaling: see DpiUtil.h for why this is needed instead of relying on
    // GetStockObject(DEFAULT_GUI_FONT) and design-time pixel constants.
    int m_dpi = DpiUtil::kDefaultDpi;
    UniqueGdiObject<HFONT> m_uiFont;

    IndexManager m_indexManager;
    LiveSearchManager m_liveSearchManager;
    std::unique_ptr<MetadataEnricher> m_enricher;
    AppSettings m_settings;
    SearchOptions m_persistentSearchOptions;

    std::vector<IndexedItem> m_currentResults;
    bool m_lastSearchTruncated = false;
    uint64_t m_lastSearchTotalMatched = 0;
    DWORD m_lastSearchDurationMs = 0;
    DWORD m_liveSearchStartTick = 0;
    bool m_liveSearchFirstUpdatePending = false;
    std::wstring m_lastSearchQuery;
    std::wstring m_lastStatusOverride;

    int m_sortColumn = 0;
    bool m_sortAscending = true;

    bool m_indexDrivesOnStartupDone = false;
    std::set<std::wstring> m_warnedStaleDrives;

    // Batches WM_APP_METADATA_ENRICHED-driven repaints onto a single timer tick instead of
    // invalidating the list view once per file, which is what caused visible flicker.
    bool m_enrichRepaintPending = false;
};
