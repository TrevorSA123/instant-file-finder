#include "QueryParser.h"
#include "StringUtil.h"
#include "FormatUtil.h"
#include "FileTimeUtil.h"

#include <cwctype>

namespace QueryParser {

namespace {

bool TryApplyToken(const std::wstring& token, SearchOptions& opts) {
    size_t colon = token.find(L':');
    if (colon == std::wstring::npos || colon == 0) return false;

    std::wstring key = StringUtil::ToLower(token.substr(0, colon));
    std::wstring value = token.substr(colon + 1);
    if (value.empty()) return false;

    if (key == L"ext") {
        std::wstring ext = value;
        if (!ext.empty() && ext.front() == L'.') ext.erase(ext.begin());
        opts.extensionFilter = ext;
        return true;
    }

    if (key == L"path") {
        opts.pathSubstring = value;
        return true;
    }

    if (key == L"drive") {
        std::wstring drive = value;
        if (!drive.empty() && drive.back() == L':') drive.pop_back();
        if (!drive.empty()) {
            opts.driveFilter = StringUtil::ToLower(drive.substr(0, 1)) + L":";
            // Normalize to the display form used elsewhere (upper-case letter, e.g. "C:").
            opts.driveFilter[0] = towupper(opts.driveFilter[0]);
        }
        return true;
    }

    if (key == L"type") {
        std::wstring t = StringUtil::ToLower(value);
        if (t == L"file" || t == L"files") {
            opts.filesOnly = true;
            opts.foldersOnly = false;
        } else if (t == L"folder" || t == L"folders" || t == L"dir" || t == L"directory") {
            opts.foldersOnly = true;
            opts.filesOnly = false;
        }
        return true;
    }

    if (key == L"hidden") {
        std::wstring v = StringUtil::ToLower(value);
        opts.includeHidden = (v == L"true" || v == L"1" || v == L"yes");
        return true;
    }

    if (key == L"system") {
        std::wstring v = StringUtil::ToLower(value);
        opts.includeSystem = (v == L"true" || v == L"1" || v == L"yes");
        return true;
    }

    if (key == L"size") {
        wchar_t op = L'>'; // default: at least this size
        std::wstring numeric = value;
        if (!numeric.empty() && (numeric.front() == L'>' || numeric.front() == L'<')) {
            op = numeric.front();
            numeric.erase(numeric.begin());
        }
        uint64_t bytes = 0;
        if (!FormatUtil::ParseSize(numeric, bytes)) return true; // recognized key, unparsable value: ignore value only

        opts.sizeFilterEnabled = true;
        if (op == L'>') {
            opts.minSize = bytes;
            if (opts.maxSize == 0) opts.maxSize = UINT64_MAX;
        } else {
            opts.maxSize = bytes;
        }
        return true;
    }

    if (key == L"modified") {
        std::wstring v = StringUtil::ToLower(value);
        FILETIME now = FileTimeUtil::Now();
        if (v == L"today") {
            opts.modifiedAfter = FileTimeUtil::LocalMidnightDaysAgo(0);
            opts.modifiedBefore = now;
            opts.modifiedDateFilterEnabled = true;
        } else if (v == L"yesterday") {
            opts.modifiedAfter = FileTimeUtil::LocalMidnightDaysAgo(1);
            opts.modifiedBefore = FileTimeUtil::LocalMidnightDaysAgo(0);
            opts.modifiedDateFilterEnabled = true;
        } else if (v == L"this-week" || v == L"thisweek") {
            opts.modifiedAfter = FileTimeUtil::StartOfThisWeekLocal();
            opts.modifiedBefore = now;
            opts.modifiedDateFilterEnabled = true;
        }
        return true;
    }

    return false; // unrecognized key: treat the whole token as literal text
}

} // namespace

SearchOptions Parse(const std::wstring& rawQuery, const SearchOptions& base) {
    SearchOptions opts = base;

    std::vector<std::wstring> freeTextParts;
    std::wstring trimmed = StringUtil::Trim(rawQuery);

    size_t start = 0;
    while (start <= trimmed.size()) {
        size_t sp = trimmed.find(L' ', start);
        std::wstring token = (sp == std::wstring::npos) ? trimmed.substr(start) : trimmed.substr(start, sp - start);
        if (!token.empty()) {
            if (!TryApplyToken(token, opts)) {
                freeTextParts.push_back(token);
            }
        }
        if (sp == std::wstring::npos) break;
        start = sp + 1;
    }

    std::wstring freeText;
    for (size_t i = 0; i < freeTextParts.size(); ++i) {
        if (i > 0) freeText += L' ';
        freeText += freeTextParts[i];
    }
    opts.query = freeText;

    if (opts.mode == SearchMode::Contains &&
        (freeText.find(L'*') != std::wstring::npos || freeText.find(L'?') != std::wstring::npos)) {
        opts.mode = SearchMode::Wildcard;
        opts.useWildcard = true;
    }

    return opts;
}

} // namespace QueryParser
