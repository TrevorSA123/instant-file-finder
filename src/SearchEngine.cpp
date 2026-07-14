#include "SearchEngine.h"
#include "StringUtil.h"
#include "FileTimeUtil.h"

#include <windows.h>

namespace SearchEngine {

namespace {

bool MatchesName(const IndexedItem& item, const SearchOptions& opts, const std::wregex* compiledRegex) {
    if (opts.query.empty()) return true;

    switch (opts.mode) {
        case SearchMode::Contains:
            return opts.caseSensitive ? (item.name.find(opts.query) != std::wstring::npos)
                                       : StringUtil::ContainsIgnoreCase(item.name, opts.query);
        case SearchMode::StartsWith:
            return opts.caseSensitive ? StringUtil::StartsWith(item.name, opts.query)
                                       : StringUtil::StartsWithIgnoreCase(item.name, opts.query);
        case SearchMode::EndsWith:
            return opts.caseSensitive ? StringUtil::EndsWith(item.name, opts.query)
                                       : StringUtil::EndsWithIgnoreCase(item.name, opts.query);
        case SearchMode::Exact:
            return opts.caseSensitive ? (item.name == opts.query)
                                       : StringUtil::EqualsIgnoreCase(item.name, opts.query);
        case SearchMode::Wildcard:
            return StringUtil::WildcardMatch(opts.query, item.name, opts.caseSensitive);
        case SearchMode::Regex:
            return compiledRegex != nullptr && std::regex_search(item.name, *compiledRegex);
    }
    return false;
}

bool PassesFilters(const IndexedItem& item, const SearchOptions& opts) {
    if (opts.filesOnly && item.isDirectory) return false;
    if (opts.foldersOnly && !item.isDirectory) return false;

    if (!opts.includeHidden && (item.attributes & FILE_ATTRIBUTE_HIDDEN)) return false;
    if (!opts.includeSystem && (item.attributes & FILE_ATTRIBUTE_SYSTEM)) return false;

    if (!opts.driveFilter.empty()) {
        wchar_t wanted = towupper(opts.driveFilter[0]);
        if (towupper(item.driveLetter) != wanted) return false;
    }

    if (!opts.extensionFilter.empty()) {
        if (!StringUtil::EqualsIgnoreCase(item.extension, opts.extensionFilter)) return false;
    }

    if (!opts.pathSubstring.empty()) {
        if (!StringUtil::ContainsIgnoreCase(item.fullPath, opts.pathSubstring)) return false;
    }

    // Size and modified-date filters. The active index often lacks this data: NTFS USN
    // enumeration provides no file size at all, and its enumerated records' timestamps come back
    // as zero (a full-volume enumeration isn't a real journal event), so a USN-built index has no
    // usable modified time either. So for any item whose needed value is missing - treating a zero
    // FILETIME as missing regardless of the stored "known" flag, which also repairs older caches
    // that persisted a zero time as "known" - resolve it on demand with a single
    // GetFileAttributesExW. IsMatch checks the cheap in-memory name match before calling this, so
    // these stats run only for items that actually match the query, not the whole index.
    if (opts.sizeFilterEnabled || opts.modifiedDateFilterEnabled) {
        uint64_t size = item.size;
        bool sizeKnown = item.sizeKnown;
        FILETIME modifiedTime = item.modifiedTime;
        bool modifiedKnown = item.modifiedTimeKnown &&
                             (item.modifiedTime.dwLowDateTime != 0 || item.modifiedTime.dwHighDateTime != 0);

        bool needSize = opts.sizeFilterEnabled && !item.isDirectory && !sizeKnown;
        bool needDate = opts.modifiedDateFilterEnabled && !modifiedKnown;
        if (needSize || needDate) {
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (GetFileAttributesExW(item.fullPath.c_str(), GetFileExInfoStandard, &data)) {
                bool statIsDir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                if (needSize && !statIsDir) {
                    ULARGE_INTEGER sz{};
                    sz.LowPart = data.nFileSizeLow;
                    sz.HighPart = data.nFileSizeHigh;
                    size = sz.QuadPart;
                    sizeKnown = true;
                }
                if (needDate) {
                    modifiedTime = data.ftLastWriteTime;
                    modifiedKnown = true;
                }
            }
        }

        if (opts.sizeFilterEnabled) {
            if (item.isDirectory) return false; // folders have no size to compare against
            if (!sizeKnown) return false;
            if (size < opts.minSize) return false;
            if (opts.maxSize > 0 && size > opts.maxSize) return false;
        }

        if (opts.modifiedDateFilterEnabled) {
            if (!modifiedKnown) return false;
            if (FileTimeUtil::Compare(modifiedTime, opts.modifiedAfter) < 0) return false;
            if (FileTimeUtil::Compare(modifiedTime, opts.modifiedBefore) > 0) return false;
        }
    }

    return true;
}

} // namespace

bool TryCompileRegex(const SearchOptions& opts, std::wregex& outRegex, std::wstring& outError) {
    if (opts.mode != SearchMode::Regex || opts.query.empty()) return true;

    try {
        auto flags = std::regex_constants::ECMAScript;
        if (!opts.caseSensitive) flags |= std::regex_constants::icase;
        outRegex = std::wregex(opts.query, flags);
        return true;
    } catch (const std::regex_error&) {
        outError = L"That regular expression isn't valid. Showing no results for this search.";
        return false;
    }
}

bool IsMatch(const IndexedItem& item, const SearchOptions& opts, const std::wregex* compiledRegex) {
    // Name match first: it's a cheap in-memory check, whereas PassesFilters may fall back to a
    // per-file disk stat when a size/date filter is active against an index that lacks that data.
    // Ordering the name match first means that stat only happens for items that actually match the
    // query, instead of once per item across the whole index.
    return MatchesName(item, opts, compiledRegex) && PassesFilters(item, opts);
}

SearchOutcome Execute(const ItemSource& source, const SearchOptions& opts, const CancelPredicate& isCancelled) {
    SearchOutcome outcome;

    std::wregex compiledRegex;
    std::wstring regexError;
    if (!TryCompileRegex(opts, compiledRegex, regexError)) {
        outcome.queryError = true;
        outcome.errorMessage = regexError;
        return outcome;
    }
    bool haveRegex = (opts.mode == SearchMode::Regex && !opts.query.empty());

    size_t maxResults = opts.maxResults > 0 ? opts.maxResults : 10000;

    source([&](const IndexedItem& item) {
        if (isCancelled && isCancelled()) return;
        if (!IsMatch(item, opts, haveRegex ? &compiledRegex : nullptr)) return;

        ++outcome.totalMatched;
        if (outcome.results.size() < maxResults) {
            outcome.results.push_back(item);
        } else {
            outcome.truncated = true;
        }
    });

    return outcome;
}

} // namespace SearchEngine
