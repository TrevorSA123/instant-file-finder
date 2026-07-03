#pragma once
// Shared data model for the index: the item shape produced by both indexers,
// per-drive status tracking, and the options structure consumed by SearchEngine.

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

// A single indexed file or folder.
struct IndexedItem {
    uint64_t fileReferenceNumber = 0;
    uint64_t parentFileReferenceNumber = 0;

    std::wstring name;
    std::wstring fullPath;
    std::wstring extension;

    wchar_t driveLetter = L'\0';
    bool isDirectory = false;
    uint32_t attributes = 0;

    uint64_t size = 0;
    bool sizeKnown = false;

    FILETIME createdTime{};
    bool createdTimeKnown = false;

    FILETIME modifiedTime{};
    bool modifiedTimeKnown = false;

    std::wstring source; // "NTFS USN", "Raw MFT", or "Recursive"

    std::vector<std::wstring> diagnostics;
};

enum class ScanMethod {
    Disabled,
    FastNtfsUsn,
    RawMft,
    Recursive,
};

// Per-drive indexing state, shown in the UI and the Index Drives dialog.
struct DriveIndexStatus {
    std::wstring driveRoot;      // e.g. L"C:\\"
    std::wstring label;          // volume label
    std::wstring fileSystem;     // e.g. NTFS, FAT32
    UINT driveType = 0;          // GetDriveTypeW result
    std::wstring indexingMethod; // human readable, e.g. "NTFS USN" / "Raw MFT" / "Recursive" / "Disabled"
    ScanMethod scanMethod = ScanMethod::Disabled;
    bool selected = false;       // user opted this drive in

    bool indexed = false;
    bool indexing = false;
    bool requiresElevation = false;

    uint64_t itemCount = 0;
    std::wstring statusMessage;
    std::vector<std::wstring> errors;

    // Non-empty when the actually-used method ended up lower than the configured scanMethod
    // ceiling for this drive (e.g. Raw MFT needs Administrator, which isn't available). Empty
    // when no downgrade happened. Shown in the status bar so it's visible while searching, not
    // just in the one-time notice shown right after indexing.
    std::wstring downgradeReason;

    DWORD volumeSerialNumber = 0;
    DWORDLONG usnJournalId = 0;
    USN lastProcessedUsn = 0;
    bool hasUsnJournal = false;

    // When this drive's index was last built/refreshed (from a fresh scan, an incremental
    // update, or the timestamp stored in an on-disk cache). Used to warn when the index is old.
    FILETIME lastIndexedTime{};
    bool lastIndexedTimeKnown = false;
};

enum class SearchMode {
    Contains,
    StartsWith,
    EndsWith,
    Exact,
    Wildcard,
    Regex,
};

enum class TypeFilter {
    FilesAndFolders,
    FilesOnly,
    FoldersOnly,
};

struct SearchOptions {
    std::wstring query;
    SearchMode mode = SearchMode::Contains;
    std::wstring driveFilter; // empty or L"*" = all indexed drives
    bool filesOnly = false;
    bool foldersOnly = false;
    bool caseSensitive = false;
    bool includeHidden = true;
    bool includeSystem = false;
    bool useRegex = false;
    bool useWildcard = false;

    std::wstring extensionFilter; // from ext:
    std::wstring pathSubstring;   // from path:

    uint64_t minSize = 0;
    uint64_t maxSize = 0;
    bool sizeFilterEnabled = false;

    FILETIME modifiedAfter{};
    FILETIME modifiedBefore{};
    bool modifiedDateFilterEnabled = false;

    size_t maxResults = 10000;
};
