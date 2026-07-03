#pragma once
// Lazily resolves size/timestamp/attribute metadata for visible search results using
// GetFileAttributesExW, off the UI thread, so an initial USN-based index (which has no size
// data) doesn't require touching every file up front. Results are cached and posted back to
// the UI one at a time via WM_APP_METADATA_ENRICHED.

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
    // Replaces any previously queued-but-not-yet-processed paths.
    void RequestEnrichment(std::vector<std::wstring> paths);

    bool TryGetCached(const std::wstring& fullPath, EnrichedResult& out) const;

    void Shutdown();

private:
    void WorkerLoop();

    HWND m_notifyWnd;

    mutable std::mutex m_mutex;
    std::unordered_map<std::wstring, EnrichedResult> m_cache;
    std::deque<std::wstring> m_queue;
    std::condition_variable m_cv;

    std::thread m_thread;
    std::atomic<bool> m_shutdown{false};
};
