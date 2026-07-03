#include "StringUtil.h"

#include <windows.h>
#include <algorithm>
#include <cwctype>

namespace StringUtil {

std::wstring ToLower(const std::wstring& s) {
    if (s.empty()) return s;
    std::wstring result(s);
    // CharLowerBuffW mutates in place; falls back to ::towlower which we already avoid depending on locale.
    CharLowerBuffW(&result[0], static_cast<DWORD>(result.size()));
    return result;
}

bool EqualsIgnoreCase(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size()) return false;
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

bool ContainsIgnoreCase(const std::wstring& haystack, const std::wstring& needle) {
    if (needle.empty()) return true;
    std::wstring h = ToLower(haystack);
    std::wstring n = ToLower(needle);
    return h.find(n) != std::wstring::npos;
}

bool StartsWithIgnoreCase(const std::wstring& s, const std::wstring& prefix) {
    if (prefix.size() > s.size()) return false;
    return _wcsnicmp(s.c_str(), prefix.c_str(), prefix.size()) == 0;
}

bool EndsWithIgnoreCase(const std::wstring& s, const std::wstring& suffix) {
    if (suffix.size() > s.size()) return false;
    return _wcsnicmp(s.c_str() + (s.size() - suffix.size()), suffix.c_str(), suffix.size()) == 0;
}

bool StartsWith(const std::wstring& s, const std::wstring& prefix) {
    if (prefix.size() > s.size()) return false;
    return s.compare(0, prefix.size(), prefix) == 0;
}

bool EndsWith(const std::wstring& s, const std::wstring& suffix) {
    if (suffix.size() > s.size()) return false;
    return s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Classic recursive-with-backtracking wildcard match, iterative to avoid deep recursion on long names.
bool WildcardMatch(const std::wstring& pattern, const std::wstring& text, bool caseSensitive) {
    std::wstring p = caseSensitive ? pattern : ToLower(pattern);
    std::wstring t = caseSensitive ? text : ToLower(text);

    size_t pi = 0, ti = 0;
    size_t starIdx = std::wstring::npos, matchIdx = 0;

    while (ti < t.size()) {
        if (pi < p.size() && (p[pi] == L'?' || p[pi] == t[ti])) {
            ++pi;
            ++ti;
        } else if (pi < p.size() && p[pi] == L'*') {
            starIdx = pi;
            matchIdx = ti;
            ++pi;
        } else if (starIdx != std::wstring::npos) {
            pi = starIdx + 1;
            ++matchIdx;
            ti = matchIdx;
        } else {
            return false;
        }
    }

    while (pi < p.size() && p[pi] == L'*') {
        ++pi;
    }

    return pi == p.size();
}

std::wstring Trim(const std::wstring& s) {
    size_t start = 0;
    size_t end = s.size();
    while (start < end && iswspace(s[start])) ++start;
    while (end > start && iswspace(s[end - 1])) --end;
    return s.substr(start, end - start);
}

std::vector<std::wstring> Split(const std::wstring& s, wchar_t delimiter) {
    std::vector<std::wstring> result;
    size_t start = 0;
    while (start <= s.size()) {
        size_t pos = s.find(delimiter, start);
        if (pos == std::wstring::npos) {
            result.push_back(s.substr(start));
            break;
        }
        result.push_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    return result;
}

std::wstring GetExtension(const std::wstring& fileName) {
    size_t dot = fileName.find_last_of(L'.');
    size_t sep = fileName.find_last_of(L"\\/");
    if (dot == std::wstring::npos) return L"";
    if (sep != std::wstring::npos && dot < sep) return L"";
    if (dot + 1 >= fileName.size()) return L"";
    return fileName.substr(dot + 1);
}

std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    if (len <= 0) return L"";
    std::wstring result(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), &result[0], len);
    return result;
}

std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string result(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), &result[0], len, nullptr, nullptr);
    return result;
}

} // namespace StringUtil
