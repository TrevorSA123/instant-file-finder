#include "ShellHelpers.h"
#include "FormatUtil.h"
#include "Handle.h"

#include <shellapi.h>
#include <shlwapi.h>

#pragma comment(lib, "shlwapi.lib")

namespace ShellHelpers {

ActionResult OpenPath(HWND owner, const std::wstring& path) {
    ActionResult result;

    DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        result.success = false;
        result.message = L"This item no longer exists at:\n" + path;
        return result;
    }

    HINSTANCE h = ShellExecuteW(owner, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    auto code = reinterpret_cast<INT_PTR>(h);
    if (code <= 32) {
        result.success = false;
        result.message = L"Could not open item: " + FormatUtil::FormatWin32Error(static_cast<DWORD>(code));
        return result;
    }

    result.success = true;
    return result;
}

ActionResult OpenContainingFolder(HWND owner, const std::wstring& path, bool isDirectory) {
    ActionResult result;

    DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        result.success = false;
        result.message = L"This item no longer exists at:\n" + path;
        return result;
    }

    HINSTANCE h = nullptr;
    if (isDirectory) {
        h = ShellExecuteW(owner, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else {
        std::wstring args = L"/select,\"" + path + L"\"";
        h = ShellExecuteW(owner, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    }

    auto code = reinterpret_cast<INT_PTR>(h);
    if (code <= 32) {
        result.success = false;
        result.message = L"Could not open Explorer: " + FormatUtil::FormatWin32Error(static_cast<DWORD>(code));
        return result;
    }

    result.success = true;
    return result;
}

ActionResult DeleteToRecycleBin(HWND owner, const std::wstring& path) {
    ActionResult result;

    DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        result.success = false;
        result.message = L"This item no longer exists at:\n" + path;
        return result;
    }

    // SHFileOperationW's pFrom needs a double-null-terminated list; appending one explicit
    // '\0' before c_str() gives exactly that (the string's own terminator supplies the second).
    std::wstring buffer = path;
    buffer.push_back(L'\0');

    SHFILEOPSTRUCTW op{};
    op.hwnd = owner;
    op.wFunc = FO_DELETE;
    op.pFrom = buffer.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI;

    int code = SHFileOperationW(&op);
    if (code != 0 || op.fAnyOperationsAborted) {
        result.success = false;
        result.message = L"Could not delete this item. It may be in use, or you may not have permission.";
        return result;
    }

    result.success = true;
    return result;
}

bool IsProcessElevated() {
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken)) {
        return false;
    }
    UniqueHandle token(rawToken);

    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    if (!GetTokenInformation(token.Get(), TokenElevation, &elevation, sizeof(elevation), &size)) {
        return false;
    }
    return elevation.TokenIsElevated != 0;
}

bool RelaunchElevated(HWND owner) {
    wchar_t exePath[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (len == 0 || len == MAX_PATH) {
        return false;
    }

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_FLAG_NO_UI;
    sei.hwnd = owner;
    sei.lpVerb = L"runas";
    sei.lpFile = exePath;
    sei.nShow = SW_SHOWNORMAL;

    return ShellExecuteExW(&sei) != FALSE;
}

std::wstring GetAppDataDirectory(bool localAppData) {
    const wchar_t* varName = localAppData ? L"LOCALAPPDATA" : L"APPDATA";

    wchar_t buffer[MAX_PATH];
    DWORD len = GetEnvironmentVariableW(varName, buffer, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return L"";
    }

    std::wstring dir = buffer;
    if (!dir.empty() && dir.back() != L'\\') dir += L'\\';
    dir += L"InstantFileFinder";

    CreateDirectoryW(dir.c_str(), nullptr); // ignore ERROR_ALREADY_EXISTS and other soft failures

    return dir;
}

} // namespace ShellHelpers
