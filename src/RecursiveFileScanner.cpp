#include "RecursiveFileScanner.h"
#include "StringUtil.h"
#include "FormatUtil.h"
#include "Handle.h"

#include <windows.h>
#include <vector>

IndexedItem RecursiveFileScanner::BuildItem(const WIN32_FIND_DATAW& findData, const std::wstring& fullPath, wchar_t driveLetter) {
    IndexedItem item;
    item.name = findData.cFileName;
    item.fullPath = fullPath;
    item.driveLetter = driveLetter;
    item.isDirectory = (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    item.attributes = findData.dwFileAttributes;
    item.source = L"Recursive";

    if (!item.isDirectory) {
        item.extension = StringUtil::GetExtension(item.name);
        ULARGE_INTEGER size{};
        size.LowPart = findData.nFileSizeLow;
        size.HighPart = findData.nFileSizeHigh;
        item.size = size.QuadPart;
        item.sizeKnown = true;
    }

    item.createdTime = findData.ftCreationTime;
    item.createdTimeKnown = true;
    item.modifiedTime = findData.ftLastWriteTime;
    item.modifiedTimeKnown = true;

    return item;
}

RecursiveFileScanner::Result RecursiveFileScanner::Scan(const std::wstring& rootPath, wchar_t driveLetter,
                                                          const Options& options, const CancelPredicate& isCancelled,
                                                          const ProgressCallback& onProgress) {
    Result result;

    std::vector<std::wstring> pending;
    pending.push_back(rootPath);

    uint64_t processed = 0;
    const uint64_t kProgressInterval = 500;

    while (!pending.empty()) {
        if (isCancelled && isCancelled()) {
            result.success = false;
            result.failureReason = L"Indexing cancelled.";
            return result;
        }

        std::wstring dir = std::move(pending.back());
        pending.pop_back();

        std::wstring searchPattern = dir;
        if (!searchPattern.empty() && searchPattern.back() != L'\\') searchPattern += L'\\';
        searchPattern += L'*';

        WIN32_FIND_DATAW findData{};
        UniqueFindHandle findHandle(FindFirstFileExW(
            searchPattern.c_str(), FindExInfoBasic, &findData, FindExSearchNameMatch, nullptr,
            FIND_FIRST_EX_LARGE_FETCH));

        if (!findHandle.IsValid()) {
            DWORD err = GetLastError();
            if (err == ERROR_ACCESS_DENIED) {
                ++result.accessDeniedCount;
            }
            // Not fatal for the whole scan: this directory is skipped, siblings continue.
            continue;
        }

        do {
            const wchar_t* name = findData.cFileName;
            if (wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0) continue;

            bool isDir = (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            bool isReparse = (findData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;

            std::wstring fullPath = dir;
            if (!fullPath.empty() && fullPath.back() != L'\\') fullPath += L'\\';
            fullPath += name;

            IndexedItem item = BuildItem(findData, fullPath, driveLetter);

            ++processed;
            result.items.push_back(std::move(item));

            if (isDir && !(isReparse && options.avoidReparsePoints)) {
                pending.push_back(fullPath);
            }

            if (onProgress && (processed % kProgressInterval) == 0) {
                onProgress(processed, fullPath);
            }
            if ((processed % kProgressInterval) == 0 && isCancelled && isCancelled()) {
                result.success = false;
                result.failureReason = L"Indexing cancelled.";
                return result;
            }
        } while (FindNextFileW(findHandle.Get(), &findData));
    }

    if (onProgress) {
        onProgress(processed, rootPath);
    }

    result.success = true;
    return result;
}
