#include "RawMftIndexer.h"
#include "PathReconstructor.h"
#include "StringUtil.h"
#include "Handle.h"
#include "FormatUtil.h"

#include <winioctl.h>
#include <algorithm>
#include <unordered_map>
#include <utility>
#include <vector>
#include <cstring>

namespace {

// Bounds-checked view over one file record's bytes. Nothing in this file trusts on-disk data to
// be well-formed: every multi-byte read is validated against the buffer size before touching
// memory, so a corrupt or unexpected record is skipped rather than risking an out-of-bounds read.
class ByteReader {
public:
    ByteReader(const BYTE* data, size_t size) : m_data(data), m_size(size) {}

    template <typename T>
    bool Read(size_t offset, T& out) const {
        if (offset + sizeof(T) > m_size) return false;
        memcpy(&out, m_data + offset, sizeof(T));
        return true;
    }

    bool ReadBytes(size_t offset, size_t len, void* out) const {
        if (len > 0 && offset + len > m_size) return false;
        if (len > 0) memcpy(out, m_data + offset, len);
        return true;
    }

    size_t Size() const { return m_size; }

private:
    const BYTE* m_data;
    size_t m_size;
};

constexpr DWORD kFileRecordMagic = 0x454C4946; // "FILE"
constexpr DWORD kAttrEndMarker = 0xFFFFFFFFu;
constexpr DWORD kAttrStandardInformation = 0x10;
constexpr DWORD kAttrFileName = 0x30;
constexpr DWORD kAttrData = 0x80;
constexpr uint16_t kFlagInUse = 0x0001;
constexpr uint16_t kFlagIsDirectory = 0x0002;
constexpr uint32_t kReservedRecordCount = 16; // $MFT, $MFTMirr, ..., root directory, $Extend, etc.

FILETIME Int64ToFileTime(int64_t value) {
    ULARGE_INTEGER u{};
    u.QuadPart = static_cast<uint64_t>(value);
    FILETIME ft{};
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    return ft;
}

// Applies the NTFS "update sequence array" fixup in place: the last two bytes of every sector in
// a file record are overwritten on disk with a sequence tag, and the real bytes are stashed in
// the record header for exactly this purpose. Returns false (record unusable) if the stored tag
// doesn't match what's on disk, which indicates a torn/inconsistent read rather than a record we
// should trust.
bool ApplyFixup(BYTE* record, size_t recordSize, DWORD bytesPerSector) {
    if (bytesPerSector == 0 || bytesPerSector > recordSize) return false;

    ByteReader reader(record, recordSize);
    uint16_t usaOffset = 0, usaCount = 0;
    if (!reader.Read(4, usaOffset) || !reader.Read(6, usaCount)) return false;
    if (usaCount == 0) return false;

    size_t sectorCount = recordSize / bytesPerSector;
    if (static_cast<size_t>(usaCount) - 1 < sectorCount) sectorCount = usaCount - 1;

    size_t usaBytes = static_cast<size_t>(usaCount) * 2;
    if (static_cast<size_t>(usaOffset) + usaBytes > recordSize) return false;

    uint16_t tag = 0;
    memcpy(&tag, record + usaOffset, sizeof(tag));

    for (size_t i = 0; i < sectorCount; ++i) {
        size_t sectorLastWord = (i + 1) * bytesPerSector - 2;
        uint16_t onDisk = 0;
        memcpy(&onDisk, record + sectorLastWord, sizeof(onDisk));
        if (onDisk != tag) {
            return false; // torn or corrupt sector; don't trust this record
        }
        uint16_t original = 0;
        memcpy(&original, record + usaOffset + 2 + i * 2, sizeof(original));
        memcpy(record + sectorLastWord, &original, sizeof(original));
    }
    return true;
}

struct ParsedRecord {
    bool valid = false;
    bool isDirectory = false;
    uint16_t sequenceNumber = 0;

    bool hasStdAttributes = false;
    uint32_t stdAttributes = 0;
    bool hasCreatedTime = false;
    FILETIME createdTime{};
    bool hasModifiedTime = false;
    FILETIME modifiedTime{};

    bool hasDataSize = false;
    uint64_t dataSize = 0;

