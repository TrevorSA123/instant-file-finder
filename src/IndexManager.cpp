#include "IndexManager.h"
#include "AppMessages.h"
#include "DriveEnumerator.h"
#include "NtfsUsnIndexer.h"
#include "RawMftIndexer.h"
#include "RecursiveFileScanner.h"
#include "UsnIncrementalUpdater.h"
#include "ShellHelpers.h"
#include "StringUtil.h"
#include "FileTimeUtil.h"

#include <winioctl.h>
#include <algorithm>
#include <fstream>

namespace {

constexpr char kCacheMagic[4] = { 'I', 'F', 'F', 'C' };
constexpr uint8_t kCacheVersion = 2; // v2 adds the "built at" timestamp used for staleness warnings
constexpr size_t kMaxStringLen = 32768; // sanity cap against corrupt cache files

// NTFS USN and Raw MFT are the two sources that carry a real parent-FRN graph usable for path
// reconstruction and incremental journal updates; Recursive scans don't.
bool IsFrnGraphSource(const std::wstring& source) {
    return source == L"NTFS USN" || source == L"Raw MFT";
}

const wchar_t* ScanMethodDisplayName(ScanMethod method) {
    switch (method) {
        case ScanMethod::RawMft: return L"Raw MFT";
        case ScanMethod::FastNtfsUsn: return L"NTFS USN";
        case ScanMethod::Recursive: return L"Recursive";
        default: return L"Disabled";
    }
}

void WriteWString(std::ofstream& out, const std::wstring& s) {
    uint32_t len = static_cast<uint32_t>(std::min<size_t>(s.size(), kMaxStringLen));
    out.write(reinterpret_cast<const char*>(&len), sizeof(len));
    if (len > 0) {
        out.write(reinterpret_cast<const char*>(s.data()), len * sizeof(wchar_t));
    }
}

bool ReadWString(std::ifstream& in, std::wstring& s) {
    uint32_t len = 0;
    in.read(reinterpret_cast<char*>(&len), sizeof(len));
    if (!in.good() || len > kMaxStringLen) return false;
    s.assign(len, L'\0');
    if (len > 0) {
        in.read(reinterpret_cast<char*>(&s[0]), len * sizeof(wchar_t));
        if (!in.good()) return false;
    }
    return true;
}

template <typename T>
void WriteRaw(std::ofstream& out, const T& value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
bool ReadRaw(std::ifstream& in, T& value) {
    in.read(reinterpret_cast<char*>(&value), sizeof(T));
    return in.good();
}

} // namespace

IndexManager::IndexManager() = default;

IndexManager::~IndexManager() {
    CancelIndexing();
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
}

void IndexManager::Initialize(HWND notifyWnd) {
    m_notifyWnd = notifyWnd;
    RefreshDriveList();
}

std::vector<DriveIndexStatus> IndexManager::RefreshDriveList() {
    auto freshDrives = DriveEnumerator::EnumerateDrives();

    std::unique_lock lock(m_mutex);

    // Preserve user selection/scan-method overrides for drives that already exist.
    for (auto& fresh : freshDrives) {
        auto existing = std::find_if(m_driveStatuses.begin(), m_driveStatuses.end(),
            [&](const DriveIndexStatus& s) { return s.driveRoot == fresh.driveRoot; });
        if (existing != m_driveStatuses.end()) {
            fresh.selected = existing->selected;
            fresh.scanMethod = existing->scanMethod;
            fresh.indexed = existing->indexed;
            fresh.itemCount = existing->itemCount;
            fresh.volumeSerialNumber = existing->volumeSerialNumber;
            fresh.usnJournalId = existing->usnJournalId;
            fresh.lastProcessedUsn = existing->lastProcessedUsn;
            fresh.hasUsnJournal = existing->hasUsnJournal;
            fresh.lastIndexedTime = existing->lastIndexedTime;
            fresh.lastIndexedTimeKnown = existing->lastIndexedTimeKnown;
        }
    }

    m_driveStatuses = std::move(freshDrives);
    return m_driveStatuses;
}

std::vector<DriveIndexStatus> IndexManager::GetDriveStatuses() const {
    std::shared_lock lock(m_mutex);
    return m_driveStatuses;
}

void IndexManager::SetDriveSelection(wchar_t letter, bool selected) {
    std::unique_lock lock(m_mutex);
    if (auto* status = FindStatusLocked(letter)) {
        status->selected = selected;
    }
}

void IndexManager::SetDriveScanMethod(wchar_t letter, ScanMethod method) {
    std::unique_lock lock(m_mutex);
    if (auto* status = FindStatusLocked(letter)) {
        status->scanMethod = method;
        status->indexingMethod = (method == ScanMethod::RawMft) ? L"Raw MFT"
                                  : (method == ScanMethod::FastNtfsUsn) ? L"NTFS USN"
                                  : (method == ScanMethod::Recursive) ? L"Recursive"
                                                                        : L"Disabled";
    }
}

DriveIndexStatus* IndexManager::FindStatusLocked(wchar_t letter) {
    for (auto& s : m_driveStatuses) {
        if (!s.driveRoot.empty() && towupper(s.driveRoot[0]) == towupper(letter)) {
            return &s;
        }
    }
    return nullptr;
}

void IndexManager::StartIndexing(std::vector<wchar_t> driveLetters, bool forceRebuild, const IndexOptions& options) {
    if (m_indexingActive.load()) {
        return; // one indexing pass at a time; UI disables the actions that would call this
    }
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }

