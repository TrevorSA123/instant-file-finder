#include "IndexedSearchWorker.h"
#include "AppMessages.h"

IndexedSearchWorker::IndexedSearchWorker() = default;

IndexedSearchWorker::~IndexedSearchWorker() {
    CancelSearch();
}

void IndexedSearchWorker::Initialize(HWND notifyWnd) {
    m_notifyWnd = notifyWnd;
}

void IndexedSearchWorker::CancelSearch() {
    m_generation.fetch_add(1);
    std::unique_lock<std::mutex> lock(m_liveMutex);
    m_liveCv.wait(lock, [this] { return m_liveThreads == 0; });
    m_searching.store(false);
}

void IndexedSearchWorker::StartSearch(SearchOptions options, SearchEngine::ItemSource source) {
    uint64_t myGen = m_generation.fetch_add(1) + 1;

    m_searching.store(true);
    {
        std::lock_guard<std::mutex> lock(m_liveMutex);
        ++m_liveThreads;
    }
    std::thread(&IndexedSearchWorker::WorkerMain, this, myGen, std::move(options), std::move(source), GetTickCount())
        .detach();
}

void IndexedSearchWorker::WorkerMain(uint64_t generation, SearchOptions options, SearchEngine::ItemSource source,
                                      DWORD startTick) {
    auto cancelled = [this, generation] { return IsStale(generation); };
    SearchEngine::SearchOutcome outcome = SearchEngine::Execute(source, options, cancelled);

    if (!IsStale(generation)) {
        auto* completed = new IndexedSearchWorker::CompletedSearch();
        completed->outcome = std::move(outcome);
        completed->durationMs = GetTickCount() - startTick;

        m_searching.store(false);

        if (m_notifyWnd != nullptr) {
            PostMessageW(m_notifyWnd, WM_APP_INDEXED_SEARCH_COMPLETE, 0, reinterpret_cast<LPARAM>(completed));
        }
    } // else: superseded by a newer search; drop silently

    {
        std::lock_guard<std::mutex> lock(m_liveMutex);
        --m_liveThreads;
    }
    m_liveCv.notify_all();
}