    bool hasFileName = false;
    bool fileNameIsDosOnly = false;
    uint64_t parentFrn = 0;
    std::wstring name;
    uint32_t fileNameAttributes = 0;
    uint64_t fileNameRealSize = 0;
};

// Parses one already-fixed-up file record. recordIndex is this record's position in $MFT
// (== its base MFT record number), used to reconstruct the record's own file reference number.
ParsedRecord ParseRecord(const BYTE* record, size_t recordSize) {
    ParsedRecord out;
    ByteReader reader(record, recordSize);

    DWORD magic = 0;
    if (!reader.Read(0, magic) || magic != kFileRecordMagic) return out;

    uint16_t seqNumber = 0, firstAttrOffset = 0, flags = 0;
    uint32_t bytesInUse = 0;
    uint64_t baseRecordRef = 0;
    if (!reader.Read(0x10, seqNumber) || !reader.Read(0x14, firstAttrOffset) ||
        !reader.Read(0x16, flags) || !reader.Read(0x18, bytesInUse) || !reader.Read(0x20, baseRecordRef)) {
        return out;
    }

    if ((flags & kFlagInUse) == 0) return out;         // free/deleted slot
    if ((baseRecordRef & 0xFFFFFFFFFFFFULL) != 0) return out; // extension record, not a base record

    if (bytesInUse > recordSize) bytesInUse = static_cast<uint32_t>(recordSize);

    out.sequenceNumber = seqNumber;
    out.isDirectory = (flags & kFlagIsDirectory) != 0;

    size_t offset = firstAttrOffset;
    while (offset + 8 <= bytesInUse) {
        DWORD typeCode = 0, attrLen = 0;
        if (!reader.Read(offset, typeCode)) break;
        if (typeCode == kAttrEndMarker) break;
        if (!reader.Read(offset + 4, attrLen)) break;
        if (attrLen < 8 || offset + attrLen > recordSize) break; // malformed; stop walking defensively

        BYTE nonResident = 0, nameLength = 0;
        reader.Read(offset + 8, nonResident);
        reader.Read(offset + 9, nameLength);

        if (typeCode == kAttrStandardInformation && nonResident == 0) {
            uint16_t valueOffset = 0;
            if (reader.Read(offset + 0x14, valueOffset)) {
                size_t v = offset + valueOffset;
                int64_t created = 0, modified = 0;
                uint32_t attrs = 0;
                if (reader.Read(v + 0x00, created)) { out.createdTime = Int64ToFileTime(created); out.hasCreatedTime = true; }
                if (reader.Read(v + 0x08, modified)) { out.modifiedTime = Int64ToFileTime(modified); out.hasModifiedTime = true; }
                if (reader.Read(v + 0x20, attrs)) { out.stdAttributes = attrs; out.hasStdAttributes = true; }
            }
        } else if (typeCode == kAttrFileName && nonResident == 0) {
            uint16_t valueOffset = 0;
            if (reader.Read(offset + 0x14, valueOffset)) {
                size_t v = offset + valueOffset;
                uint64_t parentRef = 0;
                uint64_t realSize = 0;
                uint32_t fileAttrs = 0;
                BYTE nameCharCount = 0, nameType = 0;
                bool ok = reader.Read(v + 0x00, parentRef) && reader.Read(v + 0x30, realSize) &&
                          reader.Read(v + 0x38, fileAttrs) && reader.Read(v + 0x40, nameCharCount) &&
                          reader.Read(v + 0x41, nameType);
                if (ok) {
                    size_t nameBytes = static_cast<size_t>(nameCharCount) * sizeof(wchar_t);
                    std::wstring name;
                    if (nameBytes > 0) {
                        name.resize(nameCharCount);
                        ok = reader.ReadBytes(v + 0x42, nameBytes, name.data());
                    }
                    bool isDosOnly = (nameType == 2);
                    // Prefer the first Win32/POSIX name; only fall back to a DOS-8.3-only name
                    // if that's all this record has.
                    if (ok && !name.empty() && (!out.hasFileName || (out.fileNameIsDosOnly && !isDosOnly))) {
                        out.hasFileName = true;
                        out.fileNameIsDosOnly = isDosOnly;
                        out.parentFrn = parentRef;
                        out.name = std::move(name);
                        out.fileNameAttributes = fileAttrs;
                        out.fileNameRealSize = realSize;
                    }
                }
            }
        } else if (typeCode == kAttrData && nameLength == 0 && !out.hasDataSize) {
            if (nonResident == 0) {
                uint32_t valueLength = 0;
                if (reader.Read(offset + 0x10, valueLength)) {
                    out.dataSize = valueLength;
                    out.hasDataSize = true;
                }
            } else {
                uint64_t realSize = 0;
                if (reader.Read(offset + 0x30, realSize)) {
                    out.dataSize = realSize;
                    out.hasDataSize = true;
                }
            }
        }

        offset += attrLen;
    }

    out.valid = out.hasFileName;
    return out;
}

} // namespace

