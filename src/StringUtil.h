#pragma once
// Small string helpers used throughout the app. Kept dependency-free (no <regex> here;
// that lives in SearchEngine so QueryParser/StringUtil stay lightweight).

#include <string>
#include <vector>

namespace StringUtil {

// Returns a lowercase copy using the invariant Win32 API (fast, locale-stable enough for search keys).
std::wstring ToLower(const std::wstring& s);

bool EqualsIgnoreCase(const std::wstring& a, const std::wstring& b);
bool ContainsIgnoreCase(const std::wstring& haystack, const std::wstring& needle);
bool StartsWithIgnoreCase(const std::wstring& s, const std::wstring& prefix);
bool EndsWithIgnoreCase(const std::wstring& s, const std::wstring& suffix);

bool StartsWith(const std::wstring& s, const std::wstring& prefix);
bool EndsWith(const std::wstring& s, const std::wstring& suffix);

// Simple '*'/'?' wildcard matcher (case-insensitive unless caseSensitive is true).
bool WildcardMatch(const std::wstring& pattern, const std::wstring& text, bool caseSensitive);

std::wstring Trim(const std::wstring& s);
std::vector<std::wstring> Split(const std::wstring& s, wchar_t delimiter);

std::wstring GetExtension(const std::wstring& fileName);

// Converts a narrow UTF-8 string to wide, and back. Used only for the settings/cache files.
std::wstring Utf8ToWide(const std::string& utf8);
std::string WideToUtf8(const std::wstring& wide);

} // namespace StringUtil
