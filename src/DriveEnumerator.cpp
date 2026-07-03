#include "DriveEnumerator.h"

#include <windows.h>

namespace DriveEnumerator {

ScanMethod SuggestScanMethod(UINT driveType, const std::wstring& fileSystem) {
    if (driveType == DRIVE_FIXED) {
        // RawMft is the ceiling, not a guarantee: IndexManager's cascade tries it only if
        // enabled in Preferences and actually works (needs Administrator), otherwise it falls
        // through to NTFS USN, then Recursive - so this default is safe even for non-elevated,
        // default-settings runs (Raw MFT is off by default, so the cascade lands on USN).
        return (fileSystem == L"NTFS") ? ScanMethod::RawMft : ScanMethod::Recursive;
    }
    if (driveType == DRIVE_REMOVABLE) {
        return ScanMethod::Recursive; // off by default via 'selected', but method suggestion is recursive
    }
    if (driveType == DRIVE_REMOTE) {
        return ScanMethod::Recursive; // off by default via 'selected'
    }
    // DRIVE_CDROM, DRIVE_RAMDISK, DRIVE_UNKNOWN, DRIVE_NO_ROOT_DIR
    return ScanMethod::Disabled;
}

std::vector<DriveIndexStatus> EnumerateDrives() {
    std::vector<DriveIndexStatus> drives;

    DWORD mask = GetLogicalDrives();
    if (mask == 0) {
        return drives;
    }

    for (wchar_t letter = L'A'; letter <= L'Z'; ++letter) {
        DWORD bit = 1u << (letter - L'A');
        if ((mask & bit) == 0) continue;

        DriveIndexStatus status;
        status.driveRoot = std::wstring(1, letter) + L":\\";
        status.driveType = GetDriveTypeW(status.driveRoot.c_str());

        wchar_t volumeName[MAX_PATH + 1] = {};
        wchar_t fsName[MAX_PATH + 1] = {};
        DWORD serial = 0;
        DWORD maxComponentLen = 0;
        DWORD fsFlags = 0;

        // Optical/removable drives without media, or network drives that are offline, will fail here;
        // that's expected and reported to the user rather than treated as a crash.
        BOOL ok = GetVolumeInformationW(
            status.driveRoot.c_str(),
            volumeName, ARRAYSIZE(volumeName),
            &serial, &maxComponentLen, &fsFlags,
            fsName, ARRAYSIZE(fsName));

        if (ok) {
            status.label = volumeName;
            status.fileSystem = fsName;
            status.volumeSerialNumber = serial;
            status.statusMessage = L"Not indexed";
        } else {
            DWORD err = GetLastError();
            if (err == ERROR_NOT_READY) {
                status.statusMessage = L"No media";
            } else if (err == ERROR_ACCESS_DENIED) {
                status.statusMessage = L"Access denied";
                status.requiresElevation = true;
            } else {
                status.statusMessage = L"Unavailable";
            }
        }

        status.scanMethod = SuggestScanMethod(status.driveType, status.fileSystem);
        // Placeholder only, describing the configured ceiling before any scan has actually run;
        // IndexManager overwrites this with whichever method truly ran once indexing completes.
        status.indexingMethod = (status.scanMethod == ScanMethod::RawMft) ? L"Raw MFT"
                                 : (status.scanMethod == ScanMethod::FastNtfsUsn) ? L"NTFS USN"
                                 : (status.scanMethod == ScanMethod::Recursive) ? L"Recursive"
                                                                                 : L"Disabled";

        // Sensible defaults: only fixed NTFS drives are pre-selected; removable/network require opt-in.
        status.selected = (status.driveType == DRIVE_FIXED && !status.fileSystem.empty());

        drives.push_back(std::move(status));
    }

    return drives;
}

} // namespace DriveEnumerator
