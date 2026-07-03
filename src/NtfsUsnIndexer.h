#pragma once
// Fast NTFS metadata enumeration using the Windows-supported USN journal / MFT enumeration
// APIs (FSCTL_QUERY_USN_JOURNAL, FSCTL_ENUM_USN_DATA). Never parses raw NTFS structures.
//
// USN records give names, parent relationships, attributes and a change timestamp, but not
// file size. Size is left unknown here and resolved lazily by MetadataEnricher for visible
// search results (see MetadataEnricher.h).

#include "IndexTypes.h"

#include <windows.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class NtfsUsnIndexer {
public:
    struct Result {
        bool success = false;
        bool accessDenied = false;
        std::wstring failureReason;

        std::vector<IndexedItem> items;

        DWORDLONG usnJournalId = 0;
        USN nextUsn = 0;
        bool hasUsnJournal = false;
    };

    using CancelPredicate = std::function<bool()>;
    using ProgressCallback = std::function<void(uint64_t itemsProcessed)>;

    // Builds a full index for a single local NTFS drive (e.g. driveLetter = L'C').
    // Best-effort: on any failure, Result::success is false and failureReason explains why,
    // so the caller can fall back to RecursiveFileScanner.
    static Result BuildIndex(wchar_t driveLetter, const CancelPredicate& isCancelled,
                              const ProgressCallback& onProgress);
};
