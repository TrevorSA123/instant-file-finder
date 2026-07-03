#include "FileTimeUtil.h"

#include <cwchar>

namespace FileTimeUtil {

int Compare(const FILETIME& a, const FILETIME& b) {
    ULARGE_INTEGER ua{}, ub{};
    ua.LowPart = a.dwLowDateTime; ua.HighPart = a.dwHighDateTime;
    ub.LowPart = b.dwLowDateTime; ub.HighPart = b.dwHighDateTime;
    if (ua.QuadPart < ub.QuadPart) return -1;
    if (ua.QuadPart > ub.QuadPart) return 1;
    return 0;
}

FILETIME Now() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ft;
}

static FILETIME LocalToFileTime(const SYSTEMTIME& localSt) {
    SYSTEMTIME utcSt{};
    TzSpecificLocalTimeToSystemTime(nullptr, &localSt, &utcSt);
    FILETIME ft{};
    SystemTimeToFileTime(&utcSt, &ft);
    return ft;
}

FILETIME LocalMidnightDaysAgo(int daysAgo) {
    FILETIME nowFt = Now();
    SYSTEMTIME utcNow{};
    FileTimeToSystemTime(&nowFt, &utcNow);
    SYSTEMTIME localNow{};
    SystemTimeToTzSpecificLocalTime(nullptr, &utcNow, &localNow);

    // Convert to a day count using FILETIME arithmetic to safely subtract days across month/year boundaries.
    SYSTEMTIME localMidnight = localNow;
    localMidnight.wHour = 0;
    localMidnight.wMinute = 0;
    localMidnight.wSecond = 0;
    localMidnight.wMilliseconds = 0;

    FILETIME midnightFt = LocalToFileTime(localMidnight);
    ULARGE_INTEGER u{};
    u.LowPart = midnightFt.dwLowDateTime;
    u.HighPart = midnightFt.dwHighDateTime;

    const ULONGLONG ticksPerDay = 10000000ULL * 60ULL * 60ULL * 24ULL;
    u.QuadPart -= static_cast<ULONGLONG>(daysAgo) * ticksPerDay;

    FILETIME result{};
    result.dwLowDateTime = u.LowPart;
    result.dwHighDateTime = u.HighPart;
    return result;
}

FILETIME StartOfThisWeekLocal() {
    FILETIME nowFt = Now();
    SYSTEMTIME utcNow{};
    FileTimeToSystemTime(&nowFt, &utcNow);
    SYSTEMTIME localNow{};
    SystemTimeToTzSpecificLocalTime(nullptr, &utcNow, &localNow);

    // wDayOfWeek: 0 = Sunday .. 6 = Saturday. Treat Monday as the first day of the week.
    int daysSinceMonday = (localNow.wDayOfWeek == 0) ? 6 : (localNow.wDayOfWeek - 1);
    return LocalMidnightDaysAgo(daysSinceMonday);
}

std::wstring Format(const FILETIME& ft, bool known) {
    if (!known) return L"";
    if (ft.dwLowDateTime == 0 && ft.dwHighDateTime == 0) return L"";

    SYSTEMTIME utc{};
    if (!FileTimeToSystemTime(&ft, &utc)) return L"";
    SYSTEMTIME local{};
    if (!SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) local = utc;

    wchar_t buf[64];
    swprintf_s(buf, L"%04u-%02u-%02u %02u:%02u", local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute);
    return buf;
}

} // namespace FileTimeUtil
