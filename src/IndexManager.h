#pragma once
// Coordinates indexing across drives, owns the in-memory index, and runs indexing on a
// background worker thread. UI code talks to this class and never touches the index storage
// directly; search reads go through ForEachItem(), which takes a shared lock internally.

#include "IndexTypes.h"
#include "PathReconstructor.h"

#include <windows.h>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <shared_mutex>
#include <thread>
#include <vector>

class IndexManager {
public:
    struct IndexOptions {
        bool avoidReparsePoints = true;
        bool persistIndexCache = true;
        bool useIncrementalUsnUpdates = true;
        bool useFastNtfsUsnScan = true;
        // Off by default: reading $MFT directly usually requires an elevated process (see
        // RawMftIndexer.h). Attempting it is opt-in even for drives whose scan method is set to
        // RawMft, so a non-elevated run never surprises the user with an access-denied error.
        bool useRawMftScan = false;
    };

    IndexManager();
    ~IndexManager();

    IndexManager(const IndexManager&) = delete;
    IndexManager& operator=(const IndexManager&) = delete;

    // Must be called once before use. notifyWnd receives WM_APP_* messages (see AppMessages.h).
    void Initialize(HWND notifyWnd);

    // Refreshes the drive list from DriveEnumerator, preserving selection/scanMethod for drives
    // that already existed. Safe to call any time indexing is not running.
    std::vector<DriveIndexStatus> RefreshDriveList();

    std::vector<DriveIndexStatus> GetDriveStatuses() const;
    void SetDriveSelection(wchar_t letter, bool selected);
    void SetDriveScanMethod(wchar_t letter, ScanMethod method);

    bool IsIndexing() const { return m_indexingActive.load(); }

    // Starts indexing the given drives on a background thread. If forceRebuild is false and a
    // valid on-disk/in-memory cache exists, an incremental USN update is attempted first.
    void StartIndexing(std::vector<wchar_t> driveLetters, bool forceRebuild, const IndexOptions& options);

    // Clears the in-memory index for the given drives (or all drives if empty) and re-indexes.
    void RebuildDrives(std::vector<wchar_t> driveLetters, const IndexOptions& options);

    void CancelIndexing();

    // Deletes on-disk cache files and clears the in-memory index for all drives.
    void ClearAllCaches();

    // Iterates every indexed item across all drives under a shared (read) lock. 'fn' must be
    // fast and must not call back into IndexManager (it is called while the lock is held).
    void ForEachItem(const std::function<void(const IndexedItem&)>& fn) const;

    uint64_t TotalIndexedItemCount() const;

private:
    struct DriveData {
        std::vector<IndexedItem> items;
        std::unordered_map<uint64_t, UsnNode> nodes; // only populated for NTFS USN / Raw MFT sourced drives
        std::unique_ptr<PathReconstructor> reconstructor;
    };

    void WorkerThreadMain(std::vector<wchar_t> driveLetters, bool forceRebuild, IndexOptions options);
    void IndexOneDrive(wchar_t letter, bool forceRebuild, const IndexOptions& options);
    // Tries 'ceiling' first, falling through to progressively slower tiers whenever a tier is
    // turned off in Preferences (not just when it fails at runtime, which the individual
    // IndexOneDriveX methods already handle themselves). This is what makes "MFT, then USN,
    // then Recursive, assuming they're enabled" the actual behavior regardless of which single
    // ceiling a drive is configured with.
    void IndexOneDriveCascade(wchar_t letter, DriveIndexStatus& status, const IndexOptions& options, ScanMethod ceiling);
    void IndexOneDriveRawMft(wchar_t letter, DriveIndexStatus& status, const IndexOptions& options);
    void IndexOneDriveFast(wchar_t letter, DriveIndexStatus& status, const IndexOptions& options);
    void IndexOneDriveRecursive(wchar_t letter, DriveIndexStatus& status, const IndexOptions& options);
    enum class IncrementalResult {
        Applied,       // journal changes were read and applied on top of the in-memory index
        NotApplicable, // no in-memory index or no USN journal for this drive; nothing attempted
        Failed,        // journal read failed or journal was invalidated; a full rescan is needed
    };

    // Manages its own locking: the USN journal read (disk I/O) runs with no lock held so the
    // UI thread's status queries never block behind it; the lock is only taken briefly to
    // snapshot journal identity and then to apply the decoded changes. 'appliedChangeCount'
    // (optional) receives how many changes were applied, letting callers skip persisting an
    // index whose contents did not actually change.
    IncrementalResult TryIncrementalUpdate(wchar_t letter, size_t* appliedChangeCount = nullptr);

    void PostStatusChanged(wchar_t letter);
    void PostProgress(wchar_t letter, uint64_t count);
    bool IsCancelled() const { return m_cancelRequested.load(); }

    void SaveDriveCache(wchar_t letter);
    // Rewrites only the fixed-size header of an existing cache file (journal position, built-at
    // timestamp), leaving the item payload untouched. For when the in-memory index still matches
    // the file contents but the journal position or timestamp advanced.
    void UpdateDriveCacheHeader(wchar_t letter);
    // Manages its own locking: the file parse (potentially hundreds of MB) runs unlocked so
    // status queries from the UI thread don't block behind it.
    bool LoadDriveCache(wchar_t letter);
    void DeleteDriveCache(wchar_t letter);
    std::wstring CacheFilePath(wchar_t letter) const;

    DriveIndexStatus* FindStatusLocked(wchar_t letter);

    mutable std::shared_mutex m_mutex;
    std::vector<DriveIndexStatus> m_driveStatuses;
    std::map<wchar_t, DriveData> m_driveData;

    HWND m_notifyWnd = nullptr;
    std::thread m_workerThread;
    std::atomic<bool> m_cancelRequested{false};
    std::atomic<bool> m_indexingActive{false};
};
