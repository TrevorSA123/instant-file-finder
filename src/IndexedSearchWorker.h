#pragma once
// Runs SearchEngine::Execute() against the in-memory index on a background thread instead of the
// UI thread.
//
// This exists because of the size filter: NTFS USN records don't carry file size (see
// NtfsUsnIndexer), so PassesFilters falls back to a synchronous GetFileAttributesExW call per
// item that doesn't have one yet. Across a large index that's real, unbounded disk I/O - running
// it inline on the UI thread (as indexed search used to, back when it was pure in-memory
// scanning) freezes the window for as long as the scan takes. Routing every indexed search
// through here, size filter or not, keeps the UI thread free and keeps indexed search on one code
// path instead of branching between a synchronous fast path and an async one.
//
// Cancellation/staleness uses the same generation-counter pattern as LiveSearchManager, with one
// difference: SearchEngine::Execute has no natural per-item break point of its own, so it's given
// a cancellation predicate that makes the remaining per-item work (matching, filtering, and any
// I/O) a no-op once superseded, so a stale scan winds down quickly instead of running to
// completion.

#include "IndexTypes.h"
#include "SearchEngine.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <windows.h>

class IndexedSearchWorker {
public:
    struct CompletedSearch {
        SearchEngine::SearchOutcome outcome;
        DWORD durationMs = 0;
    };

    IndexedSearchWorker();
    ~IndexedSearchWorker();

    IndexedSearchWorker(const IndexedSearchWorker&) = delete;
    IndexedSearchWorker& operator=(const IndexedSearchWorker&) = delete;

    void Initialize(HWND notifyWnd);

    // Cancels any in-progress search and starts a new one. 'source' is expected to read through
    // IndexManager, which outlives this worker for MainWindow's lifetime.
    //
    // The superseded search's thread is detached rather than joined here: with the size filter
    // on, that thread may be mid-way through a long run of synchronous per-item disk stats (see
    // SearchEngine::PassesFilters), and joining it from this call - which runs on the UI thread,
    // once per keystroke via MainWindow::RunSearch - would block the UI for however long that
    // thread takes to notice it's stale and unwind. Staleness is still enforced via the
    // generation counter, so a detached, superseded thread's result is simply dropped in
    // WorkerMain instead of being posted to the UI.
    void StartSearch(SearchOptions options, SearchEngine::ItemSource source);

    // Cancels any in-progress search and blocks until every detached search thread has actually
    // exited, then leaves the worker idle. Only meant to be called at shutdown (or other
    // non-interactive points) since, unlike StartSearch, it does wait.
    void CancelSearch();

    bool IsSearching() const { return m_searching.load(); }

private:
    void WorkerMain(uint64_t generation, SearchOptions options, SearchEngine::ItemSource source, DWORD startTick);
    bool IsStale(uint64_t generation) const { return generation != m_generation.load(); }

    HWND m_notifyWnd = nullptr;
    std::atomic<uint64_t> m_generation{0};
    std::atomic<bool> m_searching{false};

    // Tracks detached WorkerMain threads still running, so CancelSearch (and the destructor) can
    // wait for them to actually finish before this object goes away - a detached thread still
    // touches 'this' via IsStale/m_notifyWnd, so it must not outlive the worker.
    std::mutex m_liveMutex;
    std::condition_variable m_liveCv;
    int m_liveThreads = 0;
};
