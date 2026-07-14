#pragma once
#include <windows.h>
#include <string>

namespace FileTimeUtil {

// Compares a to b: <0, 0, >0.
int Compare(const FILETIME& a, const FILETIME& b);

FILETIME Now();

// Returns FILETIME for local midnight N days ago (0 = today's midnight).
FILETIME LocalMidnightDaysAgo(int daysAgo);

// Returns FILETIME for the start (Monday) of the current local week.
FILETIME StartOfThisWeekLocal();

// Formats a FILETIME as "YYYY-MM-DD HH:MM" in local time. Returns L"" if unknown/zero.
std::wstring Format(const FILETIME& ft, bool known);

// Formats how long ago 'ft' was as a coarse human-readable string, e.g. "just now",
// "5 minutes ago", "2 hours ago", "3 days ago". Returns L"" if unknown/zero/in the future.
std::wstring FormatAge(const FILETIME& ft, bool known);

} // namespace FileTimeUtil
