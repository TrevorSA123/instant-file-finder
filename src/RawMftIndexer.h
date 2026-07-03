#pragma once
// Fastest available indexing method: reads the Master File Table directly as a byte stream via
// the documented "\\.\C:\$MFT" pseudo-file path and parses NTFS file records/attributes by
// hand, instead of going through FSCTL_ENUM_USN_DATA one record at a time (NtfsUsnIndexer).
// This still goes through a normal Windows file handle and ReadFile - it does not touch the
// disk directly or bypass the filesystem driver - but avoids the per-record API call overhead
// that FSCTL_ENUM_USN_DATA has, which is what makes it faster.
//
// This typically requires the process to be elevated (opening $MFT needs SeBackupPrivilege,
// which admin tokens can enable but standard tokens cannot); BuildIndex() reports that via
// Result::accessDenied so the caller can fall back to NtfsUsnIndexer.
//
// Deliberate v1 simplifications (documented, not bugs):
//  - Attributes that spill into MFT "extension" records (rare: files with very many hard links,
//    alternate data streams, or fragmented attribute lists) are not followed; such a file's name
//    may simply be missing from the index rather than causing an error.
//  - A file with multiple hard links has one $FILE_NAME attribute per link; only the first
//    non-DOS-8.3 name found is used, so only one of its paths is indexed.
//  - Compressed/sparse files' "valid data length" nuances are not modeled beyond the standard
//    real-size field also used by Explorer's file listings.

#include "IndexTypes.h"

#include <windows.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class RawMftIndexer {
public:
    struct Result {
        bool success = false;
        bool accessDenied = false;
        std::wstring failureReason;

        std::vector<IndexedItem> items;

        // Captured the same way NtfsUsnIndexer does, so a raw-MFT-built index can still be kept
        // up to date afterwards via the normal USN journal incremental updater.
        DWORDLONG usnJournalId = 0;
        USN nextUsn = 0;
        bool hasUsnJournal = false;
    };

    using CancelPredicate = std::function<bool()>;
    using ProgressCallback = std::function<void(uint64_t itemsProcessed)>;

    static Result BuildIndex(wchar_t driveLetter, const CancelPredicate& isCancelled,
                              const ProgressCallback& onProgress);
};
