#include "FormatUtil.h"

#include <cwchar>
#include <cwctype>

namespace FormatUtil {

std::wstring FormatSize(uint64_t bytes, bool known) {
    if (!known) return L"unknown";

    const wchar_t* units[] = { L"B", L"KB", L"MB", L"GB", L"TB", L"PB" };
    double value = static_cast<double>(bytes);
    int unitIndex = 0;
    while (value >= 1024.0 && unitIndex < 5) {
        value /= 1024.0;
        ++unitIndex;
    }

    wchar_t buf[64];
    if (unitIndex == 0) {
        swprintf_s(buf, L"%llu %s", static_cast<unsigned long long>(bytes), units[unitIndex]);
    } else {
        swprintf_s(buf, L"%.1f %s", value, units[unitIndex]);
    }
    return buf;
}

std::wstring FormatAttributes(uint32_t attributes) {
    std::wstring result;
    auto append = [&result](const wchar_t* tag) {
        if (!result.empty()) result += L' ';
        result += tag;
    };

    if (attributes & FILE_ATTRIBUTE_DIRECTORY) append(L"D");
    if (attributes & FILE_ATTRIBUTE_HIDDEN) append(L"H");
    if (attributes & FILE_ATTRIBUTE_SYSTEM) append(L"S");
    if (attributes & FILE_ATTRIBUTE_READONLY) append(L"R");
    if (attributes & FILE_ATTRIBUTE_ARCHIVE) append(L"A");
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) append(L"L");
    if (attributes & FILE_ATTRIBUTE_COMPRESSED) append(L"C");
    if (attributes & FILE_ATTRIBUTE_ENCRYPTED) append(L"E");
    if (attributes & FILE_ATTRIBUTE_SPARSE_FILE) append(L"P");
    if (attributes & FILE_ATTRIBUTE_TEMPORARY) append(L"T");

    return result;
}

std::wstring FormatWin32Error(DWORD errorCode) {
    LPWSTR buffer = nullptr;
    DWORD len = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, errorCode, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);

    std::wstring message;
    if (len > 0 && buffer != nullptr) {
        message.assign(buffer, len);
        while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n')) {
            message.pop_back();
        }
    } else {
        wchar_t fallback[64];
        swprintf_s(fallback, L"Error code %lu", errorCode);
        message = fallback;
    }
    if (buffer != nullptr) LocalFree(buffer);
    return message;
}

bool ParseSize(const std::wstring& text, uint64_t& outBytes) {
    if (text.empty()) return false;

    size_t i = 0;
    while (i < text.size() && (iswdigit(text[i]) || text[i] == L'.')) ++i;
    if (i == 0) return false;

    double number = 0.0;
    try {
        number = std::stod(text.substr(0, i));
    } catch (...) {
        return false;
    }

    std::wstring unit = text.substr(i);
    while (!unit.empty() && iswspace(unit.front())) unit.erase(unit.begin());
    for (auto& c : unit) c = static_cast<wchar_t>(towupper(c));

    uint64_t multiplier = 1;
    if (unit.empty() || unit == L"B") multiplier = 1ULL;
    else if (unit == L"KB" || unit == L"K") multiplier = 1024ULL;
    else if (unit == L"MB" || unit == L"M") multiplier = 1024ULL * 1024ULL;
    else if (unit == L"GB" || unit == L"G") multiplier = 1024ULL * 1024ULL * 1024ULL;
    else if (unit == L"TB" || unit == L"T") multiplier = 1024ULL * 1024ULL * 1024ULL * 1024ULL;
    else return false;

    outBytes = static_cast<uint64_t>(number * static_cast<double>(multiplier));
    return true;
}

} // namespace FormatUtil
