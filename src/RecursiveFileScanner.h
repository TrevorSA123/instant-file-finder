#pragma once
// Fallback scanner for non-NTFS volumes (or when the fast USN scan is unavailable/denied).
// Produces the same IndexedItem shape as NtfsUsnIndexer, marked with source = "Recursive".

#include "IndexTypes.h"

#include <functional>
#include <string>
#include <vector>

class RecursiveFileScanner {
public:
    struct Result {
        bool success = false;
        std::wstring failureReason;
        std::vector<IndexedItem> items;
        uint64_t accessDeniedCount = 0;
    };

    using CancelPredicate = std::function<bool()>;
    using ProgressCallback = std::function<void(uint64_t itemsProcessed, const std::wstring& currentPath)>;

    struct Options {
        bool avoidReparsePoints = true;
        bool includeHidden = true;
        bool includeSystem = true; // filtering for display happens in SearchEngine; scanner collects broadly
    };

    // Recursively scans starting at rootPath (e.g. L"D:\\"). Uses an explicit stack rather
    // than function recursion so pathological directory depth cannot overflow the stack.
    static Result Scan(const std::wstring& rootPath, wchar_t driveLetter, const Options& options,
                        const CancelPredicate& isCancelled, const ProgressCallback& onProgress);

    // Builds an IndexedItem from a WIN32_FIND_DATAW entry. Shared with LiveSearchManager, which
    // walks the filesystem the same way but matches/streams results instead of collecting a
    // full index.
    static IndexedItem BuildItem(const WIN32_FIND_DATAW& findData, const std::wstring& fullPath, wchar_t driveLetter);
};