    m_cancelRequested.store(false);
    m_indexingActive.store(true);
    m_workerThread = std::thread(&IndexManager::WorkerThreadMain, this, std::move(driveLetters), forceRebuild, options);
}

void IndexManager::RebuildDrives(std::vector<wchar_t> driveLetters, const IndexOptions& options) {
    {
        std::unique_lock lock(m_mutex);
        for (wchar_t letter : driveLetters) {
            m_driveData.erase(towupper(letter));
            if (auto* status = FindStatusLocked(letter)) {
                status->indexed = false;
                status->itemCount = 0;
                status->errors.clear();
                status->lastProcessedUsn = 0;
            }
        }
    }
    StartIndexing(std::move(driveLetters), /*forceRebuild=*/true, options);
}

void IndexManager::CancelIndexing() {
    m_cancelRequested.store(true);
}

void IndexManager::ClearAllCaches() {
    std::vector<wchar_t> letters;
    {
        std::unique_lock lock(m_mutex);
        for (auto& s : m_driveStatuses) {
            if (!s.driveRoot.empty()) letters.push_back(s.driveRoot[0]);
        }
        m_driveData.clear();
        for (auto& s : m_driveStatuses) {
            s.indexed = false;
            s.itemCount = 0;
            s.lastProcessedUsn = 0;
        }
    }
    for (wchar_t letter : letters) {
        DeleteDriveCache(letter);
    }
}

void IndexManager::ForEachItem(const std::function<void(const IndexedItem&)>& fn) const {
    std::shared_lock lock(m_mutex);
    for (const auto& kv : m_driveData) {
        for (const auto& item : kv.second.items) {
            fn(item);
        }
    }
}

uint64_t IndexManager::TotalIndexedItemCount() const {
    std::shared_lock lock(m_mutex);
    uint64_t total = 0;
    for (const auto& kv : m_driveData) {
        total += kv.second.items.size();
    }
    return total;
}

void IndexManager::PostStatusChanged(wchar_t letter) {
    if (m_notifyWnd != nullptr) {
        PostMessageW(m_notifyWnd, WM_APP_DRIVE_STATUS_CHANGED, static_cast<WPARAM>(letter), 0);
    }
}