RawMftIndexer::Result RawMftIndexer::BuildIndex(wchar_t driveLetter, const CancelPredicate& isCancelled,
                                                  const ProgressCallback& onProgress) {
    Result result;

    std::wstring volumePath = std::wstring(L"\\\\.\\") + driveLetter + L":";
    std::wstring driveRoot = std::wstring(1, driveLetter) + L":\\";
    std::wstring mftPath = std::wstring(L"\\\\.\\") + driveLetter + L":\\$MFT";

    // Enabling SeBackupPrivilege is what lets an elevated process open $MFT directly; a non-
    // elevated process's token won't have the privilege available to enable, and the later
    // CreateFileW call will fail with access denied, which is reported normally below.
    {
        HANDLE rawToken = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &rawToken)) {
            UniqueHandle token(rawToken);
            TOKEN_PRIVILEGES priv{};
            if (LookupPrivilegeValueW(nullptr, SE_BACKUP_NAME, &priv.Privileges[0].Luid)) {
                priv.PrivilegeCount = 1;
                priv.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
                AdjustTokenPrivileges(token.Get(), FALSE, &priv, sizeof(priv), nullptr, nullptr);
                // A non-zero return from AdjustTokenPrivileges doesn't guarantee the privilege
                // was actually granted (GetLastError() may still report
                // ERROR_NOT_ALL_ASSIGNED); either way we just attempt the open below and trust
                // its own success/failure.
            }
        }
    }

    // FSCTL_GET_NTFS_VOLUME_DATA needs a handle to the volume itself, separate from $MFT.
    UniqueHandle volume(CreateFileW(
        volumePath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!volume.IsValid()) {
        DWORD err = GetLastError();
        result.accessDenied = (err == ERROR_ACCESS_DENIED);
        result.failureReason = L"Could not open volume " + volumePath + L": " + FormatUtil::FormatWin32Error(err);
        return result;
    }

    NTFS_VOLUME_DATA_BUFFER volData{};
    DWORD bytesReturned = 0;
    if (!DeviceIoControl(volume.Get(), FSCTL_GET_NTFS_VOLUME_DATA, nullptr, 0, &volData, sizeof(volData),
                          &bytesReturned, nullptr)) {
        DWORD err = GetLastError();
        result.accessDenied = (err == ERROR_ACCESS_DENIED);
        result.failureReason = L"FSCTL_GET_NTFS_VOLUME_DATA failed (not an NTFS volume?): " + FormatUtil::FormatWin32Error(err);
        return result;
    }

    USN_JOURNAL_DATA_V0 journalData{};
    if (DeviceIoControl(volume.Get(), FSCTL_QUERY_USN_JOURNAL, nullptr, 0, &journalData, sizeof(journalData),
                         &bytesReturned, nullptr)) {
        result.hasUsnJournal = true;
        result.usnJournalId = journalData.UsnJournalID;
        result.nextUsn = journalData.NextUsn;
    }

    UniqueHandle mft(CreateFileW(
        mftPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!mft.IsValid()) {
        DWORD err = GetLastError();
        result.accessDenied = (err == ERROR_ACCESS_DENIED);
        result.failureReason = L"Could not open " + mftPath + L": " + FormatUtil::FormatWin32Error(err);
        return result;
    }

    const DWORD recordSize = volData.BytesPerFileRecordSegment;
    const DWORD sectorSize = volData.BytesPerSector;
    if (recordSize == 0 || sectorSize == 0 || recordSize > (16 * 1024)) {
        result.failureReason = L"Unexpected NTFS volume geometry; falling back.";
        return result;
    }

    uint64_t mftValidBytes = static_cast<uint64_t>(volData.MftValidDataLength.QuadPart);
    uint64_t totalRecords = mftValidBytes / recordSize;

    // Read in large sequential chunks (ReadFile on the $MFT handle walks it like a normal file;
    // Windows resolves this to the underlying disk extents) rather than one record at a time,
    // which is the main source of this method's speed advantage over FSCTL_ENUM_USN_DATA.
    const size_t recordsPerChunk = (4 * 1024 * 1024) / recordSize;
    std::vector<BYTE> chunk(static_cast<size_t>(recordsPerChunk) * recordSize);

    std::unordered_map<uint64_t, UsnNode> nodes;
    nodes.reserve(1 << 16);
    // Size and timestamps aren't part of UsnNode/PathReconstructor's shape (that's shared with
    // NtfsUsnIndexer, which doesn't have this data), so they're kept in side maps keyed by frn
    // and merged into IndexedItems in the second pass below.
    std::unordered_map<uint64_t, std::pair<uint64_t, bool>> sizes; // frn -> (size, known)
    struct TimeInfo {
        FILETIME created{};
        bool createdKnown = false;
        FILETIME modified{};
        bool modifiedKnown = false;
    };
    std::unordered_map<uint64_t, TimeInfo> times;

    uint64_t recordIndex = 0;
    uint64_t processed = 0;

    while (recordIndex < totalRecords) {
        if (isCancelled && isCancelled()) {
            result.failureReason = L"Indexing cancelled.";
            return result;
        }

        DWORD toRead = static_cast<DWORD>(std::min<uint64_t>(chunk.size(), (totalRecords - recordIndex) * recordSize));
        DWORD actuallyRead = 0;
        if (!ReadFile(mft.Get(), chunk.data(), toRead, &actuallyRead, nullptr) || actuallyRead == 0) {
            break; // EOF or a transient read error; stop with whatever was parsed so far
        }

        DWORD wholeRecords = actuallyRead / recordSize;
        for (DWORD i = 0; i < wholeRecords; ++i) {
            BYTE* recordPtr = chunk.data() + static_cast<size_t>(i) * recordSize;

            if (ApplyFixup(recordPtr, recordSize, sectorSize)) {
                ParsedRecord parsed = ParseRecord(recordPtr, recordSize);
                if (parsed.valid) {
                    uint64_t frn = (static_cast<uint64_t>(parsed.sequenceNumber) << 48) | recordIndex;

                    // Reserved metadata files (record indexes 0-15: $MFT, $LogFile, the root
                    // directory, etc.) and the self-referencing root aren't meaningful search
                    // results, but the root's node is still kept in the map below so ordinary
                    // top-level files/folders can resolve their path up to it.
                    bool isReserved = recordIndex < kReservedRecordCount;

                    UsnNode node;
                    node.parentFrn = parsed.parentFrn;
                    node.name = parsed.name;
                    node.attributes = parsed.hasStdAttributes ? parsed.stdAttributes : parsed.fileNameAttributes;
                    if (parsed.isDirectory) node.attributes |= FILE_ATTRIBUTE_DIRECTORY;
                    nodes[frn] = std::move(node);

                    if (!parsed.isDirectory) {
                        uint64_t size = parsed.hasDataSize ? parsed.dataSize : parsed.fileNameRealSize;
                        sizes[frn] = { size, true };
                    }

                    TimeInfo ti;
                    ti.created = parsed.createdTime;
                    ti.createdKnown = parsed.hasCreatedTime;
                    ti.modified = parsed.modifiedTime;
                    ti.modifiedKnown = parsed.hasModifiedTime;
                    times[frn] = ti;

                    if (!isReserved) {
                        ++processed;
                    }
                }
            }
            // A fixup failure or an invalid/unusable record is simply skipped; it never crashes
            // or aborts the overall scan.
        }

        recordIndex += wholeRecords;
        if (onProgress) onProgress(processed);
        if (actuallyRead < toRead) break; // short read: end of the file
    }

    // Build IndexedItems from the collected nodes, reconstructing full paths. Timestamps were
    // already captured per node above via the StandardInformation/FileName fallback chain, but
    // we need them per-item too, so re-derive from the same ParsedRecord data is unnecessary:
    // 'nodes' already carries everything path resolution needs, and 'sizes' carries size. We
    // re-walk 'nodes' (not the raw chunks) here, which is cheap since it's already in memory.
    PathReconstructor reconstructor(driveRoot);
    result.items.reserve(nodes.size());

    for (const auto& kv : nodes) {
        uint64_t frn = kv.first;
        uint64_t recIdx = frn & 0xFFFFFFFFFFFFULL;
        if (recIdx < kReservedRecordCount) continue; // skip $MFT, root directory, etc.

        const UsnNode& node = kv.second;
        if (node.name.empty() || node.parentFrn == frn) continue;

        if (isCancelled && isCancelled()) {
            result.failureReason = L"Indexing cancelled.";
            return result;
        }

        IndexedItem item;
        item.fileReferenceNumber = frn;
        item.parentFileReferenceNumber = node.parentFrn;
        item.name = node.name;
        item.attributes = node.attributes;
        item.isDirectory = (node.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        item.driveLetter = driveLetter;
        item.source = L"Raw MFT";

        std::vector<std::wstring> diagnostics;
        item.fullPath = reconstructor.Resolve(frn, nodes, &diagnostics);
        item.diagnostics = std::move(diagnostics);

        if (!item.isDirectory) {
            item.extension = StringUtil::GetExtension(item.name);
            auto sizeIt = sizes.find(frn);
            if (sizeIt != sizes.end()) {
                item.size = sizeIt->second.first;
                item.sizeKnown = sizeIt->second.second;
            }
        }

        auto timeIt = times.find(frn);
        if (timeIt != times.end()) {
            item.createdTime = timeIt->second.created;
            item.createdTimeKnown = timeIt->second.createdKnown;
            item.modifiedTime = timeIt->second.modified;
            item.modifiedTimeKnown = timeIt->second.modifiedKnown;
        }

        result.items.push_back(std::move(item));
    }

    result.success = true;
    return result;
}
