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

// One contiguous physical extent of the MFT: a starting cluster (LCN) and a count of clusters.
struct DataRun {
    uint64_t startLcn = 0;
    uint64_t clusterCount = 0;
};

// Parses the non-resident, unnamed $DATA attribute of MFT record 0 (the $MFT describing itself)
// into the list of physical cluster runs that make up the whole MFT. This is what lets us read the
// MFT straight off the volume - following its real on-disk extents, fragmentation included -
// instead of opening \\.\X:\$MFT by name (which AV/endpoint software commonly blocks). Returns
// false if record 0 isn't the expected shape. 'record' must already be fixed up.
bool ParseMftDataRuns(const BYTE* record, size_t recordSize, std::vector<DataRun>& outRuns) {
    ByteReader reader(record, recordSize);

    DWORD magic = 0;
    if (!reader.Read(0, magic) || magic != kFileRecordMagic) return false;

    uint16_t firstAttrOffset = 0;
    uint32_t bytesInUse = 0;
    if (!reader.Read(0x14, firstAttrOffset) || !reader.Read(0x18, bytesInUse)) return false;
    if (bytesInUse > recordSize) bytesInUse = static_cast<uint32_t>(recordSize);

    size_t offset = firstAttrOffset;
    while (offset + 8 <= bytesInUse) {
        DWORD typeCode = 0, attrLen = 0;
        if (!reader.Read(offset, typeCode)) return false;
        if (typeCode == kAttrEndMarker) break;
        if (!reader.Read(offset + 4, attrLen)) return false;
        if (attrLen < 8 || offset + attrLen > recordSize) return false;

        BYTE nonResident = 0, nameLength = 0;
        reader.Read(offset + 8, nonResident);
        reader.Read(offset + 9, nameLength);

        if (typeCode == kAttrData && nonResident == 1 && nameLength == 0) {
            uint16_t mappingPairsOffset = 0;
            if (!reader.Read(offset + 0x20, mappingPairsOffset)) return false;

            size_t runPos = offset + mappingPairsOffset;
            const size_t runEnd = offset + attrLen;
            uint64_t currentLcn = 0;
            while (runPos < runEnd) {
                BYTE header = 0;
                if (!reader.Read(runPos, header)) return false;
                if (header == 0) break; // end of the run list
                ++runPos;

                size_t lenBytes = header & 0x0F;
                size_t offBytes = (header >> 4) & 0x0F;
                if (lenBytes == 0 || lenBytes > 8 || offBytes > 8) return false;

                uint64_t runLength = 0;
                for (size_t i = 0; i < lenBytes; ++i) {
                    BYTE b = 0;
                    if (!reader.Read(runPos + i, b)) return false;
                    runLength |= static_cast<uint64_t>(b) << (8 * i);
                }
                runPos += lenBytes;

                if (offBytes == 0) {
                    // Sparse run (a hole) - no physical clusters. The MFT's own $DATA isn't
                    // sparse in practice; skip rather than emit a bogus extent.
                    continue;
                }

                int64_t delta = 0;
                for (size_t i = 0; i < offBytes; ++i) {
                    BYTE b = 0;
                    if (!reader.Read(runPos + i, b)) return false;
                    delta |= static_cast<int64_t>(b) << (8 * i);
                }
                // The run offset is a signed delta from the previous run's LCN; sign-extend it.
                if (offBytes < 8) {
                    int64_t signBit = static_cast<int64_t>(1) << (offBytes * 8 - 1);
                    if (delta & signBit) delta |= -(static_cast<int64_t>(1) << (offBytes * 8));
                }
                runPos += offBytes;

                currentLcn = static_cast<uint64_t>(static_cast<int64_t>(currentLcn) + delta);
                if (runLength > 0) outRuns.push_back({ currentLcn, runLength });
            }
            return !outRuns.empty();
        }

        offset += attrLen;
    }
    return false;
}

} // namespace