void IndexManager::PostProgress(wchar_t letter, uint64_t count) {
    if (m_notifyWnd != nullptr) {
        PostMessageW(m_notifyWnd, WM_APP_INDEXING_PROGRESS, static_cast<WPARAM>(letter), static_cast<LPARAM>(count));
    }
}

void IndexManager::WorkerThreadMain(std::vector<wchar_t> driveLetters, bool forceRebuild, IndexOptions options) {
    for (wchar_t letter : driveLetters) {
        if (IsCancelled()) break;
        IndexOneDrive(letter, forceRebuild, options);
    }

    m_indexingActive.store(false);
    if (m_notifyWnd != nullptr) {
        PostMessageW(m_notifyWnd, WM_APP_INDEXING_COMPLETE, 0, 0);
    }
}

void IndexManager::IndexOneDrive(wchar_t letter, bool forceRebuild, const IndexOptions& options) {
    letter = static_cast<wchar_t>(towupper(letter));

    DriveIndexStatus statusCopy;
    {
        std::unique_lock lock(m_mutex);
        auto* status = FindStatusLocked(letter);
        if (status == nullptr) return;
        status->indexing = true;
        status->errors.clear();
        status->requiresElevation = false; // re-evaluated fresh by this run, not carried over from a stale previous one
        status->statusMessage = L"Indexing...";
        statusCopy = *status;
    }
    PostStatusChanged(letter);

    // A journal ID/next-USN was captured at build time regardless of which method built the
    // index (both NtfsUsnIndexer and RawMftIndexer query FSCTL_QUERY_USN_JOURNAL), so an
    // incremental refresh doesn't need to repeat whichever expensive full scan built it.
    bool usedIncremental = false;
    bool scanMethodSupportsIncremental =
        statusCopy.scanMethod == ScanMethod::FastNtfsUsn || statusCopy.scanMethod == ScanMethod::RawMft;
    if (!forceRebuild && scanMethodSupportsIncremental && options.useIncrementalUsnUpdates) {
        std::unique_lock lock(m_mutex);
        auto* status = FindStatusLocked(letter);
        if (status != nullptr && status->indexed && status->hasUsnJournal) {
            usedIncremental = TryIncrementalUpdate(letter, *status);
        }
    }

    // Tracks whether the index we ended up with came straight from an on-disk cache (in which
    // case its "last indexed" timestamp reflects when the cache was originally built, not now).
    bool loadedFromDiskCache = false;

    if (!usedIncremental) {
        std::unique_lock lock(m_mutex);
        auto* status = FindStatusLocked(letter);
        if (status == nullptr) return;

        // A cache on disk can satisfy this without a rescan if it's still valid and the caller
        // did not explicitly request a rebuild.
        bool loadedFromDisk = false;
        if (!forceRebuild && options.persistIndexCache && !status->indexed) {
            loadedFromDisk = LoadDriveCache(letter, *status);
        }
        loadedFromDiskCache = loadedFromDisk;

        if (!loadedFromDisk) {
            ScanMethod method = status->scanMethod;
            DriveIndexStatus statusForScan = *status;
            statusForScan.downgradeReason.clear();
            lock.unlock();

            if (method == ScanMethod::Disabled) {
                statusForScan.statusMessage = L"Disabled";
            } else {
                IndexOneDriveCascade(letter, statusForScan, options, method);
            }

            std::unique_lock lock2(m_mutex);
            if (auto* s = FindStatusLocked(letter)) {
                *s = statusForScan;
            }
        }
    }

    {
        std::unique_lock lock(m_mutex);
        if (auto* status = FindStatusLocked(letter)) {
            status->indexing = false;
            status->indexed = true;
            auto dataIt = m_driveData.find(letter);
            status->itemCount = (dataIt != m_driveData.end()) ? dataIt->second.items.size() : 0;
            if (status->errors.empty()) {
                status->statusMessage = L"Indexed";
            }
            // A fresh scan or a successful incremental sync both mean the index reflects the
            // current moment; only a straight cache load keeps its original build timestamp.
            if (!loadedFromDiskCache) {
                status->lastIndexedTime = FileTimeUtil::Now();
                status->lastIndexedTimeKnown = true;
            }
        }
    }
    PostStatusChanged(letter);

    if (options.persistIndexCache) {
        SaveDriveCache(letter);
    }
}

