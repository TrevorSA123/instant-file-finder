#include "FolderSizeCalculator.h"
#include "AppMessages.h"
#include "Handle.h"

#include <algorithm>

namespace {
// A small, fixed cap rather than using every core: this is background disk I/O work competing
// with the rest of the app (and the disk itself), not a CPU-bound task that benefits from full
// parallelism.
constexpr size_t kMaxWalkerThreads = 4;
}

FolderSizeCalculator::FolderSizeCalculator(HWND notifyWnd) : m_notifyWnd(notifyWnd) {
    size_t walkerCount = 2;
    if (unsigned int hw = std::thread::hardware_concurrency()) {
        walkerCount = std::min<size_t>(kMaxWalkerThreads, std::max<size_t>(2, hw));
    }
    for (size_t i = 0; i < walkerCount; ++i) {
        m_walkerThreads.emplace_back(&FolderSizeCalculator::WalkerLoop, this);
    }
    m_indexResolverThread = std::thread(&FolderSizeCalculator::IndexResolverLoop, this);
}

FolderSizeCalculator::~FolderSizeCalculator() {
    Shutdown();
}

void FolderSizeCalculator::Shutdown() {
    if (m_shutdown.exchange(true)) return;
    m_generation.fetch_add(1);
    m_cv.notify_all();
    for (auto& t : m_walkerThreads) {
        if (t.joinable()) t.join();
    }
    if (m_indexResolverThread.joinable()) {
        m_indexResolverThread.join();
    }
}

void FolderSizeCalculator::SetIndexSizeLookup(IndexSizeLookup lookup) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_indexSizeLookup = std::move(lookup);
}

void FolderSizeCalculator::RequestFolderSizes(std::vector<std::wstring> folderPaths, bool avoidReparsePoints) {
    // Longest paths first: a folder is more likely to be a descendant of another queued folder
    // when its path is longer, so processing deeper folders first gives ComputeFolderSize's
    // cache-reuse below something to actually find, instead of every folder always walking fresh.
    std::sort(folderPaths.begin(), folderPaths.end(),
              [](const std::wstring& a, const std::wstring& b) { return a.size() > b.size(); });

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_generation.fetch_add(1); // cancels whatever the walkers/resolver are currently doing
        if (avoidReparsePoints != m_avoidReparsePoints) {
            // A cached size may have included (or excluded) reparse-point subtrees under the old
            // setting; it's not trustworthy under the new one.
            m_cache.clear();
        }
        m_queue.clear();
        m_avoidReparsePoints = avoidReparsePoints;
        for (auto& p : folderPaths) {
            m_queue.push_back(std::move(p));
        }
    }
    m_cv.notify_all(); // wakes every walker thread and the index resolver thread
}

bool FolderSizeCalculator::TryGetCached(const std::wstring& folderPath, uint64_t& outSize) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_cache.find(folderPath);
    if (it == m_cache.end()) return false;
    outSize = it->second;
    return true;
}

bool FolderSizeCalculator::IsStale(uint64_t generation) const {
    return m_shutdown.load() || generation != m_generation.load();
}

void FolderSizeCalculator::PostResult(const std::wstring& folderPath, uint64_t size) const {
    if (m_notifyWnd == nullptr || m_shutdown.load()) return;
    auto* result = new ComputedResult{ folderPath, size };
    PostMessageW(m_notifyWnd, WM_APP_FOLDER_SIZE_COMPUTED, 0, reinterpret_cast<LPARAM>(result));
}

void FolderSizeCalculator::IndexResolverLoop() {
    uint64_t lastHandledGeneration = 0;
    for (;;) {
        uint64_t generation = 0;
        std::vector<std::wstring> batch;
        IndexSizeLookup lookup;

        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this, &lastHandledGeneration] {
                return m_shutdown.load() || m_generation.load() != lastHandledGeneration;
            });
            if (m_shutdown.load()) return;

            generation = m_generation.load();
            lastHandledGeneration = generation;
            lookup = m_indexSizeLookup;
            batch.assign(m_queue.begin(), m_queue.end());
        }

        if (!lookup || batch.empty()) continue;

        // The lookup itself (a pass over the in-memory index) runs without holding m_mutex, so it
        // never blocks the walker threads from making progress on other queued folders.
        std::unordered_map<std::wstring, uint64_t> resolvedMap = lookup(batch);
        if (resolvedMap.empty() || IsStale(generation)) continue;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (IsStale(generation)) continue;

            for (auto& kv : resolvedMap) {
                m_cache[kv.first] = kv.second;
            }
            // Remove resolved folders from the walk queue so walker threads don't redo this work.
            std::deque<std::wstring> remaining;
            for (auto& path : m_queue) {
                if (!resolvedMap.count(path)) remaining.push_back(std::move(path));
            }
            m_queue = std::move(remaining);
        }

        for (auto& kv : resolvedMap) {
            PostResult(kv.first, kv.second);
        }
    }
}

