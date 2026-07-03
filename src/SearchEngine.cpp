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

    if (opts.sizeFilterEnabled) {
        if (!item.sizeKnown) return false;
        if (item.size < opts.minSize) return false;
        if (opts.maxSize > 0 && item.size > opts.maxSize) return false;
    }

    if (opts.modifiedDateFilterEnabled) {
        if (!item.modifiedTimeKnown) return false;
        if (FileTimeUtil::Compare(item.modifiedTime, opts.modifiedAfter) < 0) return false;
        if (FileTimeUtil::Compare(item.modifiedTime, opts.modifiedBefore) > 0) return false;
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
    return PassesFilters(item, opts) && MatchesName(item, opts, compiledRegex);
}

SearchOutcome Execute(const ItemSource& source, const SearchOptions& opts) {
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