void IndexManager::IndexOneDriveCascade(wchar_t letter, DriveIndexStatus& status, const IndexOptions& options, ScanMethod ceiling) {
    size_t errorsBefore = status.errors.size();
    ScanMethod effective = ceiling;

    if (effective == ScanMethod::RawMft && !options.useRawMftScan) {
        status.errors.push_back(L"Raw MFT parsing is selected for this drive but turned off in Preferences; using NTFS USN scan instead.");
        effective = ScanMethod::FastNtfsUsn;
    }
    if (effective == ScanMethod::FastNtfsUsn && !options.useFastNtfsUsnScan) {
        status.errors.push_back(L"NTFS USN scanning is turned off in Preferences; using a recursive scan instead.");
        effective = ScanMethod::Recursive;
    }

    if (effective == ScanMethod::RawMft) {
        IndexOneDriveRawMft(letter, status, options); // falls further on its own on runtime failure
    } else if (effective == ScanMethod::FastNtfsUsn) {
        IndexOneDriveFast(letter, status, options); // falls further on its own on runtime failure
    } else {
        IndexOneDriveRecursive(letter, status, options);
    }

    // Rebuilt here, after the whole fallback chain has actually run, rather than set once at the
    // first hop: that avoided a stale/misleading note like "using NTFS USN" sticking around when
    // USN then *also* failed at runtime and the drive really ended up on a recursive scan. This
    // covers every hop (preference-disabled tiers above, plus runtime failures inside
    // IndexOneDriveRawMft/Fast) by summarizing every error message added during this call.
    if (status.errors.size() > errorsBefore &&
        status.indexingMethod != ScanMethodDisplayName(ceiling)) {
        std::wstring combined;
        for (size_t i = errorsBefore; i < status.errors.size(); ++i) {
            if (!combined.empty()) combined += L" ";
            combined += status.errors[i];
        }
        status.downgradeReason = combined;
    }
}

void IndexManager::IndexOneDriveRawMft(wchar_t letter, DriveIndexStatus& status, const IndexOptions& options) {
    auto cancelPredicate = [this]() { return IsCancelled(); };
    auto progressCallback = [this, letter](uint64_t count) { PostProgress(letter, count); };

    RawMftIndexer::Result result = RawMftIndexer::BuildIndex(letter, cancelPredicate, progressCallback);

    if (!result.success) {
        if (result.accessDenied) {
            status.requiresElevation = true;
            status.errors.push_back(L"Raw MFT scan needs Administrator privileges; using NTFS USN scan instead.");
        } else {
            status.errors.push_back(L"Raw MFT scan unavailable; using NTFS USN scan instead. (" + result.failureReason + L")");
        }
        // Falls through to the next-fastest tier rather than straight to a recursive scan: both
        // methods build the same node-graph shape, so there's no reason to give up the speed
        // advantage of USN enumeration just because the even-faster raw parse didn't pan out.
        IndexOneDriveFast(letter, status, options);
        return;
    }

    std::unique_lock lock(m_mutex);
    DriveData& data = m_driveData[letter];
    data.items = std::move(result.items);
    data.nodes.clear();
    for (const auto& item : data.items) {
        if (IsFrnGraphSource(item.source)) {
            UsnNode node;
            node.parentFrn = item.parentFileReferenceNumber;
            node.name = item.name;
            node.attributes = item.attributes;
            data.nodes[item.fileReferenceNumber] = std::move(node);
        }
    }
    data.reconstructor = std::make_unique<PathReconstructor>(std::wstring(1, letter) + L":\\");

    status.hasUsnJournal = result.hasUsnJournal;
    status.usnJournalId = result.usnJournalId;
    status.lastProcessedUsn = result.nextUsn;
    status.indexingMethod = L"Raw MFT";
}