void FolderSizeCalculator::WalkerLoop() {
    for (;;) {
        std::wstring path;
        bool avoidReparsePoints = true;
        uint64_t generation = 0;
        bool cacheHit = false;
        uint64_t cachedSize = 0;

        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] { return m_shutdown.load() || !m_queue.empty(); });
            if (m_shutdown.load() && m_queue.empty()) return;
            if (m_queue.empty()) continue;

            path = std::move(m_queue.front());
            m_queue.pop_front();
            avoidReparsePoints = m_avoidReparsePoints;
            generation = m_generation.load();

            auto cacheIt = m_cache.find(path);
            if (cacheIt != m_cache.end()) {
                cacheHit = true;
                cachedSize = cacheIt->second;
            }
        }

        uint64_t size;
        if (cacheHit) {
            size = cachedSize;
        } else {
            bool completed = false;
            size = ComputeFolderSize(path, generation, avoidReparsePoints, completed);
            if (!completed) continue; // superseded mid-walk; drop silently, don't cache a partial sum

            std::lock_guard<std::mutex> lock(m_mutex);
            m_cache[path] = size;
        }

        PostResult(path, size);
    }
}

uint64_t FolderSizeCalculator::ComputeFolderSize(const std::wstring& folderPath, uint64_t generation,
                                                  bool avoidReparsePoints, bool& completed) const {
    uint64_t total = 0;
    std::vector<std::wstring> pendingDirs;
    pendingDirs.push_back(folderPath);

    while (!pendingDirs.empty()) {
        if (IsStale(generation)) {
            completed = false;
            return 0;
        }

        std::wstring dir = std::move(pendingDirs.back());
        pendingDirs.pop_back();

        std::wstring searchPattern = dir;
        if (!searchPattern.empty() && searchPattern.back() != L'\\') searchPattern += L'\\';
        searchPattern += L'*';

        WIN32_FIND_DATAW findData{};
        UniqueFindHandle findHandle(FindFirstFileExW(
            searchPattern.c_str(), FindExInfoBasic, &findData, FindExSearchNameMatch, nullptr,
            FIND_FIRST_EX_LARGE_FETCH));
        if (!findHandle.IsValid()) {
            continue; // inaccessible/vanished directory: skip, keep going
        }

        do {
            if (IsStale(generation)) {
                completed = false;
                return 0;
            }

            const wchar_t* name = findData.cFileName;
            if (wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0) continue;

            bool isDir = (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            bool isReparse = (findData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;

            if (isDir) {
                if (!(isReparse && avoidReparsePoints)) {
                    std::wstring fullPath = dir;
                    if (!fullPath.empty() && fullPath.back() != L'\\') fullPath += L'\\';
                    fullPath += name;

                    // Already sized (this session, under the current avoidReparsePoints setting)
                    // - reuse the total instead of walking this whole subtree again from disk.
                    uint64_t cachedSubSize = 0;
                    bool cached = false;
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        auto it = m_cache.find(fullPath);
                        if (it != m_cache.end()) {
                            cachedSubSize = it->second;
                            cached = true;
                        }
                    }
                    if (cached) {
                        total += cachedSubSize;
                    } else {
                        pendingDirs.push_back(std::move(fullPath));
                    }
                }
            } else {
                ULARGE_INTEGER size{};
                size.LowPart = findData.nFileSizeLow;
                size.HighPart = findData.nFileSizeHigh;
                total += size.QuadPart;
            }
        } while (FindNextFileW(findHandle.Get(), &findData));
    }

    completed = true;
    return total;
}
