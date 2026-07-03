#pragma once
// Parses the friendly search-box syntax (ext:, path:, drive:, type:, size:>, modified:, etc.)
// into a SearchOptions. Deliberately simple: whitespace-separated tokens, no quoting. Any
// token that isn't a recognized "key:value" pair is treated as literal search text. Malformed
// input never throws; it just falls back to being treated as plain text.

#include "IndexTypes.h"

namespace QueryParser {

// 'base' supplies the starting point (UI combo selections for mode/drive/type filter/case
// sensitivity, plus maxResults). The returned SearchOptions has 'query' set to the remaining
// free-text portion and any recognized tokens applied on top of 'base'.
SearchOptions Parse(const std::wstring& rawQuery, const SearchOptions& base);

} // namespace QueryParser