void IndexManager::IndexOneDriveFast(wchar_t letter, DriveIndexStatus& status, const IndexOptions& options) {
    auto cancelPredicate = [this]() { return IsCancelled(); };
    auto progressCallback = [this, letter](uint64_t count) { PostProgress(letter, count); };

    NtfsUsnIndexer::Result result = NtfsUsnIndexer::BuildIndex(letter, cancelPredicate, progressCallback);

    if (!result.success) {
        if (result.accessDenied) {
            status.requiresElevation = true;
            status.errors.push_back(L"NTFS USN scan requires elevation or access was denied; using a recursive scan instead.");
        } else {
            status.errors.push_back(L"NTFS USN scan unavailable; using a recursive scan instead. (" + result.failureReason + L")");
        }
        IndexOneDriveRecursive(letter, status, options);
        return;
    }

    std::unique_lock lock(m_mutex);
    DriveData& data = m_driveData[letter];
    data.items = std::move(result.items);
    data.nodes.clear();
    for (const auto& item : data.items) {
        if (IsFrnGraphSource(item.source)) {
            UsnNode node;
            node.parentFrn = item.parentFileReferenceNumber;
            node.name = item.name;
            node.attributes = item.attributes;
            data.nodes[item.fileReferenceNumber] = std::move(node);
        }
    }
    data.reconstructor = std::make_unique<PathReconstructor>(std::wstring(1, letter) + L":\\");

    status.hasUsnJournal = result.hasUsnJournal;
    status.usnJournalId = result.usnJournalId;
    status.lastProcessedUsn = result.nextUsn;
    status.indexingMethod = L"NTFS USN";
}

void IndexManager::IndexOneDriveRecursive(wchar_t letter, DriveIndexStatus& status, const IndexOptions& options) {
    auto cancelPredicate = [this]() { return IsCancelled(); };
    auto progressCallback = [this, letter](uint64_t count, const std::wstring&) { PostProgress(letter, count); };

    RecursiveFileScanner::Options scanOptions;
    scanOptions.avoidReparsePoints = options.avoidReparsePoints;

    std::wstring root = std::wstring(1, letter) + L":\\";
    RecursiveFileScanner::Result result = RecursiveFileScanner::Scan(root, letter, scanOptions, cancelPredicate, progressCallback);

    if (result.accessDeniedCount > 0) {
        status.errors.push_back(L"Access denied on some folders (" + std::to_wstring(result.accessDeniedCount) + L").");
    }
    if (!result.success) {
        status.errors.push_back(result.failureReason);
    }

    std::unique_lock lock(m_mutex);
    DriveData& data = m_driveData[letter];
    data.items = std::move(result.items);
    data.nodes.clear();
    data.reconstructor.reset();
    status.hasUsnJournal = false;
    status.indexingMethod = L"Recursive";
}

