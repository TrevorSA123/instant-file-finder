#include "LiveSearchManager.h"
#include "AppMessages.h"
#include "SearchEngine.h"
#include "RecursiveFileScanner.h"
#include "Handle.h"

#include <regex>
#include <vector>

LiveSearchManager::LiveSearchManager() = default;

LiveSearchManager::~LiveSearchManager() {
    CancelSearch();
}

void LiveSearchManager::Initialize(HWND notifyWnd) {
    m_notifyWnd = notifyWnd;
}

void LiveSearchManager::CancelSearch() {
    m_generation.fetch_add(1);
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void LiveSearchManager::StartSearch(SearchOptions options, std::vector<std::wstring> rootPaths, bool avoidReparsePoints) {
    uint64_t myGen = m_generation.fetch_add(1) + 1;
    if (m_thread.joinable()) {
        m_thread.join(); // the old worker checks generation every iteration, so this returns promptly
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_snapshot = Snapshot{};
        m_snapshot.complete = false;
    }

    m_thread = std::thread(&LiveSearchManager::WorkerMain, this, myGen, std::move(options), std::move(rootPaths), avoidReparsePoints);
}

void LiveSearchManager::Flush(uint64_t generation, std::vector<IndexedItem>& batch, uint64_t totalMatched, bool truncated, bool complete) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (generation != m_generation.load()) {
            batch.clear();
            return; // superseded by a newer search; drop silently
        }
        for (auto& item : batch) {
            m_snapshot.results.push_back(std::move(item));
        }
        m_snapshot.totalMatched = totalMatched;
        m_snapshot.truncated = truncated;
        m_snapshot.complete = complete;
    }
    batch.clear();

    if (m_notifyWnd != nullptr) {
        PostMessageW(m_notifyWnd, complete ? WM_APP_LIVE_SEARCH_COMPLETE : WM_APP_LIVE_SEARCH_PROGRESS, 0,
                     static_cast<LPARAM>(generation));
    }
}

void LiveSearchManager::WorkerMain(uint64_t generation, SearchOptions options, std::vector<std::wstring> rootPaths,
                                    bool avoidReparsePoints) {
    std::wregex compiledRegex;
    std::wstring regexError;
    bool haveRegex = SearchEngine::TryCompileRegex(options, compiledRegex, regexError) &&
                      options.mode == SearchMode::Regex && !options.query.empty();
    const std::wregex* regexPtr = haveRegex ? &compiledRegex : nullptr;

    size_t maxResults = options.maxResults > 0 ? options.maxResults : 10000;

    std::vector<IndexedItem> pendingBatch;
    uint64_t totalMatched = 0;
    size_t storedCount = 0;
    bool truncated = false;
    DWORD lastFlushTick = GetTickCount();

    for (const auto& root : rootPaths) {
        if (IsStale(generation)) return;
        if (root.empty()) continue;
        wchar_t driveLetter = root[0];

        std::vector<std::wstring> pendingDirs;
        pendingDirs.push_back(root);

        while (!pendingDirs.empty()) {
            if (IsStale(generation)) return;

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
                if (IsStale(generation)) return;

                const wchar_t* name = findData.cFileName;
                if (wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0) continue;

                bool isDir = (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                bool isReparse = (findData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;

                std::wstring fullPath = dir;
                if (!fullPath.empty() && fullPath.back() != L'\\') fullPath += L'\\';
                fullPath += name;

                IndexedItem item = RecursiveFileScanner::BuildItem(findData, fullPath, driveLetter);

                if (SearchEngine::IsMatch(item, options, regexPtr)) {
                    ++totalMatched;
                    if (storedCount < maxResults) {
                        pendingBatch.push_back(std::move(item));
                        ++storedCount;
                    } else {
                        truncated = true;
                    }
                }

                if (isDir && !(isReparse && avoidReparsePoints)) {
                    pendingDirs.push_back(fullPath);
                }

                DWORD now = GetTickCount();
                if (pendingBatch.size() >= 50 || (now - lastFlushTick) >= 200) {
                    Flush(generation, pendingBatch, totalMatched, truncated, false);
                    lastFlushTick = now;
                    if (IsStale(generation)) return;
                }

                if (storedCount >= maxResults) {
                    // Stop scanning entirely once the display cap is reached: further matches
                    // wouldn't be shown, and continuing would just burn time on a huge volume.
                    Flush(generation, pendingBatch, totalMatched, truncated, true);
                    return;
                }
            } while (FindNextFileW(findHandle.Get(), &findData));
        }
    }

    Flush(generation, pendingBatch, totalMatched, truncated, true);
}

LiveSearchManager::Snapshot LiveSearchManager::GetSnapshot() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_snapshot;
}

bool LiveSearchManager::IsSearching() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return !m_snapshot.complete;
}
