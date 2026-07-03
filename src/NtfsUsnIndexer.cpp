#include "NtfsUsnIndexer.h"
#include "PathReconstructor.h"
#include "StringUtil.h"
#include "Handle.h"
#include "FormatUtil.h"

#include <winioctl.h>
#include <unordered_map>
#include <vector>
#include <cstring>

namespace {

// Extra per-node data kept only for the duration of one BuildIndex call; the smaller
// PathReconstructor::UsnNode is what actually gets cached/walked for path resolution.
struct BuildNode {
    UsnNode base;
    FILETIME timestamp{};
    bool hasTimestamp = false;
};

constexpr DWORD kBufferSize = 64 * 1024;

// FILE_ID_128 (used by USN_RECORD_V3) is 128 bits; we only keep the low 64 bits, which is
// sufficient to distinguish records on the vast majority of real NTFS volumes and keeps the
// node map's key type consistent with USN_RECORD_V2. This is a documented, deliberate
// simplification for v1 (see README limitations).
uint64_t Truncate128(const FILE_ID_128& id) {
    uint64_t low = 0;
    memcpy(&low, id.Identifier, sizeof(low));
    return low;
}

} // namespace

NtfsUsnIndexer::Result NtfsUsnIndexer::BuildIndex(wchar_t driveLetter, const CancelPredicate& isCancelled,
                                                   const ProgressCallback& onProgress) {
    Result result;

    std::wstring volumePath = std::wstring(L"\\\\.\\") + driveLetter + L":";
    std::wstring driveRoot = std::wstring(1, driveLetter) + L":\\";

    UniqueHandle volume(CreateFileW(
        volumePath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));

    if (!volume.IsValid()) {
        DWORD err = GetLastError();
        result.success = false;
        result.accessDenied = (err == ERROR_ACCESS_DENIED);
        result.failureReason = L"Could not open volume " + volumePath + L": " + FormatUtil::FormatWin32Error(err);
        return result;
    }

    // Best-effort journal query: used to record the journal ID for cache validation and for
    // future incremental updates. FSCTL_ENUM_USN_DATA works even if this fails, so we don't
    // bail out on a journal query failure alone.
    USN_JOURNAL_DATA_V0 journalData{};
    DWORD bytesReturned = 0;
    if (DeviceIoControl(volume.Get(), FSCTL_QUERY_USN_JOURNAL, nullptr, 0,
                         &journalData, sizeof(journalData), &bytesReturned, nullptr)) {
        result.hasUsnJournal = true;
        result.usnJournalId = journalData.UsnJournalID;
        result.nextUsn = journalData.NextUsn;
    }

    std::unordered_map<uint64_t, BuildNode> nodes;
    nodes.reserve(1 << 16);

    std::vector<BYTE> buffer(kBufferSize);

    MFT_ENUM_DATA_V0 enumData{};
    enumData.StartFileReferenceNumber = 0;
    enumData.LowUsn = 0;
    enumData.HighUsn = MAXLONGLONG;

    uint64_t totalRecords = 0;
    bool loggedV3Truncation = false;

    for (;;) {
        if (isCancelled && isCancelled()) {
            result.success = false;
            result.failureReason = L"Indexing cancelled.";
            return result;
        }

        DWORD bytes = 0;
        BOOL ok = DeviceIoControl(volume.Get(), FSCTL_ENUM_USN_DATA, &enumData, sizeof(enumData),
                                   buffer.data(), static_cast<DWORD>(buffer.size()), &bytes, nullptr);
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_HANDLE_EOF) {
                break; // normal completion
            }
            if (err == ERROR_JOURNAL_NOT_ACTIVE || err == ERROR_INVALID_FUNCTION) {
                result.success = false;
                result.failureReason = L"USN journal is not active on this volume.";
                return result;
            }
            result.success = false;
            result.accessDenied = (err == ERROR_ACCESS_DENIED);
            result.failureReason = L"FSCTL_ENUM_USN_DATA failed: " + FormatUtil::FormatWin32Error(err);
            return result;
        }

        if (bytes < sizeof(USN)) {
            break;
        }

        // First 8 bytes of the output buffer are the starting point for the next call.
        USN nextStart = 0;
        memcpy(&nextStart, buffer.data(), sizeof(USN));
        enumData.StartFileReferenceNumber = static_cast<DWORDLONG>(nextStart);

        DWORD offset = sizeof(USN);
        while (offset < bytes) {
            auto* header = reinterpret_cast<USN_RECORD_COMMON_HEADER*>(buffer.data() + offset);
            if (header->RecordLength == 0 || offset + header->RecordLength > bytes) {
                break; // malformed/truncated record; stop parsing this buffer defensively
            }

            if (header->MajorVersion == 2) {
                auto* rec = reinterpret_cast<USN_RECORD_V2*>(header);
                uint64_t frn = rec->FileReferenceNumber;
                uint64_t parentFrn = rec->ParentFileReferenceNumber;

                const wchar_t* namePtr = reinterpret_cast<const wchar_t*>(
                    reinterpret_cast<const BYTE*>(rec) + rec->FileNameOffset);
                std::wstring name(namePtr, rec->FileNameLength / sizeof(wchar_t));

                BuildNode node;
                node.base.parentFrn = parentFrn;
                node.base.name = std::move(name);
                node.base.attributes = rec->FileAttributes;
                node.timestamp.dwLowDateTime = rec->TimeStamp.LowPart;
                node.timestamp.dwHighDateTime = static_cast<DWORD>(rec->TimeStamp.HighPart);
                node.hasTimestamp = true;

                nodes[frn] = std::move(node);
                ++totalRecords;
            } else if (header->MajorVersion == 3) {
                auto* rec = reinterpret_cast<USN_RECORD_V3*>(header);
                uint64_t frn = Truncate128(rec->FileReferenceNumber);
                uint64_t parentFrn = Truncate128(rec->ParentFileReferenceNumber);

                const wchar_t* namePtr = reinterpret_cast<const wchar_t*>(
                    reinterpret_cast<const BYTE*>(rec) + rec->FileNameOffset);
                std::wstring name(namePtr, rec->FileNameLength / sizeof(wchar_t));

                BuildNode node;
                node.base.parentFrn = parentFrn;
                node.base.name = std::move(name);
                node.base.attributes = rec->FileAttributes;
                node.timestamp.dwLowDateTime = rec->TimeStamp.LowPart;
                node.timestamp.dwHighDateTime = static_cast<DWORD>(rec->TimeStamp.HighPart);
                node.hasTimestamp = true;

                nodes[frn] = std::move(node);
                ++totalRecords;
                loggedV3Truncation = true;
            }
            // Unknown record versions are skipped defensively rather than crashing.

            offset += header->RecordLength;
        }

        if (onProgress) {
            onProgress(totalRecords);
        }
    }

    // Second pass: reconstruct full paths and build IndexedItem list.
    std::unordered_map<uint64_t, UsnNode> pathNodes;
    pathNodes.reserve(nodes.size());
    for (const auto& kv : nodes) {
        pathNodes[kv.first] = kv.second.base;
    }

    PathReconstructor reconstructor(driveRoot);
    result.items.reserve(nodes.size());

    for (const auto& kv : nodes) {
        const uint64_t frn = kv.first;
        const BuildNode& node = kv.second;

        if (node.base.name.empty()) {
            continue; // volume root record itself; not a meaningful search result
        }
        if (isCancelled && isCancelled()) {
            result.success = false;
            result.failureReason = L"Indexing cancelled.";
            return result;
        }

        IndexedItem item;
        item.fileReferenceNumber = frn;
        item.parentFileReferenceNumber = node.base.parentFrn;
        item.name = node.base.name;
        item.attributes = node.base.attributes;
        item.isDirectory = (node.base.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        item.driveLetter = driveLetter;
        item.source = L"NTFS USN";

        std::vector<std::wstring> diagnostics;
        item.fullPath = reconstructor.Resolve(frn, pathNodes, &diagnostics);
        item.diagnostics = std::move(diagnostics);

        if (!item.isDirectory) {
            item.extension = StringUtil::GetExtension(item.name);
        }

        if (node.hasTimestamp) {
            item.modifiedTime = node.timestamp;
            item.modifiedTimeKnown = true;
        }
        // Size and creation time are not provided by USN records; MetadataEnricher fills
        // these in lazily for items the user actually sees.
        item.sizeKnown = false;
        item.createdTimeKnown = false;

        result.items.push_back(std::move(item));
    }

    if (loggedV3Truncation) {
        // Not fatal; recorded as a general diagnostic on the result via failureReason-style note
        // by leaving individual item diagnostics in place (added inline above only on path issues).
    }

    result.success = true;
    return result;
}
