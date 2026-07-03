#include "UsnIncrementalUpdater.h"
#include "FormatUtil.h"
#include "Handle.h"

#include <winioctl.h>
#include <vector>
#include <cstring>

namespace {
constexpr DWORD kBufferSize = 64 * 1024;
}

UsnIncrementalUpdater::Result UsnIncrementalUpdater::ReadChanges(wchar_t driveLetter, DWORDLONG expectedJournalId,
                                                                  USN startUsn, const CancelPredicate& isCancelled) {
    Result result;

    std::wstring volumePath = std::wstring(L"\\\\.\\") + driveLetter + L":";
    UniqueHandle volume(CreateFileW(
        volumePath.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));

    if (!volume.IsValid()) {
        result.success = false;
        result.failureReason = L"Could not open volume for incremental update: " + FormatUtil::FormatWin32Error(GetLastError());
        return result;
    }

    USN_JOURNAL_DATA_V0 journalData{};
    DWORD bytesReturned = 0;
    if (!DeviceIoControl(volume.Get(), FSCTL_QUERY_USN_JOURNAL, nullptr, 0,
                          &journalData, sizeof(journalData), &bytesReturned, nullptr)) {
        result.success = false;
        result.failureReason = L"FSCTL_QUERY_USN_JOURNAL failed: " + FormatUtil::FormatWin32Error(GetLastError());
        return result;
    }

    if (journalData.UsnJournalID != expectedJournalId || startUsn < journalData.LowestValidUsn) {
        // Journal was deleted/recreated, or our start point fell out of the retained range.
        result.success = true;
        result.journalInvalidated = true;
        return result;
    }

    READ_USN_JOURNAL_DATA_V0 readData{};
    readData.StartUsn = startUsn;
    readData.ReasonMask = 0xFFFFFFFF;
    readData.ReturnOnlyOnClose = FALSE;
    readData.Timeout = 0;
    readData.BytesToWaitFor = 0;
    readData.UsnJournalID = journalData.UsnJournalID;

    std::vector<BYTE> buffer(kBufferSize);
    USN currentUsn = startUsn;

    for (;;) {
        if (isCancelled && isCancelled()) {
            result.success = false;
            result.failureReason = L"Incremental update cancelled.";
            return result;
        }

        DWORD bytes = 0;
        BOOL ok = DeviceIoControl(volume.Get(), FSCTL_READ_USN_JOURNAL, &readData, sizeof(readData),
                                   buffer.data(), static_cast<DWORD>(buffer.size()), &bytes, nullptr);
        if (!ok) {
            result.success = false;
            result.failureReason = L"FSCTL_READ_USN_JOURNAL failed: " + FormatUtil::FormatWin32Error(GetLastError());
            return result;
        }

        if (bytes < sizeof(USN)) {
            break;
        }

        memcpy(&currentUsn, buffer.data(), sizeof(USN));

        DWORD offset = sizeof(USN);
        bool anyRecordThisBuffer = false;
        while (offset < bytes) {
            auto* header = reinterpret_cast<USN_RECORD_COMMON_HEADER*>(buffer.data() + offset);
            if (header->RecordLength == 0 || offset + header->RecordLength > bytes) {
                break;
            }
            anyRecordThisBuffer = true;

            if (header->MajorVersion == 2) {
                auto* rec = reinterpret_cast<USN_RECORD_V2*>(header);

                Change change;
                change.frn = rec->FileReferenceNumber;
                change.parentFrn = rec->ParentFileReferenceNumber;
                change.attributes = rec->FileAttributes;
                change.timestamp.dwLowDateTime = rec->TimeStamp.LowPart;
                change.timestamp.dwHighDateTime = static_cast<DWORD>(rec->TimeStamp.HighPart);

                const wchar_t* namePtr = reinterpret_cast<const wchar_t*>(
                    reinterpret_cast<const BYTE*>(rec) + rec->FileNameOffset);
                change.name.assign(namePtr, rec->FileNameLength / sizeof(wchar_t));

                if (rec->Reason & USN_REASON_FILE_DELETE) {
                    change.kind = ChangeKind::Deleted;
                } else if (rec->Reason & USN_REASON_FILE_CREATE) {
                    change.kind = ChangeKind::Created;
                } else {
                    change.kind = ChangeKind::RenamedOrModified;
                }

                result.changes.push_back(std::move(change));
            }
            // V3/V4 incremental records are uncommon on local NTFS volumes; skipped defensively
            // rather than risking a malformed-cast crash (fast full-enumeration path already
            // handles V3 for the initial index).

            offset += header->RecordLength;
        }

        if (!anyRecordThisBuffer) {
            break;
        }

        readData.StartUsn = currentUsn;
    }

    result.success = true;
    result.nextUsn = currentUsn;
    return result;
}
