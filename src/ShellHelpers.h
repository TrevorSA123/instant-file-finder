#pragma once
// Shell integration: opening files/folders, revealing files in Explorer, and locating the
// app's own per-user storage directories. Read-only with respect to user files.

#include <windows.h>
#include <string>

namespace ShellHelpers {

struct ActionResult {
    bool success = false;
    std::wstring message; // friendly error message when !success
};

// Opens a file or folder with its associated application (ShellExecuteW).
ActionResult OpenPath(HWND owner, const std::wstring& path);

// Opens Explorer with the item selected (files) or the folder itself opened (folders).
ActionResult OpenContainingFolder(HWND owner, const std::wstring& path, bool isDirectory);

// Moves a file or folder to the Recycle Bin (SHFileOperationW with FOF_ALLOWUNDO), never a
// permanent delete. Caller is responsible for confirming with the user first.
ActionResult DeleteToRecycleBin(HWND owner, const std::wstring& path);

// Returns true if the current process is running elevated.
bool IsProcessElevated();

// Relaunches the current executable with the 'runas' verb (prompts UAC). Returns false if the
// user cancels the elevation prompt or the relaunch fails.
bool RelaunchElevated(HWND owner);

// %LocalAppData%\InstantFileFinder or %AppData%\InstantFileFinder, created if missing.
// Returns an empty string if the environment variable is unavailable.
std::wstring GetAppDataDirectory(bool localAppData);

} // namespace ShellHelpers
