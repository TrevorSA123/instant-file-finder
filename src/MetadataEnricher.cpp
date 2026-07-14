#include "MetadataEnricher.h"
#include "AppMessages.h"

MetadataEnricher::MetadataEnricher(HWND notifyWnd) : m_notifyWnd(notifyWnd) {
    m_thread = std::thread(&MetadataEnricher::WorkerLoop, this);
}

MetadataEnricher::~MetadataEnricher() {
    Shutdown();
}

void MetadataEnricher::Shutdown() {
    if (m_shutdown.exchange(true)) return;
    m_cv.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void MetadataEnricher::RequestEnrichment(std::vector<std::wstring> paths) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.clear();
        for (auto& p : paths) {
            m_queue.push_back(std::move(p));
        }
    }
    m_cv.notify_one();
}

void MetadataEnricher::RequestBulkEnrichment(std::vector<std::wstring> paths) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_bulkQueue.clear();
        for (auto& p : paths) {
            m_bulkQueue.push_back(std::move(p));
        }
    }
    m_cv.notify_one();
}

bool MetadataEnricher::TryGetCached(const std::wstring& fullPath, EnrichedResult& out) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_cache.find(fullPath);
    if (it == m_cache.end()) return false;
    out = it->second;
    return true;
}

void MetadataEnricher::WorkerLoop() {
    for (;;) {
        std::wstring path;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] { return m_shutdown.load() || !m_queue.empty() || !m_bulkQueue.empty(); });
            if (m_shutdown.load() && m_queue.empty() && m_bulkQueue.empty()) return;
            if (!m_queue.empty()) {
                path = std::move(m_queue.front());
                m_queue.pop_front();
            } else if (!m_bulkQueue.empty()) {
                path = std::move(m_bulkQueue.front());
                m_bulkQueue.pop_front();
            } else {
                continue;
            }
        }

        EnrichedResult result;
        result.fullPath = path;

        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
            result.found = true;
            result.attributes = data.dwFileAttributes;

            if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                ULARGE_INTEGER size{};
                size.LowPart = data.nFileSizeLow;
                size.HighPart = data.nFileSizeHigh;
                result.size = size.QuadPart;
                result.sizeKnown = true;
            }
            result.createdTime = data.ftCreationTime;
            result.createdTimeKnown = true;
            result.modifiedTime = data.ftLastWriteTime;
            result.modifiedTimeKnown = true;
        } else {
            result.found = false;
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_cache[path] = result;
        }

        if (m_notifyWnd != nullptr && !m_shutdown.load()) {
            auto* heapResult = new EnrichedResult(result);
            PostMessageW(m_notifyWnd, WM_APP_METADATA_ENRICHED, 0, reinterpret_cast<LPARAM>(heapResult));
        }
    }
}
