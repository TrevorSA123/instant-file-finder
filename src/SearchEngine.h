#pragma once
// Fast in-memory matching over the index. Decoupled from IndexManager: the caller supplies an
// 'ItemSource' callback that iterates items (IndexManager::ForEachItem fits this signature),
// so SearchEngine has no knowledge of locking or storage.
//
// TryCompileRegex()/IsMatch() are also exposed directly so LiveSearchManager (which walks the
// filesystem on demand instead of an in-memory index) can reuse the exact same matching rules.

#include "IndexTypes.h"

#include <functional>
#include <regex>
#include <vector>

namespace SearchEngine {

struct SearchOutcome {
    std::vector<IndexedItem> results;
    uint64_t totalMatched = 0; // may exceed results.size() when truncated
    bool truncated = false;
    bool queryError = false; // e.g. invalid regex
    std::wstring errorMessage;
};

using ItemSource = std::function<void(const std::function<void(const IndexedItem&)>&)>;

// Compiles opts.query as a regex when opts.mode == Regex and query is non-empty. Returns false
// (with a friendly outError) if the pattern is invalid; returns true (leaving outRegex
// untouched) when no compilation is needed.
bool TryCompileRegex(const SearchOptions& opts, std::wregex& outRegex, std::wstring& outError);

// Single source of truth for "does this item match these options", shared by the in-memory
// Execute() below and by LiveSearchManager's on-demand filesystem walk. compiledRegex may be
// null when opts.mode != Regex.
bool IsMatch(const IndexedItem& item, const SearchOptions& opts, const std::wregex* compiledRegex);

// Never throws: invalid regex patterns are reported via SearchOutcome::queryError rather than
// propagating an exception to the caller.
SearchOutcome Execute(const ItemSource& source, const SearchOptions& opts);

} // namespace SearchEngine
