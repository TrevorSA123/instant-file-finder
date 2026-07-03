#pragma once
// Enumerates local drives and their basic identity (type, filesystem, label, serial number).
// Pure query module: no indexing happens here.

#include "IndexTypes.h"
#include <vector>

namespace DriveEnumerator {

// Returns one DriveIndexStatus entry per drive letter reported by GetLogicalDrives,
// with driveType/fileSystem/label/volumeSerialNumber populated and a suggested scanMethod.
// Drives that cannot be queried (e.g. empty optical drive) still appear, with fileSystem empty.
std::vector<DriveIndexStatus> EnumerateDrives();

// Suggests a scan method for a drive based on its type and filesystem, matching the
// documented defaults: fixed NTFS drives get fast USN scan, other fixed/removable drives
// get recursive fallback, network/optical drives default to disabled.
ScanMethod SuggestScanMethod(UINT driveType, const std::wstring& fileSystem);

} // namespace DriveEnumerator
