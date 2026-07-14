#pragma once
// Computes total recursive size for folder rows in the current search results, off the UI
// thread, when AppSettings::computeFolderSizes is enabled. Unlike file metadata (a single
// GetFileAttributesExW call - see MetadataEnricher), a folder's size isn't tracked anywhere by
// NTFS directly; it can only be found by walking every descendant file and summing sizes, or
// (fast path below) by summing already-indexed items under that folder when the index has size
// data for every one of them. Results are cached by folder path and posted back one at a time via
// WM_APP_FOLDER_SIZE_COMPUTED.
//
// Three things keep this off the UI thread and reasonably fast even for large result sets:
//  - Index fast path: an optional IndexSizeLookup callback (see SetIndexSizeLookup) is tried once
//    per request batch, before any disk walking, on a dedicated resolver thread. It's a single
//    pass over the in-memory index rather than a per-folder scan, and only "resolves" a folder if
//    every file found under it in the index has a known size (only Raw MFT scan captures size -
//    see RawMftIndexer/NtfsUsnIndexer) - otherwise that folder is left for the disk-walk fallback.
//  - Disk-walk cache reuse: ComputeFolderSize checks the cache before descending into each
//    subdirectory, so a folder whose subfolders were already sized (this session) doesn't re-walk
//    them from disk. Folders are queued longest-path-first (see RequestFolderSizes) so children
//    tend to get processed before their parents, making this reuse actually happen in practice.
//  - A small pool of walker threads processes the disk-walk queue concurrently instead of one
//    folder at a time.

#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class FolderSizeCalculator {
public:
    struct ComputedResult {
        std::wstring folderPath;
        uint64_t size = 0;
    };

    // Given a batch of folder paths, returns whatever subset could be fully resolved from an
    // already-loaded in-memory index (folderPath -> total size) without touching disk. Folders
    // absent from the returned map fall back to the normal disk walk. Invoked on a background
    // thread - implementations must only touch thread-safe state (see MainWindow's
    // LookupFolderSizesFromIndex for the one in use).
    using IndexSizeLookup = std::function<std::unordered_map<std::wstring, uint64_t>(const std::vector<std::wstring>&)>;

    explicit FolderSizeCalculator(HWND notifyWnd);
    ~FolderSizeCalculator();

    FolderSizeCalculator(const FolderSizeCalculator&) = delete;
    FolderSizeCalculator& operator=(const FolderSizeCalculator&) = delete;

    // Set once before the first RequestFolderSizes call if the index fast path should be used;
    // safe to leave unset, in which case everything goes through the disk walk.
    void SetIndexSizeLookup(IndexSizeLookup lookup);

    // Replaces any previously queued-but-not-yet-processed folders and cancels in-progress work
    // that isn't for one of these folders, then queues these for background sizing. Folders
    // already resolved in the cache are posted back immediately without rewalking.
    void RequestFolderSizes(std::vector<std::wstring> folderPaths, bool avoidReparsePoints);

    bool TryGetCached(const std::wstring& folderPath, uint64_t& outSize) const;

    void Shutdown();

private:
    void WalkerLoop();
    void IndexResolverLoop();
    bool IsStale(uint64_t generation) const;
    uint64_t ComputeFolderSize(const std::wstring& folderPath, uint64_t generation, bool avoidReparsePoints,
                                bool& completed) const;
    void PostResult(const std::wstring& folderPath, uint64_t size) const;

    HWND m_notifyWnd;

    mutable std::mutex m_mutex;
    std::unordered_map<std::wstring, uint64_t> m_cache;
    std::deque<std::wstring> m_queue;
    bool m_avoidReparsePoints = true;
    std::condition_variable m_cv;

    std::atomic<uint64_t> m_generation{ 0 };
    IndexSizeLookup m_indexSizeLookup;

    std::vector<std::thread> m_walkerThreads;
    std::thread m_indexResolverThread;
    std::atomic<bool> m_shutdown{ false };
};
