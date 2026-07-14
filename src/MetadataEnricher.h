#pragma once
// Lazily resolves size/timestamp/attribute metadata for search results using
// GetFileAttributesExW, off the UI thread, so an initial USN-based index (which has no size
// data) doesn't require touching every file up front. Results are cached and posted back to
// the UI one at a time via WM_APP_METADATA_ENRICHED.
//
// Two independent, replace-on-request queues feed the same worker: the interactive queue
// (RequestEnrichment) for the currently visible rows, serviced first for a fast turnaround, and
// a bulk queue (RequestBulkEnrichment) for larger background batches (e.g. filling in every row
// of a large result set so sorting by size/date reflects real values). Keeping them separate
// means a bulk request in flight never delays visible-row updates, and visible-row requests never
// truncate an in-progress bulk batch.

#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class MetadataEnricher {
public:
    struct EnrichedResult {
        std::wstring fullPath;
        bool found = false;
        uint64_t size = 0;
        bool sizeKnown = false;
        FILETIME createdTime{};
        bool createdTimeKnown = false;
        FILETIME modifiedTime{};
        bool modifiedTimeKnown = false;
        uint32_t attributes = 0;
    };

    explicit MetadataEnricher(HWND notifyWnd);
    ~MetadataEnricher();

    MetadataEnricher(const MetadataEnricher&) = delete;
    MetadataEnricher& operator=(const MetadataEnricher&) = delete;

    // Queues the given paths for background enrichment (typically the currently visible rows).
    // Replaces any previously queued-but-not-yet-processed paths. Serviced ahead of bulk requests
    // (see below) so visible rows resolve quickly even while a large bulk request is in flight.
    void RequestEnrichment(std::vector<std::wstring> paths);

    // Queues the given paths as a lower-priority bulk batch (e.g. every row missing metadata
    // needed to sort a large result set). Replaces any previously queued-but-not-yet-processed
    // bulk paths; independent of the interactive queue above, so neither can starve or clobber
    // the other's in-flight request.
    void RequestBulkEnrichment(std::vector<std::wstring> paths);

    bool TryGetCached(const std::wstring& fullPath, EnrichedResult& out) const;

    void Shutdown();

private:
    void WorkerLoop();

    HWND m_notifyWnd;

    mutable std::mutex m_mutex;
    std::unordered_map<std::wstring, EnrichedResult> m_cache;
    std::deque<std::wstring> m_queue;
    std::deque<std::wstring> m_bulkQueue;
    std::condition_variable m_cv;

    std::thread m_thread;
    std::atomic<bool> m_shutdown{false};
};
