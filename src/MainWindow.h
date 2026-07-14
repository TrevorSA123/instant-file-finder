#pragma once
// Owns the main HWND: menu, controls, WndProc, and coordination between IndexManager,
// SearchEngine, LiveSearchManager, and MetadataEnricher. This is the only class that touches UI
// controls; all worker threads communicate with it exclusively through PostMessage (see
// AppMessages.h).

#include "IndexManager.h"
#include "LiveSearchManager.h"
#include "IndexedSearchWorker.h"
#include "MetadataEnricher.h"
#include "FolderSizeCalculator.h"
#include "SettingsService.h"
#include "IndexTypes.h"
#include "SearchEngine.h"
#include "DpiUtil.h"
#include "Handle.h"

#include <windows.h>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
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
    void OnFolderSizeComputed(FolderSizeCalculator::ComputedResult* result);
    void OnLiveSearchUpdate(bool complete);
    void OnIndexedSearchComplete(IndexedSearchWorker::CompletedSearch* result);

    void CreateMenus();
    void CreateControls();
    void LayoutControls();
    void ApplyFont(HWND h);
    void ApplyFontToAllControls();
    void RescaleListViewColumns();
    void PopulateDriveFilterCombo();
    void UpdateStatusBar(const wchar_t* extraMessage = nullptr);
    void UpdateIndexingControls(); // greys out index actions while a scan is running
    std::wstring ComputeSearchMethodLabel(const std::vector<DriveIndexStatus>& drives) const;

    void RunSearch();
    void RunIndexedSearch(const SearchOptions& parsed);
    void RunLiveSearch(const SearchOptions& parsed);
    std::vector<std::wstring> GetLiveSearchRootPaths(const SearchOptions& parsed) const;
    void ScheduleDebouncedSearch();
    void RequestEnrichmentForVisibleRows();
    void RequestFolderSizesForResults();
    void SortResultsBy(int column);
    void ApplyCurrentSort();
    void RequestMetadataForSort();
    void RebuildPathIndex();
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

    // FolderSizeCalculator's index fast path: given a batch of folder paths, resolves whatever
    // subset can be fully summed from the in-memory index without touching disk. Runs on
    // FolderSizeCalculator's background resolver thread (not the UI thread), so this only touches
    // IndexManager, which is safe to call from any thread (see IndexManager::ForEachItem) - it
    // must not read m_settings or any other UI-thread-owned state.
    std::unordered_map<std::wstring, uint64_t> LookupFolderSizesFromIndex(
        const std::vector<std::wstring>& folderPaths) const;

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
    IndexedSearchWorker m_indexedSearchWorker;
    std::unique_ptr<MetadataEnricher> m_enricher;
    std::unique_ptr<FolderSizeCalculator> m_folderSizeCalculator;
    AppSettings m_settings;
    SearchOptions m_persistentSearchOptions;

    std::vector<IndexedItem> m_currentResults;
    // Maps fullPath -> index into m_currentResults, kept in sync with every mutation of that
    // vector (see RebuildPathIndex). Lets background-result handlers (OnMetadataEnriched,
    // OnFolderSizeComputed) find the row to update in O(1) instead of a linear scan - important
    // once those can stream back one message per row for an entire large result set rather than
    // just the visible ones.
    std::unordered_map<std::wstring, size_t> m_pathIndex;
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
