#pragma once
// Runs a search directly against the filesystem, on demand, with no pre-built index. This is
// what powers search when indexing is disabled (the default): a background thread walks the
// requested root paths with FindFirstFileExW/FindNextFileW, matching each entry against the
// same SearchOptions rules SearchEngine uses for indexed search (via SearchEngine::IsMatch),
// and streams matches back to the UI in small batches.
//
// Starting a new search cancels any search already in progress. Cancellation/staleness is
// tracked with a generation counter: a worker thread checks its own generation against the
// current one before every filesystem operation and before publishing results, so a superseded
// search simply stops touching shared state rather than needing a separate abort signal.

#include "IndexTypes.h"

#include <windows.h>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class LiveSearchManager {
public:
    struct Snapshot {
        std::vector<IndexedItem> results;
        uint64_t totalMatched = 0;
        bool truncated = false;
        bool complete = true; // true when idle (no search running) or when the last search finished
    };

    LiveSearchManager();
    ~LiveSearchManager();

    LiveSearchManager(const LiveSearchManager&) = delete;
    LiveSearchManager& operator=(const LiveSearchManager&) = delete;

    void Initialize(HWND notifyWnd);

    // Cancels any in-progress search and starts a new one over 'rootPaths' (e.g. L"C:\\").
    void StartSearch(SearchOptions options, std::vector<std::wstring> rootPaths, bool avoidReparsePoints);

    // Cancels any in-progress search and leaves the manager idle.
    void CancelSearch();

    Snapshot GetSnapshot() const;

    // Cheap check that doesn't copy the results vector (unlike GetSnapshot()); safe to call
    // often, e.g. from status bar updates.
    bool IsSearching() const;

private:
    void WorkerMain(uint64_t generation, SearchOptions options, std::vector<std::wstring> rootPaths, bool avoidReparsePoints);
    bool IsStale(uint64_t generation) const { return generation != m_generation.load(); }
    void Flush(uint64_t generation, std::vector<IndexedItem>& batch, uint64_t totalMatched, bool truncated, bool complete);

    HWND m_notifyWnd = nullptr;
    std::thread m_thread;
    std::atomic<uint64_t> m_generation{0};

    mutable std::mutex m_mutex;
    Snapshot m_snapshot;
};