bool IndexManager::TryIncrementalUpdate(wchar_t letter, DriveIndexStatus& status) {
    auto cancelPredicate = [this]() { return IsCancelled(); };
    auto result = UsnIncrementalUpdater::ReadChanges(letter, status.usnJournalId, status.lastProcessedUsn, cancelPredicate);

    if (!result.success) {
        status.errors.push_back(L"Incremental update failed; falling back to full rebuild.");
        return false;
    }
    if (result.journalInvalidated) {
        status.errors.push_back(L"Journal changed; full rebuild required.");
        return false;
    }

    auto dataIt = m_driveData.find(letter);
    if (dataIt == m_driveData.end() || !dataIt->second.reconstructor) {
        return false; // nothing to update incrementally against
    }
    DriveData& data = dataIt->second;

    for (const auto& change : result.changes) {
        if (change.kind == UsnIncrementalUpdater::ChangeKind::Deleted) {
            data.nodes.erase(change.frn);
            data.items.erase(std::remove_if(data.items.begin(), data.items.end(),
                [&](const IndexedItem& it) { return it.fileReferenceNumber == change.frn; }), data.items.end());
            continue;
        }

        UsnNode node;
        node.parentFrn = change.parentFrn;
        node.name = change.name;
        node.attributes = change.attributes;
        data.nodes[change.frn] = node;

        auto existing = std::find_if(data.items.begin(), data.items.end(),
            [&](const IndexedItem& it) { return it.fileReferenceNumber == change.frn; });

        IndexedItem item = (existing != data.items.end()) ? *existing : IndexedItem{};
        item.fileReferenceNumber = change.frn;
        item.parentFileReferenceNumber = change.parentFrn;
        item.name = change.name;
        item.attributes = change.attributes;
        item.isDirectory = (change.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        item.driveLetter = letter;
        item.source = L"NTFS USN";
        item.modifiedTime = change.timestamp;
        item.modifiedTimeKnown = true;
        if (!item.isDirectory) item.extension = StringUtil::GetExtension(item.name);

        // Only this item's own path is recomputed; descendants of a renamed folder keep their
        // previously resolved path until the next full rebuild (see UsnIncrementalUpdater.h).
        data.reconstructor->Resolve(change.frn, data.nodes, &item.diagnostics);
        item.fullPath = data.reconstructor->Resolve(change.frn, data.nodes, nullptr);

        if (existing != data.items.end()) {
            *existing = std::move(item);
        } else {
            data.items.push_back(std::move(item));
        }
    }

    status.lastProcessedUsn = result.nextUsn;
    return true;
}

std::wstring IndexManager::CacheFilePath(wchar_t letter) const {
    std::wstring dir = ShellHelpers::GetAppDataDirectory(/*localAppData=*/true);
    if (dir.empty()) return L"";
    dir += L"\\IndexCache";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\" + std::wstring(1, towupper(letter)) + L".cache";
}

void IndexManager::SaveDriveCache(wchar_t letter) {
    std::wstring path = CacheFilePath(letter);
    if (path.empty()) return;

    std::shared_lock lock(m_mutex);
    auto dataIt = m_driveData.find(towupper(letter));
    auto* status = const_cast<IndexManager*>(this)->FindStatusLocked(letter);
    if (dataIt == m_driveData.end() || status == nullptr) return;

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return;

    out.write(kCacheMagic, sizeof(kCacheMagic));
    WriteRaw(out, kCacheVersion);
    WriteRaw(out, letter);
    WriteRaw(out, status->volumeSerialNumber);
    WriteRaw(out, status->usnJournalId);
    WriteRaw(out, status->lastProcessedUsn);
    WriteRaw(out, status->hasUsnJournal);
    WriteRaw(out, status->lastIndexedTime);
    WriteRaw(out, status->lastIndexedTimeKnown);

    const auto& items = dataIt->second.items;
    uint64_t count = items.size();
    WriteRaw(out, count);

    for (const auto& item : items) {
        WriteRaw(out, item.fileReferenceNumber);
        WriteRaw(out, item.parentFileReferenceNumber);
        WriteWString(out, item.name);
        WriteWString(out, item.fullPath);
        WriteWString(out, item.extension);
        WriteRaw(out, item.driveLetter);
        WriteRaw(out, item.isDirectory);
        WriteRaw(out, item.attributes);
        WriteRaw(out, item.size);
        WriteRaw(out, item.sizeKnown);
        WriteRaw(out, item.createdTime);
        WriteRaw(out, item.createdTimeKnown);
        WriteRaw(out, item.modifiedTime);
        WriteRaw(out, item.modifiedTimeKnown);
        WriteWString(out, item.source);
    }
}

bool IndexManager::LoadDriveCache(wchar_t letter, DriveIndexStatus& status) {
    std::wstring path = CacheFilePath(letter);
    if (path.empty()) return false;

    std::ifstream in(path, std::ios::binary);
    if (!in) return false;

    char magic[4];
    in.read(magic, sizeof(magic));
    if (!in.good() || memcmp(magic, kCacheMagic, sizeof(magic)) != 0) return false;

    uint8_t version = 0;
    if (!ReadRaw(in, version) || version != kCacheVersion) return false;

    wchar_t cachedLetter = L'\0';
    DWORD serial = 0;
    DWORDLONG journalId = 0;
    USN lastUsn = 0;
    bool hasJournal = false;
    FILETIME builtTime{};
    bool builtTimeKnown = false;
    if (!ReadRaw(in, cachedLetter) || !ReadRaw(in, serial) || !ReadRaw(in, journalId) ||
        !ReadRaw(in, lastUsn) || !ReadRaw(in, hasJournal) ||
        !ReadRaw(in, builtTime) || !ReadRaw(in, builtTimeKnown)) {
        return false;
    }

    // Cache identity must match the live volume, otherwise it's stale (different disk/format).
    if (towupper(cachedLetter) != towupper(letter) || serial != status.volumeSerialNumber) {
        return false;
    }

    uint64_t count = 0;
    if (!ReadRaw(in, count) || count > 50'000'000ULL) return false; // sanity cap

    std::vector<IndexedItem> items;
    items.reserve(static_cast<size_t>(count));

    for (uint64_t i = 0; i < count; ++i) {
        IndexedItem item;
        if (!ReadRaw(in, item.fileReferenceNumber)) return false;
        if (!ReadRaw(in, item.parentFileReferenceNumber)) return false;
        if (!ReadWString(in, item.name)) return false;
        if (!ReadWString(in, item.fullPath)) return false;
        if (!ReadWString(in, item.extension)) return false;
        if (!ReadRaw(in, item.driveLetter)) return false;
        if (!ReadRaw(in, item.isDirectory)) return false;
        if (!ReadRaw(in, item.attributes)) return false;
        if (!ReadRaw(in, item.size)) return false;
        if (!ReadRaw(in, item.sizeKnown)) return false;
        if (!ReadRaw(in, item.createdTime)) return false;
        if (!ReadRaw(in, item.createdTimeKnown)) return false;
        if (!ReadRaw(in, item.modifiedTime)) return false;
        if (!ReadRaw(in, item.modifiedTimeKnown)) return false;
        if (!ReadWString(in, item.source)) return false;
        items.push_back(std::move(item));
    }

    DriveData data;
    data.items = std::move(items);
    if (hasJournal) {
        for (const auto& item : data.items) {
            if (IsFrnGraphSource(item.source)) {
                UsnNode node;
                node.parentFrn = item.parentFileReferenceNumber;
                node.name = item.name;
                node.attributes = item.attributes;
                data.nodes[item.fileReferenceNumber] = std::move(node);
            }
        }
        data.reconstructor = std::make_unique<PathReconstructor>(std::wstring(1, letter) + L":\\");
    }

    m_driveData[towupper(letter)] = std::move(data);

    status.usnJournalId = journalId;
    status.lastProcessedUsn = lastUsn;
    status.hasUsnJournal = hasJournal;
    status.itemCount = m_driveData[towupper(letter)].items.size();
    status.statusMessage = L"Indexed (from cache)";
    status.lastIndexedTime = builtTime;
    status.lastIndexedTimeKnown = builtTimeKnown;

    return true;
}

void IndexManager::DeleteDriveCache(wchar_t letter) {
    std::wstring path = CacheFilePath(letter);
    if (!path.empty()) {
        DeleteFileW(path.c_str());
    }
}