RawMftIndexer::Result RawMftIndexer::BuildIndex(wchar_t driveLetter, const CancelPredicate& isCancelled,
                                                  const ProgressCallback& onProgress) {
    Result result;

    std::wstring volumePath = std::wstring(L"\\\\.\\") + driveLetter + L":";
    std::wstring driveRoot = std::wstring(1, driveLetter) + L":\\";

    // Open the volume for raw reads. This needs an elevated (Administrator) token, but - unlike the
    // older approach of opening \\.\X:\$MFT by name - it needs neither SeBackupPrivilege nor an
    // open of the $MFT metadata file itself. Opening $MFT by name is commonly denied by AV/endpoint
    // software (and some policies) even for a fully elevated, backup-privileged process; reading the
    // MFT's clusters straight off this volume handle side-steps that entirely.
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

    const DWORD recordSize = volData.BytesPerFileRecordSegment;
    const DWORD sectorSize = volData.BytesPerSector;
    const uint64_t bytesPerCluster = volData.BytesPerCluster;
    if (recordSize == 0 || sectorSize == 0 || bytesPerCluster == 0 || recordSize > (16 * 1024)) {
        result.failureReason = L"Unexpected NTFS volume geometry; falling back.";
        return result;
    }

    uint64_t mftValidBytes = static_cast<uint64_t>(volData.MftValidDataLength.QuadPart);
    uint64_t totalRecords = mftValidBytes / recordSize;

    // Raw volume reads must be sector-aligned in both offset and length. Cluster-aligned offsets
    // and cluster-multiple lengths satisfy that (a cluster is a whole number of sectors), so we
    // seek to LCN*bytesPerCluster and read in cluster-multiple chunks throughout.
    auto readAt = [&](uint64_t byteOffset, BYTE* buf, DWORD length) -> bool {
        LARGE_INTEGER li{};
        li.QuadPart = static_cast<LONGLONG>(byteOffset);
        if (!SetFilePointerEx(volume.Get(), li, nullptr, FILE_BEGIN)) return false;
        DWORD got = 0;
        return ReadFile(volume.Get(), buf, length, &got, nullptr) && got == length;
    };

    // Bootstrap: read MFT record 0 ($MFT itself) from its start cluster and parse its $DATA runs,
    // which describe where the rest of the MFT physically lives (it can be fragmented).
    uint64_t mftStartOffset = static_cast<uint64_t>(volData.MftStartLcn.QuadPart) * bytesPerCluster;
    DWORD bootLen = ((recordSize + sectorSize - 1) / sectorSize) * sectorSize; // record 0, sector-padded
    std::vector<BYTE> bootBuf(bootLen);
    if (!readAt(mftStartOffset, bootBuf.data(), bootLen)) {
        result.failureReason = L"Could not read $MFT record 0 from volume: " + FormatUtil::FormatWin32Error(GetLastError());
        return result;
    }
    if (!ApplyFixup(bootBuf.data(), recordSize, sectorSize)) {
        result.failureReason = L"$MFT record 0 failed fixup validation.";
        return result;
    }
    std::vector<DataRun> runs;
    if (!ParseMftDataRuns(bootBuf.data(), recordSize, runs)) {
        result.failureReason = L"Could not parse $MFT data runs from record 0.";
        return result;
    }

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

    uint64_t recordIndex = 0; // positional MFT record number; advances for every slot, valid or not
    uint64_t processed = 0;

    // Turns one record-sized slot into map entries. recordIndex is the record's position in the
    // MFT (== its base file reference number) and advances here for every slot so the frn stays
    // correct across chunk/run boundaries.
    auto handleRecord = [&](BYTE* recordPtr) {
        uint64_t thisIndex = recordIndex++;
        if (!ApplyFixup(recordPtr, recordSize, sectorSize)) return;
        ParsedRecord parsed = ParseRecord(recordPtr, recordSize);
        if (!parsed.valid) return;

        uint64_t frn = (static_cast<uint64_t>(parsed.sequenceNumber) << 48) | thisIndex;

        // Reserved metadata files (record indexes 0-15: $MFT, $LogFile, the root directory, etc.)
        // and the self-referencing root aren't meaningful search results, but the root's node is
        // still kept in the map below so ordinary top-level files/folders can resolve their path.
        bool isReserved = thisIndex < kReservedRecordCount;

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

        if (!isReserved) ++processed;
    };

    // Walk the MFT's physical runs, reading in large cluster-aligned chunks. The MFT is one logical
    // stream of fixed-size records, but a record can straddle a chunk (or run) boundary, so any
    // trailing partial record is carried over and prepended to the next read.
    const uint64_t chunkBytes = std::max<uint64_t>(bytesPerCluster,
        (static_cast<uint64_t>(4 * 1024 * 1024) / bytesPerCluster) * bytesPerCluster);
    std::vector<BYTE> readBuf(static_cast<size_t>(chunkBytes));
    std::vector<BYTE> carry; // leftover bytes not yet forming a whole record
    bool done = false;

    for (const DataRun& run : runs) {
        if (done) break;
        uint64_t physOffset = run.startLcn * bytesPerCluster;
        uint64_t bytesLeft = run.clusterCount * bytesPerCluster;

        while (bytesLeft > 0 && !done) {
            if (isCancelled && isCancelled()) {
                result.failureReason = L"Indexing cancelled.";
                return result;
            }

            DWORD toRead = static_cast<DWORD>(std::min<uint64_t>(readBuf.size(), bytesLeft));
            if (!readAt(physOffset, readBuf.data(), toRead)) {
                break; // transient read error in this run; stop with whatever was parsed so far
            }
            physOffset += toRead;
            bytesLeft -= toRead;

            // Parse directly out of readBuf when nothing is carried over (the common case, since a
            // cluster is normally a whole number of records); otherwise splice the carry in front.
            BYTE* data = nullptr;
            size_t dataLen = 0;
            if (carry.empty()) {
                data = readBuf.data();
                dataLen = toRead;
            } else {
                carry.insert(carry.end(), readBuf.data(), readBuf.data() + toRead);
                data = carry.data();
                dataLen = carry.size();
            }

            size_t pos = 0;
            while (dataLen - pos >= recordSize) {
                if (recordIndex >= totalRecords) { done = true; break; }
                handleRecord(data + pos);
                pos += recordSize;
            }

            size_t remaining = dataLen - pos;
            if (carry.empty()) {
                carry.assign(data + pos, data + pos + remaining);
            } else {
                carry.erase(carry.begin(), carry.begin() + pos);
            }
        }

        if (onProgress) onProgress(processed);
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
