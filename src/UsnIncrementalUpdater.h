#pragma once
// Best-effort incremental index update using FSCTL_READ_USN_JOURNAL. Reads USN journal
// records since the last processed USN and decodes them into simple Change entries; the
// caller (IndexManager) is responsible for applying those changes to its in-memory node map
// and item list.
//
// TODO (documented v1 simplification): applying a rename/move to a directory updates that
// directory's own entry but does not cascade to recompute the paths of its descendants
// already in the index. Those descendants remain visible under their previous path until
// the next full rebuild. This keeps the incremental path well isolated from the rest of the
// index as required, at the cost of transient staleness for deep moves.

#include "IndexTypes.h"

#include <windows.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class UsnIncrementalUpdater {
public:
    enum class ChangeKind {
        Created,
        Deleted,
        RenamedOrModified, // covers rename-new-name and general metadata/name changes
    };

    struct Change {
        ChangeKind kind = ChangeKind::RenamedOrModified;
        uint64_t frn = 0;
        uint64_t parentFrn = 0;
        std::wstring name;
        uint32_t attributes = 0;
        FILETIME timestamp{};
    };

    struct Result {
        bool success = false;
        std::wstring failureReason;
        bool journalInvalidated = false; // journal ID changed or USN fell out of range: caller must do a full rebuild
        std::vector<Change> changes;
        USN nextUsn = 0;
    };

    using CancelPredicate = std::function<bool()>;

    static Result ReadChanges(wchar_t driveLetter, DWORDLONG expectedJournalId, USN startUsn,
                               const CancelPredicate& isCancelled);
};
